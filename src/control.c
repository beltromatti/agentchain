#include <arpa/inet.h>
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/types.h>
#include <unistd.h>

#include "control.h"
#include "blockchain.h"
#include "log.h"
#include "network.h"
#include "utils.h"

#define CTL_VERSION 1
#define CTL_FLAG_RESPONSE 0x80

#define CTL_CMD_PING 1
#define CTL_CMD_SEND_TX 2
#define CTL_CMD_BALANCE 3

#define CTL_DEFAULT_PORT 30304
#define CTL_HEADER_LEN 12
#define CTL_MAX_MSG 8192

static int CTL_SOCK = -1;
static uint16_t CTL_PORT = CTL_DEFAULT_PORT;
static pthread_t CTL_THREAD;
static volatile int CTL_RUNNING = 0;
static blockchain* CTL_CHAIN = NULL;

static uint16_t control_env_port(void) {
    const char* env = getenv("BC_CTL_PORT");
    if (env && *env) {
        char* endptr = NULL;
        long port = strtol(env, &endptr, 10);
        if (endptr != env && port > 0 && port <= 65535) {
            return (uint16_t)port;
        }
    }
    return CTL_DEFAULT_PORT;
}

uint16_t control_port(void) {
    if (CTL_RUNNING) return CTL_PORT;
    return control_env_port();
}

static void build_header(uint8_t* out, uint8_t type, uint32_t req_id, uint32_t payload_len) {
    out[0] = CTL_VERSION;
    out[1] = type;
    out[2] = 0;
    out[3] = 0;
    store_u32_le(&out[4], req_id);
    store_u32_le(&out[8], payload_len);
}

static int parse_header(const uint8_t* buf, size_t len, uint8_t* type,
                        uint32_t* req_id, uint32_t* payload_len) {
    if (!buf || len < CTL_HEADER_LEN) return -1;
    if (buf[0] != CTL_VERSION) return -2;
    if (type) *type = buf[1];
    if (req_id) *req_id = load_u32_le(&buf[4]);
    if (payload_len) *payload_len = load_u32_le(&buf[8]);
    return 0;
}

static void send_response(const struct sockaddr_in* dst, socklen_t dst_len,
                          uint8_t type, uint32_t req_id,
                          const uint8_t* payload, uint32_t payload_len) {
    uint8_t buf[CTL_HEADER_LEN + 256];
    uint8_t* out = buf;
    size_t total = CTL_HEADER_LEN + payload_len;

    if (payload_len > 256) {
        out = malloc(total);
        if (!out) return;
    }

    build_header(out, (uint8_t)(type | CTL_FLAG_RESPONSE), req_id, payload_len);
    if (payload_len > 0 && payload) {
        memcpy(out + CTL_HEADER_LEN, payload, payload_len);
    }

    sendto(CTL_SOCK, out, total, 0, (const struct sockaddr*)dst, dst_len);
    if (out != buf) free(out);
}

static void handle_request(const uint8_t* buf, size_t len,
                           const struct sockaddr_in* src, socklen_t src_len) {
    uint8_t type = 0;
    uint32_t req_id = 0;
    uint32_t payload_len = 0;
    if (parse_header(buf, len, &type, &req_id, &payload_len) < 0) return;
    if (type & CTL_FLAG_RESPONSE) return;
    if (CTL_HEADER_LEN + payload_len > len) return;

    const uint8_t* payload = buf + CTL_HEADER_LEN;
    uint8_t resp[16];
    int32_t status = 0;
    uint32_t resp_len = 0;

    if (type == CTL_CMD_PING) {
        resp_len = 4;
        store_u32_le(resp, (uint32_t)status);
    } else if (type == CTL_CMD_SEND_TX) {
        if (!payload || payload_len == 0) {
            status = -1;
        } else {
            int ready = 0;
            pthread_mutex_lock(&CTL_CHAIN->mtx);
            ready = (CTL_CHAIN->chain_id != 0 && CTL_CHAIN->synced);
            pthread_mutex_unlock(&CTL_CHAIN->mtx);
            if (!ready) {
                status = -11; /* not synced */
            } else {
            status = handle_incoming_tx((uint8_t*)payload, payload_len);
            if (status == 0) {
                log_info("tx received (size=%u)", payload_len);
            }
            }
        }
        resp_len = 4;
        store_u32_le(resp, (uint32_t)status);
    } else if (type == CTL_CMD_BALANCE) {
        if (payload_len != crypto_sign_PUBLICKEYBYTES || !CTL_CHAIN) {
            status = -1;
            resp_len = 4;
            store_u32_le(resp, (uint32_t)status);
        } else {
            int ready = 0;
            pthread_mutex_lock(&CTL_CHAIN->mtx);
            ready = (CTL_CHAIN->chain_id != 0 && CTL_CHAIN->synced);
            pthread_mutex_unlock(&CTL_CHAIN->mtx);
            if (!ready) {
                status = -11;
                resp_len = 4;
                store_u32_le(resp, (uint32_t)status);
            } else {
            account* acc = blockchain_get_account(CTL_CHAIN, payload);
            uint64_t balance = acc ? acc->balance : 0;
            resp_len = 12;
            store_u32_le(resp, (uint32_t)status);
            store_u64_le(resp + 4, balance);
            }
        }
    } else {
        status = -2;
        resp_len = 4;
        store_u32_le(resp, (uint32_t)status);
    }

    send_response(src, src_len, type, req_id, resp, resp_len);
}

