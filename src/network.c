#include <pthread.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/select.h>

#include "network.h"
#include "consensus.h"
#include "blockchain.h"
#include "crypto.h"
#include "txpool.h"
#include "utils.h"
#include "log.h"

#define TX_WIRE_VERSION 1
#define SEEN_TX_CAP 1024

#define NET_PROTO_VERSION 1
#define NET_MSG_HELLO 1
#define NET_MSG_TX 2
#define NET_MSG_BLOCK 3
#define NET_MSG_VOTE 4
#define NET_MSG_STATE_REQ 5
#define NET_MSG_STATE 6
#define NET_MSG_SNAPSHOT_REQ 7
#define NET_MSG_SNAPSHOT 8

#define NET_DEFAULT_PORT 30303
#define NET_DISCOVERY_INTERVAL 5
#define NET_PEER_TIMEOUT 20
#define NET_HELLO_PEERLIST_MAX 8
#define NET_MAX_MSG 4096

#define NET_BASE_HDR_SIZE (1 + 1 + 4 + 8 + 8 + crypto_sign_PUBLICKEYBYTES)
#define NET_SIG_SIZE (crypto_sign_BYTES)
#define NET_HDR_SIZE (NET_BASE_HDR_SIZE + NET_SIG_SIZE)

static pthread_mutex_t PEERS_MTX = PTHREAD_MUTEX_INITIALIZER;
static peer_state* PEERS = NULL;
static int64_t NET_TIME_OFFSET = 0;

static pthread_mutex_t SEEN_MTX = PTHREAD_MUTEX_INITIALIZER;
static seen_tx_entry SEEN_TX[SEEN_TX_CAP];
static size_t SEEN_IDX = 0;

static int NET_SOCK = -1;
static uint16_t NET_PORT = NET_DEFAULT_PORT;
static pthread_t NET_THREAD;
static volatile int NET_RUNNING = 0;

static pthread_mutex_t ID_MTX = PTHREAD_MUTEX_INITIALIZER;
static uint64_t LOCAL_NODE_ID = 0;
static pub_key_t LOCAL_PUB_KEY = { 0 };
static priv_key_t LOCAL_PRIV_KEY = { 0 };
static int HAS_LOCAL_PUB = 0;
static int HAS_LOCAL_PRIV = 0;

typedef struct {
    uint64_t chain_id;
    pub_key_t genesis_pub;
    uint64_t height;
    uint64_t tip_id;
    uint64_t last_seen;
    int has_state;
    uint8_t snapshot[NET_MAX_MSG];
    uint32_t snapshot_len;
    uint64_t snapshot_last_seen;
} chain_state_view;

static pthread_mutex_t STATE_MTX = PTHREAD_MUTEX_INITIALIZER;
static chain_state_view CHAIN_VIEW;

static void chain_view_reset_locked(void) {
    memset(&CHAIN_VIEW, 0, sizeof(CHAIN_VIEW));
}

static void chain_view_set_state_locked(uint64_t chain_id, const uint8_t* genesis_pub,
                                       uint64_t height, uint64_t tip_id, uint64_t now) {
    CHAIN_VIEW.chain_id = chain_id;
    if (genesis_pub) memcpy(CHAIN_VIEW.genesis_pub, genesis_pub, crypto_sign_PUBLICKEYBYTES);
    CHAIN_VIEW.height = height;
    CHAIN_VIEW.tip_id = tip_id;
    CHAIN_VIEW.last_seen = now;
    CHAIN_VIEW.has_state = 1;
}

static void chain_view_set_snapshot_locked(const uint8_t* data, uint32_t len, uint64_t now) {
    if (!data || len == 0 || len > NET_MAX_MSG) return;
    memcpy(CHAIN_VIEW.snapshot, data, len);
    CHAIN_VIEW.snapshot_len = len;
    CHAIN_VIEW.snapshot_last_seen = now;
}

static void tx_free_account_nodes(account_list_node* head) {
    while (head) {
        account_list_node* next = head->next;
        head->acc = NULL;
        head->next = NULL;
        free(head);
        head = next;
    }
}

static void tx_clear_partial(tx* t) {
    if (!t) return;
    if (t->data) {
        free(t->data);
        t->data = NULL;
    }
    if (t->accounts) {
        tx_free_account_nodes(t->accounts);
        t->accounts = NULL;
    }
    t->signer = NULL;
}

static size_t tx_wire_size(const tx* transaction) {
    size_t data_len = 0;
    if (transaction && transaction->data) data_len = transaction->data->data_len;
    return 1 + crypto_sign_BYTES + 8 + 1 + 4 +
        ((size_t)transaction->accounts_num * crypto_sign_PUBLICKEYBYTES) +
        4 + data_len + 1;
}

static uint64_t peer_id_from_addr(uint32_t ip_be, uint16_t port) {
    uint64_t id = ((uint64_t)ip_be << 16) | (uint64_t)port;
    return (id == 0) ? 1 : id;
}

static int pubkey_is_zero(const uint8_t* pub_key) {
    if (!pub_key) return 1;
    for (size_t i = 0; i < crypto_sign_PUBLICKEYBYTES; i++) {
        if (pub_key[i] != 0) return 0;
    }
    return 1;
}

static void local_identity_snapshot(pub_key_t out_pub_key, uint64_t* out_node_id,
                                    int* out_has_pub, int* out_has_priv) {
    pthread_mutex_lock(&ID_MTX);
    if (out_pub_key) memcpy(out_pub_key, LOCAL_PUB_KEY, crypto_sign_PUBLICKEYBYTES);
    if (out_node_id) *out_node_id = LOCAL_NODE_ID;
    if (out_has_pub) *out_has_pub = HAS_LOCAL_PUB;
    if (out_has_priv) *out_has_priv = HAS_LOCAL_PRIV;
    pthread_mutex_unlock(&ID_MTX);
}

static void ensure_local_identity(void) {
    pthread_mutex_lock(&ID_MTX);
    if (LOCAL_NODE_ID == 0) {
        randombytes_buf(&LOCAL_NODE_ID, sizeof(LOCAL_NODE_ID));
        if (LOCAL_NODE_ID == 0) LOCAL_NODE_ID = 1;
    }
    pthread_mutex_unlock(&ID_MTX);
}

