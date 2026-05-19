#include "node.h"
#include "portable.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct ac_node_s {
    ac_node_config_t cfg;
    ac_keypair_t     kp;
    ac_chain_t      *chain;
    ac_mempool_t    *mempool;
    ac_net_t        *net;
    ac_consensus_t  *cs;
    ac_rpc_t        *rpc;
};

/* -------------------------------------------------------------------------- */
/* Signals.                                                                   */
/* -------------------------------------------------------------------------- */

static volatile int SHUTDOWN = 0;
static void on_signal(int sig) { (void)sig; SHUTDOWN = 1; }

void ac_node_wait_for_signal(ac_node_t *n) {
    (void)n;
#ifdef _WIN32
    /* Windows: install a handler for SIGINT/SIGTERM via the C runtime. */
    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);
#else
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
#endif
    while (!SHUTDOWN) ac_sleep_ms(200);
}

/* -------------------------------------------------------------------------- */
/* Key file.                                                                  */
/* -------------------------------------------------------------------------- */

#define KEY_MAGIC   "AGCH:KEY:v1"
#define KEY_VERSION 1

int ac_node_keypair_load_or_create(const char *path, ac_keypair_t *out, bool *created) {
    if (created) *created = false;

    if (ac_file_exists(path)) {
        size_t len = 0;
        uint8_t *buf = ac_file_read_all(path, &len);
        if (!buf) return -1;
        if (len < sizeof(KEY_MAGIC) - 1 + 1 + AC_PUBKEY_SIZE + AC_SEED_SIZE) { free(buf); return -1; }
        if (memcmp(buf, KEY_MAGIC, sizeof(KEY_MAGIC) - 1) != 0) { free(buf); return -1; }
        size_t pos = sizeof(KEY_MAGIC) - 1;
        if (buf[pos++] != KEY_VERSION) { free(buf); return -1; }
        uint8_t pk[AC_PUBKEY_SIZE];
        uint8_t seed[AC_SEED_SIZE];
        memcpy(pk,   buf + pos, AC_PUBKEY_SIZE); pos += AC_PUBKEY_SIZE;
        memcpy(seed, buf + pos, AC_SEED_SIZE);   pos += AC_SEED_SIZE;
        ac_secure_zero(buf, len);
        free(buf);
        if (ac_keypair_from_seed(out, seed) != 0) return -1;
        if (memcmp(out->pk, pk, AC_PUBKEY_SIZE) != 0) return -1;
        ac_secure_zero(seed, sizeof(seed));
        return 0;
    }

    if (ac_keypair_random(out) != 0) return -1;

    uint8_t buf[sizeof(KEY_MAGIC) - 1 + 1 + AC_PUBKEY_SIZE + AC_SEED_SIZE];
    size_t pos = 0;
    memcpy(buf + pos, KEY_MAGIC, sizeof(KEY_MAGIC) - 1); pos += sizeof(KEY_MAGIC) - 1;
    buf[pos++] = KEY_VERSION;
    memcpy(buf + pos, out->pk, AC_PUBKEY_SIZE); pos += AC_PUBKEY_SIZE;
    uint8_t seed[AC_SEED_SIZE];
    ac_keypair_seed(seed, out);
    memcpy(buf + pos, seed, AC_SEED_SIZE);

    /* Atomic write with 0600 mode. */
    if (ac_file_write_atomic(path, buf, sizeof(buf), 0600) < 0) {
        ac_secure_zero(seed, sizeof(seed));
        ac_secure_zero(buf, sizeof(buf));
        return -1;
    }
    ac_secure_zero(seed, sizeof(seed));
    ac_secure_zero(buf, sizeof(buf));
    if (created) *created = true;
    return 0;
}

/* -------------------------------------------------------------------------- */
/* Genesis file (simple line-based format).                                   */
/* -------------------------------------------------------------------------- */
/*
 *   # comments allowed
 *   chain_id = 1
 *   timestamp_ms = 1716000000000
 *
 *   account <hex32>  <balance>  <stake>
 *   account <hex32>  <balance>  <stake>
 *   name <name> <hex32>
 */

