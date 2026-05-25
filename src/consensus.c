#include "consensus.h"
#include "net.h"

#include <inttypes.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define AC_PENDING_MAX 8

/* -------------------------------------------------------------------------- */
/* Pending blocks: blocks we have seen but not yet committed.                 */
/* -------------------------------------------------------------------------- */

typedef struct {
    bool                 occupied;
    bool                 committed;
    uint64_t             height;
    uint64_t             slot;
    ac_hash_t            block_hash;
    ac_block_t           block;          /* deep copy; we own it */
    ac_commit_signer_t   signers[AC_COMMITTEE_MAX];
    uint32_t             nsigners;
    uint64_t             first_seen_ms;

    /* Proposer's VRF priority for this slot — lower wins. Computed from
     * the block's proposer_vrf_proof and the proposer's pre-block sqrt-
     * stake. See PROTOCOL.md § 6.3. */
    uint8_t              priority[16];

    /* Wall-clock time (ms) at which this block first met the 2/3 sqrt-stake
     * threshold. We DELAY the actual seal by AC_SEAL_GRACE_MS so late votes
     * from high-latency validators (think Stockholm↔Iowa) make it into the
     * commit certificate instead of getting raced out. Zero means
     * "threshold not yet reached". */
    uint64_t             threshold_reached_ms;
} pending_t;

/* Per-slot voting state. A validator votes at most once per slot: on the
 * proposal with the lowest priority it has seen by the time the vote
 * window opens. Without this convergence step, two leader-eligible
 * validators in the same slot can both reach the 2/3 threshold on
 * different blocks — a fork. The wait window lets every committee member
 * observe the same set of proposals before locking in. */
typedef struct {
    uint64_t   slot;                /* 0 = unused */
    uint8_t    best_priority[16];   /* 0xFF...FF when no proposal yet */
    ac_hash_t  voted_block;         /* the block we ended up voting on */
    bool       voted;               /* true after we have voted in this slot */
} slot_vote_t;

struct ac_consensus_s {
    pthread_mutex_t       mu;
    ac_chain_t           *chain;
    ac_mempool_t         *mempool;
    ac_keypair_t          kp;
    ac_addr_t             my_addr;
    ac_broadcast_fn       bcast;
    void                 *bcast_ctx;
    bool                  validator;

    pthread_t             slot_thread;
    bool                  running;

    pending_t             pending[AC_PENDING_MAX];
    slot_vote_t           slot_votes[AC_PENDING_MAX];

    /* Rate-limit HEADERS_REQ so we do not spam the seed with 30 requests
     * per minute (one per incoming gossip BLOCK_ANN). With each request
     * pulling 256 blocks back, that fills the seed's send buffer faster
     * than the receiver can drain. */
    uint64_t              last_hdrs_req_ms;
    uint64_t              last_hdrs_req_target;
};

#define AC_HDRS_REQ_MIN_INTERVAL_MS 4000

/* Vote-phase wait window: how long after slot start before we vote on the
 * best proposal we have seen. 1100 ms inside a 2 s slot lets a proposal
 * from a distant proposer (Europe→US, ~150 ms RTT) propagate to every
 * committee member before voting begins. */
#define AC_VOTE_DELAY_MS  1100

/* Seal-phase delay: after the 2/3 threshold is first reached, wait this
 * much before persisting the block so straggler votes from high-latency
 * validators can still be folded into the commit certificate. Without it,
 * a validator that signs at vote_phase + RTT (e.g., 250 ms) loses the
 * race against the first quorum-completing vote and ends up dropped from
 * the cert even though it signed correctly. */
#define AC_SEAL_GRACE_MS  300

static void pending_init(pending_t *p) {
    if (p->occupied) ac_block_free(&p->block);
    memset(p, 0, sizeof(*p));
}

static pending_t *pending_find(ac_consensus_t *cs, const ac_hash_t *bh) {
    for (size_t i = 0; i < AC_PENDING_MAX; ++i) {
        if (cs->pending[i].occupied && ac_hash_eq(&cs->pending[i].block_hash, bh)) {
            return &cs->pending[i];
        }
    }
    return NULL;
}

static pending_t *pending_alloc(ac_consensus_t *cs) {
    pending_t *oldest = NULL;
    for (size_t i = 0; i < AC_PENDING_MAX; ++i) {
        if (!cs->pending[i].occupied) {
            pending_init(&cs->pending[i]);
            cs->pending[i].occupied = true;
            cs->pending[i].first_seen_ms = ac_now_ms();
            return &cs->pending[i];
        }
        if (!oldest || cs->pending[i].first_seen_ms < oldest->first_seen_ms) {
            oldest = &cs->pending[i];
        }
    }
    /* Evict oldest. */
    pending_init(oldest);
    oldest->occupied = true;
    oldest->first_seen_ms = ac_now_ms();
    return oldest;
}

