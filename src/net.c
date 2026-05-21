#include "net.h"
#include "portable.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define AC_MAX_PEERS              64
#define AC_DEDUP_CAP              256
#define AC_RECONNECT_BASE_MS      2000
#define AC_PEER_IDLE_MS           30000

/* Per-peer outbound queue: bound the memory we will spend buffering for a
 * single slow peer. When the queue exceeds this, the peer is shut down. */
#define AC_OUT_QUEUE_MAX_BYTES    (4 * 1024 * 1024)   /* 4 MB */
#define AC_OUT_QUEUE_MAX_MSGS     2048

/* -------------------------------------------------------------------------- */
/* Frame I/O.                                                                 */
/* -------------------------------------------------------------------------- */

static int read_full(int fd, void *buf, size_t len) {
    uint8_t *p = (uint8_t *)buf;
    size_t got = 0;
    while (got < len) {
        ssize_t n = ac_sock_recv(fd, p + got, len - got);
        if (n == 0) return -1;
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        got += (size_t)n;
    }
    return 0;
}
static int write_full(int fd, const void *buf, size_t len) {
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

static int frame_send(int fd, uint8_t type, const uint8_t *payload, size_t len) {
    if (len > AC_FRAME_MAX_BYTES - 2) return -1;
    uint8_t hdr[6];
    ac_be32(hdr, (uint32_t)(len + 2));
    hdr[4] = AC_FRAME_VERSION;
    hdr[5] = type;
    if (write_full(fd, hdr, sizeof(hdr)) < 0) return -1;
    if (len > 0 && write_full(fd, payload, len) < 0) return -1;
    return 0;
}

/* Reads one frame. On success, sets *out_type and returns a malloc'd buffer
 * of size *out_len (caller frees). Returns NULL on EOF/error. */
static uint8_t *frame_recv(int fd, uint8_t *out_type, size_t *out_len) {
    uint8_t hdr[6];
    if (read_full(fd, hdr, sizeof(hdr)) < 0) return NULL;
    uint32_t total = ac_rd32(hdr);
    if (total < 2 || total > AC_FRAME_MAX_BYTES) return NULL;
    uint8_t version = hdr[4];
    uint8_t type    = hdr[5];
    if (version != AC_FRAME_VERSION) return NULL;
    size_t plen = total - 2;
    uint8_t *buf = (uint8_t *)malloc(plen > 0 ? plen : 1);
    if (!buf) return NULL;
    if (plen > 0 && read_full(fd, buf, plen) < 0) { free(buf); return NULL; }
    *out_type = type;
    *out_len  = plen;
    return buf;
}

/* -------------------------------------------------------------------------- */
/* Peer slot.                                                                 */
/* -------------------------------------------------------------------------- */

typedef struct outbox_msg {
    uint8_t  type;
    uint8_t *payload;
    size_t   len;
    struct outbox_msg *next;
} outbox_msg_t;

typedef struct {
    bool        in_use;
    bool        reader_started;
    bool        writer_started;
    int         fd;
    pthread_t   reader;
    pthread_t   writer;
    ac_addr_t   peer_id;
    bool        peer_id_known;
    bool        inbound;
    char        host[128];
    uint16_t    port;
    uint64_t    last_seen_ms;
    ac_net_t   *net;

    /* Outbound write queue. The writer thread drains it into the socket;
     * the consensus thread enqueues without blocking. */
    pthread_mutex_t  out_mu;
    pthread_cond_t   out_cv;
    outbox_msg_t    *out_head;
    outbox_msg_t    *out_tail;
    size_t           out_msgs;
    size_t           out_bytes;
    bool             out_closing;       /* writer should drain + exit */
} peer_t;

struct ac_net_s {
    ac_net_config_t cfg;
    int             listen_fd;
    pthread_t       listen_thread;
    pthread_t       connector_thread;
    bool            running;

    pthread_mutex_t peers_mu;
    peer_t          peers[AC_MAX_PEERS];

    /* Dedup of broadcast messages by hash. */
    pthread_mutex_t dedup_mu;
    ac_hash_t       dedup[AC_DEDUP_CAP];
    size_t          dedup_n;
    size_t          dedup_pos;
} ;

/* -------------------------------------------------------------------------- */
/* Dedup.                                                                     */
/* -------------------------------------------------------------------------- */

static bool dedup_has(ac_net_t *n, const ac_hash_t *h) {
    pthread_mutex_lock(&n->dedup_mu);
    bool found = false;
    for (size_t i = 0; i < n->dedup_n; ++i) {
        if (ac_hash_eq(&n->dedup[i], h)) { found = true; break; }
    }
    pthread_mutex_unlock(&n->dedup_mu);
    return found;
}
static void dedup_remember(ac_net_t *n, const ac_hash_t *h) {
    pthread_mutex_lock(&n->dedup_mu);
    n->dedup[n->dedup_pos] = *h;
    n->dedup_pos = (n->dedup_pos + 1) % AC_DEDUP_CAP;
    if (n->dedup_n < AC_DEDUP_CAP) n->dedup_n++;
    pthread_mutex_unlock(&n->dedup_mu);
}

/* -------------------------------------------------------------------------- */
/* Peer-slot management.                                                      */
/* -------------------------------------------------------------------------- */

static peer_t *peer_alloc(ac_net_t *n) {
    pthread_mutex_lock(&n->peers_mu);
    for (size_t i = 0; i < AC_MAX_PEERS; ++i) {
        if (!n->peers[i].in_use) {
            peer_t *p = &n->peers[i];
            memset(p, 0, sizeof(*p));
            p->in_use = true;
            p->fd = -1;
            p->net = n;
            pthread_mutex_init(&p->out_mu, NULL);
            pthread_cond_init (&p->out_cv, NULL);
            pthread_mutex_unlock(&n->peers_mu);
            return p;
        }
    }
    pthread_mutex_unlock(&n->peers_mu);
    return NULL;
}

/* -------------------------------------------------------------------------- */
/* Per-peer outbound queue + writer thread.                                   */
/*                                                                            */
/* The consensus, network, and RPC threads enqueue messages here. A dedicated */
/* writer thread per peer drains the queue into the socket. This decouples    */
/* the producers from a slow peer: a high-latency or wedged peer fills up its */
/* own queue without blocking the rest of the engine. If the queue exceeds    */
/* AC_OUT_QUEUE_MAX_BYTES, the peer is shut down by the enqueue side.         */
/* -------------------------------------------------------------------------- */

static void out_drop_all_locked(peer_t *p) {
    outbox_msg_t *m = p->out_head;
    while (m) {
        outbox_msg_t *next = m->next;
        free(m->payload);
        free(m);
        m = next;
    }
    p->out_head = p->out_tail = NULL;
    p->out_msgs = p->out_bytes = 0;
}

/* Returns 0 on success, -1 if queue is full (peer too slow) or peer is
 * closing. On failure the caller should shut the peer down. */
static int peer_enqueue(peer_t *p, uint8_t type,
                        const uint8_t *payload, size_t len) {
    if (len > AC_FRAME_MAX_BYTES - 2) return -1;
    pthread_mutex_lock(&p->out_mu);
    if (p->out_closing) {
        pthread_mutex_unlock(&p->out_mu);
        return -1;
    }
    if (p->out_msgs + 1 > AC_OUT_QUEUE_MAX_MSGS ||
        p->out_bytes + len > AC_OUT_QUEUE_MAX_BYTES) {
        pthread_mutex_unlock(&p->out_mu);
        return -1;
    }
    outbox_msg_t *m = (outbox_msg_t *)malloc(sizeof(*m));
    if (!m) { pthread_mutex_unlock(&p->out_mu); return -1; }
    m->payload = (uint8_t *)malloc(len > 0 ? len : 1);
    if (!m->payload) { free(m); pthread_mutex_unlock(&p->out_mu); return -1; }
    m->type = type;
    if (len > 0) memcpy(m->payload, payload, len);
    m->len = len;
    m->next = NULL;
    if (p->out_tail) p->out_tail->next = m;
    else             p->out_head = m;
    p->out_tail = m;
    p->out_msgs++;
    p->out_bytes += len;
    pthread_cond_signal(&p->out_cv);
    pthread_mutex_unlock(&p->out_mu);
    return 0;
}

static void *writer_loop(void *arg) {
    peer_t   *p = (peer_t *)arg;
    ac_net_t *n = p->net;
    while (n->running) {
        outbox_msg_t *m = NULL;
        pthread_mutex_lock(&p->out_mu);
        while (n->running && !p->out_closing && !p->out_head) {
            pthread_cond_wait(&p->out_cv, &p->out_mu);
        }
        if (!n->running || p->out_closing) {
            pthread_mutex_unlock(&p->out_mu);
            break;
        }
        m = p->out_head;
        p->out_head = m->next;
        if (!p->out_head) p->out_tail = NULL;
        p->out_msgs--;
        p->out_bytes -= m->len;
        pthread_mutex_unlock(&p->out_mu);

        int fd = p->fd;
        if (fd < 0 || frame_send(fd, m->type, m->payload, m->len) < 0) {
            /* Wedged or closed peer. Signal the reader to exit. */
            if (p->fd >= 0) ac_sock_shutdown(p->fd, SHUT_RDWR);
            free(m->payload); free(m);
            break;
        }
        free(m->payload); free(m);
    }
    /* On exit, drop any remaining messages so memory is reclaimed. */
    pthread_mutex_lock(&p->out_mu);
    p->out_closing = true;
    out_drop_all_locked(p);
    pthread_mutex_unlock(&p->out_mu);
    return NULL;
}

/* Called from inside the reader thread when the connection is finished.
 * Closes the fd and marks the slot for reaping by ac_net_stop (which joins
 * the thread). We deliberately do NOT free the mutex or release the slot
 * here — that happens during teardown to avoid races with ac_net_broadcast
 * iterating the peer list. */
static void peer_close_fd(peer_t *p) {
    if (p->fd >= 0) { ac_sock_close(p->fd); p->fd = -1; }
}

static size_t outbound_count(ac_net_t *n) {
    size_t c = 0;
    pthread_mutex_lock(&n->peers_mu);
    for (size_t i = 0; i < AC_MAX_PEERS; ++i) {
        peer_t *p = &n->peers[i];
        if (p->in_use && !p->inbound && p->fd >= 0) c++;
    }
    pthread_mutex_unlock(&n->peers_mu);
    return c;
}

size_t ac_net_peer_count(ac_net_t *n) {
    size_t c = 0;
    pthread_mutex_lock(&n->peers_mu);
    for (size_t i = 0; i < AC_MAX_PEERS; ++i) {
        if (n->peers[i].in_use && n->peers[i].peer_id_known) c++;
    }
    pthread_mutex_unlock(&n->peers_mu);
    return c;
}

void ac_net_each_peer(ac_net_t *n, ac_net_peer_fn fn, void *ctx) {
    pthread_mutex_lock(&n->peers_mu);
    for (size_t i = 0; i < AC_MAX_PEERS; ++i) {
        peer_t *p = &n->peers[i];
        if (!p->in_use || !p->peer_id_known) continue;
        if (fn(&p->peer_id, p->host, p->port, p->inbound, ctx) != 0) break;
    }
    pthread_mutex_unlock(&n->peers_mu);
}

/* -------------------------------------------------------------------------- */
/* HELLO.                                                                     */
/* -------------------------------------------------------------------------- */

/* HELLO payload (extended in v1.1.0 to carry peer-list gossip so a fresh
 * node discovers the mesh from any single bootstrap seed):
 *
 *   u64be(chain_id)
 *   pubkey(32)
 *   u16be(listen_port)
 *   u8(host_len) || host_bytes               (external_host advertised; may be empty)
 *   u8(n_peers)                              (0..AC_HELLO_PEER_MAX, optional — see below)
 *   for each peer:
 *       pubkey(32)
 *       u16be(port)
 *       u8(host_len) || host_bytes
 *
 * The peer-list section is appended at the end so older parsers (v1.0.x)
 * stop after the host field and ignore the extra bytes. New parsers detect
 * extra bytes after the host and decode the peer list. */
#define AC_HELLO_PEER_MAX  8

/* Forward declaration: HELLO gossip handlers (reader thread) want to dial
 * back peers they learn about, but dial_peer is defined further down with
 * the connector-loop helpers. */
static int dial_peer(ac_net_t *n, const char *hp);

static int send_hello(ac_net_t *n, int fd) {
    uint8_t buf[1500];
    size_t pos = 0;
    ac_be64(buf + pos, n->cfg.chain_id); pos += 8;
    memcpy(buf + pos, n->cfg.keypair.pk, AC_PUBKEY_SIZE); pos += AC_PUBKEY_SIZE;
    uint8_t pbuf[2];
    ac_be16(pbuf, n->cfg.listen_port);
    memcpy(buf + pos, pbuf, 2); pos += 2;
    size_t hl = strnlen(n->cfg.external_host, sizeof(n->cfg.external_host) - 1);
    if (hl > 250) hl = 250;
    buf[pos++] = (uint8_t)hl;
    if (hl > 0) { memcpy(buf + pos, n->cfg.external_host, hl); pos += hl; }

    /* Peer-list gossip: include up to AC_HELLO_PEER_MAX peers we already
     * know about so the receiver can mesh-connect without a static seed
     * list. We skip ourselves and any peer without an advertised host
     * (its inbound IP is not addressable). */
    uint8_t *npeers_at = buf + pos;
    *npeers_at = 0;
    pos += 1;
    pthread_mutex_lock(&n->peers_mu);
    for (size_t i = 0; i < AC_MAX_PEERS && *npeers_at < AC_HELLO_PEER_MAX; ++i) {
        peer_t *q = &n->peers[i];
        if (!q->in_use || !q->peer_id_known) continue;
        if (q->host[0] == '\0' || q->port == 0) continue;
        size_t qhl = strnlen(q->host, sizeof(q->host));
        if (qhl > 250) continue;
        if (pos + AC_PUBKEY_SIZE + 2 + 1 + qhl + 32 > sizeof(buf)) break;
        memcpy(buf + pos, q->peer_id.b, AC_PUBKEY_SIZE); pos += AC_PUBKEY_SIZE;
        ac_be16(pbuf, q->port);
        memcpy(buf + pos, pbuf, 2); pos += 2;
        buf[pos++] = (uint8_t)qhl;
        memcpy(buf + pos, q->host, qhl); pos += qhl;
        (*npeers_at)++;
    }
    pthread_mutex_unlock(&n->peers_mu);

    return frame_send(fd, AC_MSG_HELLO, buf, pos);
}

/* Gossiped peer reported by a HELLO sender. */
typedef struct {
    ac_addr_t peer_id;
    uint16_t  port;
    char      host[128];
} hello_peer_t;

static int parse_hello(const uint8_t *buf, size_t len,
                       uint64_t *chain_id, ac_addr_t *peer_id,
                       uint16_t *port, char *host, size_t host_cap,
                       hello_peer_t *gossip_out, size_t gossip_cap,
                       size_t *gossip_n) {
    if (gossip_n) *gossip_n = 0;
    if (len < 8 + AC_PUBKEY_SIZE + 2 + 1) return -1;
    size_t pos = 0;
    *chain_id = ac_rd64(buf + pos); pos += 8;
    memcpy(peer_id->b, buf + pos, AC_PUBKEY_SIZE); pos += AC_PUBKEY_SIZE;
    uint8_t pp[2]; memcpy(pp, buf + pos, 2); pos += 2;
    *port = ((uint16_t)pp[0] << 8) | pp[1];
    uint8_t hl = buf[pos++];
    if (pos + hl > len) return -1;
    size_t copy = hl < host_cap - 1 ? hl : host_cap - 1;
    memcpy(host, buf + pos, copy);
    host[copy] = '\0';
    pos += hl;

    /* v1.1.0 extension: optional peer-list. v1.0.x senders end the payload
     * here, so we treat any further data as the gossip section. */
    if (pos >= len || !gossip_out || gossip_cap == 0) return 0;
    uint8_t np = buf[pos++];
    if (np > AC_HELLO_PEER_MAX) np = AC_HELLO_PEER_MAX;
    size_t kept = 0;
    for (uint8_t i = 0; i < np; ++i) {
        if (pos + AC_PUBKEY_SIZE + 2 + 1 > len) break;
        ac_addr_t pid;
        memcpy(pid.b, buf + pos, AC_PUBKEY_SIZE); pos += AC_PUBKEY_SIZE;
        uint8_t pp2[2]; memcpy(pp2, buf + pos, 2); pos += 2;
        uint16_t pport = ((uint16_t)pp2[0] << 8) | pp2[1];
        uint8_t phl = buf[pos++];
        if (pos + phl > len) break;
        if (kept < gossip_cap) {
            gossip_out[kept].peer_id = pid;
            gossip_out[kept].port    = pport;
            size_t c = phl < sizeof(gossip_out[kept].host) - 1 ? phl : sizeof(gossip_out[kept].host) - 1;
            memcpy(gossip_out[kept].host, buf + pos, c);
            gossip_out[kept].host[c] = '\0';
            kept++;
        }
        pos += phl;
    }
    if (gossip_n) *gossip_n = kept;
    return 0;
}

/* -------------------------------------------------------------------------- */
/* Reader thread.                                                             */
/* -------------------------------------------------------------------------- */

static void dispatch_message(peer_t *p, uint8_t type, const uint8_t *buf, size_t len) {
    ac_net_t *n = p->net;
    p->last_seen_ms = ac_now_ms();

    switch (type) {
    case AC_MSG_BLOCK_ANN: {
        ac_hash_t h; ac_hash(&h, buf, len);
        if (dedup_has(n, &h)) break;
        dedup_remember(n, &h);
        if (n->cfg.cb.on_block_ann) n->cfg.cb.on_block_ann(buf, len, n->cfg.cb.ctx);
        ac_net_broadcast(n, AC_MSG_BLOCK_ANN, buf, len, &p->peer_id);
        break;
    }
    case AC_MSG_TX_ANN: {
        ac_hash_t h; ac_hash(&h, buf, len);
        if (dedup_has(n, &h)) break;
        dedup_remember(n, &h);
        if (n->cfg.cb.on_tx_ann) n->cfg.cb.on_tx_ann(buf, len, n->cfg.cb.ctx);
        ac_net_broadcast(n, AC_MSG_TX_ANN, buf, len, &p->peer_id);
        break;
    }
    case AC_MSG_COMMIT_VOTE: {
        ac_hash_t h; ac_hash(&h, buf, len);
        if (dedup_has(n, &h)) break;
        dedup_remember(n, &h);
        if (n->cfg.cb.on_commit_vote) n->cfg.cb.on_commit_vote(buf, len, n->cfg.cb.ctx);
        ac_net_broadcast(n, AC_MSG_COMMIT_VOTE, buf, len, &p->peer_id);
        break;
    }
    case AC_MSG_HEADERS_REQ: {
        if (len < 12) break;
        uint64_t from = ac_rd64(buf);
        uint32_t count = ac_rd32(buf + 8);
        if (n->cfg.cb.on_headers_req) n->cfg.cb.on_headers_req(from, count, &p->peer_id, n->cfg.cb.ctx);
        break;
    }
    case AC_MSG_BLOCK_REQ: {
        if (len < 8) break;
        uint64_t h = ac_rd64(buf);
        if (n->cfg.cb.on_block_req) n->cfg.cb.on_block_req(h, &p->peer_id, n->cfg.cb.ctx);
        break;
    }
    case AC_MSG_BLOCK_RES: {
        if (n->cfg.cb.on_block_ann) n->cfg.cb.on_block_ann(buf, len, n->cfg.cb.ctx);
        break;
    }
    case AC_MSG_PING: {
        /* Echo as PONG via the queue (still non-blocking on producer side). */
        peer_enqueue(p, AC_MSG_PONG, buf, len);
        break;
    }
    default:
        /* Unknown type: ignore. */
        break;
    }
}

static void *reader_loop(void *arg) {
    peer_t *p = (peer_t *)arg;
    ac_net_t *n = p->net;

    /* First message must be HELLO. */
    uint8_t type;
    size_t len;
    uint8_t *buf = frame_recv(p->fd, &type, &len);
    if (!buf || type != AC_MSG_HELLO) {
        if (buf) free(buf);
        LOG_D("net", "peer disconnected before HELLO");
        peer_close_fd(p);
        return NULL;
    }
    uint64_t their_chain_id = 0;
    ac_addr_t their_id;
    uint16_t their_port = 0;
    char their_host[128] = {0};
    hello_peer_t gossip[AC_HELLO_PEER_MAX];
    size_t gossip_n = 0;
    if (parse_hello(buf, len, &their_chain_id, &their_id, &their_port,
                    their_host, sizeof(their_host),
                    gossip, AC_HELLO_PEER_MAX, &gossip_n) < 0) {
        free(buf);
        LOG_W("net", "malformed HELLO from peer");
        peer_close_fd(p);
        return NULL;
    }
    free(buf);
    if (their_chain_id != n->cfg.chain_id) {
        LOG_W("net", "peer on different chain_id=%lu", (unsigned long)their_chain_id);
        peer_close_fd(p);
        return NULL;
    }
    /* Reject self-connection. */
    if (memcmp(their_id.b, n->cfg.keypair.pk, AC_PUBKEY_SIZE) == 0) {
        LOG_D("net", "self-connection rejected");
        peer_close_fd(p);
        return NULL;
    }
    /* Reject a duplicate peer_id. The first connection wins; redundant ones
     * are dropped before they can multiply broadcast write fanout. */
    pthread_mutex_lock(&n->peers_mu);
    bool dup_pid = false;
    for (size_t i = 0; i < AC_MAX_PEERS; ++i) {
        peer_t *other = &n->peers[i];
        if (other == p || !other->in_use || !other->peer_id_known) continue;
        if (memcmp(other->peer_id.b, their_id.b, AC_PUBKEY_SIZE) == 0) {
            dup_pid = true;
            break;
        }
    }
    pthread_mutex_unlock(&n->peers_mu);
    if (dup_pid) {
        LOG_D("net", "duplicate peer_id, dropping new connection");
        peer_close_fd(p);
        return NULL;
    }
    p->peer_id = their_id;
    p->peer_id_known = true;
    if (their_host[0]) snprintf(p->host, sizeof(p->host), "%s", their_host);
    p->port = their_port;
    p->last_seen_ms = ac_now_ms();

    char hex[2 * AC_PUBKEY_SIZE + 1];
    ac_hex_encode(hex, p->peer_id.b, 8);
    LOG_I("net", "%s peer %s @ %s:%u",
          p->inbound ? "inbound" : "outbound", hex, p->host, p->port);

    /* Mesh bootstrap: dial every peer the new neighbour told us about.
     * dial_peer is idempotent (already_dialed dedup by host:port), so it is
     * safe to spam it. This turns the previous hub-and-spoke topology into
     * a proper mesh as nodes learn about each other through HELLO gossip. */
    for (size_t gi = 0; gi < gossip_n; ++gi) {
        /* Skip if it's us, or the peer we're talking to right now. */
        if (memcmp(gossip[gi].peer_id.b, n->cfg.keypair.pk, AC_PUBKEY_SIZE) == 0) continue;
        if (memcmp(gossip[gi].peer_id.b, p->peer_id.b, AC_PUBKEY_SIZE) == 0) continue;
        if (gossip[gi].host[0] == '\0' || gossip[gi].port == 0) continue;
        char hp[160];
        snprintf(hp, sizeof(hp), "%s:%u", gossip[gi].host, (unsigned)gossip[gi].port);
        dial_peer(n, hp);
    }

    /* Steady-state read loop. */
    while (n->running) {
        buf = frame_recv(p->fd, &type, &len);
        if (!buf) break;
        dispatch_message(p, type, buf, len);
        free(buf);
    }

    LOG_I("net", "peer %s disconnected", hex);
    peer_close_fd(p);
    return NULL;
}

/* -------------------------------------------------------------------------- */
/* Listen thread.                                                             */
/* -------------------------------------------------------------------------- */

static int set_socket_options(int fd) {
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof(one));
#ifdef TCP_NODELAY
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof(one));
#endif
    /* Hard cap on per-write blocking time. Without this, a slow peer can stall
     * the consensus thread (which writes blocks synchronously via gossip) for
     * the full TCP retransmit window — minutes. With the timeout, a stuck
     * write fails and the broadcast loop moves on; the reader thread detects
     * EOF and reaps the peer slot. */
