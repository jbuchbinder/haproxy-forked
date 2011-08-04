/*
 * Fast Weighted Round Robin load balancing algorithm.
 *
 * Copyright 2000-2009 Willy Tarreau <w@1wt.eu>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 *
 */

#include <common/compat.h>
#include <common/config.h>
#include <common/debug.h>
#include <eb32tree.h>

#include <types/global.h>
#include <types/server.h>

#include <proto/backend.h>
#include <proto/queue.h>

static inline void fwrr_remove_from_tree(struct server *s);
static inline void fwrr_queue_by_weight(struct eb_root *root, struct server *s);
static inline void fwrr_dequeue_srv(struct server *s);
static void fwrr_get_srv(struct server *s);
static void fwrr_queue_srv(struct server *s);


/* This function updates the server trees according to server <srv>'s new
 * state. It should be called when server <srv>'s status changes to down.
 * It is not important whether the server was already down or not. It is not
 * important either that the new state is completely down (the caller may not
 * know all the variables of a server's state).
 */
static void fwrr_set_server_status_down(struct server *srv)
{
	struct proxy *p = srv->proxy;
	struct fwrr_group *grp;

	if (srv->state == srv->prev_state &&
	    srv->eweight == srv->prev_eweight)
		return;

	if (srv_is_usable(srv->state, srv->eweight))
		goto out_update_state;

	if (!srv_is_usable(srv->prev_state, srv->prev_eweight))
		/* server was already down */
		goto out_update_backend;

	grp = (srv->state & SRV_BACKUP) ? &p->lbprm.fwrr.bck : &p->lbprm.fwrr.act;
	grp->next_weight -= srv->prev_eweight;

	if (srv->state & SRV_BACKUP) {
		p->lbprm.tot_wbck = p->lbprm.fwrr.bck.next_weight;
		p->srv_bck--;

		if (srv == p->lbprm.fbck) {
			/* we lost the first backup server in a single-backup
			 * configuration, we must search another one.
			 */
			struct server *srv2 = p->lbprm.fbck;
			do {
				srv2 = srv2->next;
			} while (srv2 &&
				 !((srv2->state & SRV_BACKUP) &&
				   srv_is_usable(srv2->state, srv2->eweight)));
			p->lbprm.fbck = srv2;
		}
	} else {
		p->lbprm.tot_wact = p->lbprm.fwrr.act.next_weight;
		p->srv_act--;
	}

	fwrr_dequeue_srv(srv);
	fwrr_remove_from_tree(srv);

out_update_backend:
	/* check/update tot_used, tot_weight */
	update_backend_weight(p);
 out_update_state:
	srv->prev_state = srv->state;
	srv->prev_eweight = srv->eweight;
}

/* This function updates the server trees according to server <srv>'s new
 * state. It should be called when server <srv>'s status changes to up.
 * It is not important whether the server was already down or not. It is not
 * important either that the new state is completely UP (the caller may not
 * know all the variables of a server's state). This function will not change
 * the weight of a server which was already up.
 */
