/*
 * FD polling functions for Speculative I/O combined with Linux epoll()
 *
 * Copyright 2000-2009 Willy Tarreau <w@1wt.eu>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 *
 *
 * This code implements "speculative I/O" under Linux. The principle is to
 * try to perform expected I/O before registering the events in the poller.
 * Each time this succeeds, it saves an expensive epoll_ctl(). It generally
 * succeeds for all reads after an accept(), and for writes after a connect().
 * It also improves performance for streaming connections because even if only
 * one side is polled, the other one may react accordingly depending on the
 * level of the buffer.
 *
 * It has a presents drawbacks though. If too many events are set for spec I/O,
 * those ones can starve the polled events. Experiments show that when polled
 * events starve, they quickly turn into spec I/O, making the situation even
 * worse. While we can reduce the number of polled events processed at once,
 * we cannot do this on speculative events because most of them are new ones
 * (avg 2/3 new - 1/3 old from experiments).
 *
 * The solution against this problem relies on those two factors :
 *   1) one FD registered as a spec event cannot be polled at the same time
 *   2) even during very high loads, we will almost never be interested in
 *      simultaneous read and write streaming on the same FD.
 *
 * The first point implies that during starvation, we will not have more than
 * half of our FDs in the poll list, otherwise it means there is less than that
 * in the spec list, implying there is no starvation.
 *
 * The second point implies that we're statically only interested in half of
 * the maximum number of file descriptors at once, because we will unlikely
 * have simultaneous read and writes for a same buffer during long periods.
 *
 * So, if we make it possible to drain maxsock/2/2 during peak loads, then we
 * can ensure that there will be no starvation effect. This means that we must
 * always allocate maxsock/4 events for the poller.
 *
 *
 */

#include <unistd.h>
#include <sys/time.h>
#include <sys/types.h>

#include <common/compat.h>
#include <common/config.h>
#include <common/debug.h>
#include <common/standard.h>
#include <common/ticks.h>
#include <common/time.h>
#include <common/tools.h>

#include <types/global.h>

#include <proto/fd.h>
#include <proto/signal.h>
#include <proto/task.h>

#if defined(USE_MY_EPOLL)
#include <common/epoll.h>
#include <errno.h>
#include <sys/syscall.h>
static _syscall1 (int, epoll_create, int, size);
static _syscall4 (int, epoll_ctl, int, epfd, int, op, int, fd, struct epoll_event *, event);
static _syscall4 (int, epoll_wait, int, epfd, struct epoll_event *, events, int, maxevents, int, timeout);
#else
#include <sys/epoll.h>
#endif

/*
 * We define 4 states for each direction of a file descriptor, which we store
 * as 2 bits :
 *
 *  00 = IDLE : we're not interested in this event
 *  01 = SPEC : perform speculative I/O on this FD
 *  10 = WAIT : really wait for an availability event on this FD (poll)
 *  11 = STOP : was marked WAIT, but disabled. It can switch back to WAIT if
 *              the application changes its mind, otherwise disable FD polling
 *              and switch back to IDLE.
 *
 * Since we do not want to scan all the FD list to find speculative I/O events,
 * we store them in a list consisting in a linear array holding only the FD
 * indexes right now. Note that a closed FD cannot exist in the spec list,
 * because it is closed by fd_delete() which in turn calls __fd_clo() which
 * always removes it from the list.
 *
 * The STOP state requires the event to be present in the spec list so that
 * it can be detected and flushed upon next scan without having to scan the
 * whole FD list.
 *
 * This translates like this :
 *
 *   EVENT_IN_SPEC_LIST = 01
 *   EVENT_IN_POLL_LIST = 10
 *
 *   IDLE = 0
 *   SPEC = (EVENT_IN_SPEC_LIST)
 *   WAIT = (EVENT_IN_POLL_LIST)
 *   STOP = (EVENT_IN_SPEC_LIST|EVENT_IN_POLL_LIST)
 *
 * fd_is_set() just consists in checking that the status is 01 or 10.
 *
 * For efficiency reasons, we will store the Read and Write bits interlaced to
 * form a 4-bit field, so that we can simply shift the value right by 0/1 and
 * get what we want :
 *    3  2  1  0
 *   Wp Rp Ws Rs
 *
 * The FD array has to hold a back reference to the speculative list. This
 * reference is only valid if at least one of the directions is marked SPEC.
 *
 */

#define FD_EV_IN_SL	1
#define FD_EV_IN_PL	4

#define FD_EV_IDLE	0
#define FD_EV_SPEC	(FD_EV_IN_SL)
#define FD_EV_WAIT	(FD_EV_IN_PL)
#define FD_EV_STOP	(FD_EV_IN_SL|FD_EV_IN_PL)

