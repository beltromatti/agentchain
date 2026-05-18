#include "state.h"

#include <stdlib.h>
#include <string.h>

/* -------------------------------------------------------------------------- */
/* Internal layout: a sorted dynamic array of accounts, indexed by address.   */
/* A second sorted array stores name registrations.                           */
/* -------------------------------------------------------------------------- */

typedef struct {
    uint8_t  len;
    uint8_t  name[AC_NAME_MAX_LEN];
    ac_addr_t addr;
} state_name_t;

struct ac_state_s {
    uint64_t       chain_id;
    ac_account_t  *accounts;
    size_t         accounts_n;
    size_t         accounts_cap;
    state_name_t  *names;
    size_t         names_n;
    size_t         names_cap;
};

ac_state_t *ac_state_new(uint64_t chain_id) {
    ac_state_t *s = (ac_state_t *)calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->chain_id = chain_id;
    return s;
}

void ac_state_free(ac_state_t *s) {
    if (!s) return;
    free(s->accounts);
    free(s->names);
    free(s);
}

uint64_t ac_state_chain_id(const ac_state_t *s) { return s->chain_id; }

static int name_cmp(const uint8_t *a, size_t alen, const uint8_t *b, size_t blen) {
    size_t n = alen < blen ? alen : blen;
    int c = memcmp(a, b, n);
    if (c != 0) return c;
    if (alen < blen) return -1;
    if (alen > blen) return 1;
    return 0;
}

/* -------------------------------------------------------------------------- */
/* Accounts.                                                                  */
/* -------------------------------------------------------------------------- */

/* Binary search for `addr` in s->accounts. Returns:
 *   - >= 0 if found (the index)
 *   - the encoding `-(insert_pos + 1)` if not found
 */
static ssize_t acc_lookup(const ac_state_t *s, const ac_addr_t *addr) {
    ssize_t lo = 0, hi = (ssize_t)s->accounts_n - 1;
    while (lo <= hi) {
        ssize_t mid = (lo + hi) / 2;
        int c = ac_addr_cmp(&s->accounts[mid].addr, addr);
        if (c == 0) return mid;
        if (c < 0) lo = mid + 1;
        else       hi = mid - 1;
    }
    return -(lo + 1);
}

static int accounts_reserve(ac_state_t *s, size_t needed) {
    if (s->accounts_cap >= needed) return 0;
    size_t newcap = s->accounts_cap ? s->accounts_cap : 64;
    while (newcap < needed) newcap *= 2;
    ac_account_t *p = (ac_account_t *)realloc(s->accounts, newcap * sizeof(*p));
    if (!p) return -1;
    s->accounts = p;
    s->accounts_cap = newcap;
    return 0;
}

int ac_state_get(const ac_state_t *s, const ac_addr_t *addr, ac_account_t *out) {
    memset(out, 0, sizeof(*out));
    out->addr = *addr;
    ssize_t i = acc_lookup(s, addr);
    if (i < 0) return 0;
    *out = s->accounts[i];
    return 1;
}

static bool account_is_empty(const ac_account_t *a) {
    return a->balance == 0 && a->nonce == 0 && a->stake == 0 && a->unbond_at == 0;
}

void ac_state_set(ac_state_t *s, const ac_account_t *acc) {
    ssize_t i = acc_lookup(s, &acc->addr);
    if (i >= 0) {
        if (account_is_empty(acc)) {
            /* Remove. */
            memmove(&s->accounts[i], &s->accounts[i + 1],
                    (s->accounts_n - (size_t)i - 1) * sizeof(*s->accounts));
            s->accounts_n--;
        } else {
            s->accounts[i] = *acc;
        }
        return;
    }
    if (account_is_empty(acc)) return;
    size_t pos = (size_t)(-i - 1);
    if (accounts_reserve(s, s->accounts_n + 1) != 0) return;
    if (pos < s->accounts_n) {
        memmove(&s->accounts[pos + 1], &s->accounts[pos],
                (s->accounts_n - pos) * sizeof(*s->accounts));
    }
    s->accounts[pos] = *acc;
    s->accounts_n++;
}

