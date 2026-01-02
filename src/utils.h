#pragma once

#include <stddef.h>
#include <stdint.h>

void sleep_ms(long milliseconds);

void store_u64_le(uint8_t out[8], uint64_t x);
void store_u32_le(uint8_t out[4], uint32_t x);
uint64_t load_u64_le(const uint8_t in[8]);
uint32_t load_u32_le(const uint8_t in[4]);

int hex_to_bytes(const char* hex, uint8_t* out, size_t out_len);
int bytes_to_hex(const uint8_t* in, size_t len, char* out, size_t out_len);
