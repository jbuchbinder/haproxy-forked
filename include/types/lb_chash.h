/*
 * include/types/lb_chash.h
 * Types for Consistent Hash LB algorithm.
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

#ifndef _TYPES_LB_CHASH_H
#define _TYPES_LB_CHASH_H

#include <common/config.h>
#include <ebtree.h>
#include <eb32tree.h>

struct lb_chash {
	struct eb_root act;	/* weighted chash entries of active servers */
	struct eb_root bck;	/* weighted chash entries of backup servers */
	struct eb32_node *last;	/* last node found in case of round robin (or NULL) */
};

#endif /* _TYPES_LB_CHASH_H */

/*
 * Local variables:
 *  c-indent-level: 8
 *  c-basic-offset: 8
 * End:
 */
