/*
 * include/common/version.h
 * This file serves as a template for future include files.
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

#ifndef _COMMON_VERSION_H
#define _COMMON_VERSION_H

#include <common/config.h>

#ifdef  CONFIG_PRODUCT_NAME
#define PRODUCT_NAME    CONFIG_PRODUCT_NAME
#else
#define PRODUCT_NAME    "HAProxy"
#endif

#ifdef  CONFIG_PRODUCT_BRANCH
#define PRODUCT_BRANCH    CONFIG_PRODUCT_BRANCH
#else
#define PRODUCT_BRANCH   "1.5"
#endif

#ifdef  CONFIG_PRODUCT_URL
#define PRODUCT_URL    CONFIG_PRODUCT_URL
#else
#define PRODUCT_URL    "http://haproxy.1wt.eu/"
#endif

#ifdef  CONFIG_PRODUCT_URL_UPD
#define PRODUCT_URL_UPD  CONFIG_PRODUCT_URL_UPD
#else
#define PRODUCT_URL_UPD "http://haproxy.1wt.eu/#down"
#endif

#ifdef  CONFIG_PRODUCT_URL_DOC
#define PRODUCT_URL_DOC  CONFIG_PRODUCT_URL_DOC
#else
#define PRODUCT_URL_DOC "http://haproxy.1wt.eu/#docs"
#endif

#ifdef CONFIG_HAPROXY_VERSION
#define HAPROXY_VERSION CONFIG_HAPROXY_VERSION
#else
#error "Must define CONFIG_HAPROXY_VERSION"
#endif

#ifdef CONFIG_HAPROXY_DATE
#define HAPROXY_DATE    CONFIG_HAPROXY_DATE
#else
#error "Must define CONFIG_HAPROXY_DATE"
#endif

#endif /* _COMMON_VERSION_H */