/* Those match any of R or W for Spec list or Poll list */
#define FD_EV_RW_SL	(FD_EV_IN_SL | (FD_EV_IN_SL << 1))
#define FD_EV_RW_PL	(FD_EV_IN_PL | (FD_EV_IN_PL << 1))
#define FD_EV_MASK_DIR	(FD_EV_IN_SL|FD_EV_IN_PL)

#define FD_EV_IDLE_R	0
#define FD_EV_SPEC_R	(FD_EV_IN_SL)
#define FD_EV_WAIT_R	(FD_EV_IN_PL)
#define FD_EV_STOP_R	(FD_EV_IN_SL|FD_EV_IN_PL)
#define FD_EV_MASK_R	(FD_EV_IN_SL|FD_EV_IN_PL)

#define FD_EV_IDLE_W	(FD_EV_IDLE_R << 1)
#define FD_EV_SPEC_W	(FD_EV_SPEC_R << 1)
#define FD_EV_WAIT_W	(FD_EV_WAIT_R << 1)
#define FD_EV_STOP_W	(FD_EV_STOP_R << 1)
#define FD_EV_MASK_W	(FD_EV_MASK_R << 1)

#define FD_EV_MASK	(FD_EV_MASK_W | FD_EV_MASK_R)

/* This is the minimum number of events successfully processed in speculative
 * mode above which we agree to return without checking epoll() (1/2 times).
 */
#define MIN_RETURN_EVENTS	25

static int nbspec = 0;          // current size of the spec list
static int absmaxevents = 0;    // absolute maximum amounts of polled events
static int fd_created = 0;      // fd creation detector, reset upon poll() entry.

static unsigned int *spec_list = NULL;	// speculative I/O list

/* private data */
static struct epoll_event *epoll_events;
static int epoll_fd;

/* This structure may be used for any purpose. Warning! do not use it in
 * recursive functions !
 */
static struct epoll_event ev;


REGPRM1 static inline void alloc_spec_entry(const int fd)
{
	if (fdtab[fd].spec.s1)
		/* sometimes the entry already exists for the other direction */
		return;
	fdtab[fd].spec.s1 = nbspec + 1;
	spec_list[nbspec] = fd;
	nbspec++;
}

/* Removes entry used by fd <fd> from the spec list and replaces it with the
 * last one. The fdtab.spec is adjusted to match the back reference if needed.
 * If the fd has no entry assigned, return immediately.
 */
REGPRM1 static void release_spec_entry(int fd)
{
	unsigned int pos;

	pos = fdtab[fd].spec.s1;
	if (!pos)
		return;

	fdtab[fd].spec.s1 = 0;
	pos--;
	/* we have spec_list[pos]==fd */

	nbspec--;
	if (pos == nbspec)
		return;

	/* we replace current FD by the highest one, which may sometimes be the same */
	fd = spec_list[nbspec];
	spec_list[pos] = fd;
	fdtab[fd].spec.s1 = pos + 1;
}

/*
 * Returns non-zero if <fd> is already monitored for events in direction <dir>.
 */
REGPRM2 static int __fd_is_set(const int fd, int dir)
{
	int ret;

#if DEBUG_DEV
	if (fdtab[fd].state == FD_STCLOSE) {
		fprintf(stderr, "sepoll.fd_isset called on closed fd #%d.\n", fd);
		ABORT_NOW();
	}
#endif
	ret = ((unsigned)fdtab[fd].spec.e >> dir) & FD_EV_MASK_DIR;
	return (ret == FD_EV_SPEC || ret == FD_EV_WAIT);
}

/*
 * Don't worry about the strange constructs in __fd_set/__fd_clr, they are
 * designed like this in order to reduce the number of jumps (verified).
 */
REGPRM2 static int __fd_set(const int fd, int dir)
{
	unsigned int i;

#if DEBUG_DEV
	if (fdtab[fd].state == FD_STCLOSE) {
		fprintf(stderr, "sepoll.fd_set called on closed fd #%d.\n", fd);
		ABORT_NOW();
	}
#endif
	i = ((unsigned)fdtab[fd].spec.e >> dir) & FD_EV_MASK_DIR;

	if (i != FD_EV_STOP) {
		if (unlikely(i != FD_EV_IDLE))
			return 0;
		// switch to SPEC state and allocate a SPEC entry.
		fd_created++;
		alloc_spec_entry(fd);
	}
	fdtab[fd].spec.e ^= (unsigned int)(FD_EV_IN_SL << dir);
	return 1;
}

