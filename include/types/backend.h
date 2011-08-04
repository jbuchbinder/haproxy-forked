/*
 * include/types/backend.h
 * This file assembles definitions for backends
 *
 * Copyright (C) 2000-2009 Willy Tarreau - w@1wt.eu
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation, version 2.1
 * exclusively.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#ifndef _TYPES_BACKEND_H
#define _TYPES_BACKEND_H

#include <common/config.h>
#include <types/lb_chash.h>
#include <types/lb_fwlc.h>
#include <types/lb_fwrr.h>
#include <types/lb_map.h>
#include <types/server.h>

/* Parameters for lbprm.algo */

/* Lower bits define the kind of load balancing method, which means the type of
 * algorithm, and which criterion it is based on. For this reason, those bits
 * also include information about dependencies, so that the config parser can
 * detect incompatibilities.
 */

/* LB parameters. Depends on the LB kind. Right now, only hashing uses this. */
#define BE_LB_HASH_SRC  0x00000  /* hash source IP */
#define BE_LB_HASH_URI  0x00001  /* hash HTTP URI */
#define BE_LB_HASH_PRM  0x00002  /* hash HTTP URL parameter */
#define BE_LB_HASH_HDR  0x00003  /* hash HTTP header value */
#define BE_LB_HASH_RDP  0x00004  /* hash RDP cookie value */

#define BE_LB_RR_DYN    0x00000  /* dynamic round robin (default) */
#define BE_LB_RR_STATIC 0x00001  /* static round robin */
#define BE_LB_PARM      0x000FF  /* mask to get/clear the LB param */

/* Required input(s) */
#define BE_LB_NEED_NONE	0x00000  /* no input needed            */
#define BE_LB_NEED_ADDR	0x00100  /* only source address needed */
#define BE_LB_NEED_DATA	0x00200  /* some payload is needed     */
#define BE_LB_NEED_HTTP	0x00400  /* an HTTP request is needed  */
/* not used: 0x0800 */
#define BE_LB_NEED      0x00F00  /* mask to get/clear dependencies */

/* Algorithm */
#define BE_LB_KIND_NONE 0x00000  /* algorithm not set */
#define BE_LB_KIND_RR   0x01000  /* round-robin */
#define BE_LB_KIND_LC   0x02000  /* least connections */
#define BE_LB_KIND_HI   0x03000  /* hash of input (see hash inputs above) */
#define BE_LB_KIND      0x07000  /* mask to get/clear LB algorithm */

/* All known variants of load balancing algorithms. These can be cleared using
 * the BE_LB_ALGO mask. For a check, using BE_LB_KIND is preferred.
 */
#define BE_LB_ALGO_NONE (BE_LB_KIND_NONE | BE_LB_NEED_NONE)    /* not defined */
#define BE_LB_ALGO_RR   (BE_LB_KIND_RR | BE_LB_NEED_NONE)      /* round robin */
#define BE_LB_ALGO_LC   (BE_LB_KIND_LC | BE_LB_NEED_NONE)      /* least connections */
#define BE_LB_ALGO_SRR  (BE_LB_KIND_RR | BE_LB_NEED_NONE | BE_LB_RR_STATIC) /* static round robin */
#define BE_LB_ALGO_SH	(BE_LB_KIND_HI | BE_LB_NEED_ADDR | BE_LB_HASH_SRC) /* hash: source IP */
#define BE_LB_ALGO_UH	(BE_LB_KIND_HI | BE_LB_NEED_HTTP | BE_LB_HASH_URI) /* hash: HTTP URI  */
#define BE_LB_ALGO_PH	(BE_LB_KIND_HI | BE_LB_NEED_HTTP | BE_LB_HASH_PRM) /* hash: HTTP URL parameter */
#define BE_LB_ALGO_HH	(BE_LB_KIND_HI | BE_LB_NEED_HTTP | BE_LB_HASH_HDR) /* hash: HTTP header value  */
#define BE_LB_ALGO_RCH	(BE_LB_KIND_HI | BE_LB_NEED_DATA | BE_LB_HASH_RDP) /* hash: RDP cookie value   */
#define BE_LB_ALGO      (BE_LB_KIND    | BE_LB_NEED      | BE_LB_PARM    ) /* mask to clear algo */

/* Higher bits define how a given criterion is mapped to a server. In fact it
 * designates the LB function by itself. The dynamic algorithms will also have
 * the DYN bit set. These flags are automatically set at the end of the parsing.
 */
#define BE_LB_LKUP_NONE   0x00000  /* not defined */
#define BE_LB_LKUP_MAP    0x10000  /* static map based lookup */
#define BE_LB_LKUP_RRTREE 0x20000  /* FWRR tree lookup */
#define BE_LB_LKUP_LCTREE 0x30000  /* FWLC tree lookup */
#define BE_LB_LKUP_CHTREE 0x40000  /* consistent hash  */
#define BE_LB_LKUP        0x70000  /* mask to get just the LKUP value */

/* additional properties */
#define BE_LB_PROP_DYN    0x80000 /* bit to indicate a dynamic algorithm */

/* hash types */
#define BE_LB_HASH_MAP    0x000000 /* map-based hash (default) */
#define BE_LB_HASH_CONS   0x100000 /* consistent hashbit to indicate a dynamic algorithm */
#define BE_LB_HASH_AVAL   0x200000 /* run an avalanche hash before a map */
#define BE_LB_HASH_TYPE   0x300000 /* get/clear hash types */

/* various constants */

/* The scale factor between user weight and effective weight allows smooth
 * weight modulation even with small weights (eg: 1). It should not be too high
 * though because it limits the number of servers in FWRR mode in order to
 * prevent any integer overflow. The max number of servers per backend is
 * limited to about 2^32/255^2/scale ~= 66051/scale. A scale of 16 looks like
 * a good value, as it allows more than 4000 servers per backend while leaving
 * modulation steps of about 6% for servers with the lowest weight (1).
 */
#define BE_WEIGHT_SCALE 16

/* LB parameters for all algorithms */
struct lbprm {
	int algo;			/* load balancing algorithm and variants: BE_LB_* */
	int tot_wact, tot_wbck;		/* total effective weights of active and backup servers */
	int tot_weight;			/* total effective weight of servers participating to LB */
	int tot_used;			/* total number of servers used for LB */
	int wmult;			/* ratio between user weight and effective weight */
	int wdiv;			/* ratio between effective weight and user weight */
	struct server *fbck;		/* first backup server when !PR_O_USE_ALL_BK, or NULL */
	struct lb_map map;		/* LB parameters for map-based algorithms */
	struct lb_fwrr fwrr;
	struct lb_fwlc fwlc;
	struct lb_chash chash;
	/* Call backs for some actions. Some may be NULL (thus should be ignored). */
	void (*update_server_eweight)(struct server *);  /* to be called after eweight change */
	void (*set_server_status_up)(struct server *);   /* to be called after status changes to UP */
	void (*set_server_status_down)(struct server *); /* to be called after status changes to DOWN */
	void (*server_take_conn)(struct server *);       /* to be called when connection is assigned */
	void (*server_drop_conn)(struct server *);       /* to be called when connection is dropped */
};

#endif /* _TYPES_BACKEND_H */

/*
 * Local variables:
 *  c-indent-level: 8
 *  c-basic-offset: 8
 * End:
 */
