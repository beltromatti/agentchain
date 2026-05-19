#include "rpc.h"
#include "portable.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

struct ac_rpc_s {
    ac_rpc_config_t cfg;
    int             fd;
    pthread_t       thread;
    bool            running;
};

/* -------------------------------------------------------------------------- */
/* Minimal JSON helpers.                                                      */
/* -------------------------------------------------------------------------- */

/* Find the JSON key `"name"` and extract its string value into `out` (max
 * cap-1 chars, NUL-terminated). Returns 1 on success, 0 on miss. Does NOT
 * decode escapes — sufficient for the small surface we accept. */
static int json_string(const char *body, const char *name, char *out, size_t cap) {
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\"", name);
    const char *p = strstr(body, pattern);
    if (!p) return 0;
    p += strlen(pattern);
    while (*p == ' ' || *p == '\t' || *p == ':') p++;
    if (*p != '"') return 0;
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < cap) {
        out[i++] = *p++;
    }
    out[i] = '\0';
    return *p == '"' ? 1 : 0;
}

static int json_uint64(const char *body, const char *name, uint64_t *out) {
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\"", name);
    const char *p = strstr(body, pattern);
    if (!p) return 0;
    p += strlen(pattern);
    while (*p == ' ' || *p == '\t' || *p == ':') p++;
    if (!isdigit((unsigned char)*p)) return 0;
    *out = strtoull(p, NULL, 10);
    return 1;
}

/* -------------------------------------------------------------------------- */
/* HTTP plumbing.                                                             */
/* -------------------------------------------------------------------------- */

static int write_all(int fd, const void *buf, size_t len) {
    const uint8_t *p = (const uint8_t *)buf;
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = ac_sock_send(fd, p + sent, len - sent);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        sent += (size_t)n;
    }
    return 0;
}

static void send_response(int fd, int status, const char *body) {
    char header[256];
    int n = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Server: AgentChain-Engine\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n\r\n",
        status, status == 200 ? "OK" : "Error", strlen(body));
    if (n > 0) write_all(fd, header, (size_t)n);
    write_all(fd, body, strlen(body));
}

static void send_rpc_result(int fd, const char *id_value, const char *result_json) {
    char body[8192];
    if (id_value && id_value[0]) {
        snprintf(body, sizeof(body),
            "{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":%s}",
            id_value, result_json);
    } else {
        snprintf(body, sizeof(body),
            "{\"jsonrpc\":\"2.0\",\"id\":null,\"result\":%s}", result_json);
    }
    send_response(fd, 200, body);
}

static void send_rpc_error(int fd, const char *id_value, int code, const char *msg) {
    char body[1024];
    snprintf(body, sizeof(body),
        "{\"jsonrpc\":\"2.0\",\"id\":%s,\"error\":{\"code\":%d,\"message\":\"%s\"}}",
        (id_value && id_value[0]) ? id_value : "null", code, msg);
    send_response(fd, 200, body);
}

/* -------------------------------------------------------------------------- */
/* Method handlers.                                                           */
/* -------------------------------------------------------------------------- */

static void handle_chain_info(ac_rpc_t *r, int fd, const char *id) {
    ac_chain_lock(r->cfg.chain);
    uint64_t cid     = ac_chain_chain_id(r->cfg.chain);
    uint64_t height  = ac_chain_height(r->cfg.chain);
    uint64_t bfee    = ac_chain_base_fee(r->cfg.chain);
    uint64_t gen_ts  = ac_chain_genesis_time(r->cfg.chain);
    const ac_hash_t *tip = ac_chain_tip_hash(r->cfg.chain);
    char hex[2 * AC_HASH_SIZE + 1];
    ac_hex_encode(hex, tip->b, AC_HASH_SIZE);
    ac_chain_unlock(r->cfg.chain);
    char buf[512];
    snprintf(buf, sizeof(buf),
        "{\"chain_id\":%" PRIu64 ",\"height\":%" PRIu64 ",\"tip_hash\":\"%s\","
        "\"base_fee\":%" PRIu64 ",\"genesis_timestamp_ms\":%" PRIu64 "}",
        cid, height, hex, bfee, gen_ts);
    send_rpc_result(fd, id, buf);
}

