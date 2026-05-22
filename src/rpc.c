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
    /* result_json can be large (top-N lists, full blocks, peers). Allocate to
     * fit the worst-case envelope plus the prefix. */
    size_t need = strlen(result_json) + 128;
    char *body = (char *)malloc(need);
    if (!body) return;
    if (id_value && id_value[0]) {
        snprintf(body, need,
            "{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":%s}",
            id_value, result_json);
    } else {
        snprintf(body, need,
            "{\"jsonrpc\":\"2.0\",\"id\":null,\"result\":%s}", result_json);
    }
    send_response(fd, 200, body);
    free(body);
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

/* tx_get — lookup a transaction by hash. Returns the full tx fields plus
 * the containing block height + index, or error -32004 if not found. */
static void handle_tx_get(ac_rpc_t *r, int fd, const char *id, const char *params) {
    char hex[2 * AC_HASH_SIZE + 1] = {0};
    if (!json_string(params, "hash", hex, sizeof(hex))) {
        send_rpc_error(fd, id, -32602, "missing hash"); return;
    }
    ac_hash_t th;
    if (ac_hex_decode(th.b, AC_HASH_SIZE, hex) != 0) {
        send_rpc_error(fd, id, -32602, "bad hex"); return;
    }

    ac_chain_lock(r->cfg.chain);
    uint64_t height = 0;
    uint32_t idx = 0;
    int found = ac_chain_tx_find(r->cfg.chain, &th, &height, &idx);
    if (!found) { ac_chain_unlock(r->cfg.chain); send_rpc_error(fd, id, -32004, "tx not found"); return; }

    ac_block_t b;
    if (ac_chain_get_block_by_height(r->cfg.chain, height, &b) < 0) {
        ac_chain_unlock(r->cfg.chain); send_rpc_error(fd, id, -32004, "block missing"); return;
    }
    uint64_t slot = b.header.slot;
    uint64_t ts   = b.header.timestamp_ms;
    if (idx >= b.tx_count) { ac_block_free(&b); ac_chain_unlock(r->cfg.chain); send_rpc_error(fd, id, -32004, "tx index out of range"); return; }
    const ac_tx_t *t = &b.txs[idx];

    char sender_hex[2 * AC_PUBKEY_SIZE + 1];
    ac_hex_encode(sender_hex, t->sender.b, AC_PUBKEY_SIZE);
    char *body_hex = (char *)malloc(t->body_len * 2 + 1);
    if (!body_hex) { ac_block_free(&b); ac_chain_unlock(r->cfg.chain); send_rpc_error(fd, id, -32603, "oom"); return; }
    ac_hex_encode(body_hex, t->body, t->body_len);
    char *memo_hex = (char *)malloc(t->memo_len * 2 + 1);
    if (!memo_hex) { free(body_hex); ac_block_free(&b); ac_chain_unlock(r->cfg.chain); send_rpc_error(fd, id, -32603, "oom"); return; }
    ac_hex_encode(memo_hex, t->memo, t->memo_len);

    size_t need = t->body_len * 2 + t->memo_len * 2 + 1024;
    char *out = (char *)malloc(need);
    if (out) {
        snprintf(out, need,
            "{\"hash\":\"%s\",\"height\":%" PRIu64 ",\"slot\":%" PRIu64 ",\"tx_index\":%u,"
            "\"timestamp_ms\":%" PRIu64 ",\"version\":%u,\"chain_id\":%" PRIu64 ",\"kind\":%u,"
            "\"sender\":\"%s\",\"nonce\":%" PRIu64 ",\"gas_limit\":%u,\"tip\":%" PRIu64 ","
            "\"valid_until\":%" PRIu64 ",\"body_hex\":\"%s\",\"memo_hex\":\"%s\"}",
            hex, height, slot, idx, ts, t->version, t->chain_id, t->kind,
            sender_hex, t->nonce, t->gas_limit, t->tip, t->valid_until,
            body_hex, memo_hex);
        send_rpc_result(fd, id, out);
        free(out);
    } else {
        send_rpc_error(fd, id, -32603, "oom");
    }
    free(body_hex);
    free(memo_hex);
    ac_block_free(&b);
    ac_chain_unlock(r->cfg.chain);
}

