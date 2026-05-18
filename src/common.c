#include "common.h"
#include "portable.h"

#include <errno.h>
#include <stdlib.h>

/* -------------------------------------------------------------------------- */
/* Hex.                                                                       */
/* -------------------------------------------------------------------------- */

static const char HEX_DIGITS[] = "0123456789abcdef";

void ac_hex_encode(char *out, const uint8_t *in, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        out[2 * i]     = HEX_DIGITS[in[i] >> 4];
        out[2 * i + 1] = HEX_DIGITS[in[i] & 0x0F];
    }
    out[2 * len] = '\0';
}

static int hex_nibble(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int ac_hex_decode(uint8_t *out, size_t out_len, const char *in) {
    if (in == NULL) return -1;
    /* Allow optional "0x" prefix. */
    if (in[0] == '0' && (in[1] == 'x' || in[1] == 'X')) in += 2;
    if (strlen(in) != 2 * out_len) return -1;
    for (size_t i = 0; i < out_len; ++i) {
        int hi = hex_nibble((unsigned char)in[2 * i]);
        int lo = hex_nibble((unsigned char)in[2 * i + 1]);
        if (hi < 0 || lo < 0) return -1;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return 0;
}

/* -------------------------------------------------------------------------- */
/* Time.                                                                      */
/* -------------------------------------------------------------------------- */

uint64_t ac_now_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000);
}

void ac_sleep_ms(uint64_t ms) { ac_os_sleep_ms(ms); }

/* -------------------------------------------------------------------------- */
/* Logging.                                                                   */
/* -------------------------------------------------------------------------- */

static pthread_mutex_t LOG_MU      = PTHREAD_MUTEX_INITIALIZER;
static ac_log_level_t  LOG_LEVEL   = AC_LOG_INFO;

static const char *LEVEL_NAME[] = {"ERROR", "WARN ", "INFO ", "DEBUG"};
static const char *LEVEL_COLOR[] = {
    "\x1b[31m", /* red    */
    "\x1b[33m", /* yellow */
    "\x1b[32m", /* green  */
    "\x1b[36m", /* cyan   */
};

void ac_log_init(ac_log_level_t level) { LOG_LEVEL = level; }
void ac_log_set_level(ac_log_level_t level) { LOG_LEVEL = level; }

void ac_log(ac_log_level_t lvl, const char *module, const char *fmt, ...) {
    if ((int)lvl > (int)LOG_LEVEL) return;

    pthread_mutex_lock(&LOG_MU);

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    ac_localtime_r(&ts.tv_sec, &tm);

    char tbuf[32];
    strftime(tbuf, sizeof(tbuf), "%Y-%m-%dT%H:%M:%S", &tm);

    bool color = ac_os_isatty(fileno(stderr));
    if (color) fputs(LEVEL_COLOR[lvl], stderr);
    fprintf(stderr, "%s.%03ld %s ", tbuf, ts.tv_nsec / 1000000, LEVEL_NAME[lvl]);
    if (color) fputs("\x1b[0m", stderr);
    fprintf(stderr, "[%-7s] ", module);

    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);

    fputc('\n', stderr);

    pthread_mutex_unlock(&LOG_MU);
}

/* -------------------------------------------------------------------------- */
/* File I/O.                                                                  */
/* -------------------------------------------------------------------------- */

uint8_t *ac_file_read_all(const char *path, size_t *out_len) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;

    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        close(fd);
        return NULL;
    }

    size_t   len = (size_t)st.st_size;
    uint8_t *buf = (uint8_t *)malloc(len > 0 ? len : 1);
    if (!buf) { close(fd); return NULL; }

    size_t read_total = 0;
    while (read_total < len) {
        ssize_t r = read(fd, buf + read_total, len - read_total);
        if (r < 0) {
            if (errno == EINTR) continue;
            free(buf); close(fd); return NULL;
        }
        if (r == 0) break;
        read_total += (size_t)r;
    }
    close(fd);

    if (out_len) *out_len = read_total;
    return buf;
}

int ac_file_write_atomic(const char *path, const uint8_t *buf, size_t len, int mode) {
    char tmp[1024];
    if ((size_t)snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= sizeof(tmp)) return -1;

    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (fd < 0) return -1;

    size_t written = 0;
    while (written < len) {
        ssize_t w = write(fd, buf + written, len - written);
        if (w < 0) {
            if (errno == EINTR) continue;
            close(fd); ac_os_unlink(tmp); return -1;
        }
        written += (size_t)w;
    }

    if (ac_os_fsync(fd) != 0) { close(fd); ac_os_unlink(tmp); return -1; }
    close(fd);

#ifdef _WIN32
    /* Windows refuses rename-over-existing. Remove first; tolerate ENOENT. */
    ac_os_unlink(path);
#endif
    if (rename(tmp, path) != 0) { ac_os_unlink(tmp); return -1; }

    /* Best-effort directory fsync (POSIX only). */
#ifndef _WIN32
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = '\0';
        int dfd = open(dir, O_RDONLY);
        if (dfd >= 0) { ac_os_fsync(dfd); close(dfd); }
    }
#endif
    return 0;
}

int ac_mkdir_p(const char *path) {
    char buf[1024];
    if ((size_t)snprintf(buf, sizeof(buf), "%s", path) >= sizeof(buf)) return -1;

    /* Walk forward, creating intermediates. Treat both '/' and '\\' as
     * separators so Windows paths work too. */
    for (char *p = buf + 1; *p; ++p) {
        if (*p == '/' || *p == '\\') {
            char saved = *p;
            *p = '\0';
            if (ac_os_mkdir(buf) != 0 && errno != EEXIST) return -1;
            *p = saved;
        }
    }
    if (ac_os_mkdir(buf) != 0 && errno != EEXIST) return -1;
    return 0;
}

bool ac_file_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

/* -------------------------------------------------------------------------- */
/* Misc.                                                                      */
/* -------------------------------------------------------------------------- */

int ac_memeq(const void *a, const void *b, size_t n) {
    const volatile uint8_t *x = (const volatile uint8_t *)a;
    const volatile uint8_t *y = (const volatile uint8_t *)b;
    uint8_t r = 0;
    for (size_t i = 0; i < n; ++i) r |= x[i] ^ y[i];
    return r == 0;
}

uint64_t ac_isqrt_u64(uint64_t x) {
    if (x == 0) return 0;
    uint64_t r = 1ULL << 32;
    while (r * r > x) r = (r + x / r) / 2;
    /* One more Newton step to be safe. */
    while ((r + 1) * (r + 1) <= x) ++r;
    while (r * r > x) --r;
    return r;
}

int ac_join_path(char *out, size_t out_size, const char *dir, const char *name) {
    int n = snprintf(out, out_size, "%s/%s", dir, name);
    if (n < 0 || (size_t)n >= out_size) return -1;
    return n;
}

void ac_secure_zero(void *p, size_t n) {
    volatile uint8_t *vp = (volatile uint8_t *)p;
    while (n--) *vp++ = 0;
}
