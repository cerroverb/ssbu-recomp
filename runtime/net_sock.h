/* net_sock.h: the few Winsock calls net_relay.c / net_minilobby.c make that POSIX spells differently, in one place.
 *
 * Windows: the SDK's winsock2.h / ws2tcpip.h / mstcpip.h.
 * Linux / macOS: runtime/plat/winsock2.h (the BSD-socket subset nnnet.c uses; the WSA error codes are the errno
 * values), found with the plat/ include folder the POSIX build adds.
 *
 * Its helpers are static, so the header can go into both translation units (net_relay.c includes net_minilobby.c). */
#ifndef NET_SOCK_H
#define NET_SOCK_H

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
#else
#include <winsock2.h>
#include <ws2tcpip.h>
#include <sys/time.h>
#endif

/* SO_RCVTIMEO: Windows takes a DWORD of milliseconds, POSIX a struct timeval. */
static void net_sock_rcv_timeout(SOCKET s, int ms) {
#ifdef _WIN32
    DWORD to = (DWORD)ms;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&to, sizeof to);
#else
    struct timeval tv;
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const void *)&tv, sizeof tv);
#endif
}

/* Windows reports an ICMP port-unreachable as WSAECONNRESET on the next recvfrom of a connected-less UDP socket unless
 * this is turned off. POSIX sockets do not report it that way: there is nothing to turn off. */
static void net_sock_no_connreset(SOCKET s) {
#ifdef _WIN32
    DWORD bytes = 0;
    BOOL off = FALSE;
    WSAIoctl(s, SIO_UDP_CONNRESET, &off, sizeof off, NULL, 0, &bytes, NULL, NULL);
#else
    (void)s;
#endif
}

#endif /* NET_SOCK_H */
