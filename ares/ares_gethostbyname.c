/* $Id$ */

/* Copyright 1998 by the Massachusetts Institute of Technology.
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

#include "setup.h"

#if defined(WIN32) && !defined(WATT32)
#include "nameser.h"
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#ifdef HAVE_ARPA_NAMESER_H
#include <arpa/nameser.h>
#endif
#ifdef HAVE_ARPA_NAMESER_COMPAT_H
#include <arpa/nameser_compat.h>
#endif
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#ifdef HAVE_STRINGS_H
#include <strings.h>
#endif

#include "ares.h"
#include "ares_private.h"
#include "inet_net_pton.h"
#include "bitncmp.h"

#ifdef WATT32
#undef WIN32
#endif

struct host_query {
  /* Arguments passed to ares_gethostbyname() */
  ares_channel channel;
  char *name;
  ares_host_callback callback;
  void *arg;
  int family;
  const char *remaining_lookups;
  int timeouts;
};

static void next_lookup(struct host_query *hquery, int status_code);
static void host_callback(void *arg, int status, int timeouts,
                          unsigned char *abuf, int alen);
static void end_hquery(struct host_query *hquery, int status,
                       struct hostent *host);
static int fake_hostent(const char *name, int family, ares_host_callback callback,
                        void *arg);
static int file_lookup(const char *name, int family, struct hostent **host);
static void sort_addresses(struct hostent *host, struct apattern *sortlist,
                           int nsort);
static void sort6_addresses(struct hostent *host, struct apattern *sortlist,
                           int nsort);
static int get_address_index(struct in_addr *addr, struct apattern *sortlist,
                             int nsort);
static int get6_address_index(struct in6_addr *addr, struct apattern *sortlist,
                             int nsort);

void ares_gethostbyname(ares_channel channel, const char *name, int family,
                        ares_host_callback callback, void *arg)
{
  struct host_query *hquery;

  /* Right now we only know how to look up Internet addresses. */
  if (family != AF_INET && family != AF_INET6)
    {
      callback(arg, ARES_ENOTIMP, 0, NULL);
      return;
    }

  if (fake_hostent(name, family, callback, arg))
    return;

  /* Allocate and fill in the host query structure. */
  hquery = malloc(sizeof(struct host_query));
  if (!hquery)
    {
      callback(arg, ARES_ENOMEM, 0, NULL);
      return;
    }
  hquery->channel = channel;
  hquery->name = strdup(name);
  hquery->family = family;
  if (!hquery->name)
    {
      free(hquery);
      callback(arg, ARES_ENOMEM, 0, NULL);
      return;
    }
  hquery->callback = callback;
  hquery->arg = arg;
  hquery->remaining_lookups = channel->lookups;
  hquery->timeouts = 0;

  /* Start performing lookups according to channel->lookups. */
  next_lookup(hquery, ARES_ECONNREFUSED /* initial error code */);
}

static void next_lookup(struct host_query *hquery, int status_code)
{
  const char *p;
  struct hostent *host;
  int status = status_code;

  for (p = hquery->remaining_lookups; *p; p++)
    {
      switch (*p)
        {
        case 'b':
          /* DNS lookup */
          hquery->remaining_lookups = p + 1;
          if (hquery->family == AF_INET6)
            ares_search(hquery->channel, hquery->name, C_IN, T_AAAA,
                        host_callback, hquery);
          else
            ares_search(hquery->channel, hquery->name, C_IN, T_A, host_callback,
                        hquery);
          return;

        case 'f':
          /* Host file lookup */
          status = file_lookup(hquery->name, hquery->family, &host);

          /* this status check below previously checked for !ARES_ENOTFOUND,
             but we should not assume that this single error code is the one
             that can occur, as that is in fact no longer the case */
          if (status == ARES_SUCCESS)
            {
              end_hquery(hquery, status, host);
              return;
            }
          status = status_code;   /* Use original status code */
          break;
        }
    }
  end_hquery(hquery, status, NULL);
}

