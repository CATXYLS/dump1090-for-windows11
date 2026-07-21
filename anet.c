// Part of dump1090, a Mode S message decoder for RTLSDR devices.
//
// anet.c: Basic TCP socket stuff made a bit less boring
//
// Copyright (c) 2016 Oliver Jowett <oliver@mutability.co.uk>
//
// This file is free software: you may copy, redistribute and/or modify it
// under the terms of the GNU General Public License as published by the
// Free Software Foundation, either version 2 of the License, or (at your
// option) any later version.
//
// This file is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.

// This file incorporates work covered by the following copyright and
// permission notice:
//

/* anet.c -- Basic TCP socket stuff made a bit less boring
 *
 * Copyright (c) 2006-2012, Salvatore Sanfilippo <antirez at gmail dot com>
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

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <io.h>       // 提供 read/write/close 的 MinGW 映射
#include <fcntl.h>    // 提供 F_GETFL, F_SETFL, O_NONBLOCK
#include <errno.h>
#include <stdarg.h>
#include <string.h>

// Windows 与 Unix 的 socket 类型差异
#define ANET_SOCKET SOCKET
#define ANET_INVALID_SOCKET INVALID_SOCKET
#define ANET_SOCKET_ERROR SOCKET_ERROR
#define ANET_CLOSE closesocket
#define ANET_ERRNO WSAGetLastError()

// 将 read/write/close 重定向为 MinGW 兼容版本
#define read(fd, buf, len) _read(fd, buf, len)
#define write(fd, buf, len) _write(fd, buf, len)
#define close(fd) _close(fd)

#else
// Unix 原生头文件
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <netdb.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>

// Unix 下的类型映射
#define ANET_SOCKET int
#define ANET_INVALID_SOCKET -1
#define ANET_SOCKET_ERROR -1
#define ANET_CLOSE close
#define ANET_ERRNO errno

#endif

#include "anet.h"

static void anetSetError(char *err, const char *fmt, ...)
{
    va_list ap;

    if (!err) return;
    va_start(ap, fmt);
    vsnprintf(err, ANET_ERR_LEN, fmt, ap);
    va_end(ap);
}

int anetNonBlock(char *err, int fd)
{
#ifdef _WIN32
    // Windows: use ioctlsocket to set non-blocking mode
    unsigned long mode = 1;
    if (ioctlsocket(fd, FIONBIO, &mode) != 0) {
        anetSetError(err, "ioctlsocket(FIONBIO): %s", strerror(WSAGetLastError()));
        return ANET_ERR;
    }
#else
    int flags;   // <-- flags 移到这里
    // Unix: use fcntl
    if ((flags = fcntl(fd, F_GETFL)) == -1) {
        anetSetError(err, "fcntl(F_GETFL): %s", strerror(errno));
        return ANET_ERR;
    }
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        anetSetError(err, "fcntl(F_SETFL,O_NONBLOCK): %s", strerror(errno));
        return ANET_ERR;
    }
#endif

    return ANET_OK;
}

int anetTcpNoDelay(char *err, int fd)
{
    int yes = 1;
    if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (void*)&yes, sizeof(yes)) == -1)
    {
        anetSetError(err, "setsockopt TCP_NODELAY: %s", strerror(errno));
        return ANET_ERR;
    }
    return ANET_OK;
}

int anetSetSendBuffer(char *err, int fd, int buffsize)
{
    if (setsockopt(fd, SOL_SOCKET, SO_SNDBUF, (void*)&buffsize, sizeof(buffsize)) == -1)
    {
        anetSetError(err, "setsockopt SO_SNDBUF: %s", strerror(errno));
        return ANET_ERR;
    }
    return ANET_OK;
}

int anetTcpKeepAlive(char *err, int fd)
{
    int yes = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, (void*)&yes, sizeof(yes)) == -1) {
        anetSetError(err, "setsockopt SO_KEEPALIVE: %s", strerror(errno));
        return ANET_ERR;
    }
    return ANET_OK;
}

static int anetCreateSocket(char *err, int domain)
{
    int s, on = 1;
    if ((s = socket(domain, SOCK_STREAM, 0)) == (int)ANET_INVALID_SOCKET) {
#ifdef _WIN32
        fprintf(stderr, "socket() failed: WSAGetLastError=%d\n", WSAGetLastError());
#endif
        anetSetError(err, "creating socket: %s", strerror(ANET_ERRNO));
        return ANET_ERR;
    }

    if (setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&on, sizeof(on)) == ANET_SOCKET_ERROR) {
        // SO_REUSEADDR failure is not fatal on Windows, just warn
    }
    return s;
}

#define ANET_CONNECT_NONE 0
#define ANET_CONNECT_NONBLOCK 1
static int anetTcpGenericConnect(char *err, char *addr, char *service, int flags)
{
    int s;
    struct addrinfo gai_hints;
    struct addrinfo *gai_result, *p;
    int gai_error;

    gai_hints.ai_family = AF_UNSPEC;
    gai_hints.ai_socktype = SOCK_STREAM;
    gai_hints.ai_protocol = 0;
    gai_hints.ai_flags = 0;
    gai_hints.ai_addrlen = 0;
    gai_hints.ai_addr = NULL;
    gai_hints.ai_canonname = NULL;
    gai_hints.ai_next = NULL;

    gai_error = getaddrinfo(addr, service, &gai_hints, &gai_result);
    if (gai_error != 0) {
        anetSetError(err, "can't resolve %s: %s", addr, gai_strerror(gai_error));
        return ANET_ERR;
    }

    for (p = gai_result; p != NULL; p = p->ai_next) {
        if ((s = anetCreateSocket(err, p->ai_family)) == ANET_ERR)
            continue;

        if (flags & ANET_CONNECT_NONBLOCK) {
            if (anetNonBlock(err,s) != ANET_OK)
                return ANET_ERR;
        }

        if (connect(s, p->ai_addr, p->ai_addrlen) >= 0) {
            freeaddrinfo(gai_result);
            return s;
        }

        if (errno == EINPROGRESS && (flags & ANET_CONNECT_NONBLOCK)) {
            freeaddrinfo(gai_result);
            return s;
        }

        anetSetError(err, "connect: %s", strerror(errno));
        close(s);
    }

    freeaddrinfo(gai_result);
    return ANET_ERR;
}

int anetTcpConnect(char *err, char *addr, char *service)
{
    return anetTcpGenericConnect(err,addr,service,ANET_CONNECT_NONE);
}

int anetTcpNonBlockConnect(char *err, char *addr, char *service)
{
    return anetTcpGenericConnect(err,addr,service,ANET_CONNECT_NONBLOCK);
}

/* Like read(2) but make sure 'count' is read before to return
 * (unless error or EOF condition is encountered) */