/* -------------------------------------------------------------------------- */
/* VRF helpers (mirror of chain.c internals, kept local for decoupling).      */
/* -------------------------------------------------------------------------- */

static int leader_alpha(uint8_t out[64], const ac_hash_t *seed, uint64_t slot) {
    static const char DOMAIN[] = "AGCH:LEADER";
    memcpy(out, DOMAIN, sizeof(DOMAIN) - 1);
    memcpy(out + sizeof(DOMAIN) - 1, seed->b, AC_HASH_SIZE);
    ac_be64(out + sizeof(DOMAIN) - 1 + AC_HASH_SIZE, slot);
    return (int)(sizeof(DOMAIN) - 1 + AC_HASH_SIZE + 8);
}

static int committee_alpha(uint8_t out[64], const ac_hash_t *seed, uint64_t slot) {
    static const char DOMAIN[] = "AGCH:COMMITTEE";
    memcpy(out, DOMAIN, sizeof(DOMAIN) - 1);
    memcpy(out + sizeof(DOMAIN) - 1, seed->b, AC_HASH_SIZE);
    ac_be64(out + sizeof(DOMAIN) - 1 + AC_HASH_SIZE, slot);
    return (int)(sizeof(DOMAIN) - 1 + AC_HASH_SIZE + 8);
}

static int committee_eligible(uint64_t draw_u64, uint64_t sqrt_stake, uint64_t total_sqrt) {
    if (total_sqrt == 0 || sqrt_stake == 0) return 0;
    long double p = (long double)sqrt_stake * (long double)AC_COMMITTEE_TARGET
                  / (long double)total_sqrt;
    if (p >= 1.0L) return 1;
    long double thr = p * (long double)UINT64_MAX;
    return draw_u64 < (uint64_t)thr;
}

/* Returns 1 if `my_addr` is leader-eligible at `slot`, with VRF proof written
 * to *out_proof. We intentionally allow ~2 eligible leaders per slot on
 * average: this gives liveness redundancy when one leader is offline or
 * partitioned. Safety with multiple proposers is provided by the priority-
 * based vote convergence in vote_phase() — every honest committee member
 * votes on the same lowest-priority proposal, so at most one block per slot
 * can reach the 2/3 sqrt-stake commit threshold. See PROTOCOL.md § 6.3. */
static int am_i_leader(ac_consensus_t *cs, uint64_t slot, ac_vrf_proof_t *out_proof) {
    ac_chain_lock(cs->chain);

    ac_account_t me;
    int has = ac_state_get(ac_chain_state(cs->chain), &cs->my_addr, &me);
    if (!has || me.stake < AC_MIN_STAKE_UCRD) {
        ac_chain_unlock(cs->chain);
        return 0;
    }

    ac_hash_t seed;
    ac_chain_epoch_seed(cs->chain, ac_epoch_of(slot), &seed);

    uint8_t alpha[64];
    int an = leader_alpha(alpha, &seed, slot);

    ac_vrf_prove(out_proof, NULL, alpha, (size_t)an, &cs->kp);

    /* In v1 we use a simple eligibility threshold: probability per active
     * validator = 2 / |active_set|, targeting ~2 leader candidates per slot.
     * If the VRF output's first 8 bytes (interpreted as uniform [0,1)) fall
     * below the threshold, we consider ourselves a candidate. */
    size_t nactive = ac_chain_active_count(cs->chain);
    ac_chain_unlock(cs->chain);
    if (nactive == 0) return 0;

    ac_vrf_out_t beta;
    ac_vrf_proof_t tmp = *out_proof;
    ac_vrf_verify(&beta, &tmp, alpha, (size_t)an, cs->kp.pk);

    long double p = (long double)2.0L / (long double)nactive;
    if (p > 1.0L) p = 1.0L;
    long double thr = p * (long double)UINT64_MAX;
    uint64_t draw = ac_rd64(beta.b);
    return draw < (uint64_t)thr ? 1 : 0;
}

/* Returns 1 if `my_addr` is in the committee at `slot`. */
static int am_i_committee(ac_consensus_t *cs, uint64_t slot, ac_vrf_proof_t *out_proof) {
    ac_chain_lock(cs->chain);

    ac_account_t me;
    int has = ac_state_get(ac_chain_state(cs->chain), &cs->my_addr, &me);
    if (!has || me.stake < AC_MIN_STAKE_UCRD) {
        ac_chain_unlock(cs->chain);
        return 0;
    }

    ac_hash_t seed;
    ac_chain_epoch_seed(cs->chain, ac_epoch_of(slot), &seed);
    uint64_t total_sqrt = ac_chain_total_sqrt_stake(cs->chain);
    ac_chain_unlock(cs->chain);

    uint64_t sqrt_me = ac_isqrt_u64(me.stake);

    uint8_t alpha[64];
    int an = committee_alpha(alpha, &seed, slot);
    ac_vrf_prove(out_proof, NULL, alpha, (size_t)an, &cs->kp);

    ac_vrf_out_t beta;
    ac_vrf_proof_t tmp = *out_proof;
    ac_vrf_verify(&beta, &tmp, alpha, (size_t)an, cs->kp.pk);
    uint64_t draw = ac_rd64(beta.b);

    return committee_eligible(draw, sqrt_me, total_sqrt);
}

