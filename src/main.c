/* AgentChain Engine — CLI entry point.
 *
 * Defaults aim to make the common path one-liner-short:
 *   - No --rpc           → talk to the mainnet alpha public endpoint over HTTPS.
 *   - No --from-key / --key → use ~/.agentchain/node.key.
 *   - No --data-dir      → use ~/.agentchain.
 *   - No --genesis       → use the mainnet alpha genesis embedded in this binary.
 *   - No --seeds         → use the mainnet alpha bootstrap seeds.
 *
 * Every default is overridable via the obvious flag. Operators running
 * testnets or air-gapped clusters keep the same command surface.
 */

#include "chain.h"
#include "codec.h"
#include "common.h"
#include "crypto.h"
#include "mainnet.h"
#include "node.h"
#include "portable.h"
#include "version.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifndef _WIN32
#  include <sys/wait.h>
#  include <unistd.h>
#endif

/* -------------------------------------------------------------------------- */
/* argv helpers.                                                              */
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
/* Default paths under $HOME/.agentchain.                                     */
/* -------------------------------------------------------------------------- */

static const char *home_dir(void) {
    const char *h = getenv("HOME");
#ifdef _WIN32
    if (!h || !*h) h = getenv("USERPROFILE");
#endif
    return (h && *h) ? h : ".";
}

/* Fill `out` with $HOME/.agentchain (or just .agentchain if HOME is unset). */
static void default_data_dir(char *out, size_t cap) {
    snprintf(out, cap, "%s/.agentchain", home_dir());
}
static void default_key_path(char *out, size_t cap) {
    snprintf(out, cap, "%s/.agentchain/node.key", home_dir());
}

/* -------------------------------------------------------------------------- */
/* Usage banner.                                                              */
/* -------------------------------------------------------------------------- */

static void print_usage(const char *argv0) {
    fprintf(stderr,
        "AgentChain Engine %s (%s)\n"
        "Usage: %s <command> [options]\n"
        "\n"
        "Wallet & queries\n"
        "  keygen     Create a new keypair (defaults to ~/.agentchain/node.key)\n"
        "  pubkey     Print your public address (hex)\n"
        "  balance    Look up an account on the chain\n"
        "  info       Print current chain state\n"
        "\n"
        "Transactions (signed locally, broadcast via JSON-RPC)\n"
        "  send       Transfer CRD to another address\n"
        "  bond       Bond CRD into stake to become / top-up a validator\n"
        "  unbond     Release stake back to balance\n"
        "\n"
        "Operating a node\n"
        "  node       Join the network (sync). Add --validator to propose blocks.\n"
        "  genesis    Write a genesis file (advanced; for new testnets)\n"
        "\n"
        "  version    Print version and exit\n"
        "  help       Show this banner; 'agentchain <cmd> --help' for details\n"
        "\n"
        "Defaults: RPC = %s   key = ~/.agentchain/node.key   data = ~/.agentchain\n",
        AGENTCHAIN_VERSION, AGENTCHAIN_GIT_COMMIT, argv0, AC_MAINNET_RPC);
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
/* RPC client — supports http:// and https://.                                */
/*                                                                            */
/* For https:// we shell out to `curl` (universally available on the          */
/* platforms we ship binaries for). For http:// or bare host:port we use a    */
/* small native client to keep loopback queries dependency-free.              */
/* -------------------------------------------------------------------------- */

static int parse_host_port(const char *hp, char *host, size_t host_cap, uint16_t *port) {
    const char *colon = strrchr(hp, ':');
    if (!colon) return -1;
    size_t hl = (size_t)(colon - hp);
    if (hl >= host_cap) return -1;
    memcpy(host, hp, hl); host[hl] = '\0';
    int p = atoi(colon + 1);
    if (p <= 0 || p > 65535) return -1;
    *port = (uint16_t)p;
    return 0;
}

static int http_post_native(const char *host, uint16_t port,
                            const char *body, char *resp, size_t resp_cap) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

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
        "User-Agent: %s\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
        host, AGENTCHAIN_USER_AGENT, strlen(body));
    if (ac_sock_send(fd, header, (size_t)hn) < 0) { ac_sock_close(fd); return -1; }
    if (ac_sock_send(fd, body, strlen(body)) < 0) { ac_sock_close(fd); return -1; }

    size_t total = 0;
    while (total + 1 < resp_cap) {
        ssize_t n = ac_sock_recv(fd, resp + total, resp_cap - 1 - total);
        if (n <= 0) break;
        total += (size_t)n;
    }
    resp[total] = '\0';
    ac_sock_close(fd);

    /* Strip HTTP headers — caller only wants the body. */
    const char *p = strstr(resp, "\r\n\r\n");
    if (p) {
        size_t body_len = total - (size_t)(p + 4 - resp);
        memmove(resp, p + 4, body_len);
        resp[body_len] = '\0';
        return (int)body_len;
    }
    return (int)total;
}

