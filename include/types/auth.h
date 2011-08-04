/*
 * User authentication & authorization.
 *
 * Copyright 2010 Krzysztof Piotr Oledzki <ole@ans.pl>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 *
 */

#ifndef _TYPES_AUTH_H
#define _TYPES_AUTH_H

#include <common/config.h>
#include <common/mini-clist.h>

#include <types/auth.h>

#define MAX_AUTH_GROUPS (unsigned int)(sizeof(int)*8)

#define AU_O_INSECURE	0x00000001		/* insecure, unencrypted password */

struct auth_users {
	struct auth_users *next;
	unsigned int flags;
	char *user, *pass;
	union {
		char *groups;
		unsigned int group_mask;
	} u;
};

struct userlist {
	struct userlist *next;
	char *name;
	struct auth_users *users;
	int grpcnt;
	char *groups[MAX_AUTH_GROUPS];
	char **groupusers;
};

#endif /* _TYPES_AUTH_H */

/*
 * Local variables:
 *  c-indent-level: 8
 *  c-basic-offset: 8
 * End:
 */