#ifdef _WIN32
    DWORD tv = 30000; /* ms */
    setsockopt((SOCKET)fd, SOL_SOCKET, SO_SNDTIMEO, (const char *)&tv, sizeof(tv));
#else
    struct timeval tv = { .tv_sec = 30, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (const char *)&tv, sizeof(tv));
    /* RCV timeout is unbounded by design: peer readers block on incoming data
     * until EOF; we don't want to spuriously drop healthy idle peers. */
#endif
    return 0;
}

static void *listen_loop(void *arg) {
    ac_net_t *n = (ac_net_t *)arg;

    while (n->running) {
        struct sockaddr_storage addr;
        socklen_t alen = sizeof(addr);
        int fd = accept(n->listen_fd, (struct sockaddr *)&addr, &alen);
        if (fd < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            if (!n->running) break;
            LOG_W("net", "accept failed: %s", strerror(errno));
            ac_sleep_ms(200);
            continue;
        }
        set_socket_options(fd);

        peer_t *p = peer_alloc(n);
        if (!p) { ac_sock_close(fd); continue; }
        p->fd = fd;
        p->inbound = true;

        char host[64] = "?";
        uint16_t port = 0;
        if (addr.ss_family == AF_INET) {
            struct sockaddr_in *sin = (struct sockaddr_in *)&addr;
            inet_ntop(AF_INET, &sin->sin_addr, host, sizeof(host));
            port = ntohs(sin->sin_port);
        } else if (addr.ss_family == AF_INET6) {
            struct sockaddr_in6 *sin = (struct sockaddr_in6 *)&addr;
            inet_ntop(AF_INET6, &sin->sin6_addr, host, sizeof(host));
            port = ntohs(sin->sin6_port);
        }
        snprintf(p->host, sizeof(p->host), "%s", host);
        p->port = port;

        /* We send HELLO first (inline, synchronous, while the queue's writer
         * hasn't taken over the socket), then start the writer + reader. */
        send_hello(n, fd);

        if (pthread_create(&p->writer, NULL, writer_loop, p) == 0) {
            p->writer_started = true;
        }
        if (pthread_create(&p->reader, NULL, reader_loop, p) == 0) {
            p->reader_started = true;
        }
    }
    return NULL;
}