/* block_get_full — header + tx hashes for every transaction in the block. */
static void handle_block_get_full(ac_rpc_t *r, int fd, const char *id, const char *params) {
    uint64_t h = 0;
    if (!json_uint64(params, "height", &h)) {
        send_rpc_error(fd, id, -32602, "missing height"); return;
    }
    ac_chain_lock(r->cfg.chain);
    ac_block_t b;
    if (ac_chain_get_block_by_height(r->cfg.chain, h, &b) < 0) {
        ac_chain_unlock(r->cfg.chain); send_rpc_error(fd, id, -32004, "block not found"); return;
    }
    ac_hash_t bh;
    ac_block_hash(&bh, &b.header);
    char hex_bh[2 * AC_HASH_SIZE + 1]; ac_hex_encode(hex_bh, bh.b, AC_HASH_SIZE);
    char ph[2 * AC_HASH_SIZE + 1]; ac_hex_encode(ph, b.header.parent_hash.b, AC_HASH_SIZE);
    char sr[2 * AC_HASH_SIZE + 1]; ac_hex_encode(sr, b.header.state_root.b,  AC_HASH_SIZE);
    char tr[2 * AC_HASH_SIZE + 1]; ac_hex_encode(tr, b.header.tx_root.b,     AC_HASH_SIZE);
    char pp[2 * AC_PUBKEY_SIZE + 1]; ac_hex_encode(pp, b.header.proposer.b,  AC_PUBKEY_SIZE);

    size_t need = 1024 + b.tx_count * (2 * AC_HASH_SIZE + 4);
    char *out = (char *)malloc(need);
    if (!out) { ac_block_free(&b); ac_chain_unlock(r->cfg.chain); send_rpc_error(fd, id, -32603, "oom"); return; }
    int pos = snprintf(out, need,
        "{\"height\":%" PRIu64 ",\"hash\":\"%s\",\"slot\":%" PRIu64 ",\"timestamp_ms\":%" PRIu64
        ",\"parent_hash\":\"%s\",\"state_root\":\"%s\",\"tx_root\":\"%s\",\"proposer\":\"%s\","
        "\"base_fee\":%" PRIu64 ",\"gas_used\":%" PRIu64 ",\"gas_limit\":%" PRIu64
        ",\"tx_count\":%u,\"tx_hashes\":[",
        b.header.height, hex_bh, b.header.slot, b.header.timestamp_ms, ph, sr, tr, pp,
        b.header.base_fee, b.header.gas_used, b.header.gas_limit, b.header.tx_count);
    for (uint32_t i = 0; i < b.tx_count && pos > 0 && (size_t)pos + 80 < need; ++i) {
        ac_hash_t th; ac_tx_hash(&th, &b.txs[i]);
        char hh[2 * AC_HASH_SIZE + 1]; ac_hex_encode(hh, th.b, AC_HASH_SIZE);
        pos += snprintf(out + pos, need - pos, "%s\"%s\"", i ? "," : "", hh);
    }
    if (pos > 0 && (size_t)pos + 2 < need) {
        snprintf(out + pos, need - pos, "]}");
    }
    send_rpc_result(fd, id, out);
    free(out);
    ac_block_free(&b);
    ac_chain_unlock(r->cfg.chain);
}

/* validators_list — active validators sorted by stake descending. */
static void handle_validators_list(ac_rpc_t *r, int fd, const char *id, const char *params) {
    uint64_t limit_u = 0;
    json_uint64(params, "limit", &limit_u);
    if (limit_u == 0) limit_u = 100;
    if (limit_u > 1000) limit_u = 1000;
    size_t cap = (size_t)limit_u;

    ac_chain_lock(r->cfg.chain);
    uint64_t total_sqrt = ac_chain_total_sqrt_stake(r->cfg.chain);
    size_t total_active = ac_chain_active_count(r->cfg.chain);
    ac_account_t *arr = (ac_account_t *)malloc(cap * sizeof(*arr));
    if (!arr) { ac_chain_unlock(r->cfg.chain); send_rpc_error(fd, id, -32603, "oom"); return; }
    size_t n = ac_chain_top_validators(r->cfg.chain, arr, cap);
    ac_chain_unlock(r->cfg.chain);

    size_t need = 256 + n * 200;
    char *out = (char *)malloc(need);
    if (!out) { free(arr); send_rpc_error(fd, id, -32603, "oom"); return; }
    int pos = snprintf(out, need,
        "{\"total_active\":%zu,\"total_sqrt_stake\":%" PRIu64 ",\"validators\":[",
        total_active, total_sqrt);
    for (size_t i = 0; i < n && pos > 0 && (size_t)pos + 200 < need; ++i) {
        char ah[2 * AC_PUBKEY_SIZE + 1]; ac_hex_encode(ah, arr[i].addr.b, AC_PUBKEY_SIZE);
        pos += snprintf(out + pos, need - pos,
            "%s{\"address\":\"%s\",\"stake\":%" PRIu64 ",\"balance\":%" PRIu64 ",\"nonce\":%" PRIu64 "}",
            i ? "," : "", ah, arr[i].stake, arr[i].balance, arr[i].nonce);
    }
    if (pos > 0 && (size_t)pos + 2 < need) snprintf(out + pos, need - pos, "]}");
    send_rpc_result(fd, id, out);
    free(out);
    free(arr);
}