/* Shell-out via curl for HTTPS. We bind to a fixed argv list so caller-
 * supplied URLs cannot inject extra args. The body is passed via stdin to
 * avoid command-line length limits and quoting bugs. */
static int https_post_curl(const char *url, const char *body, char *resp, size_t resp_cap) {
#ifdef _WIN32
    (void)url; (void)body; (void)resp; (void)resp_cap;
    fprintf(stderr, "https client requires `curl` on the system PATH.\n");
    return -1;
#else
    int in_pipe[2], out_pipe[2];
    if (pipe(in_pipe) < 0)  return -1;
    if (pipe(out_pipe) < 0) { close(in_pipe[0]); close(in_pipe[1]); return -1; }

    pid_t pid = fork();
    if (pid < 0) {
        close(in_pipe[0]); close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);
        return -1;
    }
    if (pid == 0) {
        /* Child: wire stdin from in_pipe, stdout to out_pipe. */
        dup2(in_pipe[0], 0);
        dup2(out_pipe[1], 1);
        close(in_pipe[1]); close(out_pipe[0]);
        execlp("curl", "curl",
               "-sS",                            /* silent on success, error on stderr */
               "-X", "POST",
               "-H", "Content-Type: application/json",
               "-H", "User-Agent: " AGENTCHAIN_USER_AGENT,
               "--max-time", "20",
               "--data-binary", "@-",            /* read body from stdin */
               url,
               (char *)NULL);
        fprintf(stderr, "exec curl failed: %s\n", strerror(errno));
        _exit(127);
    }
    /* Parent: write body, read response. */
    close(in_pipe[0]); close(out_pipe[1]);
    size_t blen = strlen(body), written = 0;
    while (written < blen) {
        ssize_t n = write(in_pipe[1], body + written, blen - written);
        if (n <= 0) break;
        written += (size_t)n;
    }
    close(in_pipe[1]);
    size_t total = 0;
    while (total + 1 < resp_cap) {
        ssize_t n = read(out_pipe[0], resp + total, resp_cap - 1 - total);
        if (n <= 0) break;
        total += (size_t)n;
    }
    resp[total] = '\0';
    close(out_pipe[0]);

    int status = 0;
    waitpid(pid, &status, 0);
    if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
        /* curl printed its error on stderr (inherited from parent). */
        return -1;
    }
    return (int)total;
#endif
}

/* Top-level RPC call. Picks transport from URL scheme. */
static int rpc_call(const char *url, const char *body, char *resp, size_t resp_cap) {
    if (strncmp(url, "https://", 8) == 0) {
        return https_post_curl(url, body, resp, resp_cap);
    }
    if (strncmp(url, "http://", 7) == 0) {
        url += 7;
    }
    /* host[:port] form. */
    char host[256]; uint16_t port;
    char hp[256];
    snprintf(hp, sizeof(hp), "%.*s", (int)(sizeof(hp) - 1), url);
    /* Strip trailing path. */
    char *slash = strchr(hp, '/');
    if (slash) *slash = '\0';
    if (parse_host_port(hp, host, sizeof(host), &port) < 0) {
        /* Maybe no port — default to 30304. */
        snprintf(host, sizeof(host), "%.*s", (int)(sizeof(host) - 1), hp);
        port = 30304;
    }
    return http_post_native(host, port, body, resp, resp_cap);
}

/* JSON helper: extract a uint64 next to a "key":… occurrence. */
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

/* Pretty-print: extract a hex string field, e.g. "hash":"…". Returns
 * 0 on success and fills `out` (null-terminated). */