REGPRM2 static int __fd_clr(const int fd, int dir)
{
	unsigned int i;

#if DEBUG_DEV
	if (fdtab[fd].state == FD_STCLOSE) {
		fprintf(stderr, "sepoll.fd_clr called on closed fd #%d.\n", fd);
		ABORT_NOW();
	}
#endif
	i = ((unsigned)fdtab[fd].spec.e >> dir) & FD_EV_MASK_DIR;

	if (i != FD_EV_SPEC) {
		if (unlikely(i != FD_EV_WAIT))
			return 0;
		// switch to STOP state
		/* We will create a queue entry for this one because we want to
		 * process it later in order to merge it with other events on
		 * the same FD.
		 */
		alloc_spec_entry(fd);
	}
	fdtab[fd].spec.e ^= (unsigned int)(FD_EV_IN_SL << dir);
	return 1;
}

/* normally unused */
REGPRM1 static void __fd_rem(int fd)
{
	__fd_clr(fd, DIR_RD);
	__fd_clr(fd, DIR_WR);
}

/*
 * On valid epoll() implementations, a call to close() automatically removes
 * the fds. This means that the FD will appear as previously unset.
 */
REGPRM1 static void __fd_clo(int fd)
{
	release_spec_entry(fd);
	fdtab[fd].spec.e &= ~(FD_EV_MASK);
}

/*
 * speculative epoll() poller
 */
