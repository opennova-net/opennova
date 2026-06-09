#include "scr/scr.h"

#include <string.h>

/* Original codec: Scr_DecryptBuffer @ 0x53D090 in Jointops.exe (byte-reverse,
   then per-byte XOR with the rotating keystream below). Both retail call sites
   strip the 4-byte header before calling it; see scr_decrypt_buf. Divergence
   notes live in docs/audio/mus-sbf-re.md (D-SCR-*). */

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
    /* [orig: Scr_DecryptBuffer @ 0x53D090, keystream update @ 0x53D0D3] */
    size_t i;
    for (i = 0; i < size; ++i) {
        key = rol32(key + rol32(key, 11), 4) ^ 1;
        data[i] ^= (uint8_t)key;
    }
}

int scr_is_scr(const uint8_t *data, size_t size) {
    /* Acceptance is a deliberate multi-title superset (D-SCR-1): Jointops.exe
       requires the version byte to be EXACTLY 1 at both of its sniff sites
       [orig: File_ParseASCIIFile @ 0x53D899, ScriptFile_LoadAndDecrypt @ 0x5AE0A9].
       Accepting 0..2 lets one codec serve JO-demo-era and shader containers too.
       Either way plaintext MUS files (own magic "SCR0", so data[3] == 0x30) are
       rejected here and pass through undecoded. */
    if (size < SCR_HEADER_SIZE) return 0;
    if (!(data[0] == 'S' && data[1] == 'C' && data[2] == 'R')) return 0;
    return data[3] <= 2;
}

uint8_t scr_get_version(const uint8_t *data, size_t size) {
    if (!scr_is_scr(data, size)) return 0;
    return data[3];
}

void scr_decrypt(uint8_t *data, size_t size, uint32_t key) {
    /* [orig: Scr_DecryptBuffer @ 0x53D090] reverse pass @ 0x53D0B3, XOR pass
       @ 0x53D0DC; byte-exact structural translation. */
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