static void handle_account_get(ac_rpc_t *r, int fd, const char *id, const char *params) {
    char addr_hex[256] = {0};
    if (!json_string(params, "address", addr_hex, sizeof(addr_hex))) {
        send_rpc_error(fd, id, -32602, "missing address");
        return;
    }
    ac_addr_t a;
    if (ac_hex_decode(a.b, AC_PUBKEY_SIZE, addr_hex) != 0) {
        send_rpc_error(fd, id, -32602, "bad address hex");
        return;
    }
    ac_account_t acc;
    ac_chain_lock(r->cfg.chain);
    ac_state_get(ac_chain_state(r->cfg.chain), &a, &acc);
    ac_chain_unlock(r->cfg.chain);
    char buf[256];
    snprintf(buf, sizeof(buf),
        "{\"balance\":%" PRIu64 ",\"nonce\":%" PRIu64 ",\"stake\":%" PRIu64
        ",\"unbond_at\":%" PRIu64 "}",
        acc.balance, acc.nonce, acc.stake, acc.unbond_at);
    send_rpc_result(fd, id, buf);
}

static void handle_name_lookup(ac_rpc_t *r, int fd, const char *id, const char *params) {
    char name[64] = {0};
    if (!json_string(params, "name", name, sizeof(name))) {
        send_rpc_error(fd, id, -32602, "missing name");
        return;
    }
    ac_addr_t a;
    ac_chain_lock(r->cfg.chain);
    int found = ac_state_name_lookup(ac_chain_state(r->cfg.chain),
                                     (const uint8_t *)name, strlen(name), &a);
    ac_chain_unlock(r->cfg.chain);
    if (!found) {
        send_rpc_result(fd, id, "null");
        return;
    }
    char hex[2 * AC_PUBKEY_SIZE + 1];
    ac_hex_encode(hex, a.b, AC_PUBKEY_SIZE);
    char buf[128];
    snprintf(buf, sizeof(buf), "{\"address\":\"%s\"}", hex);
    send_rpc_result(fd, id, buf);
}

static void handle_tx_submit(ac_rpc_t *r, int fd, const char *id, const char *params) {
    char hex[4096] = {0};
    if (!json_string(params, "tx_hex", hex, sizeof(hex))) {
        send_rpc_error(fd, id, -32602, "missing tx_hex");
        return;
    }
    size_t bin_len = strlen(hex) / 2;
    if (bin_len > AC_TX_MAX_BYTES) {
        send_rpc_error(fd, id, -32602, "tx too large");
        return;
    }
    uint8_t bin[AC_TX_MAX_BYTES];
    if (ac_hex_decode(bin, bin_len, hex) != 0) {
        send_rpc_error(fd, id, -32602, "bad hex");
        return;
    }
    ac_tx_t tx;
    if (ac_tx_decode(&tx, bin, bin_len) < 0) {
        send_rpc_error(fd, id, -32602, "tx decode failed");
        return;
    }
    ac_chain_lock(r->cfg.chain);
    uint64_t slot = ac_chain_current_slot(r->cfg.chain);
    ac_mp_result_t rs = ac_mempool_add(r->cfg.mempool, ac_chain_state(r->cfg.chain), &tx, slot);
    ac_chain_unlock(r->cfg.chain);
    if (rs != AC_MP_ACCEPTED) {
        send_rpc_error(fd, id, -32000, ac_mp_result_str(rs));
        return;
    }
    /* Gossip the accepted transaction to every connected peer so it lands
     * in the next leader's mempool, regardless of whether this node is
     * itself a validator. The callback is optional so unit tests can
     * exercise the RPC without spinning up a real net layer. */
    if (r->cfg.broadcast_tx) {
        r->cfg.broadcast_tx(bin, bin_len, r->cfg.broadcast_tx_ctx);
    }
    ac_hash_t h;
    ac_tx_hash(&h, &tx);
    char hhex[2 * AC_HASH_SIZE + 1];
    ac_hex_encode(hhex, h.b, AC_HASH_SIZE);
    char buf[128];
    snprintf(buf, sizeof(buf), "{\"hash\":\"%s\"}", hhex);
    send_rpc_result(fd, id, buf);
}

static void handle_mempool_size(ac_rpc_t *r, int fd, const char *id) {
    size_t n = ac_mempool_size(r->cfg.mempool);
    char buf[32];
    snprintf(buf, sizeof(buf), "{\"size\":%zu}", n);
    send_rpc_result(fd, id, buf);
}