/* -------------------------------------------------------------------------- */
/* Vote wire format.                                                          */
/* -------------------------------------------------------------------------- */

static void vote_encode(uint8_t out[AC_VOTE_WIRE_LEN],
                        uint64_t height,
                        const ac_hash_t *block_hash,
                        const ac_addr_t *signer,
                        const ac_sig_t  *sig,
                        const ac_vrf_proof_t *proof) {
    size_t pos = 0;
    ac_be64(out + pos, height); pos += 8;
    memcpy(out + pos, block_hash->b, AC_HASH_SIZE); pos += AC_HASH_SIZE;
    memcpy(out + pos, signer->b,     AC_PUBKEY_SIZE); pos += AC_PUBKEY_SIZE;
    memcpy(out + pos, sig->b,        AC_SIG_SIZE);    pos += AC_SIG_SIZE;
    memcpy(out + pos, proof->b,      AC_VRF_PROOF_SIZE);
}

static int vote_decode(const uint8_t *buf, size_t len,
                       uint64_t *height,
                       ac_hash_t *block_hash,
                       ac_addr_t *signer,
                       ac_sig_t *sig,
                       ac_vrf_proof_t *proof) {
    if (len != AC_VOTE_WIRE_LEN) return -1;
    size_t pos = 0;
    *height = ac_rd64(buf + pos); pos += 8;
    memcpy(block_hash->b, buf + pos, AC_HASH_SIZE); pos += AC_HASH_SIZE;
    memcpy(signer->b,     buf + pos, AC_PUBKEY_SIZE); pos += AC_PUBKEY_SIZE;
    memcpy(sig->b,        buf + pos, AC_SIG_SIZE);    pos += AC_SIG_SIZE;
    memcpy(proof->b,      buf + pos, AC_VRF_PROOF_SIZE);
    return 0;
}

/* -------------------------------------------------------------------------- */
/* Per-slot priority tracking + vote convergence.                             */
/* -------------------------------------------------------------------------- */

/* Compute the proposer's priority for a slot, per PROTOCOL § 6.3.
 * Lower = better leader. We use BLAKE2b("AGCH:PRIO" || beta) shifted by the
 * proposer's sqrt-stake — higher stake gives proportionally lower priority
 * scores, so well-staked validators win sortition more often. */
static void compute_priority(uint8_t out[16], const ac_vrf_out_t *beta, uint64_t sqrt_stake) {
    if (sqrt_stake == 0) { memset(out, 0xFF, 16); return; }
    /* Domain-tagged hash of beta so callers do not interpret the raw VRF
     * output. Take the first 16 bytes as a Q.128 unsigned scalar. */
    ac_hash_t h;
    static const char DOMAIN[] = "AGCH:PRIO";
    const uint8_t *chunks[2] = { (const uint8_t *)DOMAIN, beta->b };
    const size_t   lens[2]   = { sizeof(DOMAIN) - 1,       AC_VRF_OUT_SIZE };
    ac_hash_multi(&h, chunks, lens, 2);
    /* Divide the 128-bit numerator by sqrt_stake. Compute high/low halves
     * carefully so we do not overflow. */
    uint64_t hi = ac_rd64(h.b);
    uint64_t lo = ac_rd64(h.b + 8);
    uint64_t q_hi = hi / sqrt_stake;
    uint64_t r_hi = hi % sqrt_stake;
    /* lo_total = r_hi * 2^64 + lo; approximate by upper bound on overflow. */
    uint64_t q_lo = lo / sqrt_stake;
    if (sqrt_stake > 0) q_lo += (UINT64_MAX / sqrt_stake) * r_hi;
    ac_be64(out,     q_hi);
    ac_be64(out + 8, q_lo);
}

/* Find or allocate a slot-vote tracking entry for `slot`. Reuses the
 * oldest entry when the table is full. */
static slot_vote_t *slot_vote_for(ac_consensus_t *cs, uint64_t slot) {
    slot_vote_t *oldest = NULL;
    for (size_t i = 0; i < AC_PENDING_MAX; ++i) {
        slot_vote_t *sv = &cs->slot_votes[i];
        if (sv->slot == slot) return sv;
        if (!oldest || sv->slot < oldest->slot) oldest = sv;
    }
    /* Evict oldest. */
    memset(oldest, 0, sizeof(*oldest));
    oldest->slot = slot;
    memset(oldest->best_priority, 0xFF, sizeof(oldest->best_priority));
    return oldest;
}