/* -------------------------------------------------------------------------- */
/* Outbound connector.                                                        */
/* -------------------------------------------------------------------------- */

/* Returns true iff we already have a *live* outbound peer slot for this
 * host:port. Dead slots (fd closed but thread not yet joined) do not count,
 * so the connector can re-establish lost links promptly. */
static bool already_dialed(ac_net_t *n, const char *host, uint16_t port) {
    bool found = false;
    pthread_mutex_lock(&n->peers_mu);
    for (size_t i = 0; i < AC_MAX_PEERS; ++i) {
        peer_t *p = &n->peers[i];
        if (!p->in_use || p->inbound || p->fd < 0) continue;
        if (p->port == port && strcmp(p->host, host) == 0) { found = true; break; }
    }
    pthread_mutex_unlock(&n->peers_mu);
    return found;
}

static int dial_peer(ac_net_t *n, const char *hp) {
    char host[128];
    int  port = 0;
    /* Parse "host:port". */
    const char *colon = strrchr(hp, ':');
    if (!colon) return -1;
    size_t hl = (size_t)(colon - hp);
    if (hl >= sizeof(host)) return -1;
    memcpy(host, hp, hl); host[hl] = '\0';
    port = atoi(colon + 1);
    if (port <= 0 || port > 65535) return -1;

    /* Strip [..] brackets for IPv6 literals. */
    if (host[0] == '[' && hl > 2 && host[hl - 1] == ']') {
        memmove(host, host + 1, hl - 2);
        host[hl - 2] = '\0';
    }

    /* Cheap dedup: don't open a second TCP connection to a host:port we
     * already have. Without this, a small seed list combined with
     * target_outbound=8 fans out into 8 redundant connections to the same
     * peer, multiplying every broadcast 8x and unnecessarily stressing
     * the remote write buffer. */
    if (already_dialed(n, host, (uint16_t)port)) return 0;

    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;
    char pstr[8]; snprintf(pstr, sizeof(pstr), "%d", port);
    if (getaddrinfo(host, pstr, &hints, &res) != 0) return -1;

    int fd = -1;
    for (struct addrinfo *p = res; p; p = p->ai_next) {
        fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, p->ai_addr, p->ai_addrlen) == 0) break;
        ac_sock_close(fd); fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) return -1;
    set_socket_options(fd);

    peer_t *peer = peer_alloc(n);
    if (!peer) { ac_sock_close(fd); return -1; }
    peer->fd = fd;
    peer->inbound = false;
    snprintf(peer->host, sizeof(peer->host), "%s", host);
    peer->port = (uint16_t)port;

    /* We initiate HELLO inline (the writer thread hasn't taken over the
     * socket yet) and then spawn writer + reader. Both are joined by
     * reap_dead_peers / ac_net_stop — never detached, so we always reap. */
    send_hello(n, fd);
    if (pthread_create(&peer->writer, NULL, writer_loop, peer) == 0) {
        peer->writer_started = true;
    }
    if (pthread_create(&peer->reader, NULL, reader_loop, peer) == 0) {
        peer->reader_started = true;
    }
    return 0;
}

