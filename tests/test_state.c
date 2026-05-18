/* Tests for state mutations and root determinism. */
#include "state.h"
#include "crypto.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static ac_addr_t addr_of_byte(uint8_t b) {
    ac_addr_t a;
    memset(a.b, b, AC_PUBKEY_SIZE);
    return a;
}

static void test_basic_set_get(void) {
    ac_state_t *s = ac_state_new(2025);
    assert(s);

    ac_account_t a;
    memset(&a, 0, sizeof(a));
    a.addr = addr_of_byte(0xA1);
    a.balance = 100;
    a.nonce = 1;
    ac_state_set(s, &a);

    ac_account_t got;
    int found = ac_state_get(s, &a.addr, &got);
    assert(found == 1);
    assert(got.balance == 100);
    assert(got.nonce == 1);

    /* Empty account is removed. */
    a.balance = 0;
    a.nonce = 0;
    ac_state_set(s, &a);
    found = ac_state_get(s, &a.addr, &got);
    assert(found == 0);
    (void)got;

    ac_state_free(s);
}

static void test_root_determinism(void) {
    ac_state_t *s1 = ac_state_new(2025);
    ac_state_t *s2 = ac_state_new(2025);

    /* Insert same accounts in different orders. */
    ac_account_t a;
    memset(&a, 0, sizeof(a));
    a.balance = 1;
    a.addr = addr_of_byte(0x03); ac_state_set(s1, &a);
    a.addr = addr_of_byte(0x01); ac_state_set(s1, &a);
    a.addr = addr_of_byte(0x02); ac_state_set(s1, &a);

    a.addr = addr_of_byte(0x01); ac_state_set(s2, &a);
    a.addr = addr_of_byte(0x02); ac_state_set(s2, &a);
    a.addr = addr_of_byte(0x03); ac_state_set(s2, &a);

    ac_hash_t r1, r2;
    ac_state_root(s1, &r1);
    ac_state_root(s2, &r2);
    assert(memcmp(r1.b, r2.b, AC_HASH_SIZE) == 0);

    ac_state_free(s1);
    ac_state_free(s2);
}

static void test_serialize_roundtrip(void) {
    ac_state_t *s = ac_state_new(2025);
    ac_account_t a;
    memset(&a, 0, sizeof(a));
    a.addr = addr_of_byte(0xAA); a.balance = 500; a.nonce = 2; a.stake = 100;
    ac_state_set(s, &a);

    const uint8_t name_bytes[] = "agent01";
    ac_addr_t addr = addr_of_byte(0xBB);
    a.addr = addr; a.balance = 7;
    ac_state_set(s, &a);
    assert(ac_state_name_register(s, name_bytes, sizeof(name_bytes) - 1, &addr) == 0);

    size_t len = 0;
    uint8_t *buf = ac_state_serialize(s, &len);
    assert(buf);

    ac_state_t *t = ac_state_new(2025);
    assert(ac_state_deserialize(t, buf, len) == 0);

    ac_hash_t r1, r2;
    ac_state_root(s, &r1);
    ac_state_root(t, &r2);
    assert(memcmp(r1.b, r2.b, AC_HASH_SIZE) == 0);

    free(buf);
    ac_state_free(s);
    ac_state_free(t);
}

int main(void) {
    if (ac_crypto_init() != 0) { fprintf(stderr, "crypto init failed\n"); return 1; }
    test_basic_set_get();
    test_root_determinism();
    test_serialize_roundtrip();
    printf("test_state OK\n");
    return 0;
}