/* Bytewise compare two 16-byte priorities. < 0 if a < b. */
static int priority_cmp(const uint8_t a[16], const uint8_t b[16]) {
    return memcmp(a, b, 16);
}

/* Lock-held: emit a single commit-vote for the given pending block. The
 * caller guarantees we have not already voted in this slot. */
static void our_vote_now(ac_consensus_t *cs, pending_t *p) {
    /* Recompute our own committee-membership proof for this slot — the
     * proof is what other validators verify in handle_vote. */
    ac_vrf_proof_t cproof;
    if (!am_i_committee(cs, p->slot, &cproof)) return;

    LOG_D("consen.", "voting on h=%" PRIu64 " slot=%" PRIu64 " (prio=%02x%02x..)",
          p->height, p->slot, p->priority[0], p->priority[1]);

    uint8_t vmsg[64];
    int vn = ac_vote_message(vmsg, p->height, &p->block_hash);
    ac_sig_t sig;
    ac_sign(&sig, vmsg, (size_t)vn, &cs->kp);

    /* Add to local set. */
    if (p->nsigners < AC_COMMITTEE_MAX) {
        ac_commit_signer_t *s = &p->signers[p->nsigners++];
        s->signer = cs->my_addr;
        s->sig    = sig;
        memcpy(s->vrf_proof, cproof.b, AC_VRF_PROOF_SIZE);
    }

    slot_vote_t *sv = slot_vote_for(cs, p->slot);
    sv->voted = true;
    sv->voted_block = p->block_hash;
    memcpy(sv->best_priority, p->priority, 16);

    /* Broadcast. */
    uint8_t wire[AC_VOTE_WIRE_LEN];
    vote_encode(wire, p->height, &p->block_hash, &cs->my_addr, &sig, &cproof);
    if (cs->bcast) cs->bcast(0x08, wire, sizeof(wire), cs->bcast_ctx);
}

/* If the pending block has enough signatures, finalise it. The denominator
 * is the LIVE sqrt-stake (signers active in the last AC_LIVENESS_WINDOW
 * blocks), not the full bonded set, so a freshly-bonded-but-still-syncing
 * or temporarily-offline validator cannot stall the chain. Once the
 * threshold is first crossed, we wait AC_SEAL_GRACE_MS before persisting
 * the block so late votes from high-latency validators are still folded
 * into the commit certificate. If `force_seal` is true we bypass the grace
 * (used by seal_phase at the end of the slot to flush any waiters). */
static void try_commit_inner(ac_consensus_t *cs, pending_t *p, bool force_seal);

/* Default path: respect the AC_SEAL_GRACE_MS window after threshold first
 * met. Most call sites (handle_block, handle_vote, slot_routine, vote_phase)
 * go through here. seal_phase calls try_commit_inner with force_seal=true
 * at the end of the slot to flush any pending block whose grace window has
 * elapsed without a follow-up vote. */
static void try_commit(ac_consensus_t *cs, pending_t *p) {
    try_commit_inner(cs, p, /*force_seal=*/ false);
}