static void fwrr_set_server_status_up(struct server *srv)
{
	struct proxy *p = srv->proxy;
	struct fwrr_group *grp;

	if (srv->state == srv->prev_state &&
	    srv->eweight == srv->prev_eweight)
		return;

	if (!srv_is_usable(srv->state, srv->eweight))
		goto out_update_state;

	if (srv_is_usable(srv->prev_state, srv->prev_eweight))
		/* server was already up */
		goto out_update_backend;

	grp = (srv->state & SRV_BACKUP) ? &p->lbprm.fwrr.bck : &p->lbprm.fwrr.act;
	grp->next_weight += srv->eweight;

	if (srv->state & SRV_BACKUP) {
		p->lbprm.tot_wbck = p->lbprm.fwrr.bck.next_weight;
		p->srv_bck++;

		if (!(p->options & PR_O_USE_ALL_BK)) {
			if (!p->lbprm.fbck) {
				/* there was no backup server anymore */
				p->lbprm.fbck = srv;
			} else {
				/* we may have restored a backup server prior to fbck,
				 * in which case it should replace it.
				 */
				struct server *srv2 = srv;
				do {
					srv2 = srv2->next;
				} while (srv2 && (srv2 != p->lbprm.fbck));
				if (srv2)
					p->lbprm.fbck = srv;
			}
		}
	} else {
		p->lbprm.tot_wact = p->lbprm.fwrr.act.next_weight;
		p->srv_act++;
	}

	/* note that eweight cannot be 0 here */
	fwrr_get_srv(srv);
	srv->npos = grp->curr_pos + (grp->next_weight + grp->curr_weight - grp->curr_pos) / srv->eweight;
	fwrr_queue_srv(srv);

out_update_backend:
	/* check/update tot_used, tot_weight */
	update_backend_weight(p);
 out_update_state:
	srv->prev_state = srv->state;
	srv->prev_eweight = srv->eweight;
}

/* This function must be called after an update to server <srv>'s effective
 * weight. It may be called after a state change too.
 */
static void fwrr_update_server_weight(struct server *srv)
{
	int old_state, new_state;
	struct proxy *p = srv->proxy;
	struct fwrr_group *grp;

	if (srv->state == srv->prev_state &&
	    srv->eweight == srv->prev_eweight)
		return;

	/* If changing the server's weight changes its state, we simply apply
	 * the procedures we already have for status change. If the state
	 * remains down, the server is not in any tree, so it's as easy as
	 * updating its values. If the state remains up with different weights,
	 * there are some computations to perform to find a new place and
	 * possibly a new tree for this server.
	 */
	 
	old_state = srv_is_usable(srv->prev_state, srv->prev_eweight);
	new_state = srv_is_usable(srv->state, srv->eweight);

	if (!old_state && !new_state) {
		srv->prev_state = srv->state;
		srv->prev_eweight = srv->eweight;
		return;
	}
	else if (!old_state && new_state) {
		fwrr_set_server_status_up(srv);
		return;
	}
	else if (old_state && !new_state) {
		fwrr_set_server_status_down(srv);
		return;
	}

	grp = (srv->state & SRV_BACKUP) ? &p->lbprm.fwrr.bck : &p->lbprm.fwrr.act;
	grp->next_weight = grp->next_weight - srv->prev_eweight + srv->eweight;

	p->lbprm.tot_wact = p->lbprm.fwrr.act.next_weight;
	p->lbprm.tot_wbck = p->lbprm.fwrr.bck.next_weight;

	if (srv->lb_tree == grp->init) {
		fwrr_dequeue_srv(srv);
		fwrr_queue_by_weight(grp->init, srv);
	}
	else if (!srv->lb_tree) {
		/* FIXME: server was down. This is not possible right now but
		 * may be needed soon for slowstart or graceful shutdown.
		 */
		fwrr_dequeue_srv(srv);
		fwrr_get_srv(srv);
		srv->npos = grp->curr_pos + (grp->next_weight + grp->curr_weight - grp->curr_pos) / srv->eweight;
		fwrr_queue_srv(srv);
	} else {
		/* The server is either active or in the next queue. If it's
		 * still in the active queue and it has not consumed all of its
		 * places, let's adjust its next position.
		 */
		fwrr_get_srv(srv);

		if (srv->eweight > 0) {
			int prev_next = srv->npos;
			int step = grp->next_weight / srv->eweight;

			srv->npos = srv->lpos + step;
			srv->rweight = 0;

			if (srv->npos > prev_next)
				srv->npos = prev_next;
			if (srv->npos < grp->curr_pos + 2)
				srv->npos = grp->curr_pos + step;
		} else {
			/* push it into the next tree */
			srv->npos = grp->curr_pos + grp->curr_weight;
		}

		fwrr_dequeue_srv(srv);
		fwrr_queue_srv(srv);
	}

	update_backend_weight(p);
	srv->prev_state = srv->state;
	srv->prev_eweight = srv->eweight;
}