/* Reap peer slots whose reader thread has exited. Without this, every
 * disconnect leaks a slot (in_use=true, fd<0) and after AC_MAX_PEERS
 * disconnects the connector can no longer create new peer slots. */
static void reap_dead_peers(ac_net_t *n) {
    for (size_t i = 0; i < AC_MAX_PEERS; ++i) {
        peer_t *p;
        bool need_join_reader = false;
        bool need_join_writer = false;
        pthread_t reader_th = 0, writer_th = 0;
        pthread_mutex_lock(&n->peers_mu);
        p = &n->peers[i];
        if (p->in_use && p->fd < 0) {
            if (p->reader_started) { reader_th = p->reader; need_join_reader = true; }
            if (p->writer_started) { writer_th = p->writer; need_join_writer = true; }
        }
        pthread_mutex_unlock(&n->peers_mu);

        if (need_join_writer) {
            /* Wake the writer so it observes out_closing and exits. */
            pthread_mutex_lock(&p->out_mu);
            p->out_closing = true;
            pthread_cond_broadcast(&p->out_cv);
            pthread_mutex_unlock(&p->out_mu);
            pthread_join(writer_th, NULL);
        }
        if (need_join_reader) {
            pthread_join(reader_th, NULL);
        }
        if (need_join_reader || need_join_writer) {
            pthread_mutex_lock(&n->peers_mu);
            if (p->in_use && p->fd < 0) {
                p->reader_started = false;
                p->writer_started = false;
                pthread_mutex_destroy(&p->out_mu);
                pthread_cond_destroy (&p->out_cv);
                p->in_use = false;
            }
            pthread_mutex_unlock(&n->peers_mu);
        }
    }
}

