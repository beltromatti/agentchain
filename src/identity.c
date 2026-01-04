#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "crypto.h"
#include "identity.h"
#include "utils.h"

#define IDENTITY_DIR "data"
#define IDENTITY_PATH "data/identity.key"
#define IDENTITY_MAGIC 0x44494342u /* 'BCID' */
#define IDENTITY_VERSION 1u

typedef struct {
    uint32_t magic;
    uint32_t version;
    pub_key_t pub;
    priv_key_t priv;
} identity_file;

static int ensure_identity_dir(void) {
    if (mkdir(IDENTITY_DIR, 0700) == 0) return 0;
    if (errno == EEXIST) return 0;
    return -1;
}

static int identity_file_load(identity_file* out) {
    if (!out) return -1;
    int fd = open(IDENTITY_PATH, O_RDONLY);
    if (fd < 0) {
        if (errno == ENOENT) return -2;
        return -3;
    }

    ssize_t n = read(fd, out, sizeof(*out));
    close(fd);
    if (n != (ssize_t)sizeof(*out)) return -4;
    if (out->magic != IDENTITY_MAGIC || out->version != IDENTITY_VERSION) return -5;
    return 0;
}

static int identity_file_write(const identity_file* in) {
    if (!in) return -1;
    if (ensure_identity_dir() < 0) return -2;

    int fd = open(IDENTITY_PATH, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) return -3;
    ssize_t n = write(fd, in, sizeof(*in));
    if (n != (ssize_t)sizeof(*in)) {
        close(fd);
        return -4;
    }
    fsync(fd);
    close(fd);
    return 0;
}

static int identity_file_overwrite(const identity_file* in) {
    if (!in) return -1;
    if (ensure_identity_dir() < 0) return -2;

    int fd = open(IDENTITY_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return -3;
    ssize_t n = write(fd, in, sizeof(*in));
    if (n != (ssize_t)sizeof(*in)) {
        close(fd);
        return -4;
    }
    fsync(fd);
    close(fd);
    return 0;
}

static int identity_validate_keypair(const pub_key_t pub, const priv_key_t priv) {
    if (!pub || !priv) return -1;
    pub_key_t derived;
    if (crypto_sign_ed25519_sk_to_pk(derived, priv) != 0) return -2;
    if (memcmp(derived, pub, crypto_sign_PUBLICKEYBYTES) != 0) return -3;
    return 0;
}

static int env_hex_len(const char* s) {
    if (!s) return 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    return (int)strlen(s);
}

static int identity_from_env(account* out, int require_priv) {
    if (!out) return -1;

    const char* priv_hex = getenv("BC_PRIVKEY");
    const char* pub_hex = getenv("BC_PUBKEY");
    int priv_len = env_hex_len(priv_hex);
    int pub_len = env_hex_len(pub_hex);

    memset(out, 0, sizeof(*out));

    if (priv_hex && *priv_hex) {
        if (priv_len == (int)(crypto_sign_SECRETKEYBYTES * 2)) {
            if (hex_to_bytes(priv_hex, out->priv_key, crypto_sign_SECRETKEYBYTES) < 0) return -2;
            if (pub_hex && *pub_hex) {
                if (pub_len != (int)(crypto_sign_PUBLICKEYBYTES * 2)) return -3;
                if (hex_to_bytes(pub_hex, out->pub_key, crypto_sign_PUBLICKEYBYTES) < 0) return -4;
            } else {
                if (derive_pub_key(out->priv_key, out->pub_key) < 0) return -5;
            }
            if (identity_validate_keypair(out->pub_key, out->priv_key) < 0) return -6;
            return 0;
        }

        if (priv_len == (int)(crypto_sign_SEEDBYTES * 2)) {
            uint8_t seed[crypto_sign_SEEDBYTES];
            if (hex_to_bytes(priv_hex, seed, crypto_sign_SEEDBYTES) < 0) return -7;
            if (crypto_sign_seed_keypair(out->pub_key, out->priv_key, seed) != 0) return -8;
            if (pub_hex && *pub_hex) {
                pub_key_t env_pub;
                if (pub_len != (int)(crypto_sign_PUBLICKEYBYTES * 2)) return -9;
                if (hex_to_bytes(pub_hex, env_pub, crypto_sign_PUBLICKEYBYTES) < 0) return -10;
                if (memcmp(env_pub, out->pub_key, crypto_sign_PUBLICKEYBYTES) != 0) return -11;
            }
            return 0;
        }

        return -12;
    }

    if (!require_priv && pub_hex && *pub_hex) {
        if (pub_len != (int)(crypto_sign_PUBLICKEYBYTES * 2)) return -13;
        if (hex_to_bytes(pub_hex, out->pub_key, crypto_sign_PUBLICKEYBYTES) < 0) return -14;
        return 0;
    }

    return -15;
}

int identity_load(account* out, int require_priv) {
    if (!out) return -1;

    account env;
    if (identity_from_env(&env, require_priv) == 0) {
        *out = env;
        return 0;
    }

    identity_file f;
    int rc = identity_file_load(&f);
    if (rc == 0) {
        if (identity_validate_keypair(f.pub, f.priv) < 0) return -2;
        memcpy(out->pub_key, f.pub, crypto_sign_PUBLICKEYBYTES);
        memcpy(out->priv_key, f.priv, crypto_sign_SECRETKEYBYTES);
        out->balance = 0;
        if (!require_priv) {
            memset(out->priv_key, 0, crypto_sign_SECRETKEYBYTES);
        }
        return 0;
    }

    if (rc != -2) return -3;
    if (!require_priv) return -4;

    memset(&f, 0, sizeof(f));
    f.magic = IDENTITY_MAGIC;
    f.version = IDENTITY_VERSION;
    if (crypto_sign_keypair(f.pub, f.priv) != 0) return -5;
    if (identity_file_write(&f) < 0) {
        if (identity_file_load(&f) == 0) {
            if (identity_validate_keypair(f.pub, f.priv) < 0) return -6;
            memcpy(out->pub_key, f.pub, crypto_sign_PUBLICKEYBYTES);
            memcpy(out->priv_key, f.priv, crypto_sign_SECRETKEYBYTES);
            out->balance = 0;
            return 0;
        }
        return -7;
    }

    memcpy(out->pub_key, f.pub, crypto_sign_PUBLICKEYBYTES);
    memcpy(out->priv_key, f.priv, crypto_sign_SECRETKEYBYTES);
    out->balance = 0;
    return 0;
}

int identity_rotate(account* out) {
    if (!out) return -1;

    identity_file f;
    memset(&f, 0, sizeof(f));
    f.magic = IDENTITY_MAGIC;
    f.version = IDENTITY_VERSION;
    if (crypto_sign_keypair(f.pub, f.priv) != 0) return -2;
    if (identity_validate_keypair(f.pub, f.priv) < 0) return -3;

    if (identity_file_overwrite(&f) < 0) return -4;

    memcpy(out->pub_key, f.pub, crypto_sign_PUBLICKEYBYTES);
    memcpy(out->priv_key, f.priv, crypto_sign_SECRETKEYBYTES);
    out->balance = 0;
    return 0;
}