static int json_get_hex(const char *body, const char *key, char *out, size_t cap) {
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(body, pattern);
    if (!p) return -1;
    p += strlen(pattern);
    while (*p == ' ' || *p == '\t' || *p == ':' || *p == '"') p++;
    size_t n = 0;
    while (*p && *p != '"' && n + 1 < cap) out[n++] = *p++;
    out[n] = '\0';
    return 0;
}

/* -------------------------------------------------------------------------- */
/* `keygen`.                                                                  */
/* -------------------------------------------------------------------------- */

static int cmd_keygen(int argc, char **argv) {
    if (has_flag(argc, argv, "--help") || has_flag(argc, argv, "-h")) {
        fprintf(stderr,
            "Usage: agentchain keygen [--out FILE]\n"
            "\n"
            "Create a fresh Ed25519 keypair. The private seed is written 0600 to\n"
            "the path you choose (default: ~/.agentchain/node.key). The public\n"
            "key (= your AgentChain address) is printed on stdout.\n"
            "\n"
            "Existing files are NEVER overwritten — this is a destructive op for\n"
            "your wallet. Use `--out` to pick a different path if the default is\n"
            "taken.\n");
        return 0;
    }

    const char *out = get_opt(argc, argv, "--out");
    char default_path[1024];
    if (!out) {
        default_key_path(default_path, sizeof(default_path));
        out = default_path;
    }

    if (ac_crypto_init() != 0) {
        fprintf(stderr, "keygen: libsodium init failed\n");
        return 1;
    }
    if (ac_file_exists(out)) {
        fprintf(stderr,
            "keygen: refusing to overwrite existing file %s\n"
            "        (move it aside or pass --out to write elsewhere)\n", out);
        return 1;
    }
    /* Ensure the parent dir exists. */
    {
        char dir[1024];
        snprintf(dir, sizeof(dir), "%s", out);
        char *last = strrchr(dir, '/');
        if (last) { *last = '\0'; if (dir[0]) ac_mkdir_p(dir); }
    }

    ac_keypair_t kp;
    bool created = false;
    if (ac_node_keypair_load_or_create(out, &kp, &created) != 0) {
        fprintf(stderr, "keygen: failed to write %s: %s\n", out, strerror(errno));
        return 1;
    }
    char hex[2 * AC_PUBKEY_SIZE + 1];
    ac_hex_encode(hex, kp.pk, AC_PUBKEY_SIZE);
    printf("address: %s\n", hex);
    printf("wrote:   %s (mode 0600)\n", out);
    fprintf(stderr,
        "\nKeep this file safe. Anyone with it can spend your balance and sign\n"
        "blocks under your validator key. Back it up offline.\n");
    return 0;
}

/* -------------------------------------------------------------------------- */
/* `pubkey`.                                                                  */
/* -------------------------------------------------------------------------- */