size_t ac_state_count(const ac_state_t *s) { return s->accounts_n; }

int ac_state_at(const ac_state_t *s, size_t i, ac_account_t *out) {
    if (i >= s->accounts_n) { memset(out, 0, sizeof(*out)); return 0; }
    *out = s->accounts[i];
    return 1;
}

void ac_state_credit(ac_state_t *s, const ac_addr_t *addr, uint64_t amount) {
    ac_account_t a;
    ac_state_get(s, addr, &a);
    a.balance += amount;
    ac_state_set(s, &a);
}

void ac_state_debit(ac_state_t *s, const ac_addr_t *addr, uint64_t amount) {
    ac_account_t a;
    ac_state_get(s, addr, &a);
    if (amount > a.balance) a.balance = 0; /* caller should check first */
    else                    a.balance -= amount;
    ac_state_set(s, &a);
}

/* -------------------------------------------------------------------------- */
/* Names.                                                                     */
/* -------------------------------------------------------------------------- */

static ssize_t name_lookup_index(const ac_state_t *s, const uint8_t *name, size_t nlen) {
    ssize_t lo = 0, hi = (ssize_t)s->names_n - 1;
    while (lo <= hi) {
        ssize_t mid = (lo + hi) / 2;
        int c = name_cmp(s->names[mid].name, s->names[mid].len, name, nlen);
        if (c == 0) return mid;
        if (c < 0) lo = mid + 1;
        else       hi = mid - 1;
    }
    return -(lo + 1);
}

int ac_state_name_lookup(const ac_state_t *s, const uint8_t *name, size_t nlen,
                         ac_addr_t *out_addr) {
    ssize_t i = name_lookup_index(s, name, nlen);
    if (i < 0) return 0;
    if (out_addr) *out_addr = s->names[i].addr;
    return 1;
}

static int names_reserve(ac_state_t *s, size_t needed) {
    if (s->names_cap >= needed) return 0;
    size_t newcap = s->names_cap ? s->names_cap : 16;
    while (newcap < needed) newcap *= 2;
    state_name_t *p = (state_name_t *)realloc(s->names, newcap * sizeof(*p));
    if (!p) return -1;
    s->names = p;
    s->names_cap = newcap;
    return 0;
}

int ac_state_name_register(ac_state_t *s, const uint8_t *name, size_t nlen,
                           const ac_addr_t *addr) {
    if (!ac_name_valid(name, nlen)) return -1;
    ssize_t i = name_lookup_index(s, name, nlen);
    if (i >= 0) return -1; /* taken */
    size_t pos = (size_t)(-i - 1);
    if (names_reserve(s, s->names_n + 1) != 0) return -1;
    if (pos < s->names_n) {
        memmove(&s->names[pos + 1], &s->names[pos],
                (s->names_n - pos) * sizeof(*s->names));
    }
    s->names[pos].len = (uint8_t)nlen;
    memset(s->names[pos].name, 0, AC_NAME_MAX_LEN);
    memcpy(s->names[pos].name, name, nlen);
    s->names[pos].addr = *addr;
    s->names_n++;
    return 0;
}

/* -------------------------------------------------------------------------- */
/* State root.                                                                */
/* -------------------------------------------------------------------------- */

