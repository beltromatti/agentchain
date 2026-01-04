#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
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
        "  %s bootstrap\n"
        "  %s pubkey\n"
        "  %s transfer <receiver_pub_hex> <amount>\n"
        "  %s mint <receiver_pub_hex> <amount>\n"
        "  %s balance [pub_hex]\n"
        "  %s ping\n\n"
        "node flags:\n"
        "  --new-keys               generate a new keypair and overwrite data/identity.key\n\n"
        "env:\n"
        "  BC_PRIVKEY / BC_PUBKEY   hex keys (BC_PRIVKEY can be 32B seed or 64B secret)\n"
        "  BC_PORT                  udp port for peer network (default 30303)\n"
        "  BC_CTL_PORT              local control port (default 30304)\n"
        "  BC_SEEDS                 comma list of ip:port seeds\n",
        prog, prog, prog, prog, prog, prog, prog);
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

static int sync_chain_from_peers(blockchain* bc) {
    uint64_t start = (uint64_t)time(NULL);
    uint64_t chain_id = 0;
    uint64_t height = 0;
    uint64_t tip_id = 0;
    pub_key_t genesis_pub = { 0 };

    const char* seeds_file = getenv("BC_SEEDS_FILE");
    if (!seeds_file || !*seeds_file) seeds_file = "seeds.txt";

    FILE* f = fopen(seeds_file, "r");
    if (f) {
        char line[256];
        while (fgets(line, (int)sizeof(line), f)) {
            char* comment = strchr(line, '#');
            if (comment) *comment = '\0';

            size_t n = strlen(line);
            while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r' || line[n - 1] == ' ' || line[n - 1] == '\t')) {
                line[n - 1] = '\0';
                n--;
            }
            char* s = line;
            while (*s == ' ' || *s == '\t') s++;
            if (*s == '\0') continue;

            char* sep = strrchr(s, ':');
            uint16_t port = 30303;
            if (sep) {
                *sep = '\0';
                uint64_t p = 0;
                if (parse_u64(sep + 1, &p) == 0 && p > 0 && p <= 65535) {
                    port = (uint16_t)p;
                }
            }
            network_peer_add_addr(s, port);
        }
        fclose(f);
    }

    log_info("no local chain state, syncing from peers (BC_SEEDS / %s)", seeds_file);

    pthread_mutex_lock(&bc->mtx);
    int has_chain = (bc->chain_id != 0);
    pthread_mutex_unlock(&bc->mtx);

    if (!has_chain) {
        while (((uint64_t)time(NULL)) - start < 20) {
            network_request_chain_state();

            if (network_get_remote_chain_state(&chain_id, genesis_pub, &height, &tip_id) == 0) {
                if (blockchain_accept_remote_chain_state(bc, chain_id, genesis_pub) < 0) {
                    sleep(1);
                    continue;
                }
                char gen_hex[crypto_sign_PUBLICKEYBYTES * 2 + 1];
                if (bytes_to_hex(genesis_pub, crypto_sign_PUBLICKEYBYTES, gen_hex, sizeof(gen_hex)) == 0) {
                    log_info("chain state synced chain_id=%llu genesis=%s height=%llu",
                             (unsigned long long)chain_id, gen_hex, (unsigned long long)height);
                } else {
                    log_info("chain state synced chain_id=%llu height=%llu",
                             (unsigned long long)chain_id, (unsigned long long)height);
                }
                break;
            }

            sleep(1);
        }

        pthread_mutex_lock(&bc->mtx);
        has_chain = (bc->chain_id != 0);
        pthread_mutex_unlock(&bc->mtx);
        if (!has_chain) return -1;
    }

    log_info("requesting chain snapshot");
    start = (uint64_t)time(NULL);
    while (((uint64_t)time(NULL)) - start < 20) {
        network_request_snapshot();

        size_t snap_len = 0;
        if (network_get_remote_snapshot(NULL, &snap_len) == 0 && snap_len > 0) {
            uint8_t* buf = malloc(snap_len);
            if (!buf) return -2;
            size_t cap = snap_len;
            if (network_get_remote_snapshot(buf, &cap) == 0) {
                int rc = blockchain_apply_snapshot(bc, buf, cap);
                free(buf);
                if (rc == 0) {
                    log_info("snapshot applied");
                    return 0;
                }
            } else {
                free(buf);
            }
        }

        sleep(1);
    }

    return -3;
}

static int cmd_node(int allow_bootstrap, int rotate_keys) {
    account local;
    if (rotate_keys) {
        if (identity_rotate(&local) < 0) {
            fprintf(stderr, "identity rotate failed\n");
            return 1;
        }
        log_info("generated new identity");
    } else if (identity_load(&local, 1) < 0) {
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

    int rc = blockchain_load_chain_state(&CHAIN);
    if (rc == 0) {
        log_info("chain state loaded chain_id=%llu", (unsigned long long)CHAIN.chain_id);
    } else if (allow_bootstrap) {
        if (blockchain_create_chain_state(&CHAIN, &local) < 0) {
            log_error("bootstrap failed (chain already exists?)");
            return 1;
        }
        log_info("bootstrap created chain_id=%llu", (unsigned long long)CHAIN.chain_id);
    } else {
        if (sync_chain_from_peers(&CHAIN) < 0) {
            log_error("sync failed (run `bootstrap` to create a dev chain)");
            return 1;
        }
    }

    pthread_mutex_lock(&CHAIN.mtx);
    int synced = CHAIN.synced;
    pthread_mutex_unlock(&CHAIN.mtx);
    if (!synced && !allow_bootstrap) {
        if (sync_chain_from_peers(&CHAIN) < 0) {
            log_error("sync failed (need peers to fetch snapshot)");
            return 1;
        }
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
        if (rc == -11) {
            fprintf(stderr, "node not synced yet\n");
        } else {
            fprintf(stderr, "node rejected tx (%d)\n", rc);
        }
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
        if (rc == -11) {
            fprintf(stderr, "node not synced yet\n");
        } else {
            fprintf(stderr, "node rejected tx (%d)\n", rc);
        }
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
        if (rc == -11) {
            fprintf(stderr, "node not synced yet\n");
        } else {
            fprintf(stderr, "failed to read balance (%d)\n", rc);
        }
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
        int rotate_keys = 0;
        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "--new-keys") == 0) {
                rotate_keys = 1;
            } else {
                usage(argv[0]);
                return 1;
            }
        }
        return cmd_node(0, rotate_keys);
    }
    if (strcmp(cmd, "bootstrap") == 0) {
        int rotate_keys = 0;
        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "--new-keys") == 0) {
                rotate_keys = 1;
            } else {
                usage(argv[0]);
                return 1;
            }
        }
        return cmd_node(1, rotate_keys);
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