/* Remove a server from a tree. It must have previously been dequeued. This
 * function is meant to be called when a server is going down or has its
 * weight disabled.
 */
static inline void fwrr_remove_from_tree(struct server *s)
{
	s->lb_tree = NULL;
}

/* Queue a server in the weight tree <root>, assuming the weight is >0.
 * We want to sort them by inverted weights, because we need to place
 * heavy servers first in order to get a smooth distribution.
 */
static inline void fwrr_queue_by_weight(struct eb_root *root, struct server *s)
{
	s->lb_node.key = SRV_EWGHT_MAX - s->eweight;
	eb32_insert(root, &s->lb_node);
	s->lb_tree = root;
}

/* This function is responsible for building the weight trees in case of fast
 * weighted round-robin. It also sets p->lbprm.wdiv to the eweight to uweight
 * ratio. Both active and backup groups are initialized.
 */
void fwrr_init_server_groups(struct proxy *p)
{
	struct server *srv;
	struct eb_root init_head = EB_ROOT;

	p->lbprm.set_server_status_up   = fwrr_set_server_status_up;
	p->lbprm.set_server_status_down = fwrr_set_server_status_down;
	p->lbprm.update_server_eweight  = fwrr_update_server_weight;

	p->lbprm.wdiv = BE_WEIGHT_SCALE;
	for (srv = p->srv; srv; srv = srv->next) {
		srv->prev_eweight = srv->eweight = srv->uweight * BE_WEIGHT_SCALE;
		srv->prev_state = srv->state;
	}

	recount_servers(p);
	update_backend_weight(p);

	/* prepare the active servers group */
	p->lbprm.fwrr.act.curr_pos = p->lbprm.fwrr.act.curr_weight =
		p->lbprm.fwrr.act.next_weight = p->lbprm.tot_wact;
	p->lbprm.fwrr.act.curr = p->lbprm.fwrr.act.t0 =
		p->lbprm.fwrr.act.t1 = init_head;
	p->lbprm.fwrr.act.init = &p->lbprm.fwrr.act.t0;
	p->lbprm.fwrr.act.next = &p->lbprm.fwrr.act.t1;

	/* prepare the backup servers group */
	p->lbprm.fwrr.bck.curr_pos = p->lbprm.fwrr.bck.curr_weight =
		p->lbprm.fwrr.bck.next_weight = p->lbprm.tot_wbck;
	p->lbprm.fwrr.bck.curr = p->lbprm.fwrr.bck.t0 =
		p->lbprm.fwrr.bck.t1 = init_head;
	p->lbprm.fwrr.bck.init = &p->lbprm.fwrr.bck.t0;
	p->lbprm.fwrr.bck.next = &p->lbprm.fwrr.bck.t1;

	/* queue active and backup servers in two distinct groups */
	for (srv = p->srv; srv; srv = srv->next) {
		if (!srv_is_usable(srv->state, srv->eweight))
			continue;
		fwrr_queue_by_weight((srv->state & SRV_BACKUP) ?
				p->lbprm.fwrr.bck.init :
				p->lbprm.fwrr.act.init,
				srv);
	}
}

/* simply removes a server from a weight tree */
static inline void fwrr_dequeue_srv(struct server *s)
{
	eb32_delete(&s->lb_node);
}

/* queues a server into the appropriate group and tree depending on its
 * backup status, and ->npos. If the server is disabled, simply assign
 * it to the NULL tree.
 */