static void try_commit_inner(ac_consensus_t *cs, pending_t *p, bool force_seal) {
    if (!p->occupied || p->committed) return;

    ac_chain_lock(cs->chain);
    uint64_t live_sqrt = ac_chain_live_sqrt_stake(cs->chain);
    uint64_t total_sqrt = ac_chain_total_sqrt_stake(cs->chain);
    if (live_sqrt == 0) live_sqrt = total_sqrt;
    if (live_sqrt == 0) { ac_chain_unlock(cs->chain); return; }

    uint64_t sum_sqrt = 0;
    for (uint32_t i = 0; i < p->nsigners; ++i) {
        ac_account_t a;
        ac_state_get(ac_chain_state(cs->chain), &p->signers[i].signer, &a);
        if (a.stake >= AC_MIN_STAKE_UCRD) sum_sqrt += ac_isqrt_u64(a.stake);
    }

    /* Liveness: ≥ 2/3 of the live set. Safety floor: > 1/2 of the *total*
     * bonded set, so two disjoint quorums can never both finalise (quorum
     * intersection) — prevents the split-brain fork a partition could
     * otherwise produce. Mirrors validate_commit in chain.c; both gates must
     * pass on every node or a block one node seals would be rejected by the
     * others. */
    if (sum_sqrt * 3 < live_sqrt * 2) { ac_chain_unlock(cs->chain); return; }
    if (total_sqrt > 0 && sum_sqrt * 2 <= total_sqrt) { ac_chain_unlock(cs->chain); return; }

    /* Threshold reached. Wait the grace window for late votes before sealing,
     * unless the seal_phase has explicitly forced us to flush. The grace is
     * for live consensus — when this node was up at slot start and might
     * still receive a late COMMIT_VOTE worth waiting for. During sync the
     * incoming block is already past, no further votes will arrive, and
     * waiting just throttles the catch-up rate (one block per slot tick).
     * Detect that case by comparing the block's slot against the wall slot:
     * if the block is more than one slot in the past, seal immediately. */
    uint64_t now = ac_now_ms();
    uint64_t wall_slot = ac_chain_current_slot(cs->chain);
    bool catching_up = wall_slot > p->slot + 1;
    if (p->threshold_reached_ms == 0) p->threshold_reached_ms = now;
    if (!force_seal && !catching_up && now - p->threshold_reached_ms < AC_SEAL_GRACE_MS) {
        ac_chain_unlock(cs->chain);
        return;
    }

    /* Move signers into the block and try to accept. */
    p->block.signers = (ac_commit_signer_t *)calloc(p->nsigners, sizeof(ac_commit_signer_t));
    if (!p->block.signers) { ac_chain_unlock(cs->chain); return; }
    memcpy(p->block.signers, p->signers, p->nsigners * sizeof(ac_commit_signer_t));
    p->block.nsigners = p->nsigners;

    ac_accept_t r = ac_chain_accept_block(cs->chain, &p->block);
    ac_chain_unlock(cs->chain);

    if (r == AC_ACCEPT_OK || r == AC_ACCEPT_DUP) {
        p->committed = true;
        LOG_I("consen.", "committed h=%" PRIu64 " slot=%" PRIu64 " signers=%u (status=%s)",
              p->height, p->slot, p->nsigners, ac_accept_str(r));
        /* Remove included txs from mempool. */
        for (uint32_t i = 0; i < p->block.tx_count; ++i) {
            ac_hash_t h;
            ac_tx_hash(&h, &p->block.txs[i]);
            ac_mempool_remove(cs->mempool, &h);
        }
        /* Re-broadcast the fully-signed block so lagging peers can accept it
         * directly without rebuilding the commit certificate from votes. */
        if (cs->bcast) {
            uint8_t *enc = NULL; size_t enc_len = 0;
            if (ac_block_encode(&enc, &enc_len, &p->block) >= 0) {
                cs->bcast(0x06, enc, enc_len, cs->bcast_ctx);
                free(enc);
            }
        }
    } else {
        LOG_W("consen.", "commit rejected h=%" PRIu64 ": %s", p->height, ac_accept_str(r));
        /* Reset signers ownership to local array. */
        free(p->block.signers);
        p->block.signers = NULL;
        p->block.nsigners = 0;
    }
}

/* -------------------------------------------------------------------------- */
/* Block proposal handler.                                                    */
/* -------------------------------------------------------------------------- */

