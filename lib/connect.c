/***************************************************************************
 *                                  _   _ ____  _
 *  Project                     ___| | | |  _ \| |
 *                             / __| | | | |_) | |
 *                            | (__| |_| |  _ <| |___
 *                             \___|\___/|_| \_\_____|
 *
 * Copyright (C) 1998 - 2008, Daniel Stenberg, <daniel@haxx.se>, et al.
 *
 * This software is licensed as described in the file COPYING, which
 * you should have received as part of this distribution. The terms
 * are also available at http://curl.haxx.se/docs/copyright.html.
 *
 * You may opt to use, copy, modify, merge, publish, distribute and/or sell
 * copies of the Software, and permit persons to whom the Software is
 * furnished to do so, under the terms of the COPYING file.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 * $Id$
 ***************************************************************************/

#include "setup.h"

#ifndef WIN32
/* headers for non-win32 */
#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>
#endif
#ifdef HAVE_SYS_SOCKET_H
#include <sys/socket.h>
#endif
#ifdef HAVE_NETINET_IN_H
#include <netinet/in.h> /* <netinet/tcp.h> may need it */
#endif
#ifdef HAVE_SYS_UN_H
#include <sys/un.h> /* for sockaddr_un */
#endif
#ifdef HAVE_NETINET_TCP_H
#include <netinet/tcp.h> /* for TCP_NODELAY */
#endif
#ifdef HAVE_SYS_IOCTL_H
#include <sys/ioctl.h>
#endif
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#ifdef HAVE_NETDB_H
#include <netdb.h>
#endif
#ifdef HAVE_FCNTL_H
#include <fcntl.h>
#endif
#ifdef HAVE_ARPA_INET_H
#include <arpa/inet.h>
#endif
#ifdef HAVE_STDLIB_H
#include <stdlib.h> /* required for free() prototype, without it, this crashes */
#endif              /* on macos 68K */

#if (defined(HAVE_FIONBIO) && defined(NETWARE))
#include <sys/filio.h>
#endif
#ifdef NETWARE
#undef in_addr_t
#define in_addr_t unsigned long
#endif
#ifdef VMS
#include <in.h>
#include <inet.h>
#endif

#endif  /* !WIN32 */

#include <stdio.h>
#include <errno.h>
#include <string.h>

#include "urldata.h"
#include "sendf.h"
#include "if2ip.h"
#include "strerror.h"
#include "connect.h"
#include "memory.h"
#include "select.h"
#include "url.h" /* for Curl_safefree() */
#include "multiif.h"
#include "sockaddr.h" /* required for Curl_sockaddr_storage */
#include "inet_ntop.h"
#include "inet_pton.h"
#include "sslgen.h" /* for Curl_ssl_check_cxn() */

/* The last #include file should be: */
#include "memdebug.h"

#ifdef __SYMBIAN32__
/* This isn't actually supported under Symbian OS */
#undef SO_NOSIGPIPE
#endif

static bool verifyconnect(curl_socket_t sockfd, int *error);

static curl_socket_t
singleipconnect(struct connectdata *conn,
                const Curl_addrinfo *ai, /* start connecting to this */
                long timeout_ms,
                bool *connected);

/*
 * Curl_timeleft() returns the amount of milliseconds left allowed for the
 * transfer/connection. If the value is negative, the timeout time has already
 * elapsed.
 *
 * If 'nowp' is non-NULL, it points to the current time.
 * 'duringconnect' is FALSE if not during a connect, as then of course the
 * connect timeout is not taken into account!
 */