static peer_state* peer_find_by_id_locked(uint64_t peer_id) {
    peer_state* cur = PEERS;
    while (cur) {
        if (cur->id == peer_id) return cur;
        cur = cur->next;
    }
    return NULL;
}

static peer_state* peer_find_by_addr_locked(uint32_t ip_be, uint16_t port) {
    peer_state* cur = PEERS;
    while (cur) {
        if (cur->ip_be == ip_be && cur->port == port) return cur;
        cur = cur->next;
    }
    return NULL;
}

static peer_state* peer_find_by_pub_locked(const uint8_t* peer_pub_key) {
    peer_state* cur = PEERS;
    while (cur) {
        if (memcmp(cur->pub_key, peer_pub_key, crypto_sign_PUBLICKEYBYTES) == 0) {
            return cur;
        }
        cur = cur->next;
    }
    return NULL;
}

static int cmp_i64(const void* a, const void* b) {
    const int64_t aa = *(const int64_t*)a;
    const int64_t bb = *(const int64_t*)b;
    if (aa < bb) return -1;
    if (aa > bb) return 1;
    return 0;
}

static void recompute_time_offset_locked(void) {
    size_t count = 0;
    peer_state* cur = PEERS;
    while (cur) {
        if (cur->online && cur->has_time) count++;
        cur = cur->next;
    }

    if (count == 0) {
        NET_TIME_OFFSET = 0;
        return;
    }

    int64_t* offsets = calloc(count, sizeof(*offsets));
    if (!offsets) {
        NET_TIME_OFFSET = 0;
        return;
    }

    size_t idx = 0;
    cur = PEERS;
    while (cur) {
        if (cur->online && cur->has_time) {
            offsets[idx++] = cur->time_offset;
        }
        cur = cur->next;
    }

    qsort(offsets, count, sizeof(*offsets), cmp_i64);
    NET_TIME_OFFSET = offsets[count / 2];
    free(offsets);
}

static void tx_hash_bytes(const uint8_t* data, size_t len, uint8_t out[32]) {
    crypto_generichash(out, 32, data, len, NULL, 0);
}

static int seen_tx_contains(const uint8_t hash[32]) {
    pthread_mutex_lock(&SEEN_MTX);
    for (size_t i = 0; i < SEEN_TX_CAP; i++) {
        if (SEEN_TX[i].last_seen != 0 &&
            memcmp(SEEN_TX[i].hash, hash, sizeof(SEEN_TX[i].hash)) == 0) {
            pthread_mutex_unlock(&SEEN_MTX);
            return 1;
        }
    }
    pthread_mutex_unlock(&SEEN_MTX);
    return 0;
}

static void seen_tx_add(const uint8_t hash[32]) {
    pthread_mutex_lock(&SEEN_MTX);
    memcpy(SEEN_TX[SEEN_IDX].hash, hash, sizeof(SEEN_TX[SEEN_IDX].hash));
    SEEN_TX[SEEN_IDX].last_seen = network_time_now();
    SEEN_IDX = (SEEN_IDX + 1) % SEEN_TX_CAP;
    pthread_mutex_unlock(&SEEN_MTX);
}

static size_t peer_snapshot(peer_send_target** out) {
    *out = NULL;

    pthread_mutex_lock(&PEERS_MTX);
    size_t count = 0;
    peer_state* cur = PEERS;
    while (cur) {
        if (cur->online && (cur->send_cb || cur->ip_be != 0)) count++;
        cur = cur->next;
    }

    if (count == 0) {
        pthread_mutex_unlock(&PEERS_MTX);
        return 0;
    }

    peer_send_target* targets = calloc(count, sizeof(*targets));
    if (!targets) {
        pthread_mutex_unlock(&PEERS_MTX);
        return 0;
    }

    size_t idx = 0;
    cur = PEERS;
    while (cur) {
        if (cur->online && (cur->send_cb || cur->ip_be != 0)) {
            targets[idx].id = cur->id;
            targets[idx].send_cb = cur->send_cb;
            targets[idx].user_ctx = cur->user_ctx;
            targets[idx].ip_be = cur->ip_be;
            targets[idx].port = cur->port;
            idx++;
        }
        cur = cur->next;
    }
    pthread_mutex_unlock(&PEERS_MTX);

    *out = targets;
    return idx;
}

size_t network_peer_count_online(void) {
    pthread_mutex_lock(&PEERS_MTX);
    size_t count = 0;
    peer_state* cur = PEERS;
    while (cur) {
        if (cur->online) count++;
        cur = cur->next;
    }
    pthread_mutex_unlock(&PEERS_MTX);
    return count;
}

size_t network_peer_snapshot_pubkeys(pub_key_t** out_pub_keys) {
    if (!out_pub_keys) return 0;

    pthread_mutex_lock(&PEERS_MTX);
    size_t count = 0;
    peer_state* cur = PEERS;
    while (cur) {
        if (cur->online && !pubkey_is_zero(cur->pub_key)) count++;
        cur = cur->next;
    }

    if (count == 0) {
        pthread_mutex_unlock(&PEERS_MTX);
        *out_pub_keys = NULL;
        return 0;
    }

    pub_key_t* keys = calloc(count, sizeof(*keys));
    if (!keys) {
        pthread_mutex_unlock(&PEERS_MTX);
        *out_pub_keys = NULL;
        return 0;
    }

    size_t idx = 0;
    cur = PEERS;
    while (cur && idx < count) {
        if (cur->online && !pubkey_is_zero(cur->pub_key)) {
            memcpy(keys[idx], cur->pub_key, crypto_sign_PUBLICKEYBYTES);
            idx++;
        }
        cur = cur->next;
    }
    pthread_mutex_unlock(&PEERS_MTX);

    *out_pub_keys = keys;
    return idx;
}

static int udp_send_raw(uint32_t ip_be, uint16_t port, const uint8_t* data, size_t len) {
    if (NET_SOCK < 0 || ip_be == 0 || port == 0 || !data || len == 0) return -1;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = ip_be;
    addr.sin_port = htons(port);

    ssize_t sent = sendto(NET_SOCK, data, len, 0, (struct sockaddr*)&addr, sizeof(addr));
    return (sent < 0 || (size_t)sent != len) ? -2 : 0;
}