void ac_consensus_handle_block(ac_consensus_t *cs, const uint8_t *buf, size_t len) {
    ac_block_t b;
    if (ac_block_decode(&b, buf, len) < 0) {
        LOG_D("consen.", "decode block failed");
        return;
    }

    /* Quick sanity: version, height, parent. */
    if (b.header.version != AC_BLOCK_VERSION) { ac_block_free(&b); return; }

    pthread_mutex_lock(&cs->mu);

    ac_hash_t bh;
    ac_block_hash(&bh, &b.header);

    /* If we already have it, fold any external signers into our local set. */
    pending_t *p = pending_find(cs, &bh);
    if (p) {
        /* Merge signers. */
        for (uint32_t i = 0; i < b.nsigners; ++i) {
            bool seen = false;
            for (uint32_t j = 0; j < p->nsigners; ++j) {
                if (ac_addr_eq(&p->signers[j].signer, &b.signers[i].signer)) { seen = true; break; }
            }
            if (!seen && p->nsigners < AC_COMMITTEE_MAX) {
                p->signers[p->nsigners++] = b.signers[i];
            }
        }
        ac_block_free(&b);
        try_commit(cs, p);
        pthread_mutex_unlock(&cs->mu);
        return;
    }

    /* New proposal. Verify the proposer's VRF proof at least. */
    ac_chain_lock(cs->chain);
    uint64_t our_height = ac_chain_height(cs->chain);
    if (b.header.height > our_height + 1) {
        /* Future block: we're lagging. Request the gap, but rate-limit so
         * gossip-driven floods (one BLOCK_ANN per slot) do not fan out
         * into one HEADERS_REQ per slot. */
        uint64_t target = b.header.height;
        ac_chain_unlock(cs->chain);
        ac_block_free(&b);
        uint64_t now = ac_now_ms();
        bool stale = (now - cs->last_hdrs_req_ms) >= AC_HDRS_REQ_MIN_INTERVAL_MS;
        bool gap_grew = target > cs->last_hdrs_req_target + 64;
        bool send_req = (cs->bcast != NULL) && (stale || gap_grew);
        if (send_req) {
            cs->last_hdrs_req_ms     = now;
            cs->last_hdrs_req_target = target;
        }
        pthread_mutex_unlock(&cs->mu);
        if (send_req) {
            uint8_t req[12];
            ac_be64(req, our_height + 1);
            uint32_t count = (uint32_t)(target - our_height);
            if (count > 256) count = 256;
            ac_be32(req + 8, count);
            cs->bcast(AC_MSG_HEADERS_REQ, req, sizeof(req), cs->bcast_ctx);
        }
        return;
    }
    if (b.header.height != our_height + 1 ||
        !ac_hash_eq(&b.header.parent_hash, ac_chain_tip_hash(cs->chain))) {
        ac_chain_unlock(cs->chain);
        ac_block_free(&b);
        pthread_mutex_unlock(&cs->mu);
        return;
    }
    ac_hash_t seed;
    ac_chain_epoch_seed(cs->chain, ac_epoch_of(b.header.slot), &seed);
    ac_chain_unlock(cs->chain);

    uint8_t alpha[64];
    int an = leader_alpha(alpha, &seed, b.header.slot);
    ac_vrf_proof_t proof;
    memcpy(proof.b, b.header.proposer_vrf_proof, AC_VRF_PROOF_SIZE);
    ac_vrf_out_t beta;
    if (!ac_vrf_verify(&beta, &proof, alpha, (size_t)an, b.header.proposer.b)) {
        ac_block_free(&b);
        pthread_mutex_unlock(&cs->mu);
        return;
    }

    /* Stash. */
    p = pending_alloc(cs);
    p->height = b.header.height;
    p->slot   = b.header.slot;
    p->block_hash = bh;
    p->block = b;
    /* Move signers (b owned them; we now own the structure). */
    p->nsigners = b.nsigners;
    for (uint32_t i = 0; i < b.nsigners && i < AC_COMMITTEE_MAX; ++i) {
        p->signers[i] = b.signers[i];
    }
    /* Detach signers from the embedded block (we keep them in p->signers). */
    if (p->block.signers) { free(p->block.signers); p->block.signers = NULL; p->block.nsigners = 0; }

    LOG_D("consen.", "received block h=%" PRIu64 " slot=%" PRIu64 " txs=%u",
          b.header.height, b.header.slot, b.header.tx_count);

    /* Compute the proposer's VRF priority and record it on the pending
     * entry. The actual commit vote is deferred to vote_phase() so every
     * committee member can converge on the lowest-priority proposal. */
    ac_chain_lock(cs->chain);
    ac_account_t pa;
    ac_state_get(ac_chain_state(cs->chain), &b.header.proposer, &pa);
    ac_chain_unlock(cs->chain);
    compute_priority(p->priority, &beta, ac_isqrt_u64(pa.stake));

    slot_vote_t *sv = slot_vote_for(cs, b.header.slot);
    if (priority_cmp(p->priority, sv->best_priority) < 0) {
        memcpy(sv->best_priority, p->priority, 16);
    }

    /* If we already accumulated 2/3 of votes (e.g. a fully-signed block was
     * rebroadcast), commit immediately. The vote phase will skip slots that
     * have already committed. */
    try_commit(cs, p);
    pthread_mutex_unlock(&cs->mu);
}

void ac_consensus_handle_vote(ac_consensus_t *cs, const uint8_t *buf, size_t len) {
    uint64_t height = 0;
    ac_hash_t bh;
    ac_addr_t signer;
    ac_sig_t  sig;
    ac_vrf_proof_t proof;
    if (vote_decode(buf, len, &height, &bh, &signer, &sig, &proof) < 0) return;

    /* Verify signature over vote message. */
    uint8_t vmsg[64];
    int vn = ac_vote_message(vmsg, height, &bh);
    if (!ac_verify(&sig, vmsg, (size_t)vn, signer.b)) return;

    pthread_mutex_lock(&cs->mu);
    pending_t *p = pending_find(cs, &bh);
    if (!p) { pthread_mutex_unlock(&cs->mu); return; }

    /* Verify VRF proof for committee membership at p->slot. */
    ac_chain_lock(cs->chain);
    ac_hash_t seed;
    ac_chain_epoch_seed(cs->chain, ac_epoch_of(p->slot), &seed);
    uint64_t total_sqrt = ac_chain_total_sqrt_stake(cs->chain);
    ac_account_t sa;
    ac_state_get(ac_chain_state(cs->chain), &signer, &sa);
    ac_chain_unlock(cs->chain);

    if (sa.stake < AC_MIN_STAKE_UCRD) { pthread_mutex_unlock(&cs->mu); return; }

    uint8_t calpha[64];
    int can = committee_alpha(calpha, &seed, p->slot);
    ac_vrf_out_t beta;
    if (!ac_vrf_verify(&beta, &proof, calpha, (size_t)can, signer.b)) {
        pthread_mutex_unlock(&cs->mu); return;
    }
    uint64_t draw = ac_rd64(beta.b);
    if (!committee_eligible(draw, ac_isqrt_u64(sa.stake), total_sqrt)) {
        pthread_mutex_unlock(&cs->mu); return;
    }

    /* Append unless dup. */
    for (uint32_t i = 0; i < p->nsigners; ++i) {
        if (ac_addr_eq(&p->signers[i].signer, &signer)) {
            pthread_mutex_unlock(&cs->mu); return;
        }
    }
    if (p->nsigners < AC_COMMITTEE_MAX) {
        p->signers[p->nsigners].signer = signer;
        p->signers[p->nsigners].sig    = sig;
        memcpy(p->signers[p->nsigners].vrf_proof, proof.b, AC_VRF_PROOF_SIZE);
        p->nsigners++;
    }

    try_commit(cs, p);
    pthread_mutex_unlock(&cs->mu);
}