REGPRM2 static void _do_poll(struct poller *p, int exp)
{
	static unsigned int last_skipped;
	static unsigned int spec_processed;
	int status, eo;
	int fd, opcode;
	int count;
	int spec_idx;
	int wait_time;
	int looping = 0;


 re_poll_once:
	/* Here we have two options :
	 * - either walk the list forwards and hope to match more events
	 * - or walk it backwards to minimize the number of changes and
	 *   to make better use of the cache.
	 * Tests have shown that walking backwards improves perf by 0.2%.
	 */

	status = 0;
	spec_idx = nbspec;
	while (likely(spec_idx > 0)) {
		int done;

		spec_idx--;
		fd = spec_list[spec_idx];
		eo = fdtab[fd].spec.e;  /* save old events */

		if (looping && --fd_created < 0) {
			/* we were just checking the newly created FDs */
			break;
		}
		/*
		 * Process the speculative events.
		 *
		 * Principle: events which are marked FD_EV_SPEC are processed
		 * with their assigned function. If the function returns 0, it
		 * means there is nothing doable without polling first. We will
		 * then convert the event to a pollable one by assigning them
		 * the WAIT status.
		 */

#ifdef DEBUG_DEV
		if (fdtab[fd].state == FD_STCLOSE) {
			fprintf(stderr,"fd=%d, fdtab[].ev=%x, fdtab[].spec.e=%x, .s=%d, idx=%d\n",
				fd, fdtab[fd].ev, fdtab[fd].spec.e, fdtab[fd].spec.s1, spec_idx);
		}
#endif
		done = 0;
		fdtab[fd].ev &= FD_POLL_STICKY;
		if ((eo & FD_EV_MASK_R) == FD_EV_SPEC_R) {
			/* The owner is interested in reading from this FD */
			if (fdtab[fd].state != FD_STERROR) {
				/* Pretend there is something to read */
				fdtab[fd].ev |= FD_POLL_IN;
				if (!fdtab[fd].cb[DIR_RD].f(fd))
					fdtab[fd].spec.e ^= (FD_EV_WAIT_R ^ FD_EV_SPEC_R);
				else
					done = 1;
			}
		}
		else if ((eo & FD_EV_MASK_R) == FD_EV_STOP_R) {
			/* This FD was being polled and is now being removed. */
			fdtab[fd].spec.e &= ~FD_EV_MASK_R;
		}
		
		if ((eo & FD_EV_MASK_W) == FD_EV_SPEC_W) {
			/* The owner is interested in writing to this FD */
			if (fdtab[fd].state != FD_STERROR) {
				/* Pretend there is something to write */
				fdtab[fd].ev |= FD_POLL_OUT;
				if (!fdtab[fd].cb[DIR_WR].f(fd))
					fdtab[fd].spec.e ^= (FD_EV_WAIT_W ^ FD_EV_SPEC_W);
				else
					done = 1;
			}
		}
		else if ((eo & FD_EV_MASK_W) == FD_EV_STOP_W) {
			/* This FD was being polled and is now being removed. */
			fdtab[fd].spec.e &= ~FD_EV_MASK_W;
		}

		status += done;
		/* one callback might already have closed the fd by itself */
		if (fdtab[fd].state == FD_STCLOSE)
			continue;

		/* Now, we will adjust the event in the poll list. Indeed, it
		 * is possible that an event which was previously in the poll
		 * list now goes out, and the opposite is possible too. We can
		 * have opposite changes for READ and WRITE too.
		 */

		if ((eo ^ fdtab[fd].spec.e) & FD_EV_RW_PL) {
			/* poll status changed*/
			if ((fdtab[fd].spec.e & FD_EV_RW_PL) == 0) {
				/* fd removed from poll list */
				opcode = EPOLL_CTL_DEL;
			}
			else if ((eo & FD_EV_RW_PL) == 0) {
				/* new fd in the poll list */
				opcode = EPOLL_CTL_ADD;
			}
			else {
				/* fd status changed */
				opcode = EPOLL_CTL_MOD;
			}

			/* construct the epoll events based on new state */
			ev.events = 0;
			if (fdtab[fd].spec.e & FD_EV_WAIT_R)
				ev.events |= EPOLLIN;

			if (fdtab[fd].spec.e & FD_EV_WAIT_W)
				ev.events |= EPOLLOUT;

			ev.data.fd = fd;
			epoll_ctl(epoll_fd, opcode, fd, &ev);
		}


		if (!(fdtab[fd].spec.e & FD_EV_RW_SL)) {
			/* This fd switched to combinations of either WAIT or
			 * IDLE. It must be removed from the spec list.
			 */
			release_spec_entry(fd);
			continue;
		}
	}

	/* It may make sense to immediately return here if there are enough
	 * processed events, without passing through epoll_wait() because we
	 * have exactly done a poll.
	 * Measures have shown a great performance increase if we call the
	 * epoll_wait() only the second time after speculative accesses have
	 * succeeded. This reduces the number of unsucessful calls to
	 * epoll_wait() by a factor of about 3, and the total number of calls
	 * by about 2.
	 * However, when we do that after having processed too many events,
	 * events waiting in epoll() starve for too long a time and tend to
	 * become themselves eligible for speculative polling. So we try to
	 * limit this practise to reasonable situations.
	 */

	spec_processed += status;

	if (looping) {
		last_skipped++;
		return;
	}

	if (status >= MIN_RETURN_EVENTS && spec_processed < absmaxevents) {
		/* We have processed at least MIN_RETURN_EVENTS, it's worth
		 * returning now without checking epoll_wait().
		 */
		if (++last_skipped <= 1) {
			tv_update_date(0, 1);
			return;
		}
	}
	last_skipped = 0;

	if (nbspec || status || run_queue || signal_queue_len) {
		/* Maybe we have processed some events that we must report, or
		 * maybe we still have events in the spec list, or there are
		 * some tasks left pending in the run_queue, so we must not
		 * wait in epoll() otherwise we will delay their delivery by
		 * the next timeout.
		 */
		wait_time = 0;
	}
	else {
		if (!exp)
			wait_time = MAX_DELAY_MS;
		else if (tick_is_expired(exp, now_ms))
			wait_time = 0;
		else {
			wait_time = TICKS_TO_MS(tick_remain(now_ms, exp)) + 1;
			if (wait_time > MAX_DELAY_MS)
				wait_time = MAX_DELAY_MS;
		}
	}

	/* now let's wait for real events. We normally use maxpollevents as a
	 * high limit, unless <nbspec> is already big, in which case we need
	 * to compensate for the high number of events processed there.
	 */
	fd = MIN(absmaxevents, spec_processed);
	fd = MAX(global.tune.maxpollevents, fd);
	fd = MIN(maxfd, fd);
	/* we want to detect if an accept() will create new speculative FDs here */
	fd_created = 0;
	spec_processed = 0;
	status = epoll_wait(epoll_fd, epoll_events, fd, wait_time);
	tv_update_date(wait_time, status);

	for (count = 0; count < status; count++) {
		int e = epoll_events[count].events;
		fd = epoll_events[count].data.fd;

		/* it looks complicated but gcc can optimize it away when constants
		 * have same values.
		 */
		DPRINTF(stderr, "%s:%d: fd=%d, ev=0x%08x, e=0x%08x\n",
			__FUNCTION__, __LINE__,
			fd, fdtab[fd].ev, e);

		fdtab[fd].ev &= FD_POLL_STICKY;
		fdtab[fd].ev |= 
			((e & EPOLLIN ) ? FD_POLL_IN  : 0) |
			((e & EPOLLPRI) ? FD_POLL_PRI : 0) |
			((e & EPOLLOUT) ? FD_POLL_OUT : 0) |
			((e & EPOLLERR) ? FD_POLL_ERR : 0) |
			((e & EPOLLHUP) ? FD_POLL_HUP : 0);
		
		if ((fdtab[fd].spec.e & FD_EV_MASK_R) == FD_EV_WAIT_R) {
			if (fdtab[fd].state == FD_STCLOSE || fdtab[fd].state == FD_STERROR)
				continue;
			if (fdtab[fd].ev & (FD_POLL_IN|FD_POLL_HUP|FD_POLL_ERR))
				fdtab[fd].cb[DIR_RD].f(fd);
		}

		if ((fdtab[fd].spec.e & FD_EV_MASK_W) == FD_EV_WAIT_W) {
			if (fdtab[fd].state == FD_STCLOSE || fdtab[fd].state == FD_STERROR)
				continue;
			if (fdtab[fd].ev & (FD_POLL_OUT|FD_POLL_ERR))
				fdtab[fd].cb[DIR_WR].f(fd);
		}
	}

	if (fd_created) {
		/* we have created some fds, certainly in return of an accept(),
		 * and they're marked as speculative. If we can manage to perform
		 * a read(), we're almost sure to collect all the request at once
		 * and avoid several expensive wakeups. So let's try now. Anyway,
		 * if we fail, the tasks are still woken up, and the FD gets marked
		 * for poll mode.
		 */
		looping = 1;
		goto re_poll_once;
	}
}