static void handle_block_get(ac_rpc_t *r, int fd, const char *id, const char *params) {
    uint64_t h = 0;
    if (!json_uint64(params, "height", &h)) {
        send_rpc_error(fd, id, -32602, "missing height");
        return;
    }
    ac_block_header_t hdr;
    ac_chain_lock(r->cfg.chain);
    int rc = ac_chain_get_header_by_height(r->cfg.chain, h, &hdr);
    ac_chain_unlock(r->cfg.chain);
    if (rc < 0) {
        send_rpc_error(fd, id, -32004, "block not found");
        return;
    }
    char ph[2 * AC_HASH_SIZE + 1];
    char sr[2 * AC_HASH_SIZE + 1];
    char tr[2 * AC_HASH_SIZE + 1];
    char pp[2 * AC_PUBKEY_SIZE + 1];
    ac_hex_encode(ph, hdr.parent_hash.b, AC_HASH_SIZE);
    ac_hex_encode(sr, hdr.state_root.b,  AC_HASH_SIZE);
    ac_hex_encode(tr, hdr.tx_root.b,     AC_HASH_SIZE);
    ac_hex_encode(pp, hdr.proposer.b,    AC_PUBKEY_SIZE);
    char buf[1024];
    snprintf(buf, sizeof(buf),
        "{\"height\":%" PRIu64 ",\"slot\":%" PRIu64 ",\"timestamp_ms\":%" PRIu64
        ",\"parent_hash\":\"%s\",\"state_root\":\"%s\",\"tx_root\":\"%s\","
        "\"proposer\":\"%s\",\"base_fee\":%" PRIu64 ",\"gas_used\":%" PRIu64
        ",\"gas_limit\":%" PRIu64 ",\"tx_count\":%u}",
        hdr.height, hdr.slot, hdr.timestamp_ms, ph, sr, tr, pp,
        hdr.base_fee, hdr.gas_used, hdr.gas_limit, hdr.tx_count);
    send_rpc_result(fd, id, buf);
}

/* -------------------------------------------------------------------------- */
/* Connection handler.                                                        */
/* -------------------------------------------------------------------------- */

static void handle_client(ac_rpc_t *r, int fd) {
    /* Read up to 64 KB of request. */
    char buf[65536];
    size_t total = 0;
    /* Read until end-of-headers, then content-length-determined body. */
    size_t header_end = SIZE_MAX;
    while (total < sizeof(buf) - 1) {
        ssize_t n = ac_sock_recv(fd, buf + total, sizeof(buf) - 1 - total);
        if (n <= 0) break;
        total += (size_t)n;
        buf[total] = '\0';
        const char *p = strstr(buf, "\r\n\r\n");
        if (p) { header_end = (size_t)(p - buf) + 4; break; }
    }
    if (header_end == SIZE_MAX) { send_response(fd, 400, "{\"error\":\"bad request\"}"); return; }

    /* Look for Content-Length to read full body. Case-insensitive scan. */
    size_t content_length = 0;
    static const char NEEDLE[] = "content-length:";
    char lower[64];
    /* Scan a small prefix lower-cased to find the header. */
    size_t hl_scan = total < sizeof(buf) - 1 ? total : sizeof(buf) - 1;
    /* Walk the buffer line by line for "Content-Length:". */
    const char *p = buf;
    while (p < buf + hl_scan) {
        const char *eol = strstr(p, "\r\n");
        if (!eol || eol >= buf + hl_scan) break;
        size_t llen = (size_t)(eol - p);
        if (llen >= sizeof(NEEDLE) - 1 && llen < sizeof(lower)) {
            for (size_t i = 0; i < sizeof(NEEDLE) - 1; ++i) {
                lower[i] = (char)tolower((unsigned char)p[i]);
            }
            lower[sizeof(NEEDLE) - 1] = '\0';
            if (memcmp(lower, NEEDLE, sizeof(NEEDLE) - 1) == 0) {
                content_length = (size_t)strtoul(p + sizeof(NEEDLE) - 1, NULL, 10);
                break;
            }
        }
        p = eol + 2;
    }
    while (total < header_end + content_length && total < sizeof(buf) - 1) {
        ssize_t n = ac_sock_recv(fd, buf + total, sizeof(buf) - 1 - total);
        if (n <= 0) break;
        total += (size_t)n;
    }
    buf[total] = '\0';

    if (strncmp(buf, "POST", 4) != 0) {
        send_response(fd, 405, "{\"error\":\"method not allowed\"}");
        return;
    }

    const char *body = buf + header_end;

    /* Extract method, id, params. */
    char method[64] = {0};
    if (!json_string(body, "method", method, sizeof(method))) {
        send_rpc_error(fd, "null", -32600, "missing method");
        return;
    }

    /* Capture id literal (may be number or string or null). For simplicity,
     * we just echo back the raw substring between "id": and the next , or }. */
    char id_lit[32] = "null";
    {
        const char *idp = strstr(body, "\"id\"");
        if (idp) {
            idp += 4;
            while (*idp == ' ' || *idp == '\t' || *idp == ':') idp++;
            size_t i = 0;
            while (*idp && *idp != ',' && *idp != '}' && i + 1 < sizeof(id_lit)) {
                id_lit[i++] = *idp++;
            }
            id_lit[i] = '\0';
            /* Trim trailing whitespace */
            while (i > 0 && (id_lit[i - 1] == ' ' || id_lit[i - 1] == '\t')) {
                id_lit[--i] = '\0';
            }
        }
    }

    const char *params = strstr(body, "\"params\"");
    if (!params) params = body;

    if      (strcmp(method, "chain_info")      == 0) handle_chain_info  (r, fd, id_lit);
    else if (strcmp(method, "account_get")     == 0) handle_account_get (r, fd, id_lit, params);
    else if (strcmp(method, "name_lookup")     == 0) handle_name_lookup (r, fd, id_lit, params);
    else if (strcmp(method, "tx_submit")       == 0) handle_tx_submit   (r, fd, id_lit, params);
    else if (strcmp(method, "mempool_size")    == 0) handle_mempool_size(r, fd, id_lit);
    else if (strcmp(method, "block_get")       == 0) handle_block_get   (r, fd, id_lit, params);
    else send_rpc_error(fd, id_lit, -32601, "method not found");
}