long Curl_timeleft(struct connectdata *conn,
                   struct timeval *nowp,
                   bool duringconnect)
{
  struct SessionHandle *data = conn->data;
  int timeout_set = 0;
  long timeout_ms = duringconnect?DEFAULT_CONNECT_TIMEOUT:0;
  struct timeval now;

  /* if a timeout is set, use the most restrictive one */

  if(data->set.timeout > 0)
    timeout_set |= 1;
  if(duringconnect && (data->set.connecttimeout > 0))
    timeout_set |= 2;

  switch (timeout_set) {
  case 1:
    timeout_ms = data->set.timeout;
    break;
  case 2:
    timeout_ms = data->set.connecttimeout;
    break;
  case 3:
    if(data->set.timeout < data->set.connecttimeout)
      timeout_ms = data->set.timeout;
    else
      timeout_ms = data->set.connecttimeout;
    break;
  default:
    /* use the default */
    if(!duringconnect)
      /* if we're not during connect, there's no default timeout so if we're
         at zero we better just return zero and not make it a negative number
         by the math below */
      return 0;
    break;
  }

  if(!nowp) {
    now = Curl_tvnow();
    nowp = &now;
  }

  /* substract elapsed time */
  timeout_ms -= Curl_tvdiff(*nowp, data->progress.t_startsingle);

  return timeout_ms;
}


/*
 * Curl_nonblock() set the given socket to either blocking or non-blocking
 * mode based on the 'nonblock' boolean argument. This function is highly
 * portable.
 */
int Curl_nonblock(curl_socket_t sockfd,    /* operate on this */
                  int nonblock   /* TRUE or FALSE */)
{
#undef SETBLOCK
#define SETBLOCK 0
#ifdef HAVE_O_NONBLOCK
  /* most recent unix versions */
  int flags;

  flags = fcntl(sockfd, F_GETFL, 0);
  if(FALSE != nonblock)
    return fcntl(sockfd, F_SETFL, flags | O_NONBLOCK);
  else
    return fcntl(sockfd, F_SETFL, flags & (~O_NONBLOCK));
#undef SETBLOCK
#define SETBLOCK 1
#endif

#if defined(HAVE_FIONBIO) && (SETBLOCK == 0)
  /* older unix versions */
  int flags;

  flags = nonblock;
  return ioctl(sockfd, FIONBIO, &flags);
#undef SETBLOCK
#define SETBLOCK 2
#endif

#if defined(HAVE_IOCTLSOCKET) && (SETBLOCK == 0)
  /* Windows? */
  unsigned long flags;
  flags = nonblock;

  return ioctlsocket(sockfd, FIONBIO, &flags);
#undef SETBLOCK
#define SETBLOCK 3
#endif

#if defined(HAVE_IOCTLSOCKET_CASE) && (SETBLOCK == 0)
  /* presumably for Amiga */
  return IoctlSocket(sockfd, FIONBIO, (long)nonblock);
#undef SETBLOCK
#define SETBLOCK 4
#endif

#if defined(HAVE_SO_NONBLOCK) && (SETBLOCK == 0)
  /* BeOS */
  long b = nonblock ? 1 : 0;
  return setsockopt(sockfd, SOL_SOCKET, SO_NONBLOCK, &b, sizeof(b));
#undef SETBLOCK
#define SETBLOCK 5
#endif

#ifdef HAVE_DISABLED_NONBLOCKING
  return 0; /* returns success */
#undef SETBLOCK
#define SETBLOCK 6
#endif

#if(SETBLOCK == 0)
#error "no non-blocking method was found/used/set"
#endif
}

/*
 * waitconnect() waits for a TCP connect on the given socket for the specified
 * number if milliseconds. It returns:
 * 0    fine connect
 * -1   select() error
 * 1    select() timeout
 * 2    select() returned with an error condition fd_set
 */

#define WAITCONN_CONNECTED     0
#define WAITCONN_SELECT_ERROR -1
#define WAITCONN_TIMEOUT       1
#define WAITCONN_FDSET_ERROR   2

static
int waitconnect(curl_socket_t sockfd, /* socket */
                long timeout_msec)
{
  int rc;
#ifdef mpeix
  /* Call this function once now, and ignore the results. We do this to
     "clear" the error state on the socket so that we can later read it
     reliably. This is reported necessary on the MPE/iX operating system. */
  (void)verifyconnect(sockfd, NULL);
#endif

  /* now select() until we get connect or timeout */
  rc = Curl_socket_ready(CURL_SOCKET_BAD, sockfd, (int)timeout_msec);
  if(-1 == rc)
    /* error, no connect here, try next */
    return WAITCONN_SELECT_ERROR;

  else if(0 == rc)
    /* timeout, no connect today */
    return WAITCONN_TIMEOUT;

  if(rc & CURL_CSELECT_ERR)
    /* error condition caught */
    return WAITCONN_FDSET_ERROR;

  /* we have a connect! */
  return WAITCONN_CONNECTED;
}

