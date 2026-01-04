#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "blockchain.h"
#include "consensus.h"
#include "control.h"
#include "crypto.h"
#include "identity.h"
#include "log.h"
#include "network.h"
#include "tx_builder.h"
#include "utils.h"

static volatile sig_atomic_t NODE_RUNNING = 1;

static void handle_signal(int sig) {
    (void)sig;
    NODE_RUNNING = 0;
}

static int parse_u64(const char* s, uint64_t* out) {
    if (!s || !*s || !out) return -1;
    char* endptr = NULL;
    unsigned long long val = strtoull(s, &endptr, 10);
    if (endptr == s || *endptr != '\0') return -2;
    *out = (uint64_t)val;
    return 0;
}

static void usage(const char* prog) {
    fprintf(stderr,
        "usage:\n"
        "  %s node\n"
        "  %s pubkey\n"
        "  %s transfer <receiver_pub_hex> <amount>\n"
        "  %s mint <receiver_pub_hex> <amount>\n"
        "  %s balance [pub_hex]\n"
        "  %s ping\n\n"
        "env:\n"
        "  BC_PRIVKEY / BC_PUBKEY   hex keys (BC_PRIVKEY can be 32B seed or 64B secret)\n"
        "  BC_PORT                  udp port for peer network (default 30303)\n"
        "  BC_CTL_PORT              local control port (default 30304)\n"
        "  BC_SEEDS                 comma list of ip:port seeds\n",
        prog, prog, prog, prog, prog, prog);
}

static int encode_tx_to_wire(const tx* t, uint8_t** out_buf, size_t* out_len) {
    if (!t || !out_buf || !out_len) return -1;
    size_t len = 0;
    if (encode_tx((tx*)t, NULL, &len) < 0) return -2;
    uint8_t* buf = malloc(len);
    if (!buf) return -3;
    size_t cap = len;
    if (encode_tx((tx*)t, buf, &cap) < 0) {
        free(buf);
        return -4;
    }
    *out_buf = buf;
    *out_len = cap;
    return 0;
}

static int cmd_node(void) {
    account local;
    if (identity_load(&local, 1) < 0) {
        fprintf(stderr, "identity load failed\n");
        return 1;
    }

    if (blockchain_init(&CHAIN) < 0) {
        log_error("blockchain init failed");
        return 1;
    }
    if (network_set_identity(local.pub_key, local.priv_key) < 0) {
        log_error("network identity missing");
        return 1;
    }
    if (network_init() < 0) {
        log_error("network init failed");
        return 1;
    }

    int boot = blockchain_bootstrap(&CHAIN, &local);
    if (boot < 0) {
        log_error("bootstrap failed (%d)", boot);
        return 1;
    }
    if (boot > 0) {
        log_info("bootstrap created chain_id=%llu", (unsigned long long)CHAIN.chain_id);
    } else {
        log_info("chain state loaded chain_id=%llu", (unsigned long long)CHAIN.chain_id);
    }

    if (consensus_set_validator(&local) < 0) {
        log_error("validator setup failed");
        return 1;
    }
    if (consensus_init(&CHAIN) < 0) {
        log_error("consensus init failed");
        return 1;
    }
    if (control_start(&CHAIN) < 0) {
        log_error("control init failed");
        return 1;
    }

    log_info("node running net_port=%u", network_listen_port());
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    while (NODE_RUNNING) {
        sleep(1);
    }

    log_info("shutting down");
    control_stop();
    consensus_shutdown();
    network_shutdown();
    return 0;
}

static int cmd_transfer(int argc, char** argv) {
    if (argc != 3) return -1;

    account sender;
    if (identity_load(&sender, 1) < 0) {
        fprintf(stderr, "identity load failed\n");
        return 1;
    }

    pub_key_t receiver;
    if (hex_to_bytes(argv[1], receiver, crypto_sign_PUBLICKEYBYTES) < 0) {
        fprintf(stderr, "invalid receiver pub key\n");
        return 1;
    }

    uint64_t amount = 0;
    if (parse_u64(argv[2], &amount) < 0) {
        fprintf(stderr, "invalid amount\n");
        return 1;
    }

    tx* t = NULL;
    if (tx_build_transfer(&t, &sender, receiver, amount) < 0) {
        fprintf(stderr, "failed to build transfer tx\n");
        return 1;
    }

    uint8_t* buf = NULL;
    size_t len = 0;
    if (encode_tx_to_wire(t, &buf, &len) < 0) {
        tx_free(t);
        fprintf(stderr, "failed to encode tx\n");
        return 1;
    }

    int rc = control_send_tx(buf, len);
    free(buf);
    tx_free(t);

    if (rc < 0) {
        fprintf(stderr, "node rejected tx (%d)\n", rc);
        return 1;
    }

    printf("tx sent\n");
    return 0;
}