static void fwrr_queue_srv(struct server *s)
{
	struct proxy *p = s->proxy;
	struct fwrr_group *grp;

	grp = (s->state & SRV_BACKUP) ? &p->lbprm.fwrr.bck : &p->lbprm.fwrr.act;
	
	/* Delay everything which does not fit into the window and everything
	 * which does not fit into the theorical new window.
	 */
	if (!srv_is_usable(s->state, s->eweight)) {
		fwrr_remove_from_tree(s);
	}
	else if (s->eweight <= 0 ||
		 s->npos >= 2 * grp->curr_weight ||
		 s->npos >= grp->curr_weight + grp->next_weight) {
		/* put into next tree, and readjust npos in case we could
		 * finally take this back to current. */
		s->npos -= grp->curr_weight;
		fwrr_queue_by_weight(grp->next, s);
	}
	else {
		/* The sorting key is stored in units of s->npos * user_weight
		 * in order to avoid overflows. As stated in backend.h, the
		 * lower the scale, the rougher the weights modulation, and the
		 * higher the scale, the lower the number of servers without
		 * overflow. With this formula, the result is always positive,
		 * so we can use eb3�_insert().
		 */
		s->lb_node.key = SRV_UWGHT_RANGE * s->npos +
			(unsigned)(SRV_EWGHT_MAX + s->rweight - s->eweight) / BE_WEIGHT_SCALE;

		eb32_insert(&grp->curr, &s->lb_node);
		s->lb_tree = &grp->curr;
	}
}

/* prepares a server when extracting it from the "init" tree */
static inline void fwrr_get_srv_init(struct server *s)
{
	s->npos = s->rweight = 0;
}

/* prepares a server when extracting it from the "next" tree */
static inline void fwrr_get_srv_next(struct server *s)
{
	struct fwrr_group *grp = (s->state & SRV_BACKUP) ?
		&s->proxy->lbprm.fwrr.bck :
		&s->proxy->lbprm.fwrr.act;

	s->npos += grp->curr_weight;
}

/* prepares a server when it was marked down */
static inline void fwrr_get_srv_down(struct server *s)
{
	struct fwrr_group *grp = (s->state & SRV_BACKUP) ?
		&s->proxy->lbprm.fwrr.bck :
		&s->proxy->lbprm.fwrr.act;

	s->npos = grp->curr_pos;
}

/* prepares a server when extracting it from its tree */
static void fwrr_get_srv(struct server *s)
{
	struct proxy *p = s->proxy;
	struct fwrr_group *grp = (s->state & SRV_BACKUP) ?
		&p->lbprm.fwrr.bck :
		&p->lbprm.fwrr.act;

	if (s->lb_tree == grp->init) {
		fwrr_get_srv_init(s);
	}
	else if (s->lb_tree == grp->next) {
		fwrr_get_srv_next(s);
	}
	else if (s->lb_tree == NULL) {
		fwrr_get_srv_down(s);
	}
}

/* switches trees "init" and "next" for FWRR group <grp>. "init" should be empty
 * when this happens, and "next" filled with servers sorted by weights.
 */
static inline void fwrr_switch_trees(struct fwrr_group *grp)
{
	struct eb_root *swap;
	swap = grp->init;
	grp->init = grp->next;
	grp->next = swap;
	grp->curr_weight = grp->next_weight;
	grp->curr_pos = grp->curr_weight;
}

/* return next server from the current tree in FWRR group <grp>, or a server
 * from the "init" tree if appropriate. If both trees are empty, return NULL.
 */
static struct server *fwrr_get_server_from_group(struct fwrr_group *grp)
{
	struct eb32_node *node;
	struct server *s;

	node = eb32_first(&grp->curr);
	s = eb32_entry(node, struct server, lb_node);
	
	if (!node || s->npos > grp->curr_pos) {
		/* either we have no server left, or we have a hole */
		struct eb32_node *node2;
		node2 = eb32_first(grp->init);
		if (node2) {
			node = node2;
			s = eb32_entry(node, struct server, lb_node);
			fwrr_get_srv_init(s);
			if (s->eweight == 0) /* FIXME: is it possible at all ? */
				node = NULL;
		}
	}
	if (node)
		return s;
	else
		return NULL;
}