static CURLcode bindlocal(struct connectdata *conn,
                          curl_socket_t sockfd)
{
#ifdef ENABLE_IPV6
  char ipv6_addr[16];
#endif
  struct SessionHandle *data = conn->data;
  struct sockaddr_in me;
  struct sockaddr *sock = NULL;  /* bind to this address */
  socklen_t socksize; /* size of the data sock points to */
  unsigned short port = data->set.localport; /* use this port number, 0 for
                                                "random" */
  /* how many port numbers to try to bind to, increasing one at a time */
  int portnum = data->set.localportrange;
  const char *dev = data->set.str[STRING_DEVICE];

  /*************************************************************
   * Select device to bind socket to
   *************************************************************/
  if(dev && (strlen(dev)<255) ) {
    struct Curl_dns_entry *h=NULL;
    char myhost[256] = "";
    in_addr_t in;
    int rc;
    bool was_iface = FALSE;
    int in6 = -1;

    /* First check if the given name is an IP address */
    in=inet_addr((char *) dev);

    if((in == CURL_INADDR_NONE) &&
       Curl_if2ip(dev, myhost, sizeof(myhost))) {
      /*
       * We now have the numerical IPv4-style x.y.z.w in the 'myhost' buffer
       */
      rc = Curl_resolv(conn, myhost, 0, &h);
      if(rc == CURLRESOLV_PENDING)
        (void)Curl_wait_for_resolv(conn, &h);

      if(h) {
        was_iface = TRUE;
        Curl_resolv_unlock(data, h);
      }
    }

    if(!was_iface) {
      /*
       * This was not an interface, resolve the name as a host name
       * or IP number
       */
      rc = Curl_resolv(conn, dev, 0, &h);
      if(rc == CURLRESOLV_PENDING)
        (void)Curl_wait_for_resolv(conn, &h);

      if(h) {
        if(in == CURL_INADDR_NONE)
          /* convert the resolved address, sizeof myhost >= INET_ADDRSTRLEN */
          Curl_inet_ntop(h->addr->ai_addr->sa_family,
                         &((struct sockaddr_in*)h->addr->ai_addr)->sin_addr,
                         myhost, sizeof myhost);
        else
          /* we know data->set.device is shorter than the myhost array */
          strcpy(myhost, dev);
        Curl_resolv_unlock(data, h);
      }
    }

    if(! *myhost) {
      /* need to fix this
         h=Curl_gethost(data,
         getmyhost(*myhost,sizeof(myhost)),
         hostent_buf,
         sizeof(hostent_buf));
      */
      failf(data, "Couldn't bind to '%s'", dev);
      return CURLE_INTERFACE_FAILED;
    }

    infof(data, "Bind local address to %s\n", myhost);

#ifdef SO_BINDTODEVICE
    /* I am not sure any other OSs than Linux that provide this feature, and
     * at the least I cannot test. --Ben
     *
     * This feature allows one to tightly bind the local socket to a
     * particular interface.  This will force even requests to other local
     * interfaces to go out the external interface.
     *
     */
    if(was_iface) {
      /* Only bind to the interface when specified as interface, not just as a
       * hostname or ip address.
       */
      if(setsockopt(sockfd, SOL_SOCKET, SO_BINDTODEVICE,
                     dev, strlen(dev)+1) != 0) {
        /* printf("Failed to BINDTODEVICE, socket: %d  device: %s error: %s\n",
           sockfd, dev, Curl_strerror(SOCKERRNO)); */
        infof(data, "SO_BINDTODEVICE %s failed\n", dev);
        /* This is typically "errno 1, error: Operation not permitted" if
           you're not running as root or another suitable privileged user */
      }
    }
#endif

    in=inet_addr(myhost);

#ifdef ENABLE_IPV6
    in6 = Curl_inet_pton (AF_INET6, myhost, (void *)&ipv6_addr);
#endif
    if(CURL_INADDR_NONE == in && -1 == in6) {
      failf(data,"couldn't find my own IP address (%s)", myhost);
      return CURLE_INTERFACE_FAILED;
    } /* end of inet_addr */

    if( h ) {
      Curl_addrinfo *addr = h->addr;
      sock = addr->ai_addr;
      socksize = addr->ai_addrlen;
    }
    else
      return CURLE_INTERFACE_FAILED;

  }
  else if(port) {
    /* if a local port number is requested but no local IP, extract the
       address from the socket */
    memset(&me, 0, sizeof(struct sockaddr));
    me.sin_family = AF_INET;
    me.sin_addr.s_addr = INADDR_ANY;

    sock = (struct sockaddr *)&me;
    socksize = sizeof(struct sockaddr);

  }
  else
    /* no local kind of binding was requested */
    return CURLE_OK;

  do {

    /* Set port number to bind to, 0 makes the system pick one */
    if(sock->sa_family == AF_INET)
      ((struct sockaddr_in *)sock)->sin_port = htons(port);
#ifdef ENABLE_IPV6
    else
      ((struct sockaddr_in6 *)sock)->sin6_port = htons(port);
#endif

    if( bind(sockfd, sock, socksize) >= 0) {
      /* we succeeded to bind */
      struct Curl_sockaddr_storage add;
      socklen_t size;

      size = sizeof(add);
      if(getsockname(sockfd, (struct sockaddr *) &add, &size) < 0) {
        failf(data, "getsockname() failed");
        return CURLE_INTERFACE_FAILED;
      }
      /* We re-use/clobber the port variable here below */
      if(((struct sockaddr *)&add)->sa_family == AF_INET)
        port = ntohs(((struct sockaddr_in *)&add)->sin_port);
#ifdef ENABLE_IPV6
      else
        port = ntohs(((struct sockaddr_in6 *)&add)->sin6_port);
#endif
      infof(data, "Local port: %d\n", port);
      conn->bits.bound = TRUE;
      return CURLE_OK;
    }
    if(--portnum > 0) {
      infof(data, "Bind to local port %d failed, trying next\n", port);
      port++; /* try next port */
    }
    else
      break;
  } while(1);

  data->state.os_errno = SOCKERRNO;
  failf(data, "bind failure: %s",
        Curl_strerror(conn, data->state.os_errno));
  return CURLE_INTERFACE_FAILED;

}

