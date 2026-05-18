/* AgentChain Engine — CLI entry point. */

#include "chain.h"
#include "codec.h"
#include "common.h"
#include "crypto.h"
#include "node.h"
#include "portable.h"
#include "version.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* -------------------------------------------------------------------------- */
/* Usage banner.                                                              */
/* -------------------------------------------------------------------------- */

static void print_usage(const char *argv0) {
    fprintf(stderr,
        "AgentChain Engine %s (%s)\n"
        "Usage: %s <command> [options]\n"
        "\n"
        "Commands:\n"
        "  node       Run a node (consensus + p2p + RPC)\n"
        "  keygen     Generate a new Ed25519 keypair to a file\n"
        "  pubkey     Print this node's public key\n"
        "  genesis    Write a genesis configuration file\n"
        "  send       Submit a transfer transaction via JSON-RPC\n"
        "  balance    Query an account balance via JSON-RPC\n"
        "  info       Print chain info via JSON-RPC\n"
        "  version    Print version and exit\n"
        "\n"
        "Use 'agentchain <command> --help' for command-specific options.\n",
        AGENTCHAIN_VERSION, AGENTCHAIN_GIT_COMMIT, argv0);
}

/* -------------------------------------------------------------------------- */
/* Tiny argv parser helpers.                                                  */
/* -------------------------------------------------------------------------- */

static const char *get_opt(int argc, char **argv, const char *name) {
    for (int i = 1; i < argc - 1; ++i) {
        if (strcmp(argv[i], name) == 0) return argv[i + 1];
    }
    return NULL;
}
static bool has_flag(int argc, char **argv, const char *name) {
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], name) == 0) return true;
    }
    return false;
}

/* -------------------------------------------------------------------------- */
/* `version`.                                                                 */
/* -------------------------------------------------------------------------- */

static int cmd_version(void) {
    printf("AgentChain Engine %s (%s) — protocol v%d\n",
           AGENTCHAIN_VERSION, AGENTCHAIN_GIT_COMMIT, AGENTCHAIN_PROTOCOL_VERSION);
    return 0;
}

/* -------------------------------------------------------------------------- */
/* `keygen`.                                                                  */
/* -------------------------------------------------------------------------- */

static int cmd_keygen(int argc, char **argv) {
    const char *out = get_opt(argc, argv, "--out");
    if (!out) { fprintf(stderr, "keygen: --out FILE required\n"); return 2; }
    if (ac_crypto_init() != 0) return 1;
    if (ac_file_exists(out)) {
        fprintf(stderr, "keygen: refusing to overwrite existing file %s\n", out);
        return 1;
    }
    ac_keypair_t kp;
    bool created = false;
    if (ac_node_keypair_load_or_create(out, &kp, &created) != 0) {
        fprintf(stderr, "keygen: failed to write %s\n", out);
        return 1;
    }
    char hex[2 * AC_PUBKEY_SIZE + 1];
    ac_hex_encode(hex, kp.pk, AC_PUBKEY_SIZE);
    printf("pubkey: %s\n", hex);
    printf("wrote:  %s (mode 0600)\n", out);
    return 0;
}

/* -------------------------------------------------------------------------- */
/* `pubkey`.                                                                  */
/* -------------------------------------------------------------------------- */

static int cmd_pubkey(int argc, char **argv) {
    const char *dd = get_opt(argc, argv, "--data-dir");
    const char *kp = get_opt(argc, argv, "--key");
    char path[1024];
    if (kp) {
        snprintf(path, sizeof(path), "%s", kp);
    } else if (dd) {
        ac_join_path(path, sizeof(path), dd, "node.key");
    } else {
        fprintf(stderr, "pubkey: --data-dir DIR or --key FILE required\n");
        return 2;
    }
    if (!ac_file_exists(path)) { fprintf(stderr, "pubkey: %s not found\n", path); return 1; }
    if (ac_crypto_init() != 0) return 1;
    ac_keypair_t k;
    if (ac_node_keypair_load_or_create(path, &k, NULL) != 0) {
        fprintf(stderr, "pubkey: failed to read %s\n", path);
        return 1;
    }
    char hex[2 * AC_PUBKEY_SIZE + 1];
    ac_hex_encode(hex, k.pk, AC_PUBKEY_SIZE);
    puts(hex);
    return 0;
}