static size_t net_build_packet(uint8_t type, const uint8_t* payload, uint32_t payload_len,
                               uint8_t* out_buf, size_t out_cap) {
    if (!out_buf) return 0;
    if ((size_t)payload_len + NET_HDR_SIZE > out_cap) return 0;

    pub_key_t pub_key;
    uint64_t node_id = 0;
    int has_pub = 0;
    int has_priv = 0;
    local_identity_snapshot(pub_key, &node_id, &has_pub, &has_priv);

    if (!has_pub || !has_priv) return 0;
    if (node_id == 0) node_id = 1;

    out_buf[0] = NET_PROTO_VERSION;
    out_buf[1] = type;
    store_u32_le(&out_buf[2], payload_len);
    store_u64_le(&out_buf[6], node_id);
    store_u64_le(&out_buf[14], (uint64_t)time(NULL));
    memcpy(&out_buf[22], pub_key, crypto_sign_PUBLICKEYBYTES);

    size_t base_len = NET_BASE_HDR_SIZE;
    if (payload_len > 0) {
        if (!payload) return 0;
        memcpy(&out_buf[base_len], payload, payload_len);
    }

    size_t sign_len = base_len + payload_len;
    uint8_t* sig_dst = &out_buf[base_len + payload_len];

    pthread_mutex_lock(&ID_MTX);
    int rc = crypto_sign_detached(sig_dst, NULL, out_buf, sign_len, LOCAL_PRIV_KEY);
    pthread_mutex_unlock(&ID_MTX);
    if (rc != 0) return 0;

    return sign_len + NET_SIG_SIZE;
}

static int send_packet_to_target(const peer_send_target* target,
                                 const uint8_t* packet, size_t packet_len) {
    if (!target || !packet || packet_len == 0) return -1;

    if (target->send_cb) {
        return target->send_cb(packet, packet_len, target->user_ctx);
    }

    if (target->ip_be != 0 && target->port != 0) {
        return udp_send_raw(target->ip_be, target->port, packet, packet_len);
    }

    return -2;
}

static size_t build_hello_payload(uint8_t* out_buf, size_t out_cap) {
    if (!out_buf || out_cap < 8) return 0;

    store_u32_le(&out_buf[0], (uint32_t)NET_PORT);

    size_t count = 0;
    size_t off = 8;

    pthread_mutex_lock(&PEERS_MTX);
    peer_state* cur = PEERS;
    while (cur && count < NET_HELLO_PEERLIST_MAX) {
        if (cur->online && cur->ip_be != 0 && cur->port != 0) {
            if (off + 8 > out_cap) break;
            store_u32_le(&out_buf[off], cur->ip_be);
            store_u32_le(&out_buf[off + 4], (uint32_t)cur->port);
            off += 8;
            count++;
        }
        cur = cur->next;
    }
    pthread_mutex_unlock(&PEERS_MTX);

    store_u32_le(&out_buf[4], (uint32_t)count);
    return off;
}

static int send_hello_addr(uint32_t ip_be, uint16_t port) {
    uint8_t payload[8 + (NET_HELLO_PEERLIST_MAX * 8)];
    size_t payload_len = build_hello_payload(payload, sizeof(payload));
    if (payload_len == 0) return -1;

    uint8_t packet[NET_MAX_MSG];
    size_t packet_len = net_build_packet(NET_MSG_HELLO, payload, (uint32_t)payload_len,
                                         packet, sizeof(packet));
    if (packet_len == 0) return -2;

    return udp_send_raw(ip_be, port, packet, packet_len);
}

static void send_hello_broadcast(void) {
    send_hello_addr(htonl(INADDR_BROADCAST), NET_PORT);
}

static void send_hello_to_peers(void) {
    peer_send_target* targets = NULL;
    size_t count = peer_snapshot(&targets);
    if (count == 0) return;

    for (size_t i = 0; i < count; i++) {
        if (targets[i].ip_be != 0 && targets[i].port != 0) {
            send_hello_addr(targets[i].ip_be, targets[i].port);
        }
    }

    free(targets);
}

static size_t build_state_payload(uint8_t* out, size_t out_cap) {
    if (!out || out_cap < 1 + 8 + crypto_sign_PUBLICKEYBYTES + 8 + 8) return 0;

    pthread_mutex_lock(&CHAIN.mtx);
    uint64_t chain_id = CHAIN.chain_id;
    uint64_t height = CHAIN.height;
    uint64_t tip_id = CHAIN.tip ? CHAIN.tip->id : 0;
    pub_key_t genesis_pub;
    memcpy(genesis_pub, CHAIN.genesis_pub, crypto_sign_PUBLICKEYBYTES);
    pthread_mutex_unlock(&CHAIN.mtx);

    if (chain_id == 0) return 0;

    size_t off = 0;
    out[off++] = 1; /* version */
    store_u64_le(&out[off], chain_id);
    off += 8;
    memcpy(&out[off], genesis_pub, crypto_sign_PUBLICKEYBYTES);
    off += crypto_sign_PUBLICKEYBYTES;
    store_u64_le(&out[off], height);
    off += 8;
    store_u64_le(&out[off], tip_id);
    off += 8;
    return off;
}

static int send_state_to(uint32_t ip_be, uint16_t port) {
    uint8_t payload[1 + 8 + crypto_sign_PUBLICKEYBYTES + 8 + 8];
    size_t payload_len = build_state_payload(payload, sizeof(payload));
    if (payload_len == 0) return -1;

    uint8_t packet[NET_MAX_MSG];
    size_t packet_len = net_build_packet(NET_MSG_STATE, payload, (uint32_t)payload_len,
                                         packet, sizeof(packet));
    if (packet_len == 0) return -2;

    return udp_send_raw(ip_be, port, packet, packet_len);
}

static int send_snapshot_to(uint32_t ip_be, uint16_t port) {
    uint8_t* snapshot = NULL;
    size_t snapshot_len = 0;
    if (blockchain_encode_snapshot(&CHAIN, &snapshot, &snapshot_len) < 0) return -1;

    size_t max_payload = NET_MAX_MSG - NET_HDR_SIZE;
    if (snapshot_len > max_payload) {
        log_warn("snapshot too large (%zu), not sending", snapshot_len);
        free(snapshot);
        return -2;
    }

    uint8_t packet[NET_MAX_MSG];
    size_t packet_len = net_build_packet(NET_MSG_SNAPSHOT, snapshot, (uint32_t)snapshot_len,
                                         packet, sizeof(packet));
    free(snapshot);
    if (packet_len == 0) return -3;

    return udp_send_raw(ip_be, port, packet, packet_len);
}