/* Computes next position of server <s> in the group. It is mandatory for <s>
 * to have a non-zero, positive eweight.
*/
static inline void fwrr_update_position(struct fwrr_group *grp, struct server *s)
{
	if (!s->npos) {
		/* first time ever for this server */
		s->lpos = grp->curr_pos;
		s->npos = grp->curr_pos + grp->next_weight / s->eweight;
		s->rweight += grp->next_weight % s->eweight;

		if (s->rweight >= s->eweight) {
			s->rweight -= s->eweight;
			s->npos++;
		}
	} else {
		s->lpos = s->npos;
		s->npos += grp->next_weight / s->eweight;
		s->rweight += grp->next_weight % s->eweight;

		if (s->rweight >= s->eweight) {
			s->rweight -= s->eweight;
			s->npos++;
		}
	}
}

/* Return next server from the current tree in backend <p>, or a server from
 * the init tree if appropriate. If both trees are empty, return NULL.
 * Saturated servers are skipped and requeued.
 */
struct server *fwrr_get_next_server(struct proxy *p, struct server *srvtoavoid)
{
	struct server *srv, *full, *avoided;
	struct fwrr_group *grp;
	int switched;

	if (p->srv_act)
		grp = &p->lbprm.fwrr.act;
	else if (p->lbprm.fbck)
		return p->lbprm.fbck;
	else if (p->srv_bck)
		grp = &p->lbprm.fwrr.bck;
	else
		return NULL;

	switched = 0;
	avoided = NULL;
	full = NULL; /* NULL-terminated list of saturated servers */
	while (1) {
		/* if we see an empty group, let's first try to collect weights
		 * which might have recently changed.
		 */
		if (!grp->curr_weight)
			grp->curr_pos = grp->curr_weight = grp->next_weight;

		/* get first server from the "current" tree. When the end of
		 * the tree is reached, we may have to switch, but only once.
		 */
		while (1) {
			srv = fwrr_get_server_from_group(grp);
			if (srv)
				break;
			if (switched) {
				if (avoided) {
					srv = avoided;
					break;
				}
				goto requeue_servers;
			}
			switched = 1;
			fwrr_switch_trees(grp);

		}

		/* OK, we have a server. However, it may be saturated, in which
		 * case we don't want to reconsider it for now. We'll update
		 * its position and dequeue it anyway, so that we can move it
		 * to a better place afterwards.
		 */
		fwrr_update_position(grp, srv);
		fwrr_dequeue_srv(srv);
		grp->curr_pos++;
		if (!srv->maxconn || (!srv->nbpend && srv->served < srv_dynamic_maxconn(srv))) {
			/* make sure it is not the server we are trying to exclude... */
			if (srv != srvtoavoid || avoided)
				break;

			avoided = srv; /* ...but remember that is was selected yet avoided */
		}

		/* the server is saturated or avoided, let's chain it for later reinsertion */
		srv->next_full = full;
		full = srv;
	}

	/* OK, we got the best server, let's update it */
	fwrr_queue_srv(srv);

 requeue_servers:
	/* Requeue all extracted servers. If full==srv then it was
	 * avoided (unsucessfully) and chained, omit it now.
	 */
	if (unlikely(full != NULL)) {
		if (switched) {
			/* the tree has switched, requeue all extracted servers
			 * into "init", because their place was lost, and only
			 * their weight matters.
			 */
			do {
				if (likely(full != srv))
					fwrr_queue_by_weight(grp->init, full);
				full = full->next_full;
			} while (full);
		} else {
			/* requeue all extracted servers just as if they were consumed
			 * so that they regain their expected place.
			 */
			do {
				if (likely(full != srv))
					fwrr_queue_srv(full);
				full = full->next_full;
			} while (full);
		}
	}
	return srv;
}

/*
 * Local variables:
 *  c-indent-level: 8
 *  c-basic-offset: 8
 * End:
 */