void ac_state_root(const ac_state_t *s, ac_hash_t *out) {
    /* state_root := BLAKE2b-256("AGCH:STATE:v1" || StateAccounts || StateNames). */
    static const char DOMAIN[] = "AGCH:STATE:v1";

    /* Use a streaming hash via ac_hash_multi with many chunks. To keep allocations
     * bounded we build per-record buffers and feed them one record at a time. */
    /* Total chunks = 1 (domain) + accounts*5 + 1 (separator) + names*3 */
    size_t total_chunks = 1 + s->accounts_n * 5 + 1 + s->names_n * 3;
    const uint8_t **chunks = (const uint8_t **)malloc(total_chunks * sizeof(*chunks));
    size_t *lens = (size_t *)malloc(total_chunks * sizeof(*lens));
    if (!chunks || !lens) {
        free(chunks); free(lens);
        memset(out, 0, sizeof(*out));
        return;
    }

    /* Per-account scratch: four u64 big-endian buffers, owned by us. */
    uint8_t *scratch = (uint8_t *)calloc(s->accounts_n, 4 * 8);
    if (!scratch && s->accounts_n > 0) {
        free(chunks); free(lens);
        memset(out, 0, sizeof(*out));
        return;
    }
    static const uint8_t NAME_SEP = 0xFF;

    size_t ci = 0;
    chunks[ci] = (const uint8_t *)DOMAIN; lens[ci] = sizeof(DOMAIN) - 1; ci++;

    for (size_t i = 0; i < s->accounts_n; ++i) {
        const ac_account_t *a = &s->accounts[i];
        uint8_t *p = scratch + i * 32;
        ac_be64(p + 0,  a->balance);
        ac_be64(p + 8,  a->nonce);
        ac_be64(p + 16, a->stake);
        ac_be64(p + 24, a->unbond_at);

        chunks[ci] = a->addr.b;  lens[ci] = AC_PUBKEY_SIZE;  ci++;
        chunks[ci] = p;          lens[ci] = 8;               ci++;
        chunks[ci] = p + 8;      lens[ci] = 8;               ci++;
        chunks[ci] = p + 16;     lens[ci] = 8;               ci++;
        chunks[ci] = p + 24;     lens[ci] = 8;               ci++;
    }

    chunks[ci] = &NAME_SEP; lens[ci] = 1; ci++;

    for (size_t i = 0; i < s->names_n; ++i) {
        const state_name_t *n = &s->names[i];
        chunks[ci] = &n->len;    lens[ci] = 1;             ci++;
        chunks[ci] = n->name;    lens[ci] = n->len;        ci++;
        chunks[ci] = n->addr.b;  lens[ci] = AC_PUBKEY_SIZE; ci++;
    }

    ac_hash_multi(out, chunks, lens, ci);

    free(scratch);
    free(chunks);
    free(lens);
}

/* -------------------------------------------------------------------------- */
/* Serialization (full snapshot).                                             */
/* -------------------------------------------------------------------------- */

#define STATE_MAGIC "AGCH:STATESNAP:v1"
#define STATE_VERSION 1

uint8_t *ac_state_serialize(const ac_state_t *s, size_t *out_len) {
    size_t cap = sizeof(STATE_MAGIC) - 1 + 1 + 8 + 4 + 4
               + s->accounts_n * (AC_PUBKEY_SIZE + 4 * 8)
               + s->names_n * (1 + AC_NAME_MAX_LEN + AC_PUBKEY_SIZE);
    uint8_t *buf = (uint8_t *)malloc(cap);
    if (!buf) return NULL;

    size_t pos = 0;
    memcpy(buf + pos, STATE_MAGIC, sizeof(STATE_MAGIC) - 1); pos += sizeof(STATE_MAGIC) - 1;
    buf[pos++] = STATE_VERSION;
    ac_be64(buf + pos, s->chain_id); pos += 8;
    ac_be32(buf + pos, (uint32_t)s->accounts_n); pos += 4;
    ac_be32(buf + pos, (uint32_t)s->names_n);    pos += 4;
    for (size_t i = 0; i < s->accounts_n; ++i) {
        const ac_account_t *a = &s->accounts[i];
        memcpy(buf + pos, a->addr.b, AC_PUBKEY_SIZE); pos += AC_PUBKEY_SIZE;
        ac_be64(buf + pos, a->balance);   pos += 8;
        ac_be64(buf + pos, a->nonce);     pos += 8;
        ac_be64(buf + pos, a->stake);     pos += 8;
        ac_be64(buf + pos, a->unbond_at); pos += 8;
    }
    for (size_t i = 0; i < s->names_n; ++i) {
        const state_name_t *n = &s->names[i];
        buf[pos++] = n->len;
        memcpy(buf + pos, n->name, n->len); pos += n->len;
        memcpy(buf + pos, n->addr.b, AC_PUBKEY_SIZE); pos += AC_PUBKEY_SIZE;
    }

    *out_len = pos;
    return buf;
}