/* -------------------------------------------------------------------------- */
/* `genesis`.                                                                 */
/* -------------------------------------------------------------------------- */
/* Usage:
 *   agentchain genesis --chain-id N [--timestamp-ms N] --out FILE
 *                       --account hex:bal:stake [--account ...]
 */
static int cmd_genesis(int argc, char **argv) {
    const char *cid_s = get_opt(argc, argv, "--chain-id");
    const char *ts_s  = get_opt(argc, argv, "--timestamp-ms");
    const char *out   = get_opt(argc, argv, "--out");
    if (!cid_s || !out) {
        fprintf(stderr, "genesis: --chain-id N --out FILE required (+ --account specs)\n");
        return 2;
    }
    uint64_t cid = strtoull(cid_s, NULL, 10);
    uint64_t ts  = ts_s ? strtoull(ts_s, NULL, 10) : ac_now_ms();

    FILE *f = fopen(out, "w");
    if (!f) { fprintf(stderr, "genesis: cannot write %s: %s\n", out, strerror(errno)); return 1; }
    fprintf(f, "# AgentChain genesis configuration\n");
    fprintf(f, "chain_id     = %" PRIu64 "\n", cid);
    fprintf(f, "timestamp_ms = %" PRIu64 "\n\n", ts);

    int count = 0;
    for (int i = 1; i < argc - 1; ++i) {
        if (strcmp(argv[i], "--account") != 0) continue;
        const char *spec = argv[i + 1];
        char hex[128] = {0};
        uint64_t bal = 0, stake = 0;
        if (sscanf(spec, "%127[^:]:%" SCNu64 ":%" SCNu64, hex, &bal, &stake) != 3) {
            fprintf(stderr, "genesis: bad --account spec '%s' (expected HEX:BAL:STAKE)\n", spec);
            fclose(f); return 2;
        }
        fprintf(f, "account %s %" PRIu64 " %" PRIu64 "\n", hex, bal, stake);
        count++;
    }
    fclose(f);
    fprintf(stderr, "genesis: wrote %s (%d accounts, chain_id=%" PRIu64 ")\n", out, count, cid);
    return 0;
}

/* -------------------------------------------------------------------------- */
/* HTTP client for `send`/`balance`/`info`.                                   */
/* -------------------------------------------------------------------------- */

static int http_post_json(const char *host, uint16_t port,
                          const char *body, char *resp, size_t resp_cap) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port   = htons(port);

    /* Resolve host. */
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    char pstr[8]; snprintf(pstr, sizeof(pstr), "%u", port);
    if (getaddrinfo(host, pstr, &hints, &res) != 0) { ac_sock_close(fd); return -1; }
    int rc = connect(fd, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    if (rc < 0) { ac_sock_close(fd); return -1; }

    char header[256];
    int hn = snprintf(header, sizeof(header),
        "POST / HTTP/1.1\r\nHost: %s\r\nContent-Type: application/json\r\n"
        "Content-Length: %zu\r\nConnection: close\r\n\r\n",
        host, strlen(body));
    if (ac_sock_send(fd, header, (size_t)hn) < 0) { ac_sock_close(fd); return -1; }
    if (ac_sock_send(fd, body, strlen(body)) < 0) { ac_sock_close(fd); return -1; }

    /* Read response. */
    size_t total = 0;
    while (total + 1 < resp_cap) {
        ssize_t n = ac_sock_recv(fd, resp + total, resp_cap - 1 - total);
        if (n <= 0) break;
        total += (size_t)n;
    }
    resp[total] = '\0';
    ac_sock_close(fd);
    return (int)total;
}

/* Returns pointer to start of HTTP body, or NULL. */
static const char *http_body(const char *resp) {
    const char *p = strstr(resp, "\r\n\r\n");
    return p ? p + 4 : NULL;
}

/* Parse a numeric JSON field. */
static int json_get_uint64(const char *body, const char *key, uint64_t *out) {
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(body, pattern);
    if (!p) return -1;
    p += strlen(pattern);
    while (*p == ' ' || *p == '\t' || *p == ':') p++;
    if (!isdigit((unsigned char)*p)) return -1;
    *out = strtoull(p, NULL, 10);
    return 0;
}