static void peer_apply_time_locked(peer_state* peer, uint64_t peer_time_unix, uint64_t now) {
    if (!peer) return;
    peer->time_offset = (int64_t)peer_time_unix - (int64_t)now;
    peer->has_time = 1;
    recompute_time_offset_locked();
}

static peer_state* peer_upsert_locked(uint64_t node_id, const uint8_t* pub_key,
                                      uint32_t ip_be, uint16_t port, uint64_t now) {
    peer_state* peer = NULL;

    if (node_id != 0) {
        peer = peer_find_by_id_locked(node_id);
    }
    if (!peer && pub_key) {
        peer = peer_find_by_pub_locked(pub_key);
    }
    if (!peer && ip_be != 0 && port != 0) {
        peer = peer_find_by_addr_locked(ip_be, port);
    }

    if (!peer) {
        peer = calloc(1, sizeof(*peer));
        if (!peer) return NULL;
        peer->id = (node_id != 0) ? node_id : peer_id_from_addr(ip_be, port);
        if (pub_key) memcpy(peer->pub_key, pub_key, crypto_sign_PUBLICKEYBYTES);
        peer->ip_be = ip_be;
        peer->port = port;
        peer->online = 1;
        peer->has_time = 0;
        peer->time_offset = 0;
        peer->last_seen = now;
        peer->next = PEERS;
        PEERS = peer;
        if (ip_be != 0 && port != 0) {
            char ip_str[INET_ADDRSTRLEN];
            if (inet_ntop(AF_INET, &ip_be, ip_str, sizeof(ip_str))) {
                log_info("peer discovered %s:%u", ip_str, port);
            } else {
                log_info("peer discovered id=%llu", (unsigned long long)peer->id);
            }
        } else {
            log_info("peer discovered id=%llu", (unsigned long long)peer->id);
        }
        return peer;
    }

    if (node_id != 0 && peer->id != node_id) {
        peer->id = node_id;
    }
    if (pub_key) {
        memcpy(peer->pub_key, pub_key, crypto_sign_PUBLICKEYBYTES);
    }
    if (ip_be != 0) peer->ip_be = ip_be;
    if (port != 0) peer->port = port;
    peer->online = 1;
    peer->last_seen = now;
    return peer;
}

static size_t process_hello_payload(peer_state* peer, const uint8_t* payload, uint32_t payload_len,
                                    uint32_t src_ip_be, uint16_t src_port,
                                    uint32_t out_ips[], uint16_t out_ports[], size_t out_cap) {
    if (!payload || payload_len < 8) return 0;

    uint32_t listen_port = load_u32_le(&payload[0]);
    uint32_t peer_count = load_u32_le(&payload[4]);

    (void)listen_port;

    if (peer) {
        /* NAT-friendly: use observed UDP source port, not the advertised listen_port. */
        peer->port = src_port;
        if (peer->ip_be == 0) peer->ip_be = src_ip_be;
    }

    size_t off = 8;
    size_t out_count = 0;
    for (uint32_t i = 0; i < peer_count && out_count < out_cap; i++) {
        if (off + 8 > payload_len) break;
        uint32_t ip_be = load_u32_le(&payload[off]);
        uint32_t port = load_u32_le(&payload[off + 4]);
        off += 8;

        if (ip_be == 0 || port == 0 || port > 65535) continue;

        out_ips[out_count] = ip_be;
        out_ports[out_count] = (uint16_t)port;
        out_count++;
    }

    return out_count;
}

