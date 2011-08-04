/*
 * include/common/regex.h
 * This file defines everything related to regular expressions.
 *
 * Copyright (C) 2000-2010 Willy Tarreau - w@1wt.eu
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

#ifndef _COMMON_REGEX_H
#define _COMMON_REGEX_H

#include <common/config.h>

#ifdef USE_PCRE
#include <pcre.h>
#include <pcreposix.h>
#else
#include <regex.h>
#endif

/* what to do when a header matches a regex */
#define ACT_ALLOW	0	/* allow the request */
#define ACT_REPLACE	1	/* replace the matching header */
#define ACT_REMOVE	2	/* remove the matching header */
#define ACT_DENY	3	/* deny the request */
#define ACT_PASS	4	/* pass this header without allowing or denying the request */
#define ACT_TARPIT	5	/* tarpit the connection matching this request */
#define ACT_SETBE	6	/* switch the backend */

struct hdr_exp {
    struct hdr_exp *next;
    const regex_t *preg;		/* expression to look for */
    int action;				/* ACT_ALLOW, ACT_REPLACE, ACT_REMOVE, ACT_DENY */
    const char *replace;		/* expression to set instead */
    void *cond;				/* a possible condition or NULL */
};

extern regmatch_t pmatch[MAX_MATCH];

int exp_replace(char *dst, char *src, const char *str,	const regmatch_t *matches);
const char *check_replace_string(const char *str);
const char *chain_regex(struct hdr_exp **head, const regex_t *preg,
			int action, const char *replace, void *cond);

#endif /* _COMMON_REGEX_H */

/*
 * Local variables:
 *  c-indent-level: 8
 *  c-basic-offset: 8
 * End:
 */