static char *trim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    char *end = s + strlen(s);
    while (end > s && (end[-1] == '\n' || end[-1] == '\r' ||
                        end[-1] == ' '  || end[-1] == '\t')) end--;
    *end = '\0';
    return s;
}

int ac_node_load_genesis(const char *path, ac_genesis_t *out) {
    memset(out, 0, sizeof(*out));
    FILE *f = fopen(path, "r");
    if (!f) return -1;

    size_t cap = 16;
    out->accounts = (ac_genesis_account_t *)calloc(cap, sizeof(*out->accounts));
    if (!out->accounts) { fclose(f); return -1; }

    char line[512];
    int line_no = 0;
    while (fgets(line, sizeof(line), f)) {
        line_no++;
        char *s = trim(line);
        if (!*s || *s == '#') continue;

        if (strncmp(s, "chain_id", 8) == 0) {
            const char *eq = strchr(s, '=');
            if (!eq) goto fail;
            out->chain_id = strtoull(eq + 1, NULL, 10);
        } else if (strncmp(s, "timestamp_ms", 12) == 0) {
            const char *eq = strchr(s, '=');
            if (!eq) goto fail;
            out->timestamp_ms = strtoull(eq + 1, NULL, 10);
        } else if (strncmp(s, "account", 7) == 0) {
            char hex[2 * AC_PUBKEY_SIZE + 1];
            uint64_t bal = 0, stk = 0;
            if (sscanf(s, "account %64s %" SCNu64 " %" SCNu64, hex, &bal, &stk) != 3) goto fail;
            if (out->accounts_n + 1 > cap) {
                cap *= 2;
                ac_genesis_account_t *p = (ac_genesis_account_t *)realloc(
                    out->accounts, cap * sizeof(*out->accounts));
                if (!p) goto fail;
                out->accounts = p;
            }
            ac_genesis_account_t *a = &out->accounts[out->accounts_n++];
            if (ac_hex_decode(a->addr.b, AC_PUBKEY_SIZE, hex) != 0) goto fail;
            a->balance = bal;
            a->stake   = stk;
        } else if (strncmp(s, "name", 4) == 0) {
            /* Ignored in v1 reference; names are registered via tx. */
        } else {
            goto fail;
        }
    }
    fclose(f);
    if (out->chain_id == 0 || out->timestamp_ms == 0) { ac_node_free_genesis(out); return -1; }
    return 0;

fail:
    fclose(f);
    LOG_E("node", "genesis parse failed at line %d", line_no);
    ac_node_free_genesis(out);
    return -1;
}

void ac_node_free_genesis(ac_genesis_t *g) {
    if (!g) return;
    if (g->accounts) { free(g->accounts); g->accounts = NULL; }
    g->accounts_n = 0;
}

/* -------------------------------------------------------------------------- */
/* Plumbing: net <-> consensus / mempool / chain.                             */
/* -------------------------------------------------------------------------- */

static void on_block_ann_cb(const uint8_t *payload, size_t len, void *ctx) {
    ac_node_t *n = (ac_node_t *)ctx;
    ac_consensus_handle_block(n->cs, payload, len);
}

static void on_commit_vote_cb(const uint8_t *payload, size_t len, void *ctx) {
    ac_node_t *n = (ac_node_t *)ctx;
    ac_consensus_handle_vote(n->cs, payload, len);
}

static void on_tx_ann_cb(const uint8_t *payload, size_t len, void *ctx) {
    ac_node_t *n = (ac_node_t *)ctx;
    ac_tx_t tx;
    if (ac_tx_decode(&tx, payload, len) < 0) return;
    ac_chain_lock(n->chain);
    uint64_t slot = ac_chain_current_slot(n->chain);
    ac_mempool_add(n->mempool, ac_chain_state(n->chain), &tx, slot);
    ac_chain_unlock(n->chain);
}

