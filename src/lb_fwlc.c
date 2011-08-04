/*
 * Fast Weighted Least Connection load balancing algorithm.
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


/* Remove a server from a tree. It must have previously been dequeued. This
 * function is meant to be called when a server is going down or has its
 * weight disabled.
 */
static inline void fwlc_remove_from_tree(struct server *s)
{
	s->lb_tree = NULL;
}

/* simply removes a server from a tree */
static inline void fwlc_dequeue_srv(struct server *s)
{
	eb32_delete(&s->lb_node);
}

/* Queue a server in its associated tree, assuming the weight is >0.
 * Servers are sorted by #conns/weight. To ensure maximum accuracy,
 * we use #conns*SRV_EWGHT_MAX/eweight as the sorting key.
 */
static inline void fwlc_queue_srv(struct server *s)
{
	s->lb_node.key = s->served * SRV_EWGHT_MAX / s->eweight;
	eb32_insert(s->lb_tree, &s->lb_node);
}

/* Re-position the server in the FWLC tree after it has been assigned one
 * connection or after it has released one. Note that it is possible that
 * the server has been moved out of the tree due to failed health-checks.
 */
static void fwlc_srv_reposition(struct server *s)
{
	if (!s->lb_tree)
		return;
	fwlc_dequeue_srv(s);
	fwlc_queue_srv(s);
}

/* This function updates the server trees according to server <srv>'s new
 * state. It should be called when server <srv>'s status changes to down.
 * It is not important whether the server was already down or not. It is not
 * important either that the new state is completely down (the caller may not
 * know all the variables of a server's state).
 */
static void fwlc_set_server_status_down(struct server *srv)
{
	struct proxy *p = srv->proxy;

	if (srv->state == srv->prev_state &&
	    srv->eweight == srv->prev_eweight)
		return;

	if (srv_is_usable(srv->state, srv->eweight))
		goto out_update_state;

	if (!srv_is_usable(srv->prev_state, srv->prev_eweight))
		/* server was already down */
		goto out_update_backend;

	if (srv->state & SRV_BACKUP) {
		p->lbprm.tot_wbck -= srv->prev_eweight;
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
		p->lbprm.tot_wact -= srv->prev_eweight;
		p->srv_act--;
	}

	fwlc_dequeue_srv(srv);
	fwlc_remove_from_tree(srv);

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
static void fwlc_set_server_status_up(struct server *srv)
{
	struct proxy *p = srv->proxy;

	if (srv->state == srv->prev_state &&
	    srv->eweight == srv->prev_eweight)
		return;

	if (!srv_is_usable(srv->state, srv->eweight))
		goto out_update_state;

	if (srv_is_usable(srv->prev_state, srv->prev_eweight))
		/* server was already up */
		goto out_update_backend;

	if (srv->state & SRV_BACKUP) {
		srv->lb_tree = &p->lbprm.fwlc.bck;
		p->lbprm.tot_wbck += srv->eweight;
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
		srv->lb_tree = &p->lbprm.fwlc.act;
		p->lbprm.tot_wact += srv->eweight;
		p->srv_act++;
	}

	/* note that eweight cannot be 0 here */
	fwlc_queue_srv(srv);

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
static void fwlc_update_server_weight(struct server *srv)
{
	int old_state, new_state;
	struct proxy *p = srv->proxy;

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
		fwlc_set_server_status_up(srv);
		return;
	}
	else if (old_state && !new_state) {
		fwlc_set_server_status_down(srv);
		return;
	}

	if (srv->lb_tree)
		fwlc_dequeue_srv(srv);

	if (srv->state & SRV_BACKUP) {
		p->lbprm.tot_wbck += srv->eweight - srv->prev_eweight;
		srv->lb_tree = &p->lbprm.fwlc.bck;
	} else {
		p->lbprm.tot_wact += srv->eweight - srv->prev_eweight;
		srv->lb_tree = &p->lbprm.fwlc.act;
	}

	fwlc_queue_srv(srv);

	update_backend_weight(p);
	srv->prev_state = srv->state;
	srv->prev_eweight = srv->eweight;
}

/* This function is responsible for building the trees in case of fast
 * weighted least-conns. It also sets p->lbprm.wdiv to the eweight to
 * uweight ratio. Both active and backup groups are initialized.
 */
void fwlc_init_server_tree(struct proxy *p)
{
	struct server *srv;
	struct eb_root init_head = EB_ROOT;

	p->lbprm.set_server_status_up   = fwlc_set_server_status_up;
	p->lbprm.set_server_status_down = fwlc_set_server_status_down;
	p->lbprm.update_server_eweight  = fwlc_update_server_weight;
	p->lbprm.server_take_conn = fwlc_srv_reposition;
	p->lbprm.server_drop_conn = fwlc_srv_reposition;

	p->lbprm.wdiv = BE_WEIGHT_SCALE;
	for (srv = p->srv; srv; srv = srv->next) {
		srv->prev_eweight = srv->eweight = srv->uweight * BE_WEIGHT_SCALE;
		srv->prev_state = srv->state;
	}

	recount_servers(p);
	update_backend_weight(p);

	p->lbprm.fwlc.act = init_head;
	p->lbprm.fwlc.bck = init_head;

	/* queue active and backup servers in two distinct groups */
	for (srv = p->srv; srv; srv = srv->next) {
		if (!srv_is_usable(srv->state, srv->eweight))
			continue;
		srv->lb_tree = (srv->state & SRV_BACKUP) ? &p->lbprm.fwlc.bck : &p->lbprm.fwlc.act;
		fwlc_queue_srv(srv);
	}
}

/* Return next server from the FWLC tree in backend <p>. If the tree is empty,
 * return NULL. Saturated servers are skipped.
 */
struct server *fwlc_get_next_server(struct proxy *p, struct server *srvtoavoid)
{
	struct server *srv, *avoided;
	struct eb32_node *node;

	srv = avoided = NULL;

	if (p->srv_act)
		node = eb32_first(&p->lbprm.fwlc.act);
	else if (p->lbprm.fbck)
		return p->lbprm.fbck;
	else if (p->srv_bck)
		node = eb32_first(&p->lbprm.fwlc.bck);
	else
		return NULL;

	while (node) {
		/* OK, we have a server. However, it may be saturated, in which
		 * case we don't want to reconsider it for now, so we'll simply
		 * skip it. Same if it's the server we try to avoid, in which
		 * case we simply remember it for later use if needed.
		 */
		struct server *s;

		s = eb32_entry(node, struct server, lb_node);
		if (!s->maxconn || (!s->nbpend && s->served < srv_dynamic_maxconn(s))) {
			if (s != srvtoavoid) {
				srv = s;
				break;
			}
			avoided = s;
		}
		node = eb32_next(node);
	}

	if (!srv)
		srv = avoided;

	return srv;
}


/*
 * Local variables:
 *  c-indent-level: 8
 *  c-basic-offset: 8
 * End:
 */
