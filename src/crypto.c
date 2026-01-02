#include <stdint.h>
#include <string.h>
#include <sodium.h>
#include "types.h"

static void store_u64_le(uint8_t out[8], uint64_t x) {
    out[0] = (uint8_t)(x);
    out[1] = (uint8_t)(x >> 8);
    out[2] = (uint8_t)(x >> 16);
    out[3] = (uint8_t)(x >> 24);
    out[4] = (uint8_t)(x >> 32);
    out[5] = (uint8_t)(x >> 40);
    out[6] = (uint8_t)(x >> 48);
    out[7] = (uint8_t)(x >> 56);
}

static void store_u32_le(uint8_t out[4], uint32_t x) {
    out[0] = (uint8_t)(x);
    out[1] = (uint8_t)(x >> 8);
    out[2] = (uint8_t)(x >> 16);
    out[3] = (uint8_t)(x >> 24);
}

/*
 * Hash canonico per firma (32 bytes) su tx.
 * Ritorna 0 ok, -1 errore.
 */
static int tx_hash_for_signature(const tx *t, uint8_t out_hash[32]) {
    if (!t || !out_hash || !t->signer) return -1;

    crypto_generichash_state st;
    if (crypto_generichash_init(&st, NULL, 0, 32) != 0) {
        return -1;
    }

    // Domain separation: evita collisioni di contesto con altri hash
    static const uint8_t DOMAIN[] = { 'T','X','S','I','G','v','1' };
    crypto_generichash_update(&st, DOMAIN, sizeof(DOMAIN));

    uint8_t b8[8];
    uint8_t b4[4];

    // expire (u64 LE)
    store_u64_le(b8, t->expire);
    crypto_generichash_update(&st, b8, sizeof b8);

    // function_id (u8)
    crypto_generichash_update(&st, &t->function_id, sizeof(t->function_id));

    // accounts_num (u32 LE) + lista accounts (pubkey in ordine)
    store_u32_le(b4, t->accounts_num);
    crypto_generichash_update(&st, b4, sizeof b4);

    // signer pubkey (32B)
    crypto_generichash_update(&st, t->signer->pub_key, crypto_sign_PUBLICKEYBYTES);

    // accounts list: includi pubkey di ciascun account nell'ordine della lista
    // (se vuoi un ordine canonico indipendente dalla lista, devi ordinare per pubkey prima)
    {
        uint32_t i = 0;
        account_list_node *cur = t->accounts;

        while (cur && i < t->accounts_num) {
            if (!cur->acc) {
                sodium_memzero(&st, sizeof st);
                return -1;
            }
            crypto_generichash_update(&st, cur->acc->pub_key, crypto_sign_PUBLICKEYBYTES);
            cur = cur->next;
            i++;
        }

        // Se accounts_num dichiara più elementi di quelli presenti -> formato invalido
        if (i != t->accounts_num) {
            sodium_memzero(&st, sizeof st);
            return -1;
        }
    }

    // data: (u32 LE len) + bytes
    if (t->data) {
        if (t->data->data_len > TX_DATA_MAX_SIZE) {
            sodium_memzero(&st, sizeof st);
            return -1;
        }

        store_u32_le(b4, t->data->data_len);
        crypto_generichash_update(&st, b4, sizeof b4);

        if (t->data->data_len > 0) {
            crypto_generichash_update(&st, t->data->data, t->data->data_len);
        }
    } else {
        // data assente => len = 0
        store_u32_le(b4, 0);
        crypto_generichash_update(&st, b4, sizeof b4);
    }

    crypto_generichash_final(&st, out_hash, 32);
    sodium_memzero(&st, sizeof st);
    return 0;
}


int generate_keys(account* a) {
    if (!a) return -1;
    if (crypto_sign_keypair(a->pub_key, a->priv_key) != 0) return -2;
    return 0;
}

int generate_keys_from_seed(account* a, const uint8_t seed[crypto_sign_SEEDBYTES]) {
    if (!a || !seed) return -1;
    if (crypto_sign_seed_keypair(a->pub_key, a->priv_key, seed) != 0) return -2;
    return 0;
}

int derive_pub_key(const uint8_t priv_key[crypto_sign_SECRETKEYBYTES], uint8_t pub_key_out[crypto_sign_PUBLICKEYBYTES]) {
    if (!priv_key || !pub_key_out) return -1;
    if (crypto_sign_ed25519_sk_to_pk(pub_key_out, priv_key) != 0) return -2;
    return 0;
}

int sign_tx(tx* t, const account* signer) {
    if (!t || !signer || !t->signer) return -1;

    // chi firma deve essere il signer
    if (memcmp(signer->pub_key, t->signer->pub_key, crypto_sign_PUBLICKEYBYTES) != 0) {
        return -2;
    }

    uint8_t h[32];
    if (tx_hash_for_signature(t, h) != 0) return -3;

    // Firma l'hash
    if (crypto_sign_detached(t->signature, NULL, h, sizeof h, signer->priv_key) != 0) {
        sodium_memzero(h, sizeof h);
        return -4;
    }

    sodium_memzero(h, sizeof h);
    return 0;
}

int verify_tx(const tx* t) {
    if (!t || !t->signer) return -1;

    uint8_t h[32];
    if (tx_hash_for_signature(t, h) != 0) return -2;

    int ok = crypto_sign_verify_detached(
        t->signature,
        h, sizeof h,
        t->signer->pub_key
    );

    sodium_memzero(h, sizeof h);
    return (ok == 0) ? 0 : -3; // 0 = valida, <0 = non valida
}