/* Reserved for future RPC commands that need to parse string fields. */

/* Parse host:port. */
static int split_host_port(const char *rpc, char *host, size_t host_cap, uint16_t *port) {
    const char *colon = strrchr(rpc, ':');
    if (!colon) return -1;
    size_t hl = (size_t)(colon - rpc);
    if (hl >= host_cap) return -1;
    memcpy(host, rpc, hl); host[hl] = '\0';
    int p = atoi(colon + 1);
    if (p <= 0 || p > 65535) return -1;
    *port = (uint16_t)p;
    return 0;
}

/* -------------------------------------------------------------------------- */
/* `info`.                                                                    */
/* -------------------------------------------------------------------------- */

static int cmd_info(int argc, char **argv) {
    const char *rpc = get_opt(argc, argv, "--rpc");
    if (!rpc) rpc = "127.0.0.1:30304";
    char host[64]; uint16_t port;
    if (split_host_port(rpc, host, sizeof(host), &port) < 0) {
        fprintf(stderr, "info: bad --rpc URL\n"); return 2;
    }
    char resp[8192];
    if (http_post_json(host, port,
            "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"chain_info\"}",
            resp, sizeof(resp)) < 0) {
        fprintf(stderr, "info: RPC connect failed\n"); return 1;
    }
    const char *body = http_body(resp);
    if (!body) { fprintf(stderr, "info: bad response\n"); return 1; }
    puts(body);
    return 0;
}

/* -------------------------------------------------------------------------- */
/* `balance`.                                                                 */
/* -------------------------------------------------------------------------- */

static int cmd_balance(int argc, char **argv) {
    const char *rpc  = get_opt(argc, argv, "--rpc");
    const char *addr = get_opt(argc, argv, "--address");
    if (!addr) { fprintf(stderr, "balance: --address HEX required\n"); return 2; }
    if (!rpc) rpc = "127.0.0.1:30304";
    char host[64]; uint16_t port;
    if (split_host_port(rpc, host, sizeof(host), &port) < 0) return 2;
    char req[256];
    snprintf(req, sizeof(req),
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"account_get\",\"params\":{\"address\":\"%s\"}}",
        addr);
    char resp[4096];
    if (http_post_json(host, port, req, resp, sizeof(resp)) < 0) {
        fprintf(stderr, "balance: RPC connect failed\n"); return 1;
    }
    const char *body = http_body(resp);
    if (!body) { fprintf(stderr, "balance: bad response\n"); return 1; }
    puts(body);
    return 0;
}

/* -------------------------------------------------------------------------- */
/* `send`.                                                                    */
/* -------------------------------------------------------------------------- */

