// txpool.c
#include <pthread.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
#include "types.h"

tx_pool TX_POOL = {
    .mtx = PTHREAD_MUTEX_INITIALIZER,
    .tx_num = 0,
    .txs = NULL
};

/* =========================
 *  TX destroy (pool-owned)
 * ========================= */

/*
 * Libera SOLO la lista dei nodi account_list_node.
 * Non libera i `account* acc` (owner esterno: stato/wallet).
 */
static void account_list_free_nodes(account_list_node *head) {
    while (head) {
        account_list_node *next = head->next;
        head->acc = NULL;
        head->next = NULL;
        free(head);
        head = next;
    }
}

/*
 * Distrugge completamente una tx posseduta dal pool:
 * - free(tx->data)
 * - free(lista tx->accounts) (solo nodi, non accounts)
 * - free(tx)
 */
static void tx_destroy(tx *t) {
    if (!t) return;

    if (t->data) {
        // tx_data contiene un array inline: basta free(data)
        free(t->data);
        t->data = NULL;
    }

    if (t->accounts) {
        account_list_free_nodes(t->accounts);
        t->accounts = NULL;
    }

    // signer NON viene liberato: owner esterno
    t->signer = NULL;

    // altri campi sono POD (expire, signature[], confirmed, ecc.)
    free(t);
}

/* =========================
 *  Ordering helpers
 * ========================= */

static int tx_expire_cmp_asc(const tx *a, const tx *b) {
    if (a == NULL && b == NULL) return 0;
    if (a == NULL) return 1;
    if (b == NULL) return -1;

    if (a->expire < b->expire) return -1;
    if (a->expire > b->expire) return 1;
    return 0;
}

/*
 * Inserisce t in lista ordinata per expire ASC.
 * Precondizione: mutex già acquisito.
 */
static int tx_pool_insert_sorted_locked(tx_pool *p, tx *t) {
    if (!p || !t) return -1;

    tx_list_node *node = (tx_list_node*)calloc(1, sizeof(tx_list_node));
    if (!node) return -1;

    node->transaction = t;
    node->next = NULL;

    if (p->txs == NULL) {
        p->txs = node;
        p->tx_num++;
        return 0;
    }

    // Inserimento in testa
    if (tx_expire_cmp_asc(t, p->txs->transaction) <= 0) {
        node->next = p->txs;
        p->txs = node;
        p->tx_num++;
        return 0;
    }

    // Inserimento dopo l'ultimo con expire <= t->expire
    tx_list_node *cur = p->txs;
    while (cur->next != NULL && tx_expire_cmp_asc(cur->next->transaction, t) <= 0) {
        cur = cur->next;
    }

    node->next = cur->next;
    cur->next = node;
    p->tx_num++;
    return 0;
}

/* =========================
 *  Public API
 * ========================= */

/*
 * Inserisce una tx nel pool mantenendo l'ordinamento per expire.
 * Pool diventa owner di `t` (e la libererà quando rimossa).
 *
 * Ritorna 0 ok, -1 errore.
 */
int tx_pool_push(tx* t) {
    if (!t) return -1;

    pthread_mutex_lock(&TX_POOL.mtx);
    int rc = tx_pool_insert_sorted_locked(&TX_POOL, t);
    pthread_mutex_unlock(&TX_POOL.mtx);

    return rc;
}

/*
 * Pop dei primi n elementi (expire più imminente).
 * Siccome il pool è owner, distrugge completamente le tx rimosse.
 *
 * Ritorna numero effettivo rimosso.
 */
int tx_pool_pop_n(size_t n) {
    pthread_mutex_lock(&TX_POOL.mtx);

    if (n == 0 || TX_POOL.tx_num == 0 || TX_POOL.txs == NULL) {
        pthread_mutex_unlock(&TX_POOL.mtx);
        return 0;
    }

    size_t removed = 0;
    while (removed < n && TX_POOL.txs != NULL) {
        tx_list_node *victim = TX_POOL.txs;
        TX_POOL.txs = victim->next;

        // pool-owned: distruggo la transazione
        tx_destroy(victim->transaction);
        victim->transaction = NULL;

        victim->next = NULL;
        free(victim);

        removed++;
        TX_POOL.tx_num--;
    }

    pthread_mutex_unlock(&TX_POOL.mtx);
    return (int)removed;
}

/*
 * Rimuove tutte le transazioni scadute (expire <= now).
 * Poiché la lista è ordinata per expire ASC, basta rimuovere dalla testa finché scadute.
 * Pool-owned => distrugge le tx rimosse.
 *
 * Ritorna quante rimosse. -1 su errore input.
 */
int remove_expired_txs(tx_pool *p) {
    if (!p) return -1;

    pthread_mutex_lock(&p->mtx);

    const uint64_t now = (uint64_t)time(NULL);
    int removed = 0;

    while (p->txs != NULL) {
        tx *t = p->txs->transaction;

        // nodo inconsistente: lo elimino comunque (sanitizzazione)
        if (t == NULL) {
            tx_list_node *bad = p->txs;
            p->txs = bad->next;

            bad->next = NULL;
            free(bad);

            p->tx_num--;
            removed++;
            continue;
        }

        // lista ordinata: appena ne trovo una non scaduta posso fermarmi
        if (t->expire > now) break;

        tx_list_node *victim = p->txs;
        p->txs = victim->next;

        tx_destroy(victim->transaction);
        victim->transaction = NULL;

        victim->next = NULL;
        free(victim);

        p->tx_num--;
        removed++;
    }

    pthread_mutex_unlock(&p->mtx);
    return removed;
}

/*
 * Utility: svuota completamente il pool.
 * (comodo per shutdown / reset)
 * Ritorna numero di tx distrutte.
 */
int tx_pool_clear(tx_pool *p) {
    if (!p) return -1;

    pthread_mutex_lock(&p->mtx);

    int removed = 0;
    while (p->txs) {
        tx_list_node *victim = p->txs;
        p->txs = victim->next;

        tx_destroy(victim->transaction);
        victim->transaction = NULL;

        victim->next = NULL;
        free(victim);

        removed++;
    }

    p->tx_num = 0;

    pthread_mutex_unlock(&p->mtx);
    return removed;
}
