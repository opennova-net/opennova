#include "scr/scr.h"

#include <string.h>

static uint32_t rol32(uint32_t value, int shift) {
    return (value << shift) | (value >> (32 - shift));
}

static void reverse_bytes(uint8_t *data, size_t size) {
    uint8_t *front, *back, tmp;
    if (size < 2) return;
    front = data;
    back = data + size - 1;
    while (front < back) {
        tmp = *front;
        *front = *back;
        *back = tmp;
        ++front;
        --back;
    }
}

static void xor_with_keystream(uint8_t *data, size_t size, uint32_t key) {
    size_t i;
    for (i = 0; i < size; ++i) {
        key = rol32(key + rol32(key, 11), 4) ^ 1;
        data[i] ^= (uint8_t)key;
    }
}

int scr_is_scr(const uint8_t *data, size_t size) {
    if (size < SCR_HEADER_SIZE) return 0;
    if (!(data[0] == 'S' && data[1] == 'C' && data[2] == 'R')) return 0;
    return data[3] <= 2;
}

uint8_t scr_get_version(const uint8_t *data, size_t size) {
    if (!scr_is_scr(data, size)) return 0;
    return data[3];
}

void scr_decrypt(uint8_t *data, size_t size, uint32_t key) {
    reverse_bytes(data, size);
    xor_with_keystream(data, size, key);
}

int scr_decrypt_buf(const uint8_t *data, size_t size,
                    uint8_t *out, size_t *out_size, uint32_t key) {
    size_t payload;
    if (!scr_is_scr(data, size)) return -1;
    payload = size - SCR_HEADER_SIZE;
    if (*out_size < payload) {
        *out_size = payload;
        return -2;
    }
    memcpy(out, data + SCR_HEADER_SIZE, payload);
    scr_decrypt(out, payload, key);
    *out_size = payload;
    return 0;
}