static void *connector_loop(void *arg) {
    ac_net_t *n = (ac_net_t *)arg;
    int target = n->cfg.target_outbound > 0 ? n->cfg.target_outbound : 8;
    /* Don't try to over-dial: if the seed list is smaller than the target,
     * one outbound per seed is the right amount. Otherwise we'd open
     * redundant TCP connections to the same peer. */
    if ((int)n->cfg.seed_n > 0 && (int)n->cfg.seed_n < target) {
        target = (int)n->cfg.seed_n;
    }

    while (n->running) {
        reap_dead_peers(n);

        size_t now_out = outbound_count(n);
        if (now_out < (size_t)target && n->cfg.seed_n > 0) {
            /* Try each seed in order so a single offline seed doesn't starve
             * the rest. */
            for (size_t i = 0; i < n->cfg.seed_n && n->running; ++i) {
                const char *peer = n->cfg.seed_peers[i];
                if (peer && peer[0]) dial_peer(n, peer);
            }
        }
        ac_sleep_ms(AC_RECONNECT_BASE_MS);
    }
    return NULL;
}

/* -------------------------------------------------------------------------- */
/* Public API.                                                                */
/* -------------------------------------------------------------------------- */

ac_net_t *ac_net_new(const ac_net_config_t *cfg) {
    ac_net_t *n = (ac_net_t *)calloc(1, sizeof(*n));
    if (!n) return NULL;
    n->cfg = *cfg;
    /* Deep-copy seed peers. */
    if (cfg->seed_n > 0) {
        const char **arr = (const char **)calloc(cfg->seed_n, sizeof(char *));
        for (size_t i = 0; i < cfg->seed_n; ++i) arr[i] = strdup(cfg->seed_peers[i]);
        n->cfg.seed_peers = arr;
    }
    n->listen_fd = -1;
    pthread_mutex_init(&n->peers_mu, NULL);
    pthread_mutex_init(&n->dedup_mu, NULL);
    return n;
}