static void process_packet(const uint8_t* data, size_t len, uint32_t src_ip_be, uint16_t src_port) {
    if (!data || len < NET_HDR_SIZE) return;

    if (data[0] != NET_PROTO_VERSION) return;

    uint8_t type = data[1];
    uint32_t payload_len = load_u32_le(&data[2]);
    if (payload_len > (NET_MAX_MSG - NET_HDR_SIZE)) return;
    if ((size_t)payload_len + NET_HDR_SIZE != len) return;

    uint64_t node_id = load_u64_le(&data[6]);
    uint64_t peer_time = load_u64_le(&data[14]);
    const uint8_t* peer_pub = &data[22];
    if (pubkey_is_zero(peer_pub)) return;

    const uint8_t* sig = &data[NET_BASE_HDR_SIZE + payload_len];
    size_t sign_len = NET_BASE_HDR_SIZE + payload_len;
    if (crypto_sign_verify_detached(sig, data, sign_len, peer_pub) != 0) return;

    uint8_t hash[32];
    crypto_generichash(hash, sizeof(hash), peer_pub, crypto_sign_PUBLICKEYBYTES, NULL, 0);
    uint64_t expected_id = load_u64_le(hash);
    if (expected_id == 0) expected_id = 1;
    if (node_id != expected_id) return;

    uint64_t local_id = 0;
    local_identity_snapshot(NULL, &local_id, NULL, NULL);
    if (local_id != 0 && node_id == local_id) return;

    uint64_t now = (uint64_t)time(NULL);

    pthread_mutex_lock(&PEERS_MTX);
    peer_state* peer = peer_upsert_locked(node_id, peer_pub, src_ip_be, src_port, now);
    if (peer) {
        peer_apply_time_locked(peer, peer_time, now);
    }
    pthread_mutex_unlock(&PEERS_MTX);

    const uint8_t* payload = &data[NET_BASE_HDR_SIZE];

    if (type == NET_MSG_HELLO) {
        uint32_t ips[NET_HELLO_PEERLIST_MAX];
        uint16_t ports[NET_HELLO_PEERLIST_MAX];
        size_t count = 0;

        pthread_mutex_lock(&PEERS_MTX);
        peer_state* cur = peer_find_by_id_locked(node_id);
        if (!cur) cur = peer_find_by_addr_locked(src_ip_be, src_port);
        count = process_hello_payload(cur, payload, payload_len, src_ip_be, src_port,
                                      ips, ports, NET_HELLO_PEERLIST_MAX);
        pthread_mutex_unlock(&PEERS_MTX);

        send_hello_addr(src_ip_be, src_port);
        for (size_t i = 0; i < count; i++) {
            send_hello_addr(ips[i], ports[i]);
        }
        return;
    }

    if (type == NET_MSG_TX) {
        handle_incoming_tx((uint8_t*)payload, payload_len);
        return;
    }

    if (type == NET_MSG_BLOCK) {
        consensus_handle_block(payload, payload_len);
        return;
    }

    if (type == NET_MSG_VOTE) {
        consensus_handle_vote(payload, payload_len);
        return;
    }

    if (type == NET_MSG_STATE_REQ) {
        send_state_to(src_ip_be, src_port);
        return;
    }

    if (type == NET_MSG_STATE) {
        if (payload_len < 1 + 8 + crypto_sign_PUBLICKEYBYTES + 8 + 8) return;
        size_t off = 0;
        uint8_t ver = payload[off++];
        if (ver != 1) return;
        uint64_t chain_id = load_u64_le(&payload[off]);
        off += 8;
        const uint8_t* genesis_pub = &payload[off];
        off += crypto_sign_PUBLICKEYBYTES;
        uint64_t height = load_u64_le(&payload[off]);
        off += 8;
        uint64_t tip_id = load_u64_le(&payload[off]);
        off += 8;
        if (off != payload_len) return;
        if (chain_id == 0) return;

        pthread_mutex_lock(&STATE_MTX);
        chain_view_set_state_locked(chain_id, genesis_pub, height, tip_id, now);
        pthread_mutex_unlock(&STATE_MTX);

        pthread_mutex_lock(&CHAIN.mtx);
        uint64_t local_chain_id = CHAIN.chain_id;
        uint64_t local_height = CHAIN.height;
        pthread_mutex_unlock(&CHAIN.mtx);

        if (local_chain_id == 0) {
            /* Chain not initialized yet: store state so main can adopt it. */
            return;
        }
        if (local_chain_id == chain_id && height > local_height) {
            network_request_snapshot();
        }
        return;
    }

    if (type == NET_MSG_SNAPSHOT_REQ) {
        send_snapshot_to(src_ip_be, src_port);
        return;
    }

    if (type == NET_MSG_SNAPSHOT) {
        pthread_mutex_lock(&STATE_MTX);
        chain_view_set_snapshot_locked(payload, payload_len, now);
        pthread_mutex_unlock(&STATE_MTX);

        /* Apply snapshot live (best-effort) to keep nodes synced. */
        if (payload_len >= 1 + 8 + crypto_sign_PUBLICKEYBYTES + 8 + 8 + 4) {
            uint64_t snap_chain_id = load_u64_le(&payload[1]);
            const uint8_t* snap_genesis = &payload[1 + 8];

            pthread_mutex_lock(&CHAIN.mtx);
            uint64_t local_chain_id = CHAIN.chain_id;
            pthread_mutex_unlock(&CHAIN.mtx);

            if (local_chain_id == 0) {
                blockchain_accept_remote_chain_state(&CHAIN, snap_chain_id, snap_genesis);
            }
            pthread_mutex_lock(&CHAIN.mtx);
            uint64_t post_chain_id = CHAIN.chain_id;
            pthread_mutex_unlock(&CHAIN.mtx);

            if (post_chain_id == snap_chain_id) {
                if (blockchain_apply_snapshot(&CHAIN, payload, payload_len) == 0) {
                    pthread_mutex_lock(&CHAIN.mtx);
                    uint64_t h = CHAIN.height;
                    pthread_mutex_unlock(&CHAIN.mtx);
                    log_info("snapshot synced height=%llu", (unsigned long long)h);
                }
            }
        }
        return;
    }
}

static void peer_prune_timeouts(uint64_t now) {
    pthread_mutex_lock(&PEERS_MTX);
    peer_state* cur = PEERS;
    while (cur) {
        if (cur->online && now > cur->last_seen &&
            (now - cur->last_seen) > NET_PEER_TIMEOUT) {
            cur->online = 0;
        }
        cur = cur->next;
    }
    recompute_time_offset_locked();
    pthread_mutex_unlock(&PEERS_MTX);
}

static void* network_thread(void* arg) {
    (void)arg;

    uint64_t last_hello = 0;
    uint64_t last_state_req = 0;

    while (NET_RUNNING) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(NET_SOCK, &rfds);

        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 500000;

        int rc = select(NET_SOCK + 1, &rfds, NULL, NULL, &tv);
        if (rc > 0 && FD_ISSET(NET_SOCK, &rfds)) {
            uint8_t buf[NET_MAX_MSG];
            struct sockaddr_in src;
            socklen_t src_len = sizeof(src);

            ssize_t n = recvfrom(NET_SOCK, buf, sizeof(buf), 0, (struct sockaddr*)&src, &src_len);
            if (n > 0) {
                uint32_t src_ip_be = src.sin_addr.s_addr;
                uint16_t src_port = ntohs(src.sin_port);
                process_packet(buf, (size_t)n, src_ip_be, src_port);
            }
        }

        uint64_t now = (uint64_t)time(NULL);
        if (last_hello == 0 || now - last_hello >= NET_DISCOVERY_INTERVAL) {
            send_hello_broadcast();
            send_hello_to_peers();
            last_hello = now;
        }

        if (last_state_req == 0 || now - last_state_req >= 3) {
            network_request_chain_state();
            last_state_req = now;
        }

        peer_prune_timeouts(now);
    }

    return NULL;
}

static int network_broadcast_payload(uint8_t msg_type, const uint8_t* data, size_t len) {
    if (!data || len == 0) return -1;

    uint8_t packet[NET_MAX_MSG];
    size_t packet_len = net_build_packet(msg_type, data, (uint32_t)len, packet, sizeof(packet));
    if (packet_len == 0) return -2;

    peer_send_target* targets = NULL;
    size_t count = peer_snapshot(&targets);
    if (count == 0) return 0;

    int sent = 0;
    for (size_t i = 0; i < count; i++) {
        int rc = send_packet_to_target(&targets[i], packet, packet_len);
        if (rc < 0) {
            network_peer_set_online(targets[i].id, 0);
        } else {
            network_peer_set_online(targets[i].id, 1);
            sent++;
        }
    }

    free(targets);
    return sent;
}

static int network_broadcast_encoded(const uint8_t* data, size_t len) {
    return network_broadcast_payload(NET_MSG_TX, data, len);
}

int network_broadcast_block(const uint8_t* data, size_t len) {
    return network_broadcast_payload(NET_MSG_BLOCK, data, len);
}