static void host_callback(void *arg, int status, int timeouts,
                          unsigned char *abuf, int alen)
{
  struct host_query *hquery = (struct host_query *) arg;
  ares_channel channel = hquery->channel;
  struct hostent *host = NULL;

  hquery->timeouts += timeouts;
  if (status == ARES_SUCCESS)
    {
      if (hquery->family == AF_INET)
        {
          status = ares_parse_a_reply(abuf, alen, &host, NULL, NULL);
          if (host && channel->nsort)
            sort_addresses(host, channel->sortlist, channel->nsort);
        }
      else if (hquery->family == AF_INET6)
        {
          status = ares_parse_aaaa_reply(abuf, alen, &host, NULL, NULL);
          if (host && channel->nsort)
            sort6_addresses(host, channel->sortlist, channel->nsort);
        }
      end_hquery(hquery, status, host);
    }
  else if (status == ARES_ENODATA && hquery->family == AF_INET6)
    {
      /* There was no AAAA. Now lookup an A */
      hquery->family = AF_INET;
      ares_search(hquery->channel, hquery->name, C_IN, T_A, host_callback,
                  hquery);
    }
  else if (status == ARES_EDESTRUCTION)
    end_hquery(hquery, status, NULL);
  else
    next_lookup(hquery, status);
}

static void end_hquery(struct host_query *hquery, int status,
                       struct hostent *host)
{
  hquery->callback(hquery->arg, status, hquery->timeouts, host);
  if (host)
    ares_free_hostent(host);
  free(hquery->name);
  free(hquery);
}

/* If the name looks like an IP address, fake up a host entry, end the
 * query immediately, and return true.  Otherwise return false.
 */
static int fake_hostent(const char *name, int family, ares_host_callback callback,
                        void *arg)
{
  struct hostent hostent;
  char *aliases[1] = { NULL };
  char *addrs[2];
  int result = 0;
  struct in_addr in;
  struct in6_addr in6;

  if (family == AF_INET)
    {
      /* It only looks like an IP address if it's all numbers and dots. */
      int numdots = 0;
      const char *p;
      for (p = name; *p; p++)
        {
          if (!ISDIGIT(*p) && *p != '.') {
            return 0;
          } else if (*p == '.') {
            numdots++;
          }
        }
    
      /* if we don't have 3 dots, it is illegal 
       * (although inet_addr doesn't think so).
       */
      if (numdots != 3)
        result = 0;
      else
        result = ((in.s_addr = inet_addr(name)) == INADDR_NONE ? 0 : 1);
    }
  else if (family == AF_INET6)
    result = (ares_inet_pton(AF_INET6, name, &in6) < 1 ? 0 : 1);

  if (!result)
    return 0;

  if (family == AF_INET)
    {
      hostent.h_length = sizeof(struct in_addr);
      addrs[0] = (char *)&in;
    }
  else if (family == AF_INET6)
    {
      hostent.h_length = sizeof(struct in6_addr);
      addrs[0] = (char *)&in6;
    }
  /* Duplicate the name, to avoid a constness violation. */
  hostent.h_name = strdup(name);
  if (!hostent.h_name)
    {
      callback(arg, ARES_ENOMEM, 0, NULL);
      return 1;
    }

  /* Fill in the rest of the host structure and terminate the query. */
  addrs[1] = NULL;
  hostent.h_aliases = aliases;
  hostent.h_addrtype = family;
  hostent.h_addr_list = addrs;
  callback(arg, ARES_SUCCESS, 0, &hostent);

  free((char *)(hostent.h_name));
  return 1;
}

static int file_lookup(const char *name, int family, struct hostent **host)
{
  FILE *fp;
  char **alias;
  int status;
  int error;

#ifdef WIN32
  char PATH_HOSTS[MAX_PATH];
  if (IS_NT()) {
    char tmp[MAX_PATH];
    HKEY hkeyHosts;

    if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, WIN_NS_NT_KEY, 0, KEY_READ, &hkeyHosts)
        == ERROR_SUCCESS)
    {
      DWORD dwLength = MAX_PATH;
      RegQueryValueEx(hkeyHosts, DATABASEPATH, NULL, NULL, (LPBYTE)tmp,
                      &dwLength);
      ExpandEnvironmentStrings(tmp, PATH_HOSTS, MAX_PATH);
      RegCloseKey(hkeyHosts);
    }
  }
  else
    GetWindowsDirectory(PATH_HOSTS, MAX_PATH);

  strcat(PATH_HOSTS, WIN_PATH_HOSTS);

#elif defined(WATT32)
  extern const char *_w32_GetHostsFile (void);
  const char *PATH_HOSTS = _w32_GetHostsFile();

  if (!PATH_HOSTS)
    return ARES_ENOTFOUND;