/* accounts_top — top accounts by balance descending. */
static void handle_accounts_top(ac_rpc_t *r, int fd, const char *id, const char *params) {
    uint64_t limit_u = 0;
    json_uint64(params, "limit", &limit_u);
    if (limit_u == 0) limit_u = 100;
    if (limit_u > 1000) limit_u = 1000;
    size_t cap = (size_t)limit_u;

    ac_chain_lock(r->cfg.chain);
    size_t total = ac_state_count(ac_chain_state(r->cfg.chain));
    ac_account_t *arr = (ac_account_t *)malloc(cap * sizeof(*arr));
    if (!arr) { ac_chain_unlock(r->cfg.chain); send_rpc_error(fd, id, -32603, "oom"); return; }
    size_t n = ac_chain_top_accounts_by_balance(r->cfg.chain, arr, cap);
    ac_chain_unlock(r->cfg.chain);

    size_t need = 256 + n * 220;
    char *out = (char *)malloc(need);
    if (!out) { free(arr); send_rpc_error(fd, id, -32603, "oom"); return; }
    int pos = snprintf(out, need,
        "{\"total_accounts\":%zu,\"accounts\":[", total);
    for (size_t i = 0; i < n && pos > 0 && (size_t)pos + 220 < need; ++i) {
        char ah[2 * AC_PUBKEY_SIZE + 1]; ac_hex_encode(ah, arr[i].addr.b, AC_PUBKEY_SIZE);
        pos += snprintf(out + pos, need - pos,
            "%s{\"address\":\"%s\",\"balance\":%" PRIu64 ",\"stake\":%" PRIu64 ",\"nonce\":%" PRIu64 "}",
            i ? "," : "", ah, arr[i].balance, arr[i].stake, arr[i].nonce);
    }
    if (pos > 0 && (size_t)pos + 2 < need) snprintf(out + pos, need - pos, "]}");
    send_rpc_result(fd, id, out);
    free(out);
    free(arr);
}

/* address_txs — recent transactions involving a given address, most-recent
 * first. Each entry includes the tx hash, the containing block height +
 * index, role (0=sender, 1=recipient), timestamp and a short summary so
 * the website explorer can render the history without a second round-trip.
 */
static void handle_address_txs(ac_rpc_t *r, int fd, const char *id, const char *params) {
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
    uint64_t limit_u = 0;
    json_uint64(params, "limit", &limit_u);
    if (limit_u == 0) limit_u = 100;
    if (limit_u > 1000) limit_u = 1000;
    size_t cap = (size_t)limit_u;

    ac_addr_tx_entry_t *entries = (ac_addr_tx_entry_t *)malloc(cap * sizeof(*entries));
    if (!entries) { send_rpc_error(fd, id, -32603, "oom"); return; }

    ac_chain_lock(r->cfg.chain);
    size_t n = ac_chain_addr_txs(r->cfg.chain, &a, entries, cap);

    size_t need = 256 + n * 320;
    char *out = (char *)malloc(need);
    if (!out) {
        ac_chain_unlock(r->cfg.chain);
        free(entries);
        send_rpc_error(fd, id, -32603, "oom");
        return;
    }
    int pos = snprintf(out, need, "{\"count\":%zu,\"txs\":[", n);

    for (size_t i = 0; i < n && pos > 0 && (size_t)pos + 320 < need; ++i) {
        ac_block_t b;
        if (ac_chain_get_block_by_height(r->cfg.chain, entries[i].height, &b) < 0) {
            continue;
        }
        if (entries[i].tx_idx >= b.tx_count) { ac_block_free(&b); continue; }
        const ac_tx_t *t = &b.txs[entries[i].tx_idx];
        ac_hash_t th; ac_tx_hash(&th, t);
        char hh[2 * AC_HASH_SIZE + 1]; ac_hex_encode(hh, th.b, AC_HASH_SIZE);
        char sh[2 * AC_PUBKEY_SIZE + 1]; ac_hex_encode(sh, t->sender.b, AC_PUBKEY_SIZE);
        pos += snprintf(out + pos, need - pos,
            "%s{\"hash\":\"%s\",\"height\":%" PRIu64 ",\"tx_index\":%u,\"role\":%u,"
            "\"timestamp_ms\":%" PRIu64 ",\"kind\":%u,\"sender\":\"%s\",\"nonce\":%" PRIu64 "}",
            i ? "," : "", hh, entries[i].height, entries[i].tx_idx,
            (unsigned)entries[i].role, b.header.timestamp_ms,
            (unsigned)t->kind, sh, t->nonce);
        ac_block_free(&b);
    }
    if (pos > 0 && (size_t)pos + 2 < need) snprintf(out + pos, need - pos, "]}");
    ac_chain_unlock(r->cfg.chain);

    send_rpc_result(fd, id, out);
    free(out);
    free(entries);
}