static int cmd_send(int argc, char **argv) {
    const char *rpc       = get_opt(argc, argv, "--rpc");
    const char *key_file  = get_opt(argc, argv, "--from-key");
    const char *to_hex    = get_opt(argc, argv, "--to");
    const char *amount_s  = get_opt(argc, argv, "--amount");
    const char *tip_s     = get_opt(argc, argv, "--tip");
    const char *memo      = get_opt(argc, argv, "--memo");
    const char *valid_s   = get_opt(argc, argv, "--valid-slots");

    if (!key_file || !to_hex || !amount_s) {
        fprintf(stderr, "send: --from-key FILE --to HEX --amount UCRD required\n");
        return 2;
    }
    if (!rpc) rpc = "127.0.0.1:30304";
    char host[64]; uint16_t port;
    if (split_host_port(rpc, host, sizeof(host), &port) < 0) return 2;

    if (ac_crypto_init() != 0) return 1;

    /* Load keypair. */
    ac_keypair_t kp;
    if (ac_node_keypair_load_or_create(key_file, &kp, NULL) != 0) {
        fprintf(stderr, "send: cannot load %s\n", key_file); return 1;
    }

    /* Get chain_info for chain_id and slot. */
    char resp[8192];
    if (http_post_json(host, port,
            "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"chain_info\"}",
            resp, sizeof(resp)) < 0) {
        fprintf(stderr, "send: chain_info RPC failed\n"); return 1;
    }
    const char *body = http_body(resp);
    if (!body) return 1;
    uint64_t chain_id = 0, height = 0, genesis_ts = 0;
    json_get_uint64(body, "chain_id",              &chain_id);
    json_get_uint64(body, "height",                &height);
    json_get_uint64(body, "genesis_timestamp_ms",  &genesis_ts);

    /* Get nonce. */
    char sender_hex[2 * AC_PUBKEY_SIZE + 1];
    ac_hex_encode(sender_hex, kp.pk, AC_PUBKEY_SIZE);
    char req[512];
    snprintf(req, sizeof(req),
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"account_get\",\"params\":{\"address\":\"%s\"}}",
        sender_hex);
    if (http_post_json(host, port, req, resp, sizeof(resp)) < 0) {
        fprintf(stderr, "send: account_get failed\n"); return 1;
    }
    body = http_body(resp);
    if (!body) return 1;
    uint64_t nonce = 0;
    json_get_uint64(body, "nonce", &nonce);

    /* Build tx. */
    ac_tx_t tx;
    memset(&tx, 0, sizeof(tx));
    tx.version    = AC_TX_VERSION;
    tx.chain_id   = chain_id;
    tx.kind       = AC_TX_TRANSFER;
    memcpy(tx.sender.b, kp.pk, AC_PUBKEY_SIZE);
    tx.nonce      = nonce;
    tx.gas_limit  = 1000;
    tx.tip        = tip_s ? strtoull(tip_s, NULL, 10) : 0;
    uint64_t current_slot = (ac_now_ms() - genesis_ts) / AC_SLOT_DURATION_MS;
    uint64_t valid_window = valid_s ? strtoull(valid_s, NULL, 10) : 600; /* ~20 min */
    tx.valid_until = current_slot + valid_window;

    /* body: recipient (32) + amount (8). */
    ac_body_transfer_t bt;
    if (ac_hex_decode(bt.recipient.b, AC_PUBKEY_SIZE, to_hex) != 0) {
        fprintf(stderr, "send: bad recipient hex\n"); return 2;
    }
    bt.amount = strtoull(amount_s, NULL, 10);
    int bn = ac_body_transfer_encode(tx.body, AC_TX_BODY_MAX, &bt);
    if (bn < 0) return 1;
    tx.body_len = (uint32_t)bn;

    if (memo) {
        size_t ml = strlen(memo);
        if (ml > AC_MEMO_MAX) ml = AC_MEMO_MAX;
        memcpy(tx.memo, memo, ml);
        tx.memo_len = (uint32_t)ml;
    }

    if (ac_tx_sign(&tx, &kp) != 0) { fprintf(stderr, "send: signing failed\n"); return 1; }

    uint8_t bin[AC_TX_MAX_BYTES];
    int n = ac_tx_encode(bin, sizeof(bin), &tx);
    if (n < 0) { fprintf(stderr, "send: tx encode failed\n"); return 1; }
    char tx_hex[2 * AC_TX_MAX_BYTES + 1];
    ac_hex_encode(tx_hex, bin, (size_t)n);

    char *req2 = (char *)malloc(strlen(tx_hex) + 256);
    if (!req2) return 1;
    snprintf(req2, strlen(tx_hex) + 256,
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tx_submit\",\"params\":{\"tx_hex\":\"%s\"}}",
        tx_hex);
    int rc = http_post_json(host, port, req2, resp, sizeof(resp));
    free(req2);
    if (rc < 0) { fprintf(stderr, "send: tx_submit failed\n"); return 1; }
    body = http_body(resp);
    if (!body) return 1;
    (void)height;
    puts(body);
    return 0;
}

/* -------------------------------------------------------------------------- */
/* `node`.                                                                    */
/* -------------------------------------------------------------------------- */