static void* control_thread(void* arg) {
    (void)arg;
    uint8_t buf[CTL_MAX_MSG];

    while (CTL_RUNNING) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(CTL_SOCK, &rfds);
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 200 * 1000;

        int rc = select(CTL_SOCK + 1, &rfds, NULL, NULL, &tv);
        if (rc <= 0) continue;

        struct sockaddr_in src;
        socklen_t src_len = sizeof(src);
        ssize_t n = recvfrom(CTL_SOCK, buf, sizeof(buf), 0, (struct sockaddr*)&src, &src_len);
        if (n <= 0) continue;

        handle_request(buf, (size_t)n, &src, src_len);
    }
    return NULL;
}

int control_start(blockchain* bc) {
    if (!bc) return -1;
    if (CTL_RUNNING) return 0;

    CTL_PORT = control_env_port();
    CTL_SOCK = socket(AF_INET, SOCK_DGRAM, 0);
    if (CTL_SOCK < 0) return -2;

    int opt = 1;
    setsockopt(CTL_SOCK, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(CTL_PORT);

    if (bind(CTL_SOCK, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(CTL_SOCK);
        CTL_SOCK = -1;
        return -3;
    }

    CTL_CHAIN = bc;
    CTL_RUNNING = 1;
    if (pthread_create(&CTL_THREAD, NULL, control_thread, NULL) != 0) {
        CTL_RUNNING = 0;
        close(CTL_SOCK);
        CTL_SOCK = -1;
        return -4;
    }

    log_info("control listening on 127.0.0.1:%u", CTL_PORT);
    return 0;
}

void control_stop(void) {
    if (CTL_RUNNING) {
        CTL_RUNNING = 0;
        pthread_join(CTL_THREAD, NULL);
    }
    if (CTL_SOCK >= 0) {
        close(CTL_SOCK);
        CTL_SOCK = -1;
    }
    CTL_CHAIN = NULL;
}

static int control_request(uint8_t type, const uint8_t* payload, uint32_t payload_len,
                           uint8_t* resp, size_t resp_cap, size_t* resp_len) {
    uint16_t port = control_env_port();
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) return -1;

    struct timeval tv;
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    dst.sin_port = htons(port);

    size_t total = CTL_HEADER_LEN + payload_len;
    uint8_t* buf = malloc(total);
    if (!buf) {
        close(sock);
        return -2;
    }

    uint32_t req_id = (uint32_t)randombytes_random();
    build_header(buf, type, req_id, payload_len);
    if (payload_len > 0 && payload) {
        memcpy(buf + CTL_HEADER_LEN, payload, payload_len);
    }

    ssize_t sent = sendto(sock, buf, total, 0, (struct sockaddr*)&dst, sizeof(dst));
    free(buf);
    if (sent < 0) {
        close(sock);
        return -3;
    }

    uint8_t recv_buf[CTL_MAX_MSG];
    struct sockaddr_in src;
    socklen_t src_len = sizeof(src);
    ssize_t n = recvfrom(sock, recv_buf, sizeof(recv_buf), 0, (struct sockaddr*)&src, &src_len);
    close(sock);
    if (n <= 0) return -4;

    uint8_t resp_type = 0;
    uint32_t resp_req = 0;
    uint32_t resp_payload_len = 0;
    if (parse_header(recv_buf, (size_t)n, &resp_type, &resp_req, &resp_payload_len) < 0) return -5;
    if (resp_type != (uint8_t)(type | CTL_FLAG_RESPONSE)) return -6;
    if (resp_req != req_id) return -7;
    if (CTL_HEADER_LEN + resp_payload_len > (size_t)n) return -8;
    if (resp_payload_len > resp_cap) return -9;

    if (resp && resp_payload_len > 0) {
        memcpy(resp, recv_buf + CTL_HEADER_LEN, resp_payload_len);
    }
    if (resp_len) *resp_len = resp_payload_len;
    return 0;
}

int control_send_tx(const uint8_t* data, size_t len) {
    if (!data || len == 0) return -1;
    uint8_t resp[4];
    size_t resp_len = 0;
    int rc = control_request(CTL_CMD_SEND_TX, data, (uint32_t)len, resp, sizeof(resp), &resp_len);
    if (rc < 0) return rc;
    if (resp_len < 4) return -2;
    return (int32_t)load_u32_le(resp);
}

int control_get_balance(const pub_key_t key, uint64_t* out_balance) {
    if (!key || !out_balance) return -1;
    uint8_t resp[12];
    size_t resp_len = 0;
    int rc = control_request(CTL_CMD_BALANCE, key, crypto_sign_PUBLICKEYBYTES, resp, sizeof(resp), &resp_len);
    if (rc < 0) return rc;
    if (resp_len < 4) return -2;
    int32_t status = (int32_t)load_u32_le(resp);
    if (status < 0) return status;
    if (resp_len < 12) return -3;
    *out_balance = load_u64_le(resp + 4);
    return 0;
}

int control_ping(void) {
    uint8_t resp[4];
    size_t resp_len = 0;
    int rc = control_request(CTL_CMD_PING, NULL, 0, resp, sizeof(resp), &resp_len);
    if (rc < 0) return rc;
    if (resp_len < 4) return -2;
    return (int32_t)load_u32_le(resp);
}
