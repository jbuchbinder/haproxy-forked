/*
 * FD polling functions for FreeBSD kqueue()
 *
 * Copyright 2000-2008 Willy Tarreau <w@1wt.eu>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 *
 * Note: not knowing much about kqueue, I had to rely on OpenBSD's detailed man
 * page and to check how it was implemented in lighttpd to understand it better.
 * But it is possible that I got things wrong.
 *
 */

#include <unistd.h>
#include <sys/time.h>
#include <sys/types.h>

#include <sys/event.h>
#include <sys/time.h>

#include <common/compat.h>
#include <common/config.h>
#include <common/ticks.h>
#include <common/time.h>
#include <common/tools.h>

#include <types/global.h>

#include <proto/fd.h>
#include <proto/signal.h>
#include <proto/task.h>

/* private data */
static fd_set *fd_evts[2];
static int kqueue_fd;
static struct kevent *kev = NULL;

/* speeds up conversion of DIR_RD/DIR_WR to EVFILT* */
static const int dir2filt[2] = { EVFILT_READ, EVFILT_WRITE };

/* completes a change list for deletion */
REGPRM3 static int kqev_del(struct kevent *kev, const int fd, const int dir)
{
	if (FD_ISSET(fd, fd_evts[dir])) {
		FD_CLR(fd, fd_evts[dir]);
		EV_SET(kev, fd, dir2filt[dir], EV_DELETE, 0, 0, NULL);
		return 1;
	}
	return 0;
}

/*
 * Returns non-zero if direction <dir> is already set for <fd>.
 */
REGPRM2 static int __fd_is_set(const int fd, int dir)
{
	return FD_ISSET(fd, fd_evts[dir]);
}

REGPRM2 static int __fd_set(const int fd, int dir)
{
	/* if the value was set, do nothing */
	if (FD_ISSET(fd, fd_evts[dir]))
		return 0;

	FD_SET(fd, fd_evts[dir]);
	EV_SET(kev, fd, dir2filt[dir], EV_ADD, 0, 0, NULL);
	kevent(kqueue_fd, kev, 1, NULL, 0, NULL);
	return 1;
}

REGPRM2 static int __fd_clr(const int fd, int dir)
{
	if (!kqev_del(kev, fd, dir))
		return 0;
	kevent(kqueue_fd, kev, 1, NULL, 0, NULL);
	return 1;
}

REGPRM1 static void __fd_rem(int fd)
{
	int changes = 0;

	changes += kqev_del(&kev[changes], fd, DIR_RD);
	changes += kqev_del(&kev[changes], fd, DIR_WR);

	if (changes)
		kevent(kqueue_fd, kev, changes, NULL, 0, NULL);
}

REGPRM1 static void __fd_clo(int fd)
{
	FD_CLR(fd, fd_evts[DIR_RD]);
	FD_CLR(fd, fd_evts[DIR_WR]);
}

/*
 * kqueue() poller
 */
REGPRM2 static void _do_poll(struct poller *p, int exp)
{
	int status;
	int count, fd, delta_ms;
	struct timespec timeout;

	delta_ms        = 0;
	timeout.tv_sec  = 0;
	timeout.tv_nsec = 0;

	if (!run_queue && !signal_queue_len) {
		if (!exp) {
			delta_ms        = MAX_DELAY_MS;
			timeout.tv_sec  = (MAX_DELAY_MS / 1000);
			timeout.tv_nsec = (MAX_DELAY_MS % 1000) * 1000000;
		}
		else if (!tick_is_expired(exp, now_ms)) {
			delta_ms = TICKS_TO_MS(tick_remain(now_ms, exp)) + 1;
			if (delta_ms > MAX_DELAY_MS)
				delta_ms = MAX_DELAY_MS;
			timeout.tv_sec  = (delta_ms / 1000);
			timeout.tv_nsec = (delta_ms % 1000) * 1000000;
		}
	}

	fd = MIN(maxfd, global.tune.maxpollevents);
	status = kevent(kqueue_fd, // int kq
			NULL,      // const struct kevent *changelist
			0,         // int nchanges
			kev,       // struct kevent *eventlist
			fd,        // int nevents
			&timeout); // const struct timespec *timeout
	tv_update_date(delta_ms, status);

	for (count = 0; count < status; count++) {
		fd = kev[count].ident;
		if (kev[count].filter ==  EVFILT_READ) {
			if (FD_ISSET(fd, fd_evts[DIR_RD])) {
				if (fdtab[fd].state == FD_STCLOSE)
					continue;
				fdtab[fd].cb[DIR_RD].f(fd);
			}
		} else if (kev[count].filter ==  EVFILT_WRITE) {
			if (FD_ISSET(fd, fd_evts[DIR_WR])) {
				if (fdtab[fd].state == FD_STCLOSE)
					continue;
				fdtab[fd].cb[DIR_WR].f(fd);
			}
		}
	}
}

/*
 * Initialization of the kqueue() poller.
 * Returns 0 in case of failure, non-zero in case of success. If it fails, it
 * disables the poller by setting its pref to 0.
 */
REGPRM1 static int _do_init(struct poller *p)
{
	__label__ fail_wevt, fail_revt, fail_fd;
	int fd_set_bytes;

	p->private = NULL;
	fd_set_bytes = sizeof(fd_set) * (global.maxsock + FD_SETSIZE - 1) / FD_SETSIZE;

	kqueue_fd = kqueue();
	if (kqueue_fd < 0)
		goto fail_fd;

	kev = (struct kevent*)calloc(1, sizeof(struct kevent) * global.tune.maxpollevents);

	if (kev == NULL)
		goto fail_kev;
		
	if ((fd_evts[DIR_RD] = (fd_set *)calloc(1, fd_set_bytes)) == NULL)
		goto fail_revt;

	if ((fd_evts[DIR_WR] = (fd_set *)calloc(1, fd_set_bytes)) == NULL)
		goto fail_wevt;

	return 1;

 fail_wevt:
	free(fd_evts[DIR_RD]);
 fail_revt:
	free(kev);
 fail_kev:
	close(kqueue_fd);
	kqueue_fd = -1;
 fail_fd:
	p->pref = 0;
	return 0;
}

/*
 * Termination of the kqueue() poller.
 * Memory is released and the poller is marked as unselectable.
 */
REGPRM1 static void _do_term(struct poller *p)
{
	free(fd_evts[DIR_WR]);
	free(fd_evts[DIR_RD]);
	free(kev);

	if (kqueue_fd >= 0) {
		close(kqueue_fd);
		kqueue_fd = -1;
	}

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

	fd = kqueue();
	if (fd < 0)
		return 0;
	close(fd);
	return 1;
}

/*
 * Recreate the kqueue file descriptor after a fork(). Returns 1 if OK,
 * otherwise 0. Note that some pollers need to be reopened after a fork()
 * (such as kqueue), and some others may fail to do so in a chroot.
 */
REGPRM1 static int _do_fork(struct poller *p)
{
	if (kqueue_fd >= 0)
		close(kqueue_fd);
	kqueue_fd = kqueue();
	if (kqueue_fd < 0)
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

	kqueue_fd = -1;
	p = &pollers[nbpollers++];

	p->name = "kqueue";
	p->pref = 300;
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