void ac_net_free(ac_net_t *n) {
    if (!n) return;
    if (n->running) ac_net_stop(n);
    if (n->cfg.seed_peers) {
        for (size_t i = 0; i < n->cfg.seed_n; ++i) {
            free((void *)n->cfg.seed_peers[i]);
        }
        free((void *)n->cfg.seed_peers);
    }
    pthread_mutex_destroy(&n->peers_mu);
    pthread_mutex_destroy(&n->dedup_mu);
    free(n);
}

int ac_net_start(ac_net_t *n) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    set_socket_options(fd);

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port   = htons(n->cfg.listen_port);
    if (n->cfg.listen_host[0]) {
        if (inet_pton(AF_INET, n->cfg.listen_host, &sa.sin_addr) != 1) {
            sa.sin_addr.s_addr = htonl(INADDR_ANY);
        }
    } else {
        sa.sin_addr.s_addr = htonl(INADDR_ANY);
    }
    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        LOG_E("net", "bind %u: %s", n->cfg.listen_port, strerror(errno));
        ac_sock_close(fd);
        return -1;
    }
    if (listen(fd, 32) < 0) {
        ac_sock_close(fd);
        return -1;
    }
    n->listen_fd = fd;
    n->running = true;
#if AC_HAS_SIGPIPE
    signal(SIGPIPE, SIG_IGN);