static int cmd_mint(int argc, char** argv) {
    if (argc != 3) return -1;

    account minter;
    if (identity_load(&minter, 1) < 0) {
        fprintf(stderr, "identity load failed\n");
        return 1;
    }

    pub_key_t receiver;
    if (hex_to_bytes(argv[1], receiver, crypto_sign_PUBLICKEYBYTES) < 0) {
        fprintf(stderr, "invalid receiver pub key\n");
        return 1;
    }

    uint64_t amount = 0;
    if (parse_u64(argv[2], &amount) < 0) {
        fprintf(stderr, "invalid amount\n");
        return 1;
    }

    tx* t = NULL;
    if (tx_build_mint(&t, &minter, receiver, amount) < 0) {
        fprintf(stderr, "failed to build mint tx\n");
        return 1;
    }

    uint8_t* buf = NULL;
    size_t len = 0;
    if (encode_tx_to_wire(t, &buf, &len) < 0) {
        tx_free(t);
        fprintf(stderr, "failed to encode tx\n");
        return 1;
    }

    int rc = control_send_tx(buf, len);
    free(buf);
    tx_free(t);

    if (rc < 0) {
        fprintf(stderr, "node rejected tx (%d)\n", rc);
        return 1;
    }

    printf("tx sent\n");
    return 0;
}

static int cmd_balance(int argc, char** argv) {
    pub_key_t key;
    if (argc >= 2) {
        if (hex_to_bytes(argv[1], key, crypto_sign_PUBLICKEYBYTES) < 0) {
            fprintf(stderr, "invalid pub key\n");
            return 1;
        }
    } else {
        account local;
        if (identity_load(&local, 0) < 0) {
            fprintf(stderr, "identity load failed\n");
            return 1;
        }
        memcpy(key, local.pub_key, crypto_sign_PUBLICKEYBYTES);
    }

    uint64_t balance = 0;
    int rc = control_get_balance(key, &balance);
    if (rc < 0) {
        fprintf(stderr, "failed to read balance (%d)\n", rc);
        return 1;
    }

    printf("%" PRIu64 "\n", balance);
    return 0;
}

static int cmd_ping(void) {
    int rc = control_ping();
    if (rc < 0) {
        fprintf(stderr, "node not responding (%d)\n", rc);
        return 1;
    }
    printf("ok\n");
    return 0;
}

static int cmd_pubkey(void) {
    account local;
    if (identity_load(&local, 1) < 0) {
        fprintf(stderr, "identity load failed\n");
        return 1;
    }
    char hex[crypto_sign_PUBLICKEYBYTES * 2 + 1];
    if (bytes_to_hex(local.pub_key, crypto_sign_PUBLICKEYBYTES, hex, sizeof(hex)) < 0) {
        fprintf(stderr, "failed to encode pubkey\n");
        return 1;
    }
    printf("%s\n", hex);
    return 0;
}

int main(int argc, char** argv) {
    if (sodium_init() < 0) return 1;
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    const char* cmd = argv[1];
    if (strcmp(cmd, "node") == 0) {
        return cmd_node();
    }
    if (strcmp(cmd, "transfer") == 0) {
        int rc = cmd_transfer(argc - 1, &argv[1]);
        if (rc < 0) usage(argv[0]);
        return rc < 0 ? 1 : rc;
    }
    if (strcmp(cmd, "mint") == 0) {
        int rc = cmd_mint(argc - 1, &argv[1]);
        if (rc < 0) usage(argv[0]);
        return rc < 0 ? 1 : rc;
    }
    if (strcmp(cmd, "balance") == 0) {
        int rc = cmd_balance(argc - 1, &argv[1]);
        if (rc < 0) usage(argv[0]);
        return rc < 0 ? 1 : rc;
    }
    if (strcmp(cmd, "ping") == 0) {
        return cmd_ping();
    }
    if (strcmp(cmd, "pubkey") == 0) {
        return cmd_pubkey();
    }

    usage(argv[0]);
    return 1;
}