static int cmd_node(int argc, char **argv) {
    ac_node_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));

    const char *dd  = get_opt(argc, argv, "--data-dir");
    const char *gen = get_opt(argc, argv, "--genesis");
    const char *port_s    = get_opt(argc, argv, "--port");
    const char *rpc_port_s = get_opt(argc, argv, "--rpc-port");
    const char *seeds_s = get_opt(argc, argv, "--seeds");
    const char *host = get_opt(argc, argv, "--host");
    const char *rpc_host = get_opt(argc, argv, "--rpc-host");
    const char *ext = get_opt(argc, argv, "--external-host");
    bool validator  = has_flag(argc, argv, "--validator");
    bool verbose    = has_flag(argc, argv, "-v");

    if (!dd) {
        fprintf(stderr, "node: --data-dir DIR required\n");
        return 2;
    }

    ac_log_init(verbose ? AC_LOG_DEBUG : AC_LOG_INFO);

    snprintf(cfg.data_dir,     sizeof(cfg.data_dir),     "%s", dd);
    if (gen) snprintf(cfg.genesis_path, sizeof(cfg.genesis_path), "%s", gen);
    snprintf(cfg.listen_host,  sizeof(cfg.listen_host),  "%s", host ? host : "0.0.0.0");
    cfg.listen_port = (uint16_t)(port_s     ? atoi(port_s)     : 30303);
    snprintf(cfg.rpc_host,     sizeof(cfg.rpc_host),     "%s", rpc_host ? rpc_host : "127.0.0.1");
    cfg.rpc_port    = (uint16_t)(rpc_port_s ? atoi(rpc_port_s) : 30304);
    if (ext) snprintf(cfg.external_host, sizeof(cfg.external_host), "%s", ext);
    cfg.validator = validator;

    /* Seeds (comma-separated). */
    if (seeds_s && *seeds_s) {
        char *copy = strdup(seeds_s);
        size_t cap = 8;
        cfg.seed_peers = (char **)calloc(cap, sizeof(char *));
        char *tok = strtok(copy, ",");
        while (tok) {
            if (cfg.seed_n + 1 > cap) {
                cap *= 2;
                cfg.seed_peers = (char **)realloc(cfg.seed_peers, cap * sizeof(char *));
            }
            while (*tok == ' ') tok++;
            cfg.seed_peers[cfg.seed_n++] = strdup(tok);
            tok = strtok(NULL, ",");
        }
        free(copy);
    }

    ac_node_t *n = ac_node_new(&cfg);
    if (!n) return 1;
    if (ac_node_start(n) != 0) {
        ac_node_free(n);
        return 1;
    }
    ac_node_wait_for_signal(n);
    fprintf(stderr, "shutting down…\n");
    ac_node_stop(n);
    ac_node_free(n);

    if (cfg.seed_peers) {
        for (size_t i = 0; i < cfg.seed_n; ++i) free(cfg.seed_peers[i]);
        free(cfg.seed_peers);
    }
    return 0;
}

/* -------------------------------------------------------------------------- */
/* main.                                                                      */
/* -------------------------------------------------------------------------- */

int main(int argc, char **argv) {
    if (argc < 2) { print_usage(argv[0]); return 2; }
    const char *cmd = argv[1];

    /* Winsock needs to be initialised before any socket call; on POSIX this
     * is a no-op. `node` re-initialises but that is safe. */
    ac_net_init();

    int rc = 2;
    if      (strcmp(cmd, "version") == 0 || strcmp(cmd, "--version") == 0) rc = cmd_version();
    else if (strcmp(cmd, "node") == 0)    rc = cmd_node(argc - 1, argv + 1);
    else if (strcmp(cmd, "keygen") == 0)  rc = cmd_keygen(argc - 1, argv + 1);
    else if (strcmp(cmd, "pubkey") == 0)  rc = cmd_pubkey(argc - 1, argv + 1);
    else if (strcmp(cmd, "genesis") == 0) rc = cmd_genesis(argc - 1, argv + 1);
    else if (strcmp(cmd, "send") == 0)    rc = cmd_send(argc - 1, argv + 1);
    else if (strcmp(cmd, "balance") == 0) rc = cmd_balance(argc - 1, argv + 1);
    else if (strcmp(cmd, "info") == 0)    rc = cmd_info(argc - 1, argv + 1);
    else if (strcmp(cmd, "--help") == 0 || strcmp(cmd, "-h") == 0) {
        print_usage(argv[0]); rc = 0;
    } else {
        fprintf(stderr, "Unknown command: %s\n", cmd);
        print_usage(argv[0]);
    }
    ac_net_cleanup();
    return rc;
}