/*
 * verifyconnect() returns TRUE if the connect really has happened.
 */
static bool verifyconnect(curl_socket_t sockfd, int *error)
{
  bool rc = TRUE;
#ifdef SO_ERROR
  int err = 0;
  socklen_t errSize = sizeof(err);

#ifdef WIN32
  /*
   * In October 2003 we effectively nullified this function on Windows due to
   * problems with it using all CPU in multi-threaded cases.
   *
   * In May 2004, we bring it back to offer more info back on connect failures.
   * Gisle Vanem could reproduce the former problems with this function, but
   * could avoid them by adding this SleepEx() call below:
   *
   *    "I don't have Rational Quantify, but the hint from his post was
   *    ntdll::NtRemoveIoCompletion(). So I'd assume the SleepEx (or maybe
   *    just Sleep(0) would be enough?) would release whatever
   *    mutex/critical-section the ntdll call is waiting on.
   *
   *    Someone got to verify this on Win-NT 4.0, 2000."
   */

#ifdef _WIN32_WCE
  Sleep(0);
#else
  SleepEx(0, FALSE);
#endif

#endif

  if(0 != getsockopt(sockfd, SOL_SOCKET, SO_ERROR, (void *)&err, &errSize))
    err = SOCKERRNO;
#ifdef _WIN32_WCE
  /* Old WinCE versions don't support SO_ERROR */
  if(WSAENOPROTOOPT == err) {
    SET_SOCKERRNO(0);
    err = 0;
  }
#endif
#ifdef __minix
  /* Minix 3.1.x doesn't support getsockopt on UDP sockets */
  if(EBADIOCTL == err) {
    SET_SOCKERRNO(0);
    err = 0;
  }
#endif
  if((0 == err) || (EISCONN == err))
    /* we are connected, awesome! */
    rc = TRUE;
  else
    /* This wasn't a successful connect */
    rc = FALSE;
  if(error)
    *error = err;
#else
  (void)sockfd;
  if(error)
    *error = SOCKERRNO;
#endif
  return rc;
}