#endif

    if (pthread_create(&n->listen_thread, NULL, listen_loop, n) != 0) {
        n->running = false;
        ac_sock_close(fd);
        return -1;
    }
    if (pthread_create(&n->connector_thread, NULL, connector_loop, n) != 0) {
        n->running = false;
        ac_sock_close(fd);
        pthread_join(n->listen_thread, NULL);
        return -1;
    }
    LOG_I("net", "listening on %s:%u",
          n->cfg.listen_host[0] ? n->cfg.listen_host : "0.0.0.0",
          n->cfg.listen_port);
    return 0;
}

void ac_net_stop(ac_net_t *n) {
    n->running = false;
    if (n->listen_fd >= 0) {
        ac_sock_shutdown(n->listen_fd, SHUT_RDWR);
        ac_sock_close(n->listen_fd);
        n->listen_fd = -1;
    }
    pthread_join(n->listen_thread, NULL);
    pthread_join(n->connector_thread, NULL);
    /* Force-close peer sockets so reader threads wake up. */
    pthread_mutex_lock(&n->peers_mu);
    for (size_t i = 0; i < AC_MAX_PEERS; ++i) {
        if (n->peers[i].in_use && n->peers[i].fd >= 0) {
            ac_sock_shutdown(n->peers[i].fd, SHUT_RDWR);
        }
    }
    pthread_mutex_unlock(&n->peers_mu);
    /* Wake every writer so it observes out_closing and exits. */
    for (size_t i = 0; i < AC_MAX_PEERS; ++i) {
        peer_t *p = &n->peers[i];
        if (!p->in_use) continue;
        pthread_mutex_lock(&p->out_mu);
        p->out_closing = true;
        pthread_cond_broadcast(&p->out_cv);
        pthread_mutex_unlock(&p->out_mu);
    }
    /* Join every started reader + writer thread and release the slot. */
    for (size_t i = 0; i < AC_MAX_PEERS; ++i) {
        peer_t *p = &n->peers[i];
        if (p->reader_started) { pthread_join(p->reader, NULL); p->reader_started = false; }
        if (p->writer_started) { pthread_join(p->writer, NULL); p->writer_started = false; }
        if (p->in_use) {
            pthread_mutex_destroy(&p->out_mu);
            pthread_cond_destroy (&p->out_cv);
            if (p->fd >= 0) { ac_sock_close(p->fd); p->fd = -1; }
            p->in_use = false;
        }
    }
}

