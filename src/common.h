/* AgentChain Engine — common utilities.
 *
 * Shared helpers used across modules: byte buffers, big-endian encoding,
 * hex conversion, logging, time, file I/O.
 */

#ifndef AGENTCHAIN_COMMON_H
#define AGENTCHAIN_COMMON_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* -------------------------------------------------------------------------- */
/* Fixed-size cryptographic byte arrays.                                      */
/* -------------------------------------------------------------------------- */

#define AC_PUBKEY_SIZE   32
#define AC_PRIVKEY_SIZE  64   /* Ed25519 expanded secret key */
#define AC_SEED_SIZE     32
#define AC_HASH_SIZE     32
#define AC_SIG_SIZE      64
#define AC_VRF_PROOF_SIZE 64
#define AC_VRF_OUT_SIZE  32

typedef struct { uint8_t b[AC_PUBKEY_SIZE]; } ac_addr_t;
typedef struct { uint8_t b[AC_HASH_SIZE];   } ac_hash_t;
typedef struct { uint8_t b[AC_SIG_SIZE];    } ac_sig_t;

static inline int ac_addr_cmp(const ac_addr_t *a, const ac_addr_t *b) {
    return memcmp(a->b, b->b, AC_PUBKEY_SIZE);
}
static inline int ac_hash_cmp(const ac_hash_t *a, const ac_hash_t *b) {
    return memcmp(a->b, b->b, AC_HASH_SIZE);
}
static inline bool ac_addr_eq(const ac_addr_t *a, const ac_addr_t *b) {
    return ac_addr_cmp(a, b) == 0;
}
static inline bool ac_hash_eq(const ac_hash_t *a, const ac_hash_t *b) {
    return ac_hash_cmp(a, b) == 0;
}
static inline bool ac_addr_is_zero(const ac_addr_t *a) {
    for (size_t i = 0; i < AC_PUBKEY_SIZE; ++i) if (a->b[i]) return false;
    return true;
}

/* -------------------------------------------------------------------------- */
/* Big-endian fixed-width integer reads/writes.                               */
/* -------------------------------------------------------------------------- */

static inline void ac_be8(uint8_t *p, uint8_t v)   { p[0] = v; }
static inline void ac_be16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v;
}
static inline void ac_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}
static inline void ac_be64(uint8_t *p, uint64_t v) {
    p[0] = (uint8_t)(v >> 56); p[1] = (uint8_t)(v >> 48);
    p[2] = (uint8_t)(v >> 40); p[3] = (uint8_t)(v >> 32);
    p[4] = (uint8_t)(v >> 24); p[5] = (uint8_t)(v >> 16);
    p[6] = (uint8_t)(v >> 8);  p[7] = (uint8_t)v;
}

static inline uint8_t  ac_rd8(const uint8_t *p)  { return p[0]; }
static inline uint16_t ac_rd16(const uint8_t *p) {
    return ((uint16_t)p[0] << 8) | (uint16_t)p[1];
}
static inline uint32_t ac_rd32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
         | ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}
static inline uint64_t ac_rd64(const uint8_t *p) {
    return ((uint64_t)p[0] << 56) | ((uint64_t)p[1] << 48)
         | ((uint64_t)p[2] << 40) | ((uint64_t)p[3] << 32)
         | ((uint64_t)p[4] << 24) | ((uint64_t)p[5] << 16)
         | ((uint64_t)p[6] << 8)  |  (uint64_t)p[7];
}

/* -------------------------------------------------------------------------- */
/* Hex encoding / decoding.                                                   */
/* -------------------------------------------------------------------------- */

/* Writes 2*len bytes plus a NUL into out. Caller-supplied buffer. */
void ac_hex_encode(char *out, const uint8_t *in, size_t len);
/* Returns 0 on success, -1 on malformed input. Writes exactly out_len bytes. */
int  ac_hex_decode(uint8_t *out, size_t out_len, const char *in);

/* -------------------------------------------------------------------------- */
/* Time.                                                                      */
/* -------------------------------------------------------------------------- */

uint64_t ac_now_ms(void);          /* milliseconds since the Unix epoch */
void     ac_sleep_ms(uint64_t ms); /* best-effort sleep */

/* -------------------------------------------------------------------------- */
/* Logging.                                                                   */
/* -------------------------------------------------------------------------- */

typedef enum {
    AC_LOG_ERROR = 0,
    AC_LOG_WARN  = 1,
    AC_LOG_INFO  = 2,
    AC_LOG_DEBUG = 3,
} ac_log_level_t;

void ac_log_init(ac_log_level_t level);
void ac_log_set_level(ac_log_level_t level);

void ac_log(ac_log_level_t lvl, const char *module, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

#define LOG_E(mod, ...) ac_log(AC_LOG_ERROR, mod, __VA_ARGS__)
#define LOG_W(mod, ...) ac_log(AC_LOG_WARN,  mod, __VA_ARGS__)
#define LOG_I(mod, ...) ac_log(AC_LOG_INFO,  mod, __VA_ARGS__)
#define LOG_D(mod, ...) ac_log(AC_LOG_DEBUG, mod, __VA_ARGS__)

/* -------------------------------------------------------------------------- */
/* File I/O: read full / atomic write.                                        */
/* -------------------------------------------------------------------------- */

/* Read whole file. On success returns malloc'd buffer (caller frees) and
 * writes length into *out_len. Returns NULL on error. */
uint8_t *ac_file_read_all(const char *path, size_t *out_len);

/* Write buf atomically: write to path.tmp, fsync, rename. Returns 0 on
 * success, -1 on error. mode is the final file mode. */
int ac_file_write_atomic(const char *path, const uint8_t *buf, size_t len, int mode);

/* mkdir -p — creates the full path (mode 0700 on each segment). */
int ac_mkdir_p(const char *path);

/* Returns true if a regular file exists at path. */
bool ac_file_exists(const char *path);

/* -------------------------------------------------------------------------- */
/* Misc helpers.                                                              */
/* -------------------------------------------------------------------------- */

/* Constant-time memory equality. Returns 1 iff equal. */
int ac_memeq(const void *a, const void *b, size_t n);

/* Integer square root (rounded down). */
uint64_t ac_isqrt_u64(uint64_t x);

/* Returns the path "<dir>/<name>". out must be at least dir+name+2 bytes.
 * Returns the number of bytes written (excluding NUL), or -1 if truncated. */
int ac_join_path(char *out, size_t out_size, const char *dir, const char *name);

/* Forbid copying secrets to swap, etc. — best effort. */
void ac_secure_zero(void *p, size_t n);

#endif /* AGENTCHAIN_COMMON_H */