typedef struct {
    char  *out;
    size_t cap;
    size_t pos;
    int    count;
} peer_acc_t;

static int peer_acc_cb(const ac_addr_t *id, const char *host, uint16_t port,
                       bool inbound, void *ctx) {
    peer_acc_t *a = (peer_acc_t *)ctx;
    if (a->pos + 250 >= a->cap) return 1;
    char ah[2 * AC_PUBKEY_SIZE + 1];
    ac_hex_encode(ah, id->b, AC_PUBKEY_SIZE);
    a->pos += (size_t)snprintf(a->out + a->pos, a->cap - a->pos,
        "%s{\"pubkey\":\"%s\",\"host\":\"%s\",\"port\":%u,\"inbound\":%s}",
        a->count ? "," : "", ah, host, (unsigned)port, inbound ? "true" : "false");
    a->count++;
    return 0;
}

/* peers_list — connected peers seen by the local net module. */
static void handle_peers_list(ac_rpc_t *r, int fd, const char *id) {
    if (!r->cfg.net) { send_rpc_result(fd, id, "{\"connected\":0,\"node\":null,\"peers\":[]}"); return; }
    size_t cap = 8192;
    char *out = (char *)malloc(cap);
    if (!out) { send_rpc_error(fd, id, -32603, "oom"); return; }

    /* Report the responding node itself alongside its connected peers, so the
     * view reflects the whole network this node participates in (self + peers),
     * not just the links it happens to have dialled. `connected` stays the
     * count of peer links; `node` is this node; the network size is node + the
     * peers array. */
    ac_addr_t self_id;
    char self_host[128];
    uint16_t self_port = 0;
    ac_net_self_info(r->cfg.net, &self_id, self_host, sizeof(self_host), &self_port);
    char self_hex[2 * AC_PUBKEY_SIZE + 1];
    ac_hex_encode(self_hex, self_id.b, AC_PUBKEY_SIZE);

    size_t pos = (size_t)snprintf(out, cap,
        "{\"connected\":%zu,"
        "\"node\":{\"pubkey\":\"%s\",\"host\":\"%s\",\"port\":%u,\"self\":true},"
        "\"peers\":[",
        ac_net_peer_count(r->cfg.net), self_hex, self_host, (unsigned)self_port);
    peer_acc_t a = { .out = out, .cap = cap, .pos = pos, .count = 0 };
    ac_net_each_peer(r->cfg.net, peer_acc_cb, &a);
    if (a.pos + 2 < a.cap) snprintf(a.out + a.pos, a.cap - a.pos, "]}");
    send_rpc_result(fd, id, out);
    free(out);
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
    else if (strcmp(method, "block_get_full")  == 0) handle_block_get_full(r, fd, id_lit, params);
    else if (strcmp(method, "tx_get")          == 0) handle_tx_get      (r, fd, id_lit, params);
    else if (strcmp(method, "validators_list") == 0) handle_validators_list(r, fd, id_lit, params);
    else if (strcmp(method, "accounts_top")    == 0) handle_accounts_top(r, fd, id_lit, params);
    else if (strcmp(method, "peers_list")      == 0) handle_peers_list  (r, fd, id_lit);
    else if (strcmp(method, "address_txs")     == 0) handle_address_txs (r, fd, id_lit, params);
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