CURLcode Curl_store_ip_addr(struct connectdata *conn)
{
  char addrbuf[256];
  Curl_printable_address(conn->ip_addr, addrbuf, sizeof(addrbuf));

  /* save the string */
  Curl_safefree(conn->ip_addr_str);
  conn->ip_addr_str = strdup(addrbuf);
  if(!conn->ip_addr_str)
    return CURLE_OUT_OF_MEMORY; /* FAIL */

#ifdef PF_INET6
  if(conn->ip_addr->ai_family == PF_INET6)
    conn->bits.ipv6 = TRUE;
#endif

  return CURLE_OK;
}

/* Used within the multi interface. Try next IP address, return TRUE if no
   more address exists */
static bool trynextip(struct connectdata *conn,
                      int sockindex,
                      bool *connected)
{
  curl_socket_t sockfd;
  Curl_addrinfo *ai;

  /* first close the failed socket */
  sclose(conn->sock[sockindex]);
  conn->sock[sockindex] = CURL_SOCKET_BAD;
  *connected = FALSE;

  if(sockindex != FIRSTSOCKET)
    return TRUE; /* no next */

  /* try the next address */
  ai = conn->ip_addr->ai_next;

  while(ai) {
    sockfd = singleipconnect(conn, ai, 0L, connected);
    if(sockfd != CURL_SOCKET_BAD) {
      /* store the new socket descriptor */
      conn->sock[sockindex] = sockfd;
      conn->ip_addr = ai;

      Curl_store_ip_addr(conn);
      return FALSE;
    }
    ai = ai->ai_next;
  }
  return TRUE;
}

/*
 * Curl_is_connected() is used from the multi interface to check if the
 * firstsocket has connected.
 */

CURLcode Curl_is_connected(struct connectdata *conn,
                           int sockindex,
                           bool *connected)
{
  int rc;
  struct SessionHandle *data = conn->data;
  CURLcode code = CURLE_OK;
  curl_socket_t sockfd = conn->sock[sockindex];
  long allow = DEFAULT_CONNECT_TIMEOUT;

  DEBUGASSERT(sockindex >= FIRSTSOCKET && sockindex <= SECONDARYSOCKET);

  *connected = FALSE; /* a very negative world view is best */

  if(conn->bits.tcpconnect) {
    /* we are connected already! */
    long allow_total = 0;

    /* subtract the most strict timeout of the ones */
    if(data->set.timeout)
      allow_total = data->set.timeout;

    Curl_expire(data, allow_total);
    *connected = TRUE;
    return CURLE_OK;
  }

  /* figure out how long time we have left to connect */
  allow = Curl_timeleft(conn, NULL, TRUE);

  if(allow < 0) {
    /* time-out, bail out, go home */
    failf(data, "Connection time-out");
    return CURLE_OPERATION_TIMEDOUT;
  }

  Curl_expire(data, allow);

  /* check for connect without timeout as we want to return immediately */
  rc = waitconnect(sockfd, 0);

  if(WAITCONN_CONNECTED == rc) {
    int error;
    if(verifyconnect(sockfd, &error)) {
      /* we are connected, awesome! */
      *connected = TRUE;
      return CURLE_OK;
    }
    /* nope, not connected for real */
    data->state.os_errno = error;
    infof(data, "Connection failed\n");
    if(trynextip(conn, sockindex, connected)) {
      failf(data, "Failed connect to %s:%d; %s",
            conn->host.name, conn->port, Curl_strerror(conn, error));
      code = CURLE_COULDNT_CONNECT;
    }
  }
  else if(WAITCONN_TIMEOUT != rc) {
    int error = 0;

    /* nope, not connected  */
    if(WAITCONN_FDSET_ERROR == rc) {
      (void)verifyconnect(sockfd, &error);
      data->state.os_errno = error;
      infof(data, "%s\n",Curl_strerror(conn,error));
    }
    else
      infof(data, "Connection failed\n");

    if(trynextip(conn, sockindex, connected)) {
      error = SOCKERRNO;
      data->state.os_errno = error;
      failf(data, "Failed connect to %s:%d; %s",
            conn->host.name, conn->port, Curl_strerror(conn, error));
      code = CURLE_COULDNT_CONNECT;
    }
  }
  /*
   * If the connection failed here, we should attempt to connect to the "next
   * address" for the given host.
   */

  return code;
}