static void on_headers_req_cb(uint64_t from_height, uint32_t count,
                              const ac_addr_t *peer_id, void *ctx) {
    ac_node_t *n = (ac_node_t *)ctx;
    if (count == 0) return;
    if (count > 256) count = 256;
    ac_chain_lock(n->chain);
    uint64_t tip = ac_chain_height(n->chain);
    ac_chain_unlock(n->chain);
    if (from_height > tip) return;
    if (from_height + count > tip + 1) count = (uint32_t)(tip + 1 - from_height);

    for (uint64_t i = 0; i < count; ++i) {
        ac_block_t b;
        ac_chain_lock(n->chain);
        int rc = ac_chain_get_block_by_height(n->chain, from_height + i, &b);
        ac_chain_unlock(n->chain);
        if (rc < 0) continue;
        uint8_t *enc = NULL; size_t enc_len = 0;
        if (ac_block_encode(&enc, &enc_len, &b) >= 0) {
            ac_net_send_to(n->net, peer_id, AC_MSG_BLOCK_RES, enc, enc_len);
            free(enc);
        }
        ac_block_free(&b);
    }
}

static void on_block_req_cb(uint64_t height, const ac_addr_t *peer_id, void *ctx) {
    on_headers_req_cb(height, 1, peer_id, ctx);
}

/* Broadcast callback for consensus → net. */
static void consensus_broadcast_cb(uint8_t type, const uint8_t *payload, size_t len, void *ctx) {
    ac_node_t *n = (ac_node_t *)ctx;
    ac_net_broadcast(n->net, type, payload, len, NULL);
}

/* Broadcast callback for rpc.tx_submit → net (gossip an RPC-submitted tx). */
static void rpc_broadcast_tx_cb(const uint8_t *tx_bytes, size_t tx_len, void *ctx) {
    ac_node_t *n = (ac_node_t *)ctx;
    ac_net_broadcast(n->net, AC_MSG_TX_ANN, tx_bytes, tx_len, NULL);
}

/* -------------------------------------------------------------------------- */
/* Lifecycle.                                                                 */
/* -------------------------------------------------------------------------- */

ac_node_t *ac_node_new(const ac_node_config_t *cfg) {
    ac_node_t *n = (ac_node_t *)calloc(1, sizeof(*n));
    if (!n) return NULL;
    n->cfg = *cfg;
    return n;
}

void ac_node_free(ac_node_t *n) {
    if (!n) return;
    free(n);
}

