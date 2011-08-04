/*
 * include/types/stream_interface.h
 * This file describes the stream_interface struct and associated constants.
 *
 * Copyright (C) 2000-2011 Willy Tarreau - w@1wt.eu
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

#ifndef _TYPES_STREAM_INTERFACE_H
#define _TYPES_STREAM_INTERFACE_H

#include <stdlib.h>
#include <sys/socket.h>

#include <types/buffers.h>
#include <common/config.h>

/* A stream interface must have its own errors independantly of the buffer's,
 * so that applications can rely on what the buffer reports while the stream
 * interface is performing some retries (eg: connection error). Some states are
 * transient and do not last beyond process_session().
 */
enum {
	SI_ST_INI = 0,           /* interface not sollicitated yet */
	SI_ST_REQ,               /* [transient] connection initiation desired and not started yet */
	SI_ST_QUE,               /* interface waiting in queue */
	SI_ST_TAR,               /* interface in turn-around state after failed connect attempt */
	SI_ST_ASS,               /* server just assigned to this interface */
	SI_ST_CON,               /* initiated connection request (resource exists) */
	SI_ST_CER,               /* [transient] previous connection attempt failed (resource released) */
	SI_ST_EST,               /* connection established (resource exists) */
	SI_ST_DIS,               /* [transient] disconnected from other side, but cleanup not done yet */
	SI_ST_CLO,               /* stream intf closed, might not existing anymore. Buffers shut. */
};

/* error types reported on the streams interface for more accurate reporting */
enum {
	SI_ET_NONE       = 0x0000,  /* no error yet, leave it to zero */
	SI_ET_QUEUE_TO   = 0x0001,  /* queue timeout */
	SI_ET_QUEUE_ERR  = 0x0002,  /* queue error (eg: full) */
	SI_ET_QUEUE_ABRT = 0x0004,  /* aborted in queue by external cause */
	SI_ET_CONN_TO    = 0x0008,  /* connection timeout */
	SI_ET_CONN_ERR   = 0x0010,  /* connection error (eg: no server available) */
	SI_ET_CONN_ABRT  = 0x0020,  /* connection aborted by external cause (eg: abort) */
	SI_ET_CONN_OTHER = 0x0040,  /* connection aborted for other reason (eg: 500) */
	SI_ET_DATA_TO    = 0x0080,  /* timeout during data phase */
	SI_ET_DATA_ERR   = 0x0100,  /* error during data phase */
	SI_ET_DATA_ABRT  = 0x0200,  /* data phase aborted by external cause */
};

/* flags set after I/O */
enum {
	SI_FL_NONE       = 0x0000,  /* nothing */
	SI_FL_EXP        = 0x0001,  /* timeout has expired */
	SI_FL_ERR        = 0x0002,  /* a non-recoverable error has occurred */
	SI_FL_WAIT_ROOM  = 0x0004,  /* waiting for space to store incoming data */
	SI_FL_WAIT_DATA  = 0x0008,  /* waiting for more data to send */
	SI_FL_CAP_SPLTCP = 0x0010,  /* splicing possible from/to TCP */
	SI_FL_DONT_WAKE  = 0x0020,  /* resync in progress, don't wake up */
	SI_FL_INDEP_STR  = 0x0040,  /* independant streams = don't update rex on write */
	SI_FL_NOLINGER   = 0x0080,  /* may close without lingering. One-shot. */
};

/* target types */
enum {
	TARG_TYPE_NONE = 0,         /* no target set, pointer is NULL by definition */
	TARG_TYPE_PROXY,            /* target is a proxy   ; use address with the proxy's settings */
	TARG_TYPE_SERVER,           /* target is a server  ; use address with server's and its proxy's settings */
	TARG_TYPE_APPLET,           /* target is an applet ; use only the applet */
	TARG_TYPE_TASK,             /* target is a task running an external applet */
};

#define SI_FL_CAP_SPLICE (SI_FL_CAP_SPLTCP)

struct server;
struct proxy;
struct si_applet;

struct target {
	int type;
	union {
		void *v;              /* pointer value, for any type */
		struct proxy *p;      /* when type is TARG_TYPE_PROXY  */
		struct server *s;     /* when type is TARG_TYPE_SERVER */
		struct si_applet *a;  /* when type is TARG_TYPE_APPLET */
		struct task *t;       /* when type is TARG_TYPE_TASK */
	} ptr;
};

/* A stream interface has 3 parts :
 *  - the buffer side, which interfaces to the buffers.
 *  - the remote side, which describes the state and address of the other side.
 *  - the functions, which are used by the buffer side to communicate with the
 *    remote side from the buffer side.
 */