static void tcpnodelay(struct connectdata *conn,
                       curl_socket_t sockfd)
{
#ifdef TCP_NODELAY
  struct SessionHandle *data= conn->data;
  socklen_t onoff = (socklen_t) data->set.tcp_nodelay;
  int proto = IPPROTO_TCP;

#if 0
  /* The use of getprotobyname() is disabled since it isn't thread-safe on
     numerous systems. On these getprotobyname_r() should be used instead, but
     that exists in at least one 4 arg version and one 5 arg version, and
     since the proto number rarely changes anyway we now just use the hard
     coded number. The "proper" fix would need a configure check for the
     correct function much in the same style the gethostbyname_r versions are
     detected. */
  struct protoent *pe = getprotobyname("tcp");
  if(pe)
    proto = pe->p_proto;
#endif

  if(setsockopt(sockfd, proto, TCP_NODELAY, (void *)&onoff,
                sizeof(onoff)) < 0)
    infof(data, "Could not set TCP_NODELAY: %s\n",
          Curl_strerror(conn, SOCKERRNO));
  else
    infof(data,"TCP_NODELAY set\n");
#else
  (void)conn;
  (void)sockfd;
#endif
}

#ifdef SO_NOSIGPIPE
/* The preferred method on Mac OS X (10.2 and later) to prevent SIGPIPEs when
   sending data to a dead peer (instead of relying on the 4th argument to send
   being MSG_NOSIGNAL). Possibly also existing and in use on other BSD
   systems? */
static void nosigpipe(struct connectdata *conn,
                      curl_socket_t sockfd)
{
  struct SessionHandle *data= conn->data;
  int onoff = 1;
  if(setsockopt(sockfd, SOL_SOCKET, SO_NOSIGPIPE, (void *)&onoff,
                sizeof(onoff)) < 0)
    infof(data, "Could not set SO_NOSIGPIPE: %s\n",
          Curl_strerror(conn, SOCKERRNO));
}
#else
#define nosigpipe(x,y)
#endif

/* singleipconnect() connects to the given IP only, and it may return without
   having connected if used from the multi interface. */