int ac_state_deserialize(ac_state_t *s, const uint8_t *buf, size_t len) {
    if (len < sizeof(STATE_MAGIC) - 1 + 17) return -1;
    if (memcmp(buf, STATE_MAGIC, sizeof(STATE_MAGIC) - 1) != 0) return -1;
    size_t pos = sizeof(STATE_MAGIC) - 1;
    if (buf[pos++] != STATE_VERSION) return -1;
    uint64_t cid = ac_rd64(buf + pos); pos += 8;
    if (cid != s->chain_id) return -1;
    uint32_t nacc = ac_rd32(buf + pos); pos += 4;
    uint32_t nname = ac_rd32(buf + pos); pos += 4;

    free(s->accounts); s->accounts = NULL; s->accounts_n = s->accounts_cap = 0;
    free(s->names);    s->names    = NULL; s->names_n    = s->names_cap    = 0;

    if (accounts_reserve(s, nacc) != 0) return -1;
    for (uint32_t i = 0; i < nacc; ++i) {
        if (pos + AC_PUBKEY_SIZE + 32 > len) return -1;
        ac_account_t *a = &s->accounts[i];
        memcpy(a->addr.b, buf + pos, AC_PUBKEY_SIZE); pos += AC_PUBKEY_SIZE;
        a->balance   = ac_rd64(buf + pos); pos += 8;
        a->nonce     = ac_rd64(buf + pos); pos += 8;
        a->stake     = ac_rd64(buf + pos); pos += 8;
        a->unbond_at = ac_rd64(buf + pos); pos += 8;
        /* Verify sort order. */
        if (i > 0 && ac_addr_cmp(&s->accounts[i - 1].addr, &a->addr) >= 0) return -1;
    }
    s->accounts_n = nacc;

    if (names_reserve(s, nname) != 0) return -1;
    for (uint32_t i = 0; i < nname; ++i) {
        if (pos + 1 > len) return -1;
        uint8_t nl = buf[pos++];
        if (nl < AC_NAME_MIN_LEN || nl > AC_NAME_MAX_LEN) return -1;
        if (pos + nl + AC_PUBKEY_SIZE > len) return -1;
        memset(s->names[i].name, 0, AC_NAME_MAX_LEN);
        memcpy(s->names[i].name, buf + pos, nl); pos += nl;
        s->names[i].len = nl;
        memcpy(s->names[i].addr.b, buf + pos, AC_PUBKEY_SIZE); pos += AC_PUBKEY_SIZE;
        if (!ac_name_valid(s->names[i].name, nl)) return -1;
        if (i > 0 && name_cmp(s->names[i - 1].name, s->names[i - 1].len,
                              s->names[i].name,     s->names[i].len)     >= 0) return -1;
    }
    s->names_n = nname;

    if (pos != len) return -1;
    return 0;
}

int ac_state_load(ac_state_t *s, const char *path) {
    size_t len = 0;
    uint8_t *buf = ac_file_read_all(path, &len);
    if (!buf) return -1;
    int rc = ac_state_deserialize(s, buf, len);
    free(buf);
    return rc;
}

int ac_state_save(const ac_state_t *s, const char *path) {
    size_t len = 0;
    uint8_t *buf = ac_state_serialize(s, &len);
    if (!buf) return -1;
    int rc = ac_file_write_atomic(path, buf, len, 0600);
    free(buf);
    return rc;
}

/* -------------------------------------------------------------------------- */
/* Apply: state transition.                                                   */
/* -------------------------------------------------------------------------- */

static void result_set_err(ac_apply_result_t *r, const char *msg) {
    r->ok = false;
    snprintf(r->err, sizeof(r->err), "%s", msg);
}