int anetRead(int fd, char *buf, int count)
{
    int nread, totlen = 0;
    while(totlen != count) {
        nread = read(fd,buf,count-totlen);
        if (nread == 0) return totlen;
        if (nread == -1) return -1;
        totlen += nread;
        buf += nread;
    }
    return totlen;
}

/* Like write(2) but make sure 'count' is read before to return
 * (unless error is encountered) */
int anetWrite(int fd, char *buf, int count)
{
    int nwritten, totlen = 0;
    while(totlen != count) {
        nwritten = write(fd,buf,count-totlen);
        if (nwritten == 0) return totlen;
        if (nwritten == -1) return -1;
        totlen += nwritten;
        buf += nwritten;
    }
    return totlen;
}

static int anetListen(char *err, int s, struct sockaddr *sa, socklen_t len) {
    if (sa->sa_family == AF_INET6) {
        int on = 1;
        setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, (const char*)&on, sizeof(on));
    }

    if (bind(s, sa, len) == (int)ANET_SOCKET_ERROR) {
#ifdef _WIN32
        int wsa_err = WSAGetLastError();
        char msgbuf[256];
        FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                       NULL, wsa_err, 0, msgbuf, sizeof(msgbuf), NULL);
        anetSetError(err, "bind: [%d] %s", wsa_err, msgbuf);
#else
        anetSetError(err, "bind: %s", strerror(ANET_ERRNO));
#endif
        ANET_CLOSE(s);
        return ANET_ERR;
    }

    if (listen(s, 511) == (int)ANET_SOCKET_ERROR) {
        anetSetError(err, "listen: %s", strerror(ANET_ERRNO));
        ANET_CLOSE(s);
        return ANET_ERR;
    }
    return ANET_OK;
}

