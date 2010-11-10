/* Extracted from anet.c to work properly with Hiredis error reporting.
 *
 * Copyright (c) 2006-2010, Salvatore Sanfilippo <antirez at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "fmacros.h"
#include <sys/types.h>
#ifdef _WIN32
  #include "winsock2.h"
  #include "windows.h"
  #define EINPROGRESS WSAEWOULDBLOCK
#else
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <netdb.h>
#endif
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>

#include "hiredis.h"
#include "sds.h"

/* Forward declaration */
void __redisSetError(redisContext *c, int type, sds err);

#ifdef _WIN32

static int redisCreateSocket(redisContext *c, int type) {
    int s, on=1;

    if ((unsigned)(s = socket(type, SOCK_STREAM, 0)) == INVALID_SOCKET) {
        __redisSetError(c,REDIS_ERR_IO,NULL);
        return REDIS_ERR;
    }
    if (type == AF_INET) {
        if (setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char *) &on, sizeof(on)) == -1) {
            __redisSetError(c,REDIS_ERR_IO,NULL);
            close(s);
            return REDIS_ERR;
        }
    }
    return s;
}
#else
static int redisCreateSocket(redisContext *c, int type) {
    int s, on = 1;
    if ((s = socket(type, SOCK_STREAM, 0)) == -1) {
        __redisSetError(c,REDIS_ERR_IO,NULL);
        return REDIS_ERR;
    }
    if (type == AF_INET) {
        if (setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) == -1) {
            __redisSetError(c,REDIS_ERR_IO,NULL);
            close(s);
            return REDIS_ERR;
        }
    }
    return s;
}
#endif
static int redisSetNonBlock(redisContext *c, int fd) {
#ifdef _WIN32
  // If iMode = 0, blocking is enabled;
  // If iMode != 0, non-blocking mode is enabled.
  u_long iMode = 1;
  if (ioctlsocket(fd, FIONBIO, &iMode) == SOCKET_ERROR) {
    errno = WSAGetLastError();
    __redisSetError(c,REDIS_ERR_IO,
            sdscatprintf(sdsempty(), "ioctlsocket(FIONBIO): %d\n", errno));
    return REDIS_ERR;
  };
#else
    int flags;

    /* Set the socket nonblocking.
     * Note that fcntl(2) for F_GETFL and F_SETFL can't be
     * interrupted by a signal. */
    if ((flags = fcntl(fd, F_GETFL)) == -1) {
        __redisSetError(c,REDIS_ERR_IO,
            sdscatprintf(sdsempty(), "fcntl(F_GETFL): %s", strerror(errno)));
        close(fd);
        return REDIS_ERR;
    }
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        __redisSetError(c,REDIS_ERR_IO,
            sdscatprintf(sdsempty(), "fcntl(F_SETFL,O_NONBLOCK): %s", strerror(errno)));
        close(fd);
        return REDIS_ERR;
    }
#endif
    return REDIS_OK;
}

static int redisSetTcpNoDelay(redisContext *c, int fd) {
    int yes = 1;
    if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char *)&yes, sizeof(yes)) == -1) {
        __redisSetError(c,REDIS_ERR_IO,
            sdscatprintf(sdsempty(), "setsockopt(TCP_NODELAY): %s", strerror(errno)));
        return REDIS_ERR;
    }
    return REDIS_OK;
}

int redisContextConnectTcp(redisContext *c, const char *addr, int port) {
    int s;
    int blocking = (c->flags & REDIS_BLOCK);
    struct sockaddr_in sa;

    if ((s = redisCreateSocket(c,AF_INET)) == REDIS_ERR)
        return REDIS_ERR;
    if (!blocking && redisSetNonBlock(c,s) == REDIS_ERR)
        return REDIS_ERR;

    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
#ifdef _WIN32
    unsigned long inAddress;

    inAddress = inet_addr(addr);
    if (inAddress == INADDR_NONE || inAddress == INADDR_ANY) {
        struct hostent *he;

        he = gethostbyname(addr);
        if (he == NULL) {
            __redisSetError(c,REDIS_ERR_OTHER,
                sdscatprintf(sdsempty(),"can't resolve: %s\n", addr));
            closesocket(s);
            return REDIS_ERR;;
        }
        memcpy(&sa.sin_addr, he->h_addr, sizeof(struct in_addr));
    }
    else {
      sa.sin_addr.s_addr = inAddress;
    }
#else
    if (inet_aton(addr, &sa.sin_addr) == 0) {
        struct hostent *he;

        he = gethostbyname(addr);
        if (he == NULL) {
            __redisSetError(c,REDIS_ERR_OTHER,
                sdscatprintf(sdsempty(),"can't resolve: %s",addr));
            close(s);
            return REDIS_ERR;
        }
        memcpy(&sa.sin_addr, he->h_addr, sizeof(struct in_addr));
    }
#endif
    if (connect(s, (struct sockaddr*)&sa, sizeof(sa)) == -1) {
#ifdef _WIN32
        errno = WSAGetLastError();
        if ((errno == WSAEINVAL) || (errno == WSAEWOULDBLOCK)) errno = EINPROGRESS;
#endif
        if (errno == EINPROGRESS && !blocking) {
            /* This is ok. */
        } else {
            __redisSetError(c,REDIS_ERR_IO,NULL);
#ifdef _WIN32
            closesocket(s);
#else
            close(s);
#endif
            return REDIS_ERR;
        }
    }

    if (redisSetTcpNoDelay(c,s) != REDIS_OK) {
        close(s);
        return REDIS_ERR;
    }

    c->fd = s;
    return REDIS_OK;
}

int redisContextConnectUnix(redisContext *c, const char *path) {
#ifdef _WIN32
    (void) path;
    __redisSetError(c,REDIS_ERR_IO,NULL);
    return REDIS_ERR;
#else
    int s;
    int blocking = (c->flags & REDIS_BLOCK);
    struct sockaddr_un sa;

    if ((s = redisCreateSocket(c,AF_LOCAL)) == REDIS_ERR)
        return REDIS_ERR;
    if (!blocking && redisSetNonBlock(c,s) != REDIS_OK)
        return REDIS_ERR;

    sa.sun_family = AF_LOCAL;
    strncpy(sa.sun_path,path,sizeof(sa.sun_path)-1);
    if (connect(s, (struct sockaddr*)&sa, sizeof(sa)) == -1) {
        if (errno == EINPROGRESS && !blocking) {
            /* This is ok. */
        } else {
            __redisSetError(c,REDIS_ERR_IO,NULL);
            close(s);
            return REDIS_ERR;
        }
    }

    c->fd = s;
    return REDIS_OK;
#endif
}