/* -------------------------------------------------------------------------- */
/* Slot timer thread.                                                         */
/* -------------------------------------------------------------------------- */

static void slot_routine(ac_consensus_t *cs, uint64_t slot) {
    if (!cs->validator) return;

    ac_vrf_proof_t lproof;
    if (!am_i_leader(cs, slot, &lproof)) return;

    /* Take a tx snapshot from mempool. */
    ac_tx_t *snap = (ac_tx_t *)calloc(256, sizeof(ac_tx_t));
    if (!snap) return;
    size_t n = ac_mempool_snapshot(cs->mempool, snap, 256);
    /* Prune expired. */
    ac_mempool_prune_expired(cs->mempool, slot);

    ac_chain_lock(cs->chain);
    ac_block_t b;
    int rc = ac_chain_build_block(cs->chain, slot, &cs->kp, snap, (uint32_t)n, &b);
    ac_chain_unlock(cs->chain);
    free(snap);

    if (rc < 0) {
        return;
    }

    /* Replace VRF proof with the one we just computed (proves leader). */
    memcpy(b.header.proposer_vrf_proof, lproof.b, AC_VRF_PROOF_SIZE);

    pthread_mutex_lock(&cs->mu);
    ac_hash_t bh;
    ac_block_hash(&bh, &b.header);

    /* Stash as pending. */
    pending_t *p = pending_alloc(cs);
    p->height = b.header.height;
    p->slot   = slot;
    p->block_hash = bh;
    p->block = b;
    p->nsigners = 0;
    if (p->block.signers) { free(p->block.signers); p->block.signers = NULL; p->block.nsigners = 0; }

    /* Compute and record our own proposer priority for this slot. We do
     * NOT vote yet — vote_phase() runs at slot+AC_VOTE_DELAY_MS and selects
     * the lowest-priority proposal among everything we have seen. */
    ac_chain_lock(cs->chain);
    ac_account_t me_acct;
    ac_state_get(ac_chain_state(cs->chain), &cs->my_addr, &me_acct);
    ac_hash_t pseed;
    ac_chain_epoch_seed(cs->chain, ac_epoch_of(slot), &pseed);
    ac_chain_unlock(cs->chain);

    uint8_t palpha[64];
    int pan = leader_alpha(palpha, &pseed, slot);
    ac_vrf_out_t pbeta;
    ac_vrf_proof_t ptmp = lproof;
    ac_vrf_verify(&pbeta, &ptmp, palpha, (size_t)pan, cs->kp.pk);
    compute_priority(p->priority, &pbeta, ac_isqrt_u64(me_acct.stake));

    slot_vote_t *sv = slot_vote_for(cs, slot);
    if (priority_cmp(p->priority, sv->best_priority) < 0) {
        memcpy(sv->best_priority, p->priority, 16);
    }

    /* Broadcast block (without commit cert). */
    uint8_t *enc = NULL;
    size_t enc_len = 0;
    if (ac_block_encode(&enc, &enc_len, &p->block) >= 0) {
        if (cs->bcast) cs->bcast(0x06, enc, enc_len, cs->bcast_ctx);
        free(enc);
    }

    try_commit(cs, p);
    pthread_mutex_unlock(&cs->mu);

    LOG_I("consen.", "proposed block h=%" PRIu64 " slot=%" PRIu64 " txs=%u",
          b.header.height, slot, b.header.tx_count);
}

/* Vote phase: invoked at slot_start + AC_VOTE_DELAY_MS. Selects the lowest-
 * priority pending proposal for this slot and emits exactly one commit vote
 * for it. Every honest committee member runs the same selection over (with
 * high probability) the same proposal set, so the network converges on a
 * single block per slot regardless of how many leaders proposed. */
static void vote_phase(ac_consensus_t *cs, uint64_t slot) {
    if (!cs->validator) return;
    pthread_mutex_lock(&cs->mu);

    slot_vote_t *sv = slot_vote_for(cs, slot);
    if (sv->voted) { pthread_mutex_unlock(&cs->mu); return; }

    pending_t *best = NULL;
    for (size_t i = 0; i < AC_PENDING_MAX; ++i) {
        pending_t *p = &cs->pending[i];
        if (!p->occupied || p->slot != slot || p->committed) continue;
        if (!best || priority_cmp(p->priority, best->priority) < 0) best = p;
    }
    if (!best) { pthread_mutex_unlock(&cs->mu); return; }

    our_vote_now(cs, best);
    try_commit(cs, best);
    pthread_mutex_unlock(&cs->mu);
}