int anetTcpServer(char *err, char *service, char *bindaddr, int *fds, int nfds)
{
    int s;
    int i = 0;
#ifdef _WIN32
    // Windows-specific variables declared in the Windows block
    (void)nfds;  // suppress unused parameter warning
#else
    struct addrinfo gai_hints;
    struct addrinfo *gai_result, *p;
    int gai_error;
#endif

#ifdef _WIN32
    // Windows: bypass getaddrinfo for numeric IP addresses
    // fprintf(stderr, "DEBUG: anetTcpServer Windows branch, service=%s, bindaddr=%s\n", service, bindaddr ? bindaddr : "(null)");
    struct sockaddr_in sa4;
    struct sockaddr_in6 sa6;
    int port = atoi(service);
    
    // Try IPv4 first
    memset(&sa4, 0, sizeof(sa4));
    sa4.sin_family = AF_INET;
    sa4.sin_port = htons(port);
    if (!bindaddr || !strcmp(bindaddr, "0.0.0.0") || !strcmp(bindaddr, "localhost")) {
        sa4.sin_addr.s_addr = INADDR_ANY;
    } else if (inet_pton(AF_INET, bindaddr, &sa4.sin_addr) == 1) {
        // valid IPv4 address
    } else {
        // not a valid IPv4, skip IPv4
        goto try_ipv6;
    }
    
    if ((s = anetCreateSocket(err, AF_INET)) != ANET_ERR) {
        if (anetListen(err, s, (struct sockaddr*)&sa4, sizeof(sa4)) != ANET_ERR) {
            fds[i++] = s;
        } else {
            // fprintf(stderr, "IPv4 bind failed: %s\n", err);
        }
    }
    
try_ipv6:
    // Try IPv6
    memset(&sa6, 0, sizeof(sa6));
    sa6.sin6_family = AF_INET6;
    sa6.sin6_port = htons(port);
    if (!bindaddr || !strcmp(bindaddr, "0.0.0.0") || !strcmp(bindaddr, "localhost")) {
        sa6.sin6_addr = in6addr_any;
    } else if (inet_pton(AF_INET6, bindaddr, &sa6.sin6_addr) == 1) {
        // valid IPv6 address
    } else {
        goto done;
    }
    
    if ((s = anetCreateSocket(err, AF_INET6)) != ANET_ERR) {
        if (anetListen(err, s, (struct sockaddr*)&sa6, sizeof(sa6)) != ANET_ERR) {
            fds[i++] = s;
        } else {
            // fprintf(stderr, "IPv6 bind failed: %s\n", err);
        }
    }
    
done:
    if (i == 0) {
        anetSetError(err, "unable to bind socket");
        return ANET_ERR;
    }
    return i;
#else
    // Original Unix implementation
    gai_hints.ai_family = AF_UNSPEC;
    gai_hints.ai_socktype = SOCK_STREAM;
    gai_hints.ai_protocol = 0;
    gai_hints.ai_flags = AI_PASSIVE;
    gai_hints.ai_addrlen = 0;
    gai_hints.ai_addr = NULL;
    gai_hints.ai_canonname = NULL;
    gai_hints.ai_next = NULL;

    gai_error = getaddrinfo(bindaddr, service, &gai_hints, &gai_result);
    if (gai_error != 0) {
        anetSetError(err, "can't resolve %s: %s", bindaddr, gai_strerror(gai_error));
        return ANET_ERR;
    }

    for (p = gai_result; p != NULL && i < nfds; p = p->ai_next) {
        if ((s = anetCreateSocket(err, p->ai_family)) == ANET_ERR)
            continue;

        if (anetListen(err, s, p->ai_addr, p->ai_addrlen) == ANET_ERR) {
            continue;
        }

        fds[i++] = s;
    }

    freeaddrinfo(gai_result);
    return (i > 0 ? i : ANET_ERR);
#endif
}

static int anetGenericAccept(char *err, int s, struct sockaddr *sa, socklen_t *len)
{
    int fd;
    while(1) {
        fd = accept(s,sa,len);
        if (fd == -1) {
            if (errno == EINTR) {
                continue;
            } else {
                anetSetError(err, "accept: %s", strerror(errno));
            }
        }
        break;
    }
    return fd;
}

int anetTcpAccept(char *err, int s) {
    int fd;
    struct sockaddr_storage ss;
    socklen_t sslen = sizeof(ss);

    if ((fd = anetGenericAccept(err, s, (struct sockaddr*)&ss, &sslen)) == ANET_ERR)
        return ANET_ERR;

    return fd;
}
