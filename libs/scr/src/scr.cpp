#include "scr/scr.h"

#include <stdlib.h>
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
    return data[0] == 'S' && data[1] == 'C' && data[2] == 'R';
}

uint8_t scr_get_version(const uint8_t *data, size_t size) {
    if (size < SCR_HEADER_SIZE) return 0;
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

/* Music-file variant of SCR cipher.
 *
 * Witnessed: dfvas!Scr_DecryptBuffer @ 0x4cc250, called by the audio VM load
 * chain with key SCR_KEY_JO_DFX2 (0x2A5A8EAD).
 *
 * NOTE (Jointops.exe re-anchor, 2026-06-02): this rol32 additive cipher is NOT
 * present in Jointops.exe's audio path. There, AudioVM_LoadScriptFile @ 0x672D20
 * loads via File_LoadResource @ 0x75B540 and checks the plaintext 'SCR0' magic
 * directly -- no decrypt call -- and neither key (0xABEEFACE/0x2A5A8EAD) appears
 * as an immediate in that binary. The cipher itself is independently correct
 * (verified byte-for-byte vs the retail jo_gamemus.bin), but its citation stays
 * dfvas-only because no Jointops.exe equivalent exists to re-anchor to.
 * See notes/audio/sbf-mus-format.md (SCR verdict: UNKNOWN vs Jointops.exe).
 *
 * Encrypted on-disk form has no
 * "SCR\xVV" header: every byte is ciphertext. Decryption applies the XOR
 * keystream to every input byte, reverses, and prepends a literal "SCR0"
 * magic so the output is consumable by the standard SCR0 audio script
 * loader.
 *
 * Note on cipher order: scr_decrypt() uses reverse-then-XOR, but for this
 * variant the order is XOR-then-reverse. The two orderings are NOT
 * equivalent because the keystream is generated forward starting at
 * position 0; reversing the buffer between XOR passes shifts each byte to
 * a different keystream position. Verified byte-for-byte against
 * fixtures/mus/jo_gamemus.bin (gold) decrypted from the retail disk file.
 */
int scr_decrypt_mus(const uint8_t *in, size_t in_size,
                    uint8_t **out, size_t *out_size, uint32_t key) {
    uint8_t *buf;
    size_t total;
    if (out == NULL || out_size == NULL) return -1;
    if (in == NULL && in_size != 0) return -1;
    total = in_size + 4;
    buf = (uint8_t *)malloc(total);
    if (buf == NULL) return -3;
    /* Literal "SCR0" magic at offset 0, matches dword_64CCA0 in dfvas. */
    buf[0] = 'S';
    buf[1] = 'C';
    buf[2] = 'R';
    buf[3] = '0';
    if (in_size > 0) {
        /* Step 1: XOR every byte of the on-disk ciphertext with the
         * standard SCR keystream (forward, starting at position 0). */
        size_t i;
        uint32_t k = key;
        for (i = 0; i < in_size; ++i) {
            k = rol32(k + rol32(k, 11), 4) ^ 1;
            buf[4 + i] = (uint8_t)(in[i] ^ (uint8_t)k);
        }
        /* Step 2: reverse the XOR'd payload in place. */
        reverse_bytes(buf + 4, in_size);
    }
    *out = buf;
    *out_size = total;
    return 0;
}

void scr_free_buffer(uint8_t *buf) {
    free(buf);
}
