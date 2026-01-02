#include <stdlib.h>
#include <time.h>
#include <stdint.h>
#include <ctype.h>
#include <string.h>

void sleep_ms(long milliseconds) {
    struct timespec ts;
    ts.tv_sec = milliseconds / 1000;
    ts.tv_nsec = (milliseconds % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

void store_u64_le(uint8_t out[8], uint64_t x) {
    out[0] = (uint8_t)(x);
    out[1] = (uint8_t)(x >> 8);
    out[2] = (uint8_t)(x >> 16);
    out[3] = (uint8_t)(x >> 24);
    out[4] = (uint8_t)(x >> 32);
    out[5] = (uint8_t)(x >> 40);
    out[6] = (uint8_t)(x >> 48);
    out[7] = (uint8_t)(x >> 56);
}

void store_u32_le(uint8_t out[4], uint32_t x) {
    out[0] = (uint8_t)(x);
    out[1] = (uint8_t)(x >> 8);
    out[2] = (uint8_t)(x >> 16);
    out[3] = (uint8_t)(x >> 24);
}

uint64_t load_u64_le(const uint8_t in[8]) {
    return ((uint64_t)in[0]) |
        ((uint64_t)in[1] << 8) |
        ((uint64_t)in[2] << 16) |
        ((uint64_t)in[3] << 24) |
        ((uint64_t)in[4] << 32) |
        ((uint64_t)in[5] << 40) |
        ((uint64_t)in[6] << 48) |
        ((uint64_t)in[7] << 56);
}

uint32_t load_u32_le(const uint8_t in[4]) {
    return (uint32_t)in[0] |
        ((uint32_t)in[1] << 8) |
        ((uint32_t)in[2] << 16) |
        ((uint32_t)in[3] << 24);
}

static int hex_val(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    c = tolower(c);
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    return -1;
}

int hex_to_bytes(const char* hex, uint8_t* out, size_t out_len) {
    if (!hex || !out) return -1;

    if (hex[0] == '0' && (hex[1] == 'x' || hex[1] == 'X')) {
        hex += 2;
    }

    size_t hex_len = strlen(hex);
    if (hex_len != out_len * 2) return -2;

    for (size_t i = 0; i < out_len; i++) {
        int hi = hex_val((unsigned char)hex[i * 2]);
        int lo = hex_val((unsigned char)hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) return -3;
        out[i] = (uint8_t)((hi << 4) | lo);
    }

    return 0;
}

int bytes_to_hex(const uint8_t* in, size_t len, char* out, size_t out_len) {
    static const char HEX[] = "0123456789abcdef";
    if (!in || !out) return -1;
    if (out_len < (len * 2 + 1)) return -2;

    for (size_t i = 0; i < len; i++) {
        out[i * 2] = HEX[(in[i] >> 4) & 0x0F];
        out[i * 2 + 1] = HEX[in[i] & 0x0F];
    }
    out[len * 2] = '\0';
    return 0;
}
