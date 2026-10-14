/*
 * runtime/turfrt_net.c -- REL-008 stage 5 (Windows) and stage 6 (macOS)
 * (docs/planning/ROADMAP_v2.0.0.md): the few things about TCP sockets that are
 * NOT the same on every platform, behind one portable interface, so that
 * lib/std/io/net.tr is the same Turf on all of them.
 *
 * What differs, and why it cannot be written in Turf (the language has no
 * conditional compilation and the library is written against raw libc bindings):
 *   - Windows needs WSAStartup() before any socket call, calls the close
 *     function closesocket() (close() is for C-runtime file descriptors, and
 *     passing it a SOCKET is an error), and its SOCKET is a pointer-sized
 *     unsigned handle, not an int. SOL_SOCKET is 0xffff there and SO_REUSEADDR is
 *     4; on Linux they are 1 and 2.
 *   - The BSD family (macOS) puts a length byte first in struct sockaddr_in
 *     (`sin_len`), so the Linux byte image net.tr used to hand-build -- family in
 *     the first two bytes -- is wrong there.
 * Everything else net.tr uses (bind, listen, accept, connect, recv, send,
 * gethostbyname) is the same function with the same arguments everywhere, and
 * is still bound directly.
 *
 * The address is built with the platform's own `struct sockaddr_in`, so whatever
 * its layout is, it is right. The boundary this respects (FREEZE.md 6.8, B2/B3):
 * WSAStartup is genuinely process-wide state, and nothing here is a capability a
 * user program cannot also request: a user can bind these same symbols with
 * `extern fn`, since libturfrt is linked into every program.
 *
 * Handles are int64_t so a Windows SOCKET fits; failure is -1 everywhere.
 */
#include <stdint.h>
#include <stdlib.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#ifdef _WIN32
static void turfrt_net_cleanup(void) { WSACleanup(); }
#endif

/* Winsock must be started once, before the first socket call. A no-op elsewhere.
 * Returns 0 on success, -1 on failure. (A Turf program is single-threaded at the
 * point it first touches a socket, so the flag needs no lock.) */
int32_t turfrt_net_startup(void) {
#ifdef _WIN32
  static int started = 0;
  if (started)
    return 0;
  WSADATA data;
  if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
    return -1;
  started = 1;
  atexit(turfrt_net_cleanup);
#endif
  return 0;
}

/* A TCP/IPv4 stream socket. -1 on failure. */
int64_t turfrt_net_socket(void) {
  if (turfrt_net_startup() != 0)
    return -1;
#ifdef _WIN32
  SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
  return s == INVALID_SOCKET ? -1 : (int64_t)s;
#else
  int s = socket(AF_INET, SOCK_STREAM, 0);
  return s < 0 ? -1 : (int64_t)s;
#endif
}

/* Closes a socket from turfrt_net_socket() or accept(). 0 on success. */
int32_t turfrt_net_close(int64_t s) {
#ifdef _WIN32
  return closesocket((SOCKET)s) == 0 ? 0 : -1;
#else
  return close((int)s) == 0 ? 0 : -1;
#endif
}

/* SO_REUSEADDR = 1, so a restarted server can bind while the old socket sits in
 * TIME_WAIT. 0 on success. */
int32_t turfrt_net_reuseaddr(int64_t s) {
  int one = 1;
#ifdef _WIN32
  return setsockopt((SOCKET)s, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, (int)sizeof one) == 0 ? 0 : -1;
#else
  return setsockopt((int)s, SOL_SOCKET, SO_REUSEADDR, &one, (socklen_t)sizeof one) == 0 ? 0 : -1;
#endif
}

/* A `struct sockaddr_in` for a.b.c.d:port, in this platform's own layout,
 * allocated with calloc so the caller releases it with mem::free() (as net.tr's
 * hand-built one was). NULL if out of memory. All zero octets is INADDR_ANY. */
void *turfrt_net_sockaddr(int64_t a, int64_t b, int64_t c, int64_t d, int64_t port) {
  struct sockaddr_in *sa = (struct sockaddr_in *)calloc(1, sizeof *sa);
  if (sa == NULL)
    return NULL;
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
  sa->sin_len = (uint8_t)sizeof *sa;
#endif
  sa->sin_family = AF_INET;
  sa->sin_port = htons((uint16_t)port);
  sa->sin_addr.s_addr =
      htonl(((uint32_t)(a & 0xff) << 24) | ((uint32_t)(b & 0xff) << 16) | ((uint32_t)(c & 0xff) << 8) | (uint32_t)(d & 0xff));
  return sa;
}