static void *rpc_accept_loop(void *arg) {
    ac_rpc_t *r = (ac_rpc_t *)arg;
    while (r->running) {
        struct sockaddr_storage addr;
        socklen_t alen = sizeof(addr);
        int fd = accept(r->fd, (struct sockaddr *)&addr, &alen);
        if (fd < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            if (!r->running) break;
            continue;
        }
        handle_client(r, fd);
        ac_sock_close(fd);
    }
    return NULL;
}

/* -------------------------------------------------------------------------- */
/* Public.                                                                    */
/* -------------------------------------------------------------------------- */

ac_rpc_t *ac_rpc_new(const ac_rpc_config_t *cfg) {
    ac_rpc_t *r = (ac_rpc_t *)calloc(1, sizeof(*r));
    if (!r) return NULL;
    r->cfg = *cfg;
    r->fd = -1;
    return r;
}

void ac_rpc_free(ac_rpc_t *r) {
    if (!r) return;
    if (r->running) ac_rpc_stop(r);
    free(r);
}

int ac_rpc_start(ac_rpc_t *r) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof(one));
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port   = htons(r->cfg.port);
    if (r->cfg.host[0]) inet_pton(AF_INET, r->cfg.host, &sa.sin_addr);
    else                sa.sin_addr.s_addr = htonl(0x7f000001u); /* 127.0.0.1 */
    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        LOG_E("rpc", "bind %u: %s", r->cfg.port, strerror(errno));
        ac_sock_close(fd); return -1;
    }
    if (listen(fd, 16) < 0) { ac_sock_close(fd); return -1; }
    r->fd = fd;
    r->running = true;
#if AC_HAS_SIGPIPE
    signal(SIGPIPE, SIG_IGN);
#endif
    if (pthread_create(&r->thread, NULL, rpc_accept_loop, r) != 0) {
        r->running = false;
        ac_sock_close(fd);
        return -1;
    }
    LOG_I("rpc", "listening on %s:%u",
          r->cfg.host[0] ? r->cfg.host : "127.0.0.1", r->cfg.port);
    return 0;
}

void ac_rpc_stop(ac_rpc_t *r) {
    r->running = false;
    if (r->fd >= 0) {
        ac_sock_shutdown(r->fd, SHUT_RDWR);
        ac_sock_close(r->fd);
        r->fd = -1;
    }
    pthread_join(r->thread, NULL);
}