int ac_node_start(ac_node_t *n) {
    /* Init crypto. */
    if (ac_crypto_init() != 0) {
        LOG_E("node", "crypto init failed");
        return -1;
    }
    /* Init networking (Winsock on Windows; no-op on POSIX). */
    if (ac_net_init() != 0) {
        LOG_E("node", "network init failed");
        return -1;
    }

    if (ac_mkdir_p(n->cfg.data_dir) != 0) {
        LOG_E("node", "mkdir %s failed", n->cfg.data_dir);
        return -1;
    }

    /* Keypair. */
    char keypath[1024];
    ac_join_path(keypath, sizeof(keypath), n->cfg.data_dir, "node.key");
    bool created = false;
    if (ac_node_keypair_load_or_create(keypath, &n->kp, &created) != 0) {
        LOG_E("node", "key load/create failed");
        return -1;
    }
    char hex[2 * AC_PUBKEY_SIZE + 1];
    ac_hex_encode(hex, n->kp.pk, AC_PUBKEY_SIZE);
    LOG_I("node", "%s key: %s", created ? "generated new" : "loaded", hex);

    /* Genesis (only used if no chain yet). */
    ac_genesis_t gen;
    memset(&gen, 0, sizeof(gen));
    bool have_gen = false;
    if (n->cfg.genesis_path[0] && ac_file_exists(n->cfg.genesis_path)) {
        if (ac_node_load_genesis(n->cfg.genesis_path, &gen) == 0) {
            have_gen = true;
            LOG_I("node", "loaded genesis: chain_id=%" PRIu64 " accounts=%zu",
                  gen.chain_id, gen.accounts_n);
        }
    }

    n->chain = ac_chain_open(n->cfg.data_dir, have_gen ? &gen : NULL);
    if (have_gen) ac_node_free_genesis(&gen);
    if (!n->chain) {
        LOG_E("node", "chain open failed");
        return -1;
    }

    /* Mempool. */
    n->mempool = ac_mempool_new(ac_chain_chain_id(n->chain));
    if (!n->mempool) { ac_chain_close(n->chain); return -1; }

    /* Consensus. */
    ac_consensus_config_t ccfg = {
        .chain   = n->chain,
        .mempool = n->mempool,
        .keypair = n->kp,
        .broadcast = consensus_broadcast_cb,
        .broadcast_ctx = n,
        .validator = n->cfg.validator,
    };
    n->cs = ac_consensus_new(&ccfg);
    if (!n->cs) goto fail;

    /* Net. */
    ac_net_config_t ncfg;
    memset(&ncfg, 0, sizeof(ncfg));
    ncfg.listen_port = n->cfg.listen_port;
    snprintf(ncfg.listen_host, sizeof(ncfg.listen_host), "%s",
             n->cfg.listen_host[0] ? n->cfg.listen_host : "0.0.0.0");
    snprintf(ncfg.external_host, sizeof(ncfg.external_host), "%s", n->cfg.external_host);
    ncfg.chain_id = ac_chain_chain_id(n->chain);
    ncfg.keypair  = n->kp;
    ncfg.seed_peers = (const char **)n->cfg.seed_peers;
    ncfg.seed_n     = n->cfg.seed_n;
    ncfg.target_outbound = 8;
    ncfg.cb.ctx = n;
    ncfg.cb.on_block_ann   = on_block_ann_cb;
    ncfg.cb.on_tx_ann      = on_tx_ann_cb;
    ncfg.cb.on_commit_vote = on_commit_vote_cb;
    ncfg.cb.on_headers_req = on_headers_req_cb;
    ncfg.cb.on_block_req   = on_block_req_cb;
    n->net = ac_net_new(&ncfg);
    if (!n->net) goto fail;
    if (ac_net_start(n->net) != 0) goto fail;

    /* RPC. */
    ac_rpc_config_t rcfg;
    memset(&rcfg, 0, sizeof(rcfg));
    rcfg.port = n->cfg.rpc_port;
    snprintf(rcfg.host, sizeof(rcfg.host), "%s",
             n->cfg.rpc_host[0] ? n->cfg.rpc_host : "127.0.0.1");
    rcfg.chain = n->chain;
    rcfg.mempool = n->mempool;
    rcfg.broadcast_tx = rpc_broadcast_tx_cb;
    rcfg.broadcast_tx_ctx = n;
    n->rpc = ac_rpc_new(&rcfg);
    if (!n->rpc) goto fail;
    if (ac_rpc_start(n->rpc) != 0) goto fail;

    /* Consensus. */
    if (ac_consensus_start(n->cs) != 0) goto fail;

    LOG_I("node", "AgentChain node started — height %" PRIu64 " base_fee %" PRIu64 "/gas",
          ac_chain_height(n->chain), ac_chain_base_fee(n->chain));
    return 0;

fail:
    LOG_E("node", "startup failed");
    ac_node_stop(n);
    return -1;
}

void ac_node_stop(ac_node_t *n) {
    /* Stop accepting external requests first. */
    if (n->rpc)     { ac_rpc_stop(n->rpc);      ac_rpc_free(n->rpc);      n->rpc = NULL; }
    /* Stop the network; this joins every peer reader thread, so no more
     * callbacks will fire into consensus/mempool after this returns. */
    if (n->net)     { ac_net_stop(n->net);      ac_net_free(n->net);      n->net = NULL; }
    /* Stop the slot timer. */
    if (n->cs)      { ac_consensus_stop(n->cs); ac_consensus_free(n->cs); n->cs = NULL; }
    /* Now safe to free state-holding modules. */
    if (n->mempool) { ac_mempool_free(n->mempool);                         n->mempool = NULL; }
    if (n->chain)   { ac_chain_close(n->chain);                            n->chain = NULL; }
    ac_net_cleanup();
}