#endif

  fp = fopen(PATH_HOSTS, "r");
  if (!fp)
    {
      error = ERRNO;
      switch(error)
        {
        case ENOENT:
        case ESRCH:
          return ARES_ENOTFOUND;
        default:
          DEBUGF(fprintf(stderr, "fopen() failed with error: %d %s\n",
                         error, strerror(error)));
          DEBUGF(fprintf(stderr, "Error opening file: %s\n",
                         PATH_HOSTS));
          *host = NULL;
          return ARES_EFILE;
        }
    }
  while ((status = ares__get_hostent(fp, family, host)) == ARES_SUCCESS)
    {
      if (strcasecmp((*host)->h_name, name) == 0)
        break;
      for (alias = (*host)->h_aliases; *alias; alias++)
        {
          if (strcasecmp(*alias, name) == 0)
            break;
        }
      if (*alias)
        break;
      ares_free_hostent(*host);
    }
  fclose(fp);
  if (status == ARES_EOF)
    status = ARES_ENOTFOUND;
  if (status != ARES_SUCCESS)
    *host = NULL;
  return status;
}

static void sort_addresses(struct hostent *host, struct apattern *sortlist,
                           int nsort)
{
  struct in_addr a1, a2;
  int i1, i2, ind1, ind2;

  /* This is a simple insertion sort, not optimized at all.  i1 walks
   * through the address list, with the loop invariant that everything
   * to the left of i1 is sorted.  In the loop body, the value at i1 is moved
   * back through the list (via i2) until it is in sorted order.
   */
  for (i1 = 0; host->h_addr_list[i1]; i1++)
    {
      memcpy(&a1, host->h_addr_list[i1], sizeof(struct in_addr));
      ind1 = get_address_index(&a1, sortlist, nsort);
      for (i2 = i1 - 1; i2 >= 0; i2--)
        {
          memcpy(&a2, host->h_addr_list[i2], sizeof(struct in_addr));
          ind2 = get_address_index(&a2, sortlist, nsort);
          if (ind2 <= ind1)
            break;
          memcpy(host->h_addr_list[i2 + 1], &a2, sizeof(struct in_addr));
        }
      memcpy(host->h_addr_list[i2 + 1], &a1, sizeof(struct in_addr));
    }
}

/* Find the first entry in sortlist which matches addr.  Return nsort
 * if none of them match.
 */
static int get_address_index(struct in_addr *addr, struct apattern *sortlist,
                             int nsort)
{
  int i;

  for (i = 0; i < nsort; i++)
    {
      if (sortlist[i].family != AF_INET)
        continue;
      if (sortlist[i].type == PATTERN_MASK)
        {
          if ((addr->s_addr & sortlist[i].mask.addr.addr4.s_addr)
              == sortlist[i].addr.addr4.s_addr)
            break;
        }
      else
        {
          if (!ares_bitncmp(&addr->s_addr, &sortlist[i].addr.addr4.s_addr,
                            sortlist[i].mask.bits))
            break;
        }
    }
  return i;
}

static void sort6_addresses(struct hostent *host, struct apattern *sortlist,
                           int nsort)
{
  struct in6_addr a1, a2;
  int i1, i2, ind1, ind2;

  /* This is a simple insertion sort, not optimized at all.  i1 walks
   * through the address list, with the loop invariant that everything
   * to the left of i1 is sorted.  In the loop body, the value at i1 is moved
   * back through the list (via i2) until it is in sorted order.
   */
  for (i1 = 0; host->h_addr_list[i1]; i1++)
    {
      memcpy(&a1, host->h_addr_list[i1], sizeof(struct in6_addr));
      ind1 = get6_address_index(&a1, sortlist, nsort);
      for (i2 = i1 - 1; i2 >= 0; i2--)
        {
          memcpy(&a2, host->h_addr_list[i2], sizeof(struct in6_addr));
          ind2 = get6_address_index(&a2, sortlist, nsort);
          if (ind2 <= ind1)
            break;
          memcpy(host->h_addr_list[i2 + 1], &a2, sizeof(struct in6_addr));
        }
      memcpy(host->h_addr_list[i2 + 1], &a1, sizeof(struct in6_addr));
    }
}

/* Find the first entry in sortlist which matches addr.  Return nsort
 * if none of them match.
 */
static int get6_address_index(struct in6_addr *addr, struct apattern *sortlist,
                             int nsort)
{
  int i;

  for (i = 0; i < nsort; i++)
    {
      if (sortlist[i].family != AF_INET6)
        continue;
        if (!ares_bitncmp(&addr->s6_addr, &sortlist[i].addr.addr6.s6_addr, sortlist[i].mask.bits))
          break;
    }
  return i;
}