void ac_net_broadcast(ac_net_t *n, uint8_t msg_type,
                      const uint8_t *payload, size_t len,
                      const ac_addr_t *exclude_peer_id) {
    pthread_mutex_lock(&n->peers_mu);
    for (size_t i = 0; i < AC_MAX_PEERS; ++i) {
        peer_t *p = &n->peers[i];
        if (!p->in_use || !p->peer_id_known || p->fd < 0) continue;
        if (exclude_peer_id && ac_addr_eq(&p->peer_id, exclude_peer_id)) continue;
        /* Non-blocking enqueue. If the queue is full (peer too slow), drop
         * the message and shut down the peer's connection; its reader thread
         * will exit and the slot is reaped on the next connector tick. */
        if (peer_enqueue(p, msg_type, payload, len) < 0) {
            if (p->fd >= 0) ac_sock_shutdown(p->fd, SHUT_RDWR);
        }
    }
    pthread_mutex_unlock(&n->peers_mu);
}

int ac_net_send_to(ac_net_t *n, const ac_addr_t *peer_id,
                   uint8_t msg_type, const uint8_t *payload, size_t len) {
    int rc = -1;
    pthread_mutex_lock(&n->peers_mu);
    for (size_t i = 0; i < AC_MAX_PEERS; ++i) {
        peer_t *p = &n->peers[i];
        if (!p->in_use || !p->peer_id_known || p->fd < 0) continue;
        if (!ac_addr_eq(&p->peer_id, peer_id)) continue;
        rc = peer_enqueue(p, msg_type, payload, len);
        if (rc < 0 && p->fd >= 0) ac_sock_shutdown(p->fd, SHUT_RDWR);
        break;
    }
    pthread_mutex_unlock(&n->peers_mu);
    return rc;
}