static curl_socket_t
singleipconnect(struct connectdata *conn,
                const Curl_addrinfo *ai,
                long timeout_ms,
                bool *connected)
{
  char addr_buf[128];
  int rc;
  int error;
  bool isconnected;
  struct SessionHandle *data = conn->data;
  curl_socket_t sockfd;
  CURLcode res;
  /*
   * Curl_sockaddr_storage, which is basically sockaddr_storage has a space
   * for a largest possible struct sockaddr only. We should add some space for
   * the other fields we are using. Hence the addr_storage size math.
   */
  char addr_storage[sizeof(struct curl_sockaddr)-
                    sizeof(struct sockaddr)+
                    sizeof(struct Curl_sockaddr_storage)];
  struct curl_sockaddr *addr=(struct curl_sockaddr*)&addr_storage;
  const void *iptoprint;

  addr->family=ai->ai_family;
  addr->socktype=conn->socktype;
  addr->protocol=ai->ai_protocol;
  addr->addrlen =
    (ai->ai_addrlen < (socklen_t)sizeof(struct Curl_sockaddr_storage)) ?
     (unsigned int)ai->ai_addrlen : sizeof(struct Curl_sockaddr_storage);
  memcpy(&addr->addr, ai->ai_addr, addr->addrlen);

  /* If set, use opensocket callback to get the socket */
  if(data->set.fopensocket)
    sockfd = data->set.fopensocket(data->set.opensocket_client,
                                   CURLSOCKTYPE_IPCXN, addr);
  else
    sockfd = socket(addr->family, addr->socktype, addr->protocol);
  if(sockfd == CURL_SOCKET_BAD)
    return CURL_SOCKET_BAD;

  *connected = FALSE; /* default is not connected */

  /* FIXME: do we have Curl_printable_address-like with struct sockaddr* as
     argument? */
#if defined(HAVE_SYS_UN_H) && defined(AF_UNIX)
  if(addr->family==AF_UNIX)
    infof(data, "  Trying %s... ",
          ((const struct sockaddr_un*)(&addr->addr))->sun_path);
  else
#endif
  {
#ifdef ENABLE_IPV6
    if(addr->family==AF_INET6)
      iptoprint= &((const struct sockaddr_in6*)(&addr->addr))->sin6_addr;
    else
#endif
      iptoprint = &((const struct sockaddr_in*)(&addr->addr))->sin_addr;

    if(Curl_inet_ntop(addr->family, iptoprint, addr_buf,
                      sizeof(addr_buf)) != NULL)
      infof(data, "  Trying %s... ", addr_buf);
  }

  if(data->set.tcp_nodelay)
    tcpnodelay(conn, sockfd);

  nosigpipe(conn, sockfd);

  if(data->set.fsockopt) {
    /* activate callback for setting socket options */
    error = data->set.fsockopt(data->set.sockopt_client,
                               sockfd,
                               CURLSOCKTYPE_IPCXN);
    if(error) {
      sclose(sockfd); /* close the socket and bail out */
      return CURL_SOCKET_BAD;
    }
  }

  /* possibly bind the local end to an IP, interface or port */
  res = bindlocal(conn, sockfd);
  if(res) {
    sclose(sockfd); /* close socket and bail out */
    return CURL_SOCKET_BAD;
  }

  /* set socket non-blocking */
  Curl_nonblock(sockfd, TRUE);

  /* Connect TCP sockets, bind UDP */
  if(conn->socktype == SOCK_STREAM)
    rc = connect(sockfd, &addr->addr, addr->addrlen);
  else
    rc = 0;

  if(-1 == rc) {
    error = SOCKERRNO;

    switch (error) {
    case EINPROGRESS:
    case EWOULDBLOCK:
#if defined(EAGAIN) && EAGAIN != EWOULDBLOCK
      /* On some platforms EAGAIN and EWOULDBLOCK are the
       * same value, and on others they are different, hence
       * the odd #if
       */
    case EAGAIN:
#endif
      rc = waitconnect(sockfd, timeout_ms);
      break;
    default:
      /* unknown error, fallthrough and try another address! */
      failf(data, "Failed to connect to %s: %s",
            addr_buf, Curl_strerror(conn,error));
      data->state.os_errno = error;
      break;
    }
  }

  /* The 'WAITCONN_TIMEOUT == rc' comes from the waitconnect(), and not from
     connect(). We can be sure of this since connect() cannot return 1. */
  if((WAITCONN_TIMEOUT == rc) &&
     (data->state.used_interface == Curl_if_multi)) {
    /* Timeout when running the multi interface */
    return sockfd;
  }

  isconnected = verifyconnect(sockfd, &error);

  if(!rc && isconnected) {
    /* we are connected, awesome! */
    *connected = TRUE; /* this is a true connect */
    infof(data, "connected\n");
    return sockfd;
  }
  else if(WAITCONN_TIMEOUT == rc)
    infof(data, "Timeout\n");
  else {
    data->state.os_errno = error;
    infof(data, "%s\n", Curl_strerror(conn, error));
  }

  /* connect failed or timed out */
  sclose(sockfd);

  return CURL_SOCKET_BAD;
}

/*
 * TCP connect to the given host with timeout, proxy or remote doesn't matter.
 * There might be more than one IP address to try out. Fill in the passed
 * pointer with the connected socket.
 */