/* Seal phase: invoked at slot_start + AC_VOTE_DELAY_MS + AC_SEAL_GRACE_MS,
 * the latest point in the slot still available before the next slot's
 * propose phase begins. Forces any pending block whose 2/3 threshold has
 * been reached to commit immediately, even if its grace window has not
 * fully elapsed. This guarantees a block is sealed within ~1.4 s of slot
 * start in every committable case. */
static void seal_phase(ac_consensus_t *cs, uint64_t slot) {
    pthread_mutex_lock(&cs->mu);
    for (size_t i = 0; i < AC_PENDING_MAX; ++i) {
        pending_t *p = &cs->pending[i];
        if (!p->occupied || p->slot != slot || p->committed) continue;
        try_commit_inner(cs, p, /*force_seal=*/ true);
    }
    pthread_mutex_unlock(&cs->mu);
}

static void *slot_loop(void *arg) {
    ac_consensus_t *cs = (ac_consensus_t *)arg;
    uint64_t genesis = ac_chain_genesis_time(cs->chain);

    /* Align to the start of the next slot. */
    uint64_t now = ac_now_ms();
    if (now < genesis) ac_sleep_ms(genesis - now);
    now = ac_now_ms();
    uint64_t current_slot = (now - genesis) / AC_SLOT_DURATION_MS;
    uint64_t slot_start = genesis + (current_slot + 1) * AC_SLOT_DURATION_MS;
    if (slot_start > now) ac_sleep_ms(slot_start - now);

    uint64_t slot = current_slot + 1;

    while (cs->running) {
        /* Phase 1 — at slot start: propose (if leader). */
        slot_routine(cs, slot);

        /* Phase 2 — at slot_start + AC_VOTE_DELAY_MS: vote on lowest-priority
         * proposal we've seen for this slot. */
        slot_start = genesis + slot * AC_SLOT_DURATION_MS;
        uint64_t vote_at = slot_start + AC_VOTE_DELAY_MS;
        now = ac_now_ms();
        if (vote_at > now) ac_sleep_ms(vote_at - now);
        if (!cs->running) break;

        vote_phase(cs, slot);

        /* Phase 3 — at slot_start + AC_VOTE_DELAY_MS + AC_SEAL_GRACE_MS:
         * force-seal any pending block whose threshold has been reached. */
        uint64_t seal_at = slot_start + AC_VOTE_DELAY_MS + AC_SEAL_GRACE_MS;
        now = ac_now_ms();
        if (seal_at > now) ac_sleep_ms(seal_at - now);
        if (!cs->running) break;

        seal_phase(cs, slot);

        /* Phase 4 — sleep until next slot start, then loop. */
        slot++;
        uint64_t next_start = genesis + slot * AC_SLOT_DURATION_MS;
        now = ac_now_ms();
        if (next_start > now) ac_sleep_ms(next_start - now);
    }
    return NULL;
}

/* -------------------------------------------------------------------------- */
/* Lifecycle.                                                                 */
/* -------------------------------------------------------------------------- */

ac_consensus_t *ac_consensus_new(const ac_consensus_config_t *cfg) {
    if (!cfg || !cfg->chain || !cfg->mempool) return NULL;
    ac_consensus_t *cs = (ac_consensus_t *)calloc(1, sizeof(*cs));
    if (!cs) return NULL;
    pthread_mutex_init(&cs->mu, NULL);
    cs->chain     = cfg->chain;
    cs->mempool   = cfg->mempool;
    cs->kp        = cfg->keypair;
    cs->bcast     = cfg->broadcast;
    cs->bcast_ctx = cfg->broadcast_ctx;
    cs->validator = cfg->validator;
    memcpy(cs->my_addr.b, cfg->keypair.pk, AC_PUBKEY_SIZE);
    return cs;
}

void ac_consensus_free(ac_consensus_t *cs) {
    if (!cs) return;
    if (cs->running) ac_consensus_stop(cs);
    for (size_t i = 0; i < AC_PENDING_MAX; ++i) pending_init(&cs->pending[i]);
    pthread_mutex_destroy(&cs->mu);
    free(cs);
}

int ac_consensus_start(ac_consensus_t *cs) {
    cs->running = true;
    if (pthread_create(&cs->slot_thread, NULL, slot_loop, cs) != 0) {
        cs->running = false;
        return -1;
    }
    return 0;
}

void ac_consensus_stop(ac_consensus_t *cs) {
    cs->running = false;
    pthread_join(cs->slot_thread, NULL);
}