static int cmd_pubkey(int argc, char **argv) {
    if (has_flag(argc, argv, "--help") || has_flag(argc, argv, "-h")) {
        fprintf(stderr,
            "Usage: agentchain pubkey [--key FILE | --data-dir DIR]\n"
            "\n"
            "Print the public address of the key file. With no arguments, reads\n"
            "~/.agentchain/node.key.\n");
        return 0;
    }
    const char *dd = get_opt(argc, argv, "--data-dir");
    const char *kp = get_opt(argc, argv, "--key");
    char path[1024];
    if (kp) {
        snprintf(path, sizeof(path), "%s", kp);
    } else if (dd) {
        ac_join_path(path, sizeof(path), dd, "node.key");
    } else {
        default_key_path(path, sizeof(path));
    }
    if (!ac_file_exists(path)) {
        fprintf(stderr,
            "pubkey: %s not found.\n"
            "        Run 'agentchain keygen' to create a key.\n", path);
        return 1;
    }
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

static int cmd_genesis(int argc, char **argv) {
    if (has_flag(argc, argv, "--help") || has_flag(argc, argv, "-h")) {
        fprintf(stderr,
            "Usage: agentchain genesis --chain-id N --out FILE\n"
            "                          [--timestamp-ms N] [--account HEX:BAL:STAKE ...]\n"
            "\n"
            "Write a fresh genesis configuration file. Used to bootstrap testnets\n"
            "(joining mainnet alpha does not require this — the mainnet alpha\n"
            "genesis is embedded in the binary and used automatically when no\n"
            "--genesis is passed to `agentchain node`).\n");
        return 0;
    }
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
/* `info`.                                                                    */
/* -------------------------------------------------------------------------- */

static int cmd_info(int argc, char **argv) {
    if (has_flag(argc, argv, "--help") || has_flag(argc, argv, "-h")) {
        fprintf(stderr,
            "Usage: agentchain info [--rpc URL]\n"
            "\n"
            "Print the current chain state (height, tip hash, base fee, …).\n"
            "Defaults to %s.\n", AC_MAINNET_RPC);
        return 0;
    }
    const char *rpc = get_opt(argc, argv, "--rpc");
    if (!rpc) rpc = AC_MAINNET_RPC;

    char resp[8192];
    int rc = rpc_call(rpc, "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"chain_info\"}",
                      resp, sizeof(resp));
    if (rc < 0) {
        fprintf(stderr, "info: cannot reach %s\n", rpc);
        return 1;
    }
    /* Try to pretty-print the well-known fields; fall back to raw on parse fail. */
    uint64_t cid = 0, height = 0, base_fee = 0, genesis_ts = 0;
    if (json_get_uint64(resp, "chain_id", &cid) == 0 &&
        json_get_uint64(resp, "height",   &height) == 0) {
        char tip[128] = {0};
        json_get_hex(resp, "tip_hash", tip, sizeof(tip));
        json_get_uint64(resp, "base_fee", &base_fee);
        json_get_uint64(resp, "genesis_timestamp_ms", &genesis_ts);
        printf("rpc:       %s\n", rpc);
        printf("chain_id:  %" PRIu64 "\n", cid);
        printf("height:    %" PRIu64 "\n", height);
        printf("tip_hash:  %s\n", tip);
        printf("base_fee:  %" PRIu64 " µCRD/gas\n", base_fee);
        printf("genesis:   %" PRIu64 " ms\n", genesis_ts);
        return 0;
    }
    puts(resp);
    return 0;
}

/* -------------------------------------------------------------------------- */
/* `balance`.                                                                 */
/* -------------------------------------------------------------------------- */

static int cmd_balance(int argc, char **argv) {
    if (has_flag(argc, argv, "--help") || has_flag(argc, argv, "-h")) {
        fprintf(stderr,
            "Usage: agentchain balance [--address HEX] [--rpc URL] [--key FILE]\n"
            "\n"
            "Look up an account's balance, nonce, and bonded stake. With no\n"
            "--address, queries your default wallet key (~/.agentchain/node.key).\n"
            "Defaults RPC to %s.\n", AC_MAINNET_RPC);
        return 0;
    }
    const char *rpc  = get_opt(argc, argv, "--rpc");
    const char *addr = get_opt(argc, argv, "--address");
    const char *keyf = get_opt(argc, argv, "--key");
    if (!rpc) rpc = AC_MAINNET_RPC;

    /* If no --address, derive it from the local key. */
    char addr_buf[2 * AC_PUBKEY_SIZE + 1];
    if (!addr) {
        char path[1024];
        if (keyf) snprintf(path, sizeof(path), "%s", keyf);
        else      default_key_path(path, sizeof(path));
        if (!ac_file_exists(path)) {
            fprintf(stderr,
                "balance: no --address given and %s does not exist.\n"
                "         Pass --address HEX, or run 'agentchain keygen' first.\n", path);
            return 2;
        }
        if (ac_crypto_init() != 0) return 1;
        ac_keypair_t kp;
        if (ac_node_keypair_load_or_create(path, &kp, NULL) != 0) {
            fprintf(stderr, "balance: cannot read %s\n", path); return 1;
        }
        ac_hex_encode(addr_buf, kp.pk, AC_PUBKEY_SIZE);
        addr = addr_buf;
    }

    char req[256];
    snprintf(req, sizeof(req),
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"account_get\",\"params\":{\"address\":\"%s\"}}",
        addr);
    char resp[4096];
    if (rpc_call(rpc, req, resp, sizeof(resp)) < 0) {
        fprintf(stderr, "balance: cannot reach %s\n", rpc); return 1;
    }
    uint64_t bal = 0, nonce = 0, stake = 0, unbond_at = 0;
    if (json_get_uint64(resp, "balance",   &bal)       == 0 &&
        json_get_uint64(resp, "nonce",     &nonce)     == 0 &&
        json_get_uint64(resp, "stake",     &stake)     == 0) {
        json_get_uint64(resp, "unbond_at", &unbond_at);
        printf("address: %s\n", addr);
        printf("balance: %" PRIu64 " µCRD\n", bal);
        printf("nonce:   %" PRIu64 "\n", nonce);
        printf("stake:   %" PRIu64 " µCRD%s\n", stake,
               stake >= AC_MIN_STAKE_UCRD ? " (active validator)" : "");
        if (unbond_at) printf("unbond_at: slot %" PRIu64 "\n", unbond_at);
        return 0;
    }
    puts(resp);
    return 0;
}

/* -------------------------------------------------------------------------- */
/* Tx-submission helpers (shared by `send`, `stake`, `unbond`).               */
/* -------------------------------------------------------------------------- */

typedef struct {
    const char  *rpc;
    const char  *key_file;
    const char  *tip_s;
    const char  *memo;
    const char  *valid_s;
} tx_submit_opts_t;

static tx_submit_opts_t parse_submit_opts(int argc, char **argv) {
    tx_submit_opts_t o;
    o.rpc      = get_opt(argc, argv, "--rpc");
    o.key_file = get_opt(argc, argv, "--from-key");
    o.tip_s    = get_opt(argc, argv, "--tip");
    o.memo     = get_opt(argc, argv, "--memo");
    o.valid_s  = get_opt(argc, argv, "--valid-slots");
    return o;
}

static int submit_tx(const char *action,
                     const tx_submit_opts_t *opts,
                     uint8_t kind,
                     const uint8_t *body, uint32_t body_len,
                     uint32_t gas_limit) {
    const char *rpc = opts->rpc ? opts->rpc : AC_MAINNET_RPC;
    char key_default[1024];
    const char *key = opts->key_file;
    if (!key) {
        default_key_path(key_default, sizeof(key_default));
        key = key_default;
    }
    if (!ac_file_exists(key)) {
        fprintf(stderr,
            "%s: %s does not exist.\n"
            "    Create one with `agentchain keygen`, or pass --from-key FILE.\n",
            action, key);
        return 2;
    }

    if (ac_crypto_init() != 0) return 1;

    ac_keypair_t kp;
    if (ac_node_keypair_load_or_create(key, &kp, NULL) != 0) {
        fprintf(stderr, "%s: cannot load %s\n", action, key); return 1;
    }

    char resp[8192];
    if (rpc_call(rpc,
            "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"chain_info\"}",
            resp, sizeof(resp)) < 0) {
        fprintf(stderr, "%s: cannot reach %s\n", action, rpc); return 1;
    }
    uint64_t chain_id = 0, genesis_ts = 0;
    json_get_uint64(resp, "chain_id",             &chain_id);
    json_get_uint64(resp, "genesis_timestamp_ms", &genesis_ts);

    char sender_hex[2 * AC_PUBKEY_SIZE + 1];
    ac_hex_encode(sender_hex, kp.pk, AC_PUBKEY_SIZE);
    char req[512];
    snprintf(req, sizeof(req),
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"account_get\",\"params\":{\"address\":\"%s\"}}",
        sender_hex);
    if (rpc_call(rpc, req, resp, sizeof(resp)) < 0) {
        fprintf(stderr, "%s: account_get failed\n", action); return 1;
    }
    uint64_t nonce = 0;
    json_get_uint64(resp, "nonce", &nonce);

    ac_tx_t tx;
    memset(&tx, 0, sizeof(tx));
    tx.version    = AC_TX_VERSION;
    tx.chain_id   = chain_id;
    tx.kind       = kind;
    memcpy(tx.sender.b, kp.pk, AC_PUBKEY_SIZE);
    tx.nonce      = nonce;
    tx.gas_limit  = gas_limit;
    tx.tip        = opts->tip_s ? strtoull(opts->tip_s, NULL, 10) : 0;
    uint64_t current_slot = (ac_now_ms() - genesis_ts) / AC_SLOT_DURATION_MS;
    uint64_t valid_window = opts->valid_s ? strtoull(opts->valid_s, NULL, 10) : 600;
    tx.valid_until = current_slot + valid_window;

    if (body_len > 0) {
        if (body_len > AC_TX_BODY_MAX) {
            fprintf(stderr, "%s: body too large\n", action); return 1;
        }
        memcpy(tx.body, body, body_len);
        tx.body_len = body_len;
    }
    if (opts->memo) {
        size_t ml = strlen(opts->memo);
        if (ml > AC_MEMO_MAX) ml = AC_MEMO_MAX;
        memcpy(tx.memo, opts->memo, ml);
        tx.memo_len = (uint32_t)ml;
    }

    if (ac_tx_sign(&tx, &kp) != 0) {
        fprintf(stderr, "%s: signing failed\n", action); return 1;
    }

    uint8_t bin[AC_TX_MAX_BYTES];
    int n = ac_tx_encode(bin, sizeof(bin), &tx);
    if (n < 0) { fprintf(stderr, "%s: tx encode failed\n", action); return 1; }
    char tx_hex[2 * AC_TX_MAX_BYTES + 1];
    ac_hex_encode(tx_hex, bin, (size_t)n);

    char *req2 = (char *)malloc(strlen(tx_hex) + 256);
    if (!req2) return 1;
    snprintf(req2, strlen(tx_hex) + 256,
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tx_submit\",\"params\":{\"tx_hex\":\"%s\"}}",
        tx_hex);
    int rc = rpc_call(rpc, req2, resp, sizeof(resp));
    free(req2);
    if (rc < 0) { fprintf(stderr, "%s: tx_submit failed\n", action); return 1; }

    /* Friendly output: pull out the tx hash if the RPC returned one. */
    char hash[2 * AC_HASH_SIZE + 1] = {0};
    if (json_get_hex(resp, "hash", hash, sizeof(hash)) == 0 && hash[0]) {
        printf("submitted %s tx\n", action);
        printf("hash: %s\n", hash);
        printf("rpc:  %s\n", rpc);
        return 0;
    }
    /* On error the server returns a JSON-RPC error object; just print it. */
    puts(resp);
    return 1;
}

/* -------------------------------------------------------------------------- */
/* `send`, `stake`, `unbond`.                                                 */
/* -------------------------------------------------------------------------- */

static int cmd_send(int argc, char **argv) {
    if (has_flag(argc, argv, "--help") || has_flag(argc, argv, "-h")) {
        fprintf(stderr,
            "Usage: agentchain send --to HEX --amount UCRD\n"
            "                       [--from-key FILE] [--rpc URL]\n"
            "                       [--tip N] [--memo TEXT] [--valid-slots N]\n"
            "\n"
            "Transfer `amount` µCRD (= amount × 10⁻⁶ CRD) from your wallet to\n"
            "the recipient address. Signs locally, broadcasts via JSON-RPC.\n"
            "\n"
            "Defaults: from-key = ~/.agentchain/node.key, rpc = %s\n", AC_MAINNET_RPC);
        return 0;
    }
    tx_submit_opts_t opts = parse_submit_opts(argc, argv);
    const char *to_hex    = get_opt(argc, argv, "--to");
    const char *amount_s  = get_opt(argc, argv, "--amount");
    if (!to_hex || !amount_s) {
        fprintf(stderr, "send: --to HEX --amount UCRD required (run with --help for details)\n");
        return 2;
    }

    ac_body_transfer_t bt;
    if (ac_hex_decode(bt.recipient.b, AC_PUBKEY_SIZE, to_hex) != 0) {
        fprintf(stderr, "send: bad recipient hex (need 64-char address)\n"); return 2;
    }
    bt.amount = strtoull(amount_s, NULL, 10);
    uint8_t body[AC_TX_BODY_MAX];
    int bn = ac_body_transfer_encode(body, sizeof(body), &bt);
    if (bn < 0) { fprintf(stderr, "send: body encode failed\n"); return 1; }

    return submit_tx("send", &opts, AC_TX_TRANSFER, body, (uint32_t)bn, /*gas_limit*/ 1000);
}

static int cmd_bond(int argc, char **argv) {
    if (has_flag(argc, argv, "--help") || has_flag(argc, argv, "-h")) {
        fprintf(stderr,
            "Usage: agentchain bond --amount UCRD\n"
            "                       [--from-key FILE] [--rpc URL] [--tip N]\n"
            "\n"
            "Bond `amount` micro-CRD from your balance into stake. Once stake \n"
            "reaches %llu uCRD (= 100 CRD), your key becomes eligible for validator\n"
            "sortition. You still need to run `agentchain node --validator` to\n"
            "actually propose blocks.\n"
            "\n"
            "Until v1.0.13 this command was called `stake`; the old name still\n"
            "works as a deprecated alias.\n",
            (unsigned long long)AC_MIN_STAKE_UCRD);
        return 0;
    }
    tx_submit_opts_t opts = parse_submit_opts(argc, argv);
    const char *amount_s  = get_opt(argc, argv, "--amount");
    if (!amount_s) {
        fprintf(stderr, "bond: --amount UCRD required\n");
        return 2;
    }
    ac_body_stake_t b;
    b.amount = strtoull(amount_s, NULL, 10);
    uint8_t body[AC_TX_BODY_MAX];
    int bn = ac_body_stake_encode(body, sizeof(body), &b);
    if (bn < 0) { fprintf(stderr, "bond: body encode failed\n"); return 1; }
    return submit_tx("bond", &opts, AC_TX_STAKE_BOND, body, (uint32_t)bn, /*gas_limit*/ 1000);
}

static int cmd_unbond(int argc, char **argv) {
    if (has_flag(argc, argv, "--help") || has_flag(argc, argv, "-h")) {
        fprintf(stderr,
            "Usage: agentchain unbond --amount UCRD\n"
            "                         [--from-key FILE] [--rpc URL] [--tip N]\n"
            "\n"
            "Release `amount` µCRD from stake back to balance. The protocol\n"
            "specifies a 24-hour cooldown (PROTOCOL.md § 5.2); v1.0.x releases\n"
            "early — see TECHNICAL-IMPLEMENTATION.md § 9.4.\n");
        return 0;
    }
    tx_submit_opts_t opts = parse_submit_opts(argc, argv);
    const char *amount_s  = get_opt(argc, argv, "--amount");
    if (!amount_s) {
        fprintf(stderr, "unbond: --amount UCRD required\n");
        return 2;
    }
    ac_body_stake_t b;
    b.amount = strtoull(amount_s, NULL, 10);
    uint8_t body[AC_TX_BODY_MAX];
    int bn = ac_body_stake_encode(body, sizeof(body), &b);
    if (bn < 0) { fprintf(stderr, "unbond: body encode failed\n"); return 1; }
    return submit_tx("unbond", &opts, AC_TX_STAKE_UNBOND, body, (uint32_t)bn, /*gas_limit*/ 1000);
}

/* -------------------------------------------------------------------------- */
/* `node`.                                                                    */
/* -------------------------------------------------------------------------- */

static int write_embedded_mainnet_genesis(const char *path) {
    return ac_file_write_atomic(path, (const uint8_t *)AC_MAINNET_GENESIS,
                                strlen(AC_MAINNET_GENESIS), 0644);
}

static int cmd_node(int argc, char **argv) {
    if (has_flag(argc, argv, "--help") || has_flag(argc, argv, "-h")) {
        fprintf(stderr,
            "Usage: agentchain node [--validator] [-v]\n"
            "                       [--data-dir DIR] [--genesis FILE] [--seeds H:P,…]\n"
            "                       [--port N] [--rpc-port N]\n"
            "                       [--host BIND] [--rpc-host BIND]\n"
            "                       [--external-host H]\n"
            "\n"
            "Run a node. With no arguments this joins AgentChain mainnet alpha:\n"
            "  data-dir   ~/.agentchain\n"
            "  genesis    embedded mainnet alpha (chain_id=1)\n"
            "  seeds      %s\n"
            "  port       30303 (p2p)\n"
            "  rpc-port   30304 (loopback only)\n"
            "\n"
            "Add --validator to participate in consensus (requires ≥ %llu µCRD bonded).\n"
            "Expose --rpc-host 0.0.0.0 only if you want to serve RPC publicly.\n",
            AC_MAINNET_SEEDS, (unsigned long long)AC_MIN_STAKE_UCRD);
        return 0;
    }

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

    char default_dd[1024];
    if (!dd) {
        default_data_dir(default_dd, sizeof(default_dd));
        dd = default_dd;
    }

    /* Make sure data-dir exists, then resolve a genesis to feed the chain. */
    ac_mkdir_p(dd);

    char gen_path[1024];
    if (!gen) {
        ac_join_path(gen_path, sizeof(gen_path), dd, "genesis.txt");
        if (!ac_file_exists(gen_path)) {
            if (write_embedded_mainnet_genesis(gen_path) < 0) {
                fprintf(stderr,
                    "node: failed to materialise embedded mainnet alpha genesis at %s\n", gen_path);
                return 1;
            }
        }
        gen = gen_path;
    }

    ac_log_init(verbose ? AC_LOG_DEBUG : AC_LOG_INFO);

    snprintf(cfg.data_dir,     sizeof(cfg.data_dir),     "%.*s",
             (int)(sizeof(cfg.data_dir)     - 1), dd);
    snprintf(cfg.genesis_path, sizeof(cfg.genesis_path), "%.*s",
             (int)(sizeof(cfg.genesis_path) - 1), gen);
    snprintf(cfg.listen_host,  sizeof(cfg.listen_host),  "%.*s",
             (int)(sizeof(cfg.listen_host)  - 1), host ? host : "0.0.0.0");
    cfg.listen_port = (uint16_t)(port_s     ? atoi(port_s)     : 30303);
    snprintf(cfg.rpc_host,     sizeof(cfg.rpc_host),     "%.*s",
             (int)(sizeof(cfg.rpc_host)     - 1), rpc_host ? rpc_host : "127.0.0.1");
    cfg.rpc_port    = (uint16_t)(rpc_port_s ? atoi(rpc_port_s) : 30304);
    if (ext) snprintf(cfg.external_host, sizeof(cfg.external_host), "%.*s",
                      (int)(sizeof(cfg.external_host) - 1), ext);
    cfg.validator = validator;

    /* Seeds: explicit override wins, otherwise fall back to mainnet seeds. */
    const char *seeds_eff = seeds_s && *seeds_s ? seeds_s : AC_MAINNET_SEEDS;
    {
        char *copy = strdup(seeds_eff);
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

    ac_net_init();

    int rc = 2;
    if      (strcmp(cmd, "version") == 0 || strcmp(cmd, "--version") == 0) rc = cmd_version();
    else if (strcmp(cmd, "node")    == 0) rc = cmd_node(argc - 1, argv + 1);
    else if (strcmp(cmd, "keygen")  == 0) rc = cmd_keygen(argc - 1, argv + 1);
    else if (strcmp(cmd, "pubkey")  == 0) rc = cmd_pubkey(argc - 1, argv + 1);
    else if (strcmp(cmd, "genesis") == 0) rc = cmd_genesis(argc - 1, argv + 1);
    else if (strcmp(cmd, "send")    == 0) rc = cmd_send(argc - 1, argv + 1);
    else if (strcmp(cmd, "bond")    == 0) rc = cmd_bond  (argc - 1, argv + 1);
    else if (strcmp(cmd, "stake")   == 0) rc = cmd_bond  (argc - 1, argv + 1);   /* deprecated alias */
    else if (strcmp(cmd, "unbond")  == 0) rc = cmd_unbond(argc - 1, argv + 1);
    else if (strcmp(cmd, "balance") == 0) rc = cmd_balance(argc - 1, argv + 1);
    else if (strcmp(cmd, "info")    == 0) rc = cmd_info(argc - 1, argv + 1);
    else if (strcmp(cmd, "--help") == 0 || strcmp(cmd, "-h") == 0 || strcmp(cmd, "help") == 0) {
        print_usage(argv[0]); rc = 0;
    } else {
        fprintf(stderr, "Unknown command: %s\n\n", cmd);
        print_usage(argv[0]);
    }
    ac_net_cleanup();
    return rc;
}