CURLcode Curl_connecthost(struct connectdata *conn,  /* context */
                          const struct Curl_dns_entry *remotehost,
                          curl_socket_t *sockconn,   /* the connected socket */
                          Curl_addrinfo **addr,      /* the one we used */
                          bool *connected)           /* really connected? */
{
  struct SessionHandle *data = conn->data;
  curl_socket_t sockfd = CURL_SOCKET_BAD;
  int aliasindex;
  int num_addr;
  Curl_addrinfo *ai;
  Curl_addrinfo *curr_addr;

  struct timeval after;
  struct timeval before = Curl_tvnow();

  /*************************************************************
   * Figure out what maximum time we have left
   *************************************************************/
  long timeout_ms;
  long timeout_per_addr;

  *connected = FALSE; /* default to not connected */

  /* get the timeout left */
  timeout_ms = Curl_timeleft(conn, &before, TRUE);

  if(timeout_ms < 0) {
    /* a precaution, no need to continue if time already is up */
    failf(data, "Connection time-out");
    return CURLE_OPERATION_TIMEDOUT;
  }
  Curl_expire(data, timeout_ms);

  /* Max time for each address */
  num_addr = Curl_num_addresses(remotehost->addr);
  timeout_per_addr = timeout_ms / num_addr;

  ai = remotehost->addr;

  /* Below is the loop that attempts to connect to all IP-addresses we
   * know for the given host. One by one until one IP succeeds.
   */

  if(data->state.used_interface == Curl_if_multi)
    /* don't hang when doing multi */
    timeout_per_addr = 0;

  /*
   * Connecting with a Curl_addrinfo chain
   */
  for (curr_addr = ai, aliasindex=0; curr_addr;
       curr_addr = curr_addr->ai_next, aliasindex++) {

    /* start connecting to the IP curr_addr points to */
    sockfd = singleipconnect(conn, curr_addr, timeout_per_addr, connected);

    if(sockfd != CURL_SOCKET_BAD)
      break;

    /* get a new timeout for next attempt */
    after = Curl_tvnow();
    timeout_ms -= Curl_tvdiff(after, before);
    if(timeout_ms < 0) {
      failf(data, "connect() timed out!");
      return CURLE_OPERATION_TIMEDOUT;
    }
    before = after;
  }  /* end of connect-to-each-address loop */

  if(sockfd == CURL_SOCKET_BAD) {
    /* no good connect was made */
    *sockconn = CURL_SOCKET_BAD;
    failf(data, "couldn't connect to host");
    return CURLE_COULDNT_CONNECT;
  }

  /* leave the socket in non-blocking mode */

  /* store the address we use */
  if(addr)
    *addr = curr_addr;

  /* allow NULL-pointers to get passed in */
  if(sockconn)
    *sockconn = sockfd;    /* the socket descriptor we've connected */

  data->info.numconnects++; /* to track the number of connections made */

  return CURLE_OK;
}

/*
 * Used to extract socket and connectdata struct for the most recent
 * transfer on the given SessionHandle.
 *
 * The socket 'long' will be -1 in case of failure!
 */
CURLcode Curl_getconnectinfo(struct SessionHandle *data,
                             long *param_longp,
                             struct connectdata **connp)
{
  if((data->state.lastconnect != -1) &&
     (data->state.connc->connects[data->state.lastconnect] != NULL)) {
    struct connectdata *c =
      data->state.connc->connects[data->state.lastconnect];
    if(connp)
      /* only store this if the caller cares for it */
      *connp = c;
    *param_longp = c->sock[FIRSTSOCKET];
    /* we have a socket connected, let's determine if the server shut down */
    /* determine if ssl */
    if(c->ssl[FIRSTSOCKET].use) {
      /* use the SSL context */
      if(!Curl_ssl_check_cxn(c))
        *param_longp = -1;   /* FIN received */
    }
/* Minix 3.1 doesn't support any flags on recv; just assume socket is OK */
#ifdef MSG_PEEK
    else {
      /* use the socket */
      char buf;
      if(recv((RECV_TYPE_ARG1)c->sock[FIRSTSOCKET], (RECV_TYPE_ARG2)&buf,
              (RECV_TYPE_ARG3)1, (RECV_TYPE_ARG4)MSG_PEEK) == 0) {
        *param_longp = -1;   /* FIN received */
      }
    }
#endif
  }
  else
    *param_longp = -1;

  return CURLE_OK;
}
