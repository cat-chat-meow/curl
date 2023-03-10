#ifndef __ARES_INET_NET_PTON_H
#define __ARES_INET_NET_PTON_H

/* $Id$ */

/* Copyright (C) 2005 by Daniel Stenberg
 *
 * Permission to use, copy, modify, and distribute this
 * software and its documentation for any purpose and without
 * fee is hereby granted, provided that the above copyright
 * notice appear in all copies and that both that copyright
 * notice and this permission notice appear in supporting
 * documentation, and that the name of M.I.T. not be used in
 * advertising or publicity pertaining to distribution of the
 * software without specific, written prior permission.
 * M.I.T. makes no representations about the suitability of
 * this software for any purpose.  It is provided "as is"
 * without express or implied warranty.
 */

#if defined(HAVE_INET_PTON) && defined(HAVE_INET_PTON_IPV6)
#define ares_inet_pton(x,y,z) inet_pton(x,y,z)
#else
int ares_inet_pton(int af, const char *src, void *dst);
#endif
#if defined(HAVE_INET_NET_PTON) && defined(HAVE_INET_NET_PTON_IPV6)
#define ares_inet_net_pton(w,x,y,z) inet_net_pton(w,x,y,z)
#else
int ares_inet_net_pton(int af, const char *src, void *dst, size_t size);
#endif

#endif /* __ARES_INET_NET_PTON_H */