/* Note that if an applet is registered, the update function will not be called
 * by the session handler, so it may be used to resync flags at the end of the
 * applet handler. See stream_int_update_embedded() for reference.
 */
struct stream_interface {
	/* struct members used by the "buffer" side */
	unsigned int state;     /* SI_ST* */
	unsigned int prev_state;/* SI_ST*, copy of previous state */
	unsigned int flags;     /* SI_FL_* */
	struct buffer *ib, *ob; /* input and output buffers */
	unsigned int exp;       /* wake up time for connect, queue, turn-around, ... */
	void *owner;            /* generally a (struct task*) */
	unsigned int err_type;  /* first error detected, one of SI_ET_* */
	void *err_loc;          /* commonly the server, NULL when SI_ET_NONE */

	/* these struct members are used by the buffer side to act on the remote side */
	void (*update)(struct stream_interface *); /* I/O update function */
	void (*shutr)(struct stream_interface *);  /* shutr function */
	void (*shutw)(struct stream_interface *);  /* shutw function */
	void (*chk_rcv)(struct stream_interface *);/* chk_rcv function */
	void (*chk_snd)(struct stream_interface *);/* chk_snd function */
	int  (*connect)(struct stream_interface *); /* connect function if any */
	void (*release)(struct stream_interface *); /* handler to call after the last close() */

	/* struct members below are the "remote" part, as seen from the buffer side */
	struct target target;	/* the target to connect to (server, proxy, applet, ...) */
	int conn_retries;	/* number of connect retries left */
	int send_proxy_ofs;	/* <0 = offset to (re)send from the end, >0 = send all */
	int fd;                 /* file descriptor for a stream driver when known */
	struct {
		int state;                 /* applet state, initialized to zero */
		void *private;             /* may be used by any function above */
		unsigned int st0, st1;     /* may be used by any function above */
		union {
			struct {
				struct proxy *px;
				struct server *sv;
				struct listener *l;
				int px_st;		/* STAT_PX_ST* */
				unsigned int flags;	/* STAT_* */
				int iid, type, sid;	/* proxy id, type and service id if bounding of stats is enabled */
				const char *st_code;	/* pointer to the status code returned by an action */
				const char *api_action;
				const char *api_data;
			} stats;
			struct {
				struct bref bref;	/* back-reference from the session being dumped */
				void *target;		/* session we want to dump, or NULL for all */
				unsigned int uid;	/* if non-null, the uniq_id of the session being dumped */
				int section;		/* section of the session being dumped */
				int pos;		/* last position of the current session's buffer */
			} sess;
			struct {
				int iid;		/* if >= 0, ID of the proxy to filter on */
				struct proxy *px;	/* current proxy being dumped, NULL = not started yet. */
				unsigned int buf;	/* buffer being dumped, 0 = req, 1 = rep */
				unsigned int sid;	/* session ID of error being dumped */
				int ptr;		/* <0: headers, >=0 : text pointer to restart from */
				int bol;		/* pointer to beginning of current line */
			} errors;
			struct {
				void *target;		/* table we want to dump, or NULL for all */
				struct proxy *proxy;	/* table being currently dumped (first if NULL) */
				struct stksess *entry;	/* last entry we were trying to dump (or first if NULL) */
				long long value;	/* value to compare against */
				signed char data_type;	/* type of data to compare, or -1 if none */
				signed char data_op;	/* operator (STD_OP_*) when data_type set */
			} table;
			struct {
				const char *msg;	/* pointer to a persistent message to be returned in PRINT state */
			} cli;
		} ctx;					/* used by stats I/O handlers to dump the stats */
	} applet;
	union {
		struct {
			struct sockaddr_storage from;	/* the client address */
			struct sockaddr_storage to;	/* the address reached by the client if SN_FRT_ADDR_SET is set */
		} c; /* client side */
		struct {
			struct sockaddr_storage from;	/* the address to spoof when connecting to the server (transparent mode) */
			struct sockaddr_storage to;	/* the address to connect to */
		} s; /* server side */
	} addr; /* addresses of the remote side */
};

/* An applet designed to run in a stream interface */
struct si_applet {
	char *name; /* applet's name to report in logs */
	void (*fct)(struct stream_interface *);  /* internal I/O handler, may never be NULL */
};

#endif /* _TYPES_STREAM_INTERFACE_H */

/*
 * Local variables:
 *  c-indent-level: 8
 *  c-basic-offset: 8
 * End:
 */