int network_broadcast_vote(const uint8_t* data, size_t len) {
    return network_broadcast_payload(NET_MSG_VOTE, data, len);
}

int network_request_chain_state(void) {
    uint8_t payload[1] = { 1 };
    return network_broadcast_payload(NET_MSG_STATE_REQ, payload, sizeof(payload));
}

int network_request_snapshot(void) {
    uint8_t payload[1] = { 1 };
    return network_broadcast_payload(NET_MSG_SNAPSHOT_REQ, payload, sizeof(payload));
}

int network_init(void) {
    ensure_local_identity();
    int has_priv = 0;
    local_identity_snapshot(NULL, NULL, NULL, &has_priv);
    if (!has_priv) return -10;

    const char* port_env = getenv("BC_PORT");
    if (port_env && *port_env) {
        char* endptr = NULL;
        long port = strtol(port_env, &endptr, 10);
        if (endptr != port_env && port > 0 && port <= 65535) {
            NET_PORT = (uint16_t)port;
        }
    }

    NET_SOCK = socket(AF_INET, SOCK_DGRAM, 0);
    if (NET_SOCK < 0) return -1;

    int opt = 1;
    setsockopt(NET_SOCK, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(NET_SOCK, SOL_SOCKET, SO_BROADCAST, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(NET_PORT);

    if (bind(NET_SOCK, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(NET_SOCK);
        NET_SOCK = -1;
        return -2;
    }

    int flags = fcntl(NET_SOCK, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(NET_SOCK, F_SETFL, flags | O_NONBLOCK);
    }

    pthread_mutex_lock(&SEEN_MTX);
    memset(SEEN_TX, 0, sizeof(SEEN_TX));
    SEEN_IDX = 0;
    pthread_mutex_unlock(&SEEN_MTX);

    pthread_mutex_lock(&PEERS_MTX);
    NET_TIME_OFFSET = 0;
    pthread_mutex_unlock(&PEERS_MTX);

    pthread_mutex_lock(&STATE_MTX);
    chain_view_reset_locked();
    pthread_mutex_unlock(&STATE_MTX);

    NET_RUNNING = 1;
    if (pthread_create(&NET_THREAD, NULL, network_thread, NULL) != 0) {
        NET_RUNNING = 0;
        close(NET_SOCK);
        NET_SOCK = -1;
        return -3;
    }

    send_hello_broadcast();
    send_hello_to_peers();

    const char* seeds = getenv("BC_SEEDS");
    if (seeds && *seeds) {
        char* tmp = strdup(seeds);
        if (tmp) {
            char* saveptr = NULL;
            char* tok = strtok_r(tmp, ",", &saveptr);
            while (tok) {
                char* sep = strrchr(tok, ':');
                if (sep) {
                    *sep = '\0';
                    const char* ip = tok;
                    const char* port_str = sep + 1;
                    char* endptr = NULL;
                    long port = strtol(port_str, &endptr, 10);
                    if (endptr != port_str && port > 0 && port <= 65535) {
                        network_peer_add_addr(ip, (uint16_t)port);
                    }
                }
                tok = strtok_r(NULL, ",", &saveptr);
            }
            free(tmp);
        }
    }

    return 0;
}

void network_shutdown(void) {
    if (NET_RUNNING) {
        NET_RUNNING = 0;
        pthread_join(NET_THREAD, NULL);
    }
    if (NET_SOCK >= 0) {
        close(NET_SOCK);
        NET_SOCK = -1;
    }

    pthread_mutex_lock(&PEERS_MTX);
    peer_state* cur = PEERS;
    while (cur) {
        peer_state* next = cur->next;
        cur->next = NULL;
        free(cur);
        cur = next;
    }
    PEERS = NULL;
    pthread_mutex_unlock(&PEERS_MTX);

    pthread_mutex_lock(&STATE_MTX);
    chain_view_reset_locked();
    pthread_mutex_unlock(&STATE_MTX);
}

int network_set_identity(const pub_key_t pub_key, const priv_key_t priv_key) {
    if (!pub_key || !priv_key) return -1;

    pthread_mutex_lock(&ID_MTX);
    memcpy(LOCAL_PUB_KEY, pub_key, crypto_sign_PUBLICKEYBYTES);
    memcpy(LOCAL_PRIV_KEY, priv_key, crypto_sign_SECRETKEYBYTES);
    HAS_LOCAL_PUB = 1;
    HAS_LOCAL_PRIV = 1;

    uint8_t hash[32];
    crypto_generichash(hash, sizeof(hash), pub_key, crypto_sign_PUBLICKEYBYTES, NULL, 0);
    LOCAL_NODE_ID = load_u64_le(hash);
    if (LOCAL_NODE_ID == 0) LOCAL_NODE_ID = 1;
    pthread_mutex_unlock(&ID_MTX);

    return 0;
}

uint16_t network_listen_port(void) {
    return NET_PORT;
}

int network_peer_add_addr(const char* ip, uint16_t port) {
    if (!ip || port == 0) return -1;

    struct in_addr addr;
    if (inet_pton(AF_INET, ip, &addr) != 1) return -2;

    uint32_t ip_be = addr.s_addr;
    uint64_t now = (uint64_t)time(NULL);

    pthread_mutex_lock(&PEERS_MTX);
    peer_state* peer = peer_upsert_locked(peer_id_from_addr(ip_be, port), NULL, ip_be, port, now);
    if (!peer) {
        pthread_mutex_unlock(&PEERS_MTX);
        return -3;
    }
    peer->online = 1;
    peer->last_seen = now;
    pthread_mutex_unlock(&PEERS_MTX);

    send_hello_addr(ip_be, port);
    return 0;
}

int network_peer_add(uint64_t peer_id, const pub_key_t peer_pub_key, peer_send_fn send_cb, void* user_ctx) {
    if (peer_id == 0) return -1;

    pthread_mutex_lock(&PEERS_MTX);
    peer_state* existing = peer_find_by_id_locked(peer_id);
    if (existing) {
        if (peer_pub_key) memcpy(existing->pub_key, peer_pub_key, crypto_sign_PUBLICKEYBYTES);
        existing->send_cb = send_cb;
        existing->user_ctx = user_ctx;
        existing->online = 1;
        existing->last_seen = (uint64_t)time(NULL);
        pthread_mutex_unlock(&PEERS_MTX);
        return 0;
    }

    peer_state* p = calloc(1, sizeof(*p));
    if (!p) {
        pthread_mutex_unlock(&PEERS_MTX);
        return -2;
    }

    p->id = peer_id;
    if (peer_pub_key) memcpy(p->pub_key, peer_pub_key, crypto_sign_PUBLICKEYBYTES);
    p->online = 1;
    p->has_time = 0;
    p->time_offset = 0;
    p->last_seen = (uint64_t)time(NULL);
    p->send_cb = send_cb;
    p->user_ctx = user_ctx;
    p->ip_be = 0;
    p->port = 0;

    p->next = PEERS;
    PEERS = p;
    pthread_mutex_unlock(&PEERS_MTX);
    return 0;
}

int network_peer_remove(uint64_t peer_id) {
    pthread_mutex_lock(&PEERS_MTX);
    peer_state* prev = NULL;
    peer_state* cur = PEERS;
    while (cur) {
        if (cur->id == peer_id) {
            if (prev) {
                prev->next = cur->next;
            } else {
                PEERS = cur->next;
            }
            cur->next = NULL;
            free(cur);
            recompute_time_offset_locked();
            pthread_mutex_unlock(&PEERS_MTX);
            return 0;
        }
        prev = cur;
        cur = cur->next;
    }
    pthread_mutex_unlock(&PEERS_MTX);
    return -1;
}

int network_peer_set_online(uint64_t peer_id, int online) {
    pthread_mutex_lock(&PEERS_MTX);
    peer_state* cur = peer_find_by_id_locked(peer_id);
    if (!cur) {
        pthread_mutex_unlock(&PEERS_MTX);
        return -1;
    }
    cur->online = online ? 1 : 0;
    if (online) cur->last_seen = (uint64_t)time(NULL);
    pthread_mutex_unlock(&PEERS_MTX);
    return 0;
}

int network_peer_update_time(uint64_t peer_id, uint64_t peer_time_unix) {
    pthread_mutex_lock(&PEERS_MTX);
    peer_state* cur = peer_find_by_id_locked(peer_id);
    if (!cur) {
        pthread_mutex_unlock(&PEERS_MTX);
        return -1;
    }

    int64_t now = (int64_t)time(NULL);
    cur->time_offset = (int64_t)peer_time_unix - now;
    cur->has_time = 1;
    cur->online = 1;
    cur->last_seen = (uint64_t)now;

    recompute_time_offset_locked();
    pthread_mutex_unlock(&PEERS_MTX);
    return 0;
}

uint64_t network_time_now(void) {
    pthread_mutex_lock(&PEERS_MTX);
    int64_t offset = NET_TIME_OFFSET;
    pthread_mutex_unlock(&PEERS_MTX);

    int64_t now = (int64_t)time(NULL);
    int64_t adjusted = now + offset;
    if (adjusted < 0) adjusted = 0;
    return (uint64_t)adjusted;
}

int network_get_remote_chain_state(uint64_t* out_chain_id, pub_key_t out_genesis_pub,
                                  uint64_t* out_height, uint64_t* out_tip_id) {
    pthread_mutex_lock(&STATE_MTX);
    if (!CHAIN_VIEW.has_state) {
        pthread_mutex_unlock(&STATE_MTX);
        return -1;
    }
    if (out_chain_id) *out_chain_id = CHAIN_VIEW.chain_id;
    if (out_genesis_pub) memcpy(out_genesis_pub, CHAIN_VIEW.genesis_pub, crypto_sign_PUBLICKEYBYTES);
    if (out_height) *out_height = CHAIN_VIEW.height;
    if (out_tip_id) *out_tip_id = CHAIN_VIEW.tip_id;
    pthread_mutex_unlock(&STATE_MTX);
    return 0;
}

int network_get_remote_snapshot(uint8_t* out, size_t* inout_len) {
    if (!inout_len) return -1;

    pthread_mutex_lock(&STATE_MTX);
    if (CHAIN_VIEW.snapshot_len == 0) {
        pthread_mutex_unlock(&STATE_MTX);
        return -2;
    }
    if (!out) {
        *inout_len = CHAIN_VIEW.snapshot_len;
        pthread_mutex_unlock(&STATE_MTX);
        return 0;
    }
    if (*inout_len < CHAIN_VIEW.snapshot_len) {
        *inout_len = CHAIN_VIEW.snapshot_len;
        pthread_mutex_unlock(&STATE_MTX);
        return -3;
    }
    memcpy(out, CHAIN_VIEW.snapshot, CHAIN_VIEW.snapshot_len);
    *inout_len = CHAIN_VIEW.snapshot_len;
    pthread_mutex_unlock(&STATE_MTX);
    return 0;
}

int encode_tx (tx* transaction, uint8_t encoded_tx[], size_t* raw_tx_len) {
    if (!transaction || !raw_tx_len) return -1;
    if (!transaction->accounts || transaction->accounts_num == 0) return -2;

    size_t data_len = 0;
    if (transaction->data) {
        if (transaction->data->data_len > TX_DATA_MAX_SIZE) return -3;
        data_len = transaction->data->data_len;
    }

    size_t needed = tx_wire_size(transaction);
    if (!encoded_tx) {
        *raw_tx_len = needed;
        return 0;
    }
    if (*raw_tx_len < needed) {
        *raw_tx_len = needed;
        return -4;
    }

    size_t off = 0;
    encoded_tx[off++] = TX_WIRE_VERSION;
    memcpy(&encoded_tx[off], transaction->signature, crypto_sign_BYTES);
    off += crypto_sign_BYTES;

    store_u64_le(&encoded_tx[off], transaction->expire);
    off += 8;

    encoded_tx[off++] = transaction->function_id;
    store_u32_le(&encoded_tx[off], transaction->accounts_num);
    off += 4;

    account_list_node* cur = transaction->accounts;
    uint32_t count = 0;
    while (cur && count < transaction->accounts_num) {
        if (!cur->acc) return -5;
        memcpy(&encoded_tx[off], cur->acc->pub_key, crypto_sign_PUBLICKEYBYTES);
        off += crypto_sign_PUBLICKEYBYTES;
        cur = cur->next;
        count++;
    }
    if (count != transaction->accounts_num || cur != NULL) return -6;

    store_u32_le(&encoded_tx[off], (uint32_t)data_len);
    off += 4;
    if (data_len > 0) {
        memcpy(&encoded_tx[off], transaction->data->data, data_len);
        off += data_len;
    }

    encoded_tx[off++] = transaction->confirmed;
    *raw_tx_len = off;
    return 0;
}

int decode_tx (tx* transaction, uint8_t encoded_tx[], size_t raw_tx_len) {
    if (!transaction || !encoded_tx || raw_tx_len == 0) return -1;

    const size_t min_len = 1 + crypto_sign_BYTES + 8 + 1 + 4 + 4 + 1;
    if (raw_tx_len < min_len) return -2;

    memset(transaction, 0, sizeof(*transaction));

    size_t off = 0;
    uint8_t version = encoded_tx[off++];
    if (version != TX_WIRE_VERSION) return -3;

    if (off + crypto_sign_BYTES > raw_tx_len) return -4;
    memcpy(transaction->signature, &encoded_tx[off], crypto_sign_BYTES);
    off += crypto_sign_BYTES;

    if (off + 8 > raw_tx_len) return -5;
    transaction->expire = load_u64_le(&encoded_tx[off]);
    off += 8;

    if (off + 1 > raw_tx_len) return -6;
    transaction->function_id = encoded_tx[off++];

    if (off + 4 > raw_tx_len) return -7;
    uint32_t accounts_num = load_u32_le(&encoded_tx[off]);
    off += 4;
    if (accounts_num == 0) return -8;
    transaction->accounts_num = accounts_num;

    account_list_node* head = NULL;
    account_list_node* tail = NULL;
    for (uint32_t i = 0; i < accounts_num; i++) {
        if (off + crypto_sign_PUBLICKEYBYTES > raw_tx_len) {
            tx_free_account_nodes(head);
            return -9;
        }

        const uint8_t* pub = &encoded_tx[off];
        account* acc = blockchain_get_or_create_account(&CHAIN, (const uint8_t*)pub);
        if (!acc) {
            tx_free_account_nodes(head);
            return -10;
        }

        account_list_node* node = calloc(1, sizeof(*node));
        if (!node) {
            tx_free_account_nodes(head);
            return -11;
        }
        node->acc = acc;
        node->next = NULL;
        if (!head) {
            head = node;
        } else {
            tail->next = node;
        }
        tail = node;
        off += crypto_sign_PUBLICKEYBYTES;
    }

    transaction->accounts = head;
    transaction->signer = head->acc;

    if (off + 4 > raw_tx_len) {
        tx_clear_partial(transaction);
        return -12;
    }
    uint32_t data_len = load_u32_le(&encoded_tx[off]);
    off += 4;
    if (data_len > TX_DATA_MAX_SIZE) {
        tx_clear_partial(transaction);
        return -13;
    }
    if (off + data_len > raw_tx_len) {
        tx_clear_partial(transaction);
        return -14;
    }

    if (data_len > 0) {
        tx_data* data = malloc(sizeof(*data));
        if (!data) {
            tx_clear_partial(transaction);
            return -15;
        }
        data->data_len = data_len;
        memcpy(data->data, &encoded_tx[off], data_len);
        transaction->data = data;
        off += data_len;
    }

    if (off + 1 > raw_tx_len) {
        tx_clear_partial(transaction);
        return -16;
    }
    transaction->confirmed = encoded_tx[off++];

    if (off != raw_tx_len) {
        tx_clear_partial(transaction);
        return -17;
    }

    return 0;
}

int share_tx_with_peer (tx* transaction, pub_key_t peer_pub_key) {
    if (!transaction) return -1;

    size_t raw_tx_len = 0;
    if (encode_tx(transaction, NULL, &raw_tx_len) < 0) return -2;

    uint8_t* encoded_tx = malloc(raw_tx_len);
    if (!encoded_tx) return -3;

    size_t cap = raw_tx_len;
    if (encode_tx(transaction, encoded_tx, &cap) < 0) {
        free(encoded_tx);
        return -4;
    }

    uint8_t hash[32];
    tx_hash_bytes(encoded_tx, cap, hash);
    if (!seen_tx_contains(hash)) seen_tx_add(hash);

    int sent = 0;
    if (!peer_pub_key) {
        sent = network_broadcast_encoded(encoded_tx, cap);
    } else {
        uint8_t packet[NET_MAX_MSG];
        size_t packet_len = net_build_packet(NET_MSG_TX, encoded_tx, (uint32_t)cap,
                                             packet, sizeof(packet));
        if (packet_len == 0) {
            free(encoded_tx);
            return -5;
        }

        pthread_mutex_lock(&PEERS_MTX);
        peer_state* peer = peer_find_by_pub_locked(peer_pub_key);
        if (peer && peer->online) {
            peer_send_target target = {
                .id = peer->id,
                .send_cb = peer->send_cb,
                .user_ctx = peer->user_ctx,
                .ip_be = peer->ip_be,
                .port = peer->port
            };
            pthread_mutex_unlock(&PEERS_MTX);
            int rc = send_packet_to_target(&target, packet, packet_len);
            if (rc < 0) {
                network_peer_set_online(target.id, 0);
            } else {
                network_peer_set_online(target.id, 1);
                sent = 1;
            }
        } else {
            pthread_mutex_unlock(&PEERS_MTX);
        }
    }

    free(encoded_tx);
    return sent;
}

int handle_incoming_tx(uint8_t encoded_tx[], size_t raw_tx_len) {
    if (!encoded_tx || raw_tx_len == 0) return -1;

    uint8_t hash[32];
    tx_hash_bytes(encoded_tx, raw_tx_len, hash);
    if (seen_tx_contains(hash)) return 0;
    seen_tx_add(hash);

    tx* t = calloc(1, sizeof(tx));
    if (!t) return -2;
    if (decode_tx(t, encoded_tx, raw_tx_len) < 0) {
        free(t);
        return -3;
    }
    if (verify_tx(t) < 0) {
        tx_clear_partial(t);
        free(t);
        return -4;
    }
    if (tx_pool_push(t) < 0) {
        tx_clear_partial(t);
        free(t);
        return -5;
    }

    {
        char short_hash[17];
        if (bytes_to_hex(hash, 8, short_hash, sizeof(short_hash)) == 0) {
            log_info("tx accepted hash=%s", short_hash);
        } else {
            log_info("tx accepted");
        }
    }

    network_broadcast_encoded(encoded_tx, raw_tx_len);
    return 0;
}