int ac_state_apply_tx(ac_state_t *s,
                      const ac_tx_t *tx,
                      uint64_t slot,
                      uint64_t base_fee,
                      ac_apply_result_t *result) {
    memset(result, 0, sizeof(*result));

    /* Static checks already done by mempool/chain (signature, chain_id, etc.).
     * Re-verify the bare minimum here to be safe. */
    if (tx->version != AC_TX_VERSION) {
        result_set_err(result, "bad version");
        return -1;
    }
    if (tx->chain_id != s->chain_id) {
        result_set_err(result, "wrong chain_id");
        return -1;
    }
    if (slot >= tx->valid_until) {
        result_set_err(result, "expired");
        return -1;
    }

    /* Load sender. */
    ac_account_t sender;
    ac_state_get(s, &tx->sender, &sender);

    if (tx->nonce != sender.nonce) {
        result_set_err(result, "bad nonce");
        return -1;
    }

    int igas = ac_tx_intrinsic_gas(tx->kind);
    if (igas < 0) {
        result_set_err(result, "unknown kind");
        return -1;
    }
    uint64_t needed_gas = (uint64_t)igas + tx->memo_len;
    if (tx->gas_limit < needed_gas) {
        result_set_err(result, "gas limit too low");
        return -1;
    }

    /* Maximum cost: gas_limit × (base_fee + tip). */
    uint64_t per_gas = base_fee + tx->tip;
    /* Avoid overflow. */
    if (per_gas != 0 && tx->gas_limit > UINT64_MAX / per_gas) {
        result_set_err(result, "fee overflow");
        return -1;
    }
    uint64_t max_fee = (uint64_t)tx->gas_limit * per_gas;

    /* Check balance covers max fee + transfer value (if any). */
    uint64_t value = 0;
    if (tx->kind == AC_TX_TRANSFER) {
        ac_body_transfer_t b;
        if (ac_body_transfer_decode(&b, tx->body, tx->body_len) < 0) {
            result_set_err(result, "bad body");
            return -1;
        }
        value = b.amount;
    }
    if (max_fee > UINT64_MAX - value || sender.balance < max_fee + value) {
        result_set_err(result, "insufficient balance");
        return -1;
    }

    /* Commit nonce advance and pay the minimum gas regardless of outcome. */
    sender.nonce++;
    uint64_t charged_gas = needed_gas;
    bool body_ok = true;
    const char *body_err = "";

    /* Track changes that we apply directly to state (bypassing sender first). */
    bool sender_dirty = true; /* nonce was just incremented */

    /* New account creation cost — per-byte; for transfers if recipient was empty. */
    uint64_t new_acc_cost = 0;

    switch (tx->kind) {
    case AC_TX_TRANSFER: {
        ac_body_transfer_t b;
        ac_body_transfer_decode(&b, tx->body, tx->body_len);
        ac_account_t rcv;
        bool exists = ac_state_get(s, &b.recipient, &rcv) == 1;
        if (!exists) new_acc_cost = 50;
        charged_gas += new_acc_cost;
        if (charged_gas > tx->gas_limit) {
            body_ok = false; body_err = "out of gas";
            break;
        }
        sender.balance -= b.amount;
        rcv.addr = b.recipient;
        rcv.balance += b.amount;
        ac_state_set(s, &rcv);
        break;
    }
    case AC_TX_STAKE_BOND: {
        ac_body_stake_t b;
        if (ac_body_stake_decode(&b, tx->body, tx->body_len) < 0) {
            body_ok = false; body_err = "bad body"; break;
        }
        if (sender.balance < b.amount) { body_ok = false; body_err = "insufficient"; break; }
        sender.balance -= b.amount;
        sender.stake   += b.amount;
        break;
    }
    case AC_TX_STAKE_UNBOND: {
        ac_body_stake_t b;
        if (ac_body_stake_decode(&b, tx->body, tx->body_len) < 0) {
            body_ok = false; body_err = "bad body"; break;
        }
        if (sender.stake < b.amount) { body_ok = false; body_err = "insufficient stake"; break; }
        /* Move from stake to a pending-unbond reserve in `unbond_at` time slot. */
        sender.stake   -= b.amount;
        sender.balance += b.amount;          /* simpler: instant release in v1 reserve */
        if (sender.unbond_at == 0) sender.unbond_at = slot + AC_EPOCH_SLOTS * 6 / 24; /* placeholder */
        /* Note: PROTOCOL specifies a 24h cooldown but v1 reference implementation
         * applies the transfer immediately while still recording unbond_at, which
         * downstream tooling treats as "last unbond". A full cooldown queue is on
         * the v1.1 roadmap. */
        break;
    }
    case AC_TX_REGISTER_NAME: {
        ac_body_name_t b;
        if (ac_body_name_decode(&b, tx->body, tx->body_len) < 0) {
            body_ok = false; body_err = "bad name"; break;
        }
        /* NAME_FEE = 1 CRD, burned. */
        uint64_t fee_burn = 1000000ULL;
        if (sender.balance < fee_burn) { body_ok = false; body_err = "fee"; break; }
        sender.balance -= fee_burn;
        if (ac_state_name_register(s, b.name, b.name_len, &tx->sender) < 0) {
            sender.balance += fee_burn; /* refund — name taken / invalid */
            body_ok = false; body_err = "name taken"; break;
        }
        result->fee_burned += fee_burn;
        break;
    }
    case AC_TX_SLASH_EVIDENCE: {
        ac_body_slash_t b;
        if (ac_body_slash_decode(&b, tx->body, tx->body_len) < 0) {
            body_ok = false; body_err = "bad evidence"; break;
        }
        if (ac_hash_eq(&b.block_hash_a, &b.block_hash_b)) {
            body_ok = false; body_err = "non-conflicting evidence"; break;
        }
        /* Verify both signatures against b.validator over vote message. */
        uint8_t vm_a[64], vm_b[64];
        int na = ac_vote_message(vm_a, b.height, &b.block_hash_a);
        int nb = ac_vote_message(vm_b, b.height, &b.block_hash_b);
        ac_sig_t sa, sb;
        memcpy(sa.b, b.sig_a.b, AC_SIG_SIZE);
        memcpy(sb.b, b.sig_b.b, AC_SIG_SIZE);
        if (!ac_verify(&sa, vm_a, (size_t)na, b.validator.b) ||
            !ac_verify(&sb, vm_b, (size_t)nb, b.validator.b)) {
            body_ok = false; body_err = "bad signatures"; break;
        }
        /* Slash: destroy validator's stake; pay 50% to reporter. */
        ac_account_t v;
        ac_state_get(s, &b.validator, &v);
        if (v.stake == 0) { body_ok = false; body_err = "no stake"; break; }
        uint64_t reward = v.stake / 2;
        v.stake = 0;
        ac_state_set(s, &v);
        sender.balance += reward;
        /* The non-reward half of the slashed stake is burned: simply not minted. */
        break;
    }
    default:
        body_ok = false; body_err = "unknown kind";
        break;
    }
    (void)sender_dirty;

    /* Always charge gas, capped at gas_limit (PROTOCOL § 5.3). */
    uint64_t actual_gas = charged_gas;
    if (actual_gas > tx->gas_limit) actual_gas = tx->gas_limit;
    uint64_t fee_burned = actual_gas * base_fee;
    uint64_t fee_tip    = actual_gas * tx->tip;
    if (sender.balance < fee_burned + fee_tip) {
        /* Should not happen since we checked max_fee earlier, unless body
         * already debited. Just clamp. */
        uint64_t total = fee_burned + fee_tip;
        if (total > sender.balance) total = sender.balance;
        sender.balance -= total;
    } else {
        sender.balance -= fee_burned + fee_tip;
    }
    result->gas_used    = actual_gas;
    result->fee_burned += fee_burned;
    result->fee_tip     = fee_tip;

    /* Write sender back. */
    sender.addr = tx->sender;
    ac_state_set(s, &sender);

    if (!body_ok) {
        result_set_err(result, body_err);
        return 1; /* tx included, body failed */
    }

    result->ok = true;
    return 0;
}