/*
 * Initialization of the speculative epoll() poller.
 * Returns 0 in case of failure, non-zero in case of success. If it fails, it
 * disables the poller by setting its pref to 0.
 */
REGPRM1 static int _do_init(struct poller *p)
{
	__label__ fail_spec, fail_ee, fail_fd;

	p->private = NULL;

	epoll_fd = epoll_create(global.maxsock + 1);
	if (epoll_fd < 0)
		goto fail_fd;

	/* See comments at the top of the file about this formula. */
	absmaxevents = MAX(global.tune.maxpollevents, global.maxsock/4);
	epoll_events = (struct epoll_event*)
		calloc(1, sizeof(struct epoll_event) * absmaxevents);

	if (epoll_events == NULL)
		goto fail_ee;

	if ((spec_list = (uint32_t *)calloc(1, sizeof(uint32_t) * global.maxsock)) == NULL)
		goto fail_spec;

	return 1;

 fail_spec:
	free(epoll_events);
 fail_ee:
	close(epoll_fd);
	epoll_fd = -1;
 fail_fd:
	p->pref = 0;
	return 0;
}

/*
 * Termination of the speculative epoll() poller.
 * Memory is released and the poller is marked as unselectable.
 */
REGPRM1 static void _do_term(struct poller *p)
{
	free(spec_list);
	free(epoll_events);

	if (epoll_fd >= 0) {
		close(epoll_fd);
		epoll_fd = -1;
	}

	spec_list = NULL;
	epoll_events = NULL;

	p->private = NULL;
	p->pref = 0;
}

/*
 * Check that the poller works.
 * Returns 1 if OK, otherwise 0.
 */
REGPRM1 static int _do_test(struct poller *p)
{
	int fd;

	fd = epoll_create(global.maxsock + 1);
	if (fd < 0)
		return 0;
	close(fd);
	return 1;
}

/*
 * Recreate the epoll file descriptor after a fork(). Returns 1 if OK,
 * otherwise 0. It will ensure that all processes will not share their
 * epoll_fd. Some side effects were encountered because of this, such
 * as epoll_wait() returning an FD which was previously deleted.
 */
REGPRM1 static int _do_fork(struct poller *p)
{
	if (epoll_fd >= 0)
		close(epoll_fd);
	epoll_fd = epoll_create(global.maxsock + 1);
	if (epoll_fd < 0)
		return 0;
	return 1;
}

/*
 * It is a constructor, which means that it will automatically be called before
 * main(). This is GCC-specific but it works at least since 2.95.
 * Special care must be taken so that it does not need any uninitialized data.
 */
__attribute__((constructor))
static void _do_register(void)
{
	struct poller *p;

	if (nbpollers >= MAX_POLLERS)
		return;

	epoll_fd = -1;
	p = &pollers[nbpollers++];

	p->name = "sepoll";
	p->pref = 400;
	p->private = NULL;

	p->test = _do_test;
	p->init = _do_init;
	p->term = _do_term;
	p->poll = _do_poll;
	p->fork = _do_fork;

	p->is_set  = __fd_is_set;
	p->cond_s = p->set = __fd_set;
	p->cond_c = p->clr = __fd_clr;
	p->rem = __fd_rem;
	p->clo = __fd_clo;
}


/*
 * Local variables:
 *  c-indent-level: 8
 *  c-basic-offset: 8
 * End:
 */
