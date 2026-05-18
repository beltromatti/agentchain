/* AgentChain Engine — platform abstraction.
 *
 * Single source of truth for the small set of OS-level symbols we need.
 * POSIX hosts (Linux, macOS, BSD) pull in the standard headers. Windows
 * builds (MSYS2/MinGW64) use Winsock2 plus a handful of Win32 calls;
 * the abstractions exposed here paper over the differences so the rest
 * of the engine reads as portable C.
 */

#ifndef AGENTCHAIN_PORTABLE_H
#define AGENTCHAIN_PORTABLE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* -------------------------------------------------------------------------- */
/* Windows                                                                    */
/* -------------------------------------------------------------------------- */
#ifdef _WIN32

# ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
# endif
# ifndef _WIN32_WINNT
#  define _WIN32_WINNT 0x0601 /* Windows 7+ for inet_pton etc. */
# endif

# include <winsock2.h>
# include <ws2tcpip.h>
# include <windows.h>
# include <io.h>
# include <direct.h>
# include <fcntl.h>
# include <signal.h>
# include <sys/types.h>
# include <sys/stat.h>

/* ssize_t — MinGW64 provides it through sys/types when _POSIX exposure is set,
 * but to be portable we typedef explicitly. */
# include <basetsd.h>
typedef SSIZE_T ssize_t;

/* Some POSIX integers / constants that Windows headers do not always expose. */
# ifndef SHUT_RD
#  define SHUT_RD   SD_RECEIVE
#  define SHUT_WR   SD_SEND
#  define SHUT_RDWR SD_BOTH
# endif

/* Socket FD type.
 *
 * On Windows SOCKET is `uintptr_t`. The rest of this codebase carries socket
 * descriptors as `int`. That works on 64-bit MinGW for any descriptor the
 * runtime hands out in practice (kernel object handles fit in 32 bits in user
 * space) and matches POSIX in spelling. We accept the implicit truncation;
 * it does not change observable behaviour. */
typedef int ac_sock_t;

/* Initialise / tear down winsock. POSIX equivalents are no-ops. */
static inline int  ac_net_init   (void) { WSADATA d; return WSAStartup(MAKEWORD(2, 2), &d) == 0 ? 0 : -1; }
static inline void ac_net_cleanup(void) { WSACleanup(); }

/* Socket I/O. */
static inline int     ac_sock_close(ac_sock_t fd) { return closesocket((SOCKET)fd); }
static inline ssize_t ac_sock_recv (ac_sock_t fd, void *buf, size_t n) {
    int r = recv((SOCKET)fd, (char *)buf, (int)n, 0);
    return (ssize_t)r;
}
static inline ssize_t ac_sock_send (ac_sock_t fd, const void *buf, size_t n) {
    int r = send((SOCKET)fd, (const char *)buf, (int)n, 0);
    return (ssize_t)r;
}
static inline int     ac_sock_shutdown(ac_sock_t fd, int how) {
    return shutdown((SOCKET)fd, how);
}

/* File / directory helpers. */
static inline int ac_os_mkdir (const char *path) { return _mkdir(path); }
static inline int ac_os_unlink(const char *path) { return _unlink(path); }
static inline int ac_os_fsync (int fd)           { return _commit(fd); }
static inline int ac_os_isatty(int fd)           { return _isatty(fd); }

/* Sleep / time. */
static inline void ac_os_sleep_ms(uint64_t ms) { Sleep((DWORD)ms); }

/* SIGPIPE has no analogue; the network layer must check write returns instead. */
# define AC_HAS_SIGPIPE 0

/* MSYS2 mingw64 provides pthread.h via winpthreads. */
# include <pthread.h>

/* localtime_r is missing on some MinGW configurations; localtime_s exists with
 * reversed argument order. Provide a portable wrapper. */
# ifndef HAVE_LOCALTIME_R
#  include <time.h>
static inline struct tm *ac_localtime_r(const time_t *t, struct tm *out) {
    if (localtime_s(out, t) != 0) return NULL;
    return out;
}
#  define ac_localtime_r ac_localtime_r
# endif

/* -------------------------------------------------------------------------- */
/* POSIX (Linux, macOS, BSD)                                                  */
/* -------------------------------------------------------------------------- */
#else

# include <arpa/inet.h>
# include <errno.h>
# include <fcntl.h>
# include <netdb.h>
# include <netinet/in.h>
# include <netinet/tcp.h>
# include <pthread.h>
# include <signal.h>
# include <sys/socket.h>
# include <sys/stat.h>
# include <sys/time.h>
# include <sys/types.h>
# include <time.h>
# include <unistd.h>

typedef int ac_sock_t;

static inline int  ac_net_init   (void) { return 0; }
static inline void ac_net_cleanup(void) { }

static inline int     ac_sock_close(ac_sock_t fd) { return close(fd); }
static inline ssize_t ac_sock_recv (ac_sock_t fd, void *buf, size_t n) {
    return read(fd, buf, n);
}
static inline ssize_t ac_sock_send (ac_sock_t fd, const void *buf, size_t n) {
    return write(fd, buf, n);
}
static inline int     ac_sock_shutdown(ac_sock_t fd, int how) {
    return shutdown(fd, how);
}

static inline int ac_os_mkdir (const char *path) { return mkdir(path, 0700); }
static inline int ac_os_unlink(const char *path) { return unlink(path); }
static inline int ac_os_fsync (int fd)           { return fsync(fd); }
static inline int ac_os_isatty(int fd)           { return isatty(fd); }

static inline void ac_os_sleep_ms(uint64_t ms) {
    struct timespec ts;
    ts.tv_sec  = (time_t)(ms / 1000);
    ts.tv_nsec = (long)((ms % 1000) * 1000000L);
    nanosleep(&ts, NULL);
}

# define AC_HAS_SIGPIPE 1

# define ac_localtime_r localtime_r

#endif /* _WIN32 */

#endif /* AGENTCHAIN_PORTABLE_H */
