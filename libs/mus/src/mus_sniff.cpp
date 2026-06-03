/* Shared MUS sniff/decrypt. See mus/mus_sniff.h.

   Validation mirrors the witnessed loader gate (godot/engine/audio/
   mus_resource_format.cpp peek_is_valid_mus): "SCR0" magic + version stamp
   0x00000100 + a sane chunk_count. That triple is specific enough that a
   wrong-key decrypt (or a non-MUS .bin) effectively never trips it, which is
   what lets the VFS run mus_decode_to_scr0 generically over small .bin files. */

#include "mus/mus_sniff.h"
#include "mus/mus.h"
#include "scr/scr.h"

#include <stdlib.h>
#include <string.h>

namespace {

/* SCR0 (44-byte MusFileHeader) + version 0x100 + chunk_count in [1,16]. */
bool peek_valid_mus(const uint8_t *b, size_t n) {
    if (n < 44) return false;
    if (!(b[0] == 'S' && b[1] == 'C' && b[2] == 'R' && b[3] == '0')) return false;
    const uint32_t version =
        (uint32_t)b[4] | ((uint32_t)b[5] << 8) | ((uint32_t)b[6] << 16) | ((uint32_t)b[7] << 24);
    if (version != 0x00000100u) return false;
    const uint32_t chunk_count =
        (uint32_t)b[8] | ((uint32_t)b[9] << 8) | ((uint32_t)b[10] << 16) | ((uint32_t)b[11] << 24);
    return chunk_count >= 1 && chunk_count <= 16;
}

uint8_t *dup_bytes(const uint8_t *src, size_t n) {
    uint8_t *b = (uint8_t *)malloc(n ? n : 1);
    if (b && n) memcpy(b, src, n);
    return b;
}

}  // namespace

extern "C" int mus_decode_to_scr0(const uint8_t *in, size_t size,
                                  uint8_t **out, size_t *out_size) {
    if (!in || !out || !out_size) return 0;
    static const uint32_t keys[3] = { SCR_KEY_DEFAULT, SCR_KEY_JO_DFX2, SCR_KEY_SHADERS };

    /* Form 0: already plaintext SCR0. */
    if (peek_valid_mus(in, size)) {
        uint8_t *b = dup_bytes(in, size);
        if (!b) return 0;
        *out = b; *out_size = size; return 1;
    }

    /* Form 1: SCR-container wrapped ("SCR\xVV" header). scr_decrypt_buf strips
       the 4-byte header, so the inner content is the plaintext MUS. */
    if (size >= 4 && in[0] == 'S' && in[1] == 'C' && in[2] == 'R') {
        const size_t cap = size - 4;
        uint8_t *tmp = (uint8_t *)malloc(cap ? cap : 1);
        if (tmp) {
            for (int ki = 0; ki < 3; ++ki) {
                size_t sz = cap;
                if (scr_decrypt_buf(in, size, tmp, &sz, keys[ki]) == 0 && peek_valid_mus(tmp, sz)) {
                    uint8_t *b = dup_bytes(tmp, sz);
                    free(tmp);
                    if (!b) return 0;
                    *out = b; *out_size = sz; return 1;
                }
            }
            free(tmp);
        }
    }

    /* Form 2: headerless retail-disk form. scr_decrypt_mus prepends "SCR0". */
    for (int ki = 0; ki < 3; ++ki) {
        uint8_t *m = nullptr;
        size_t msz = 0;
        if (scr_decrypt_mus(in, size, &m, &msz, keys[ki]) == 0 && m) {
            if (peek_valid_mus(m, msz)) { *out = m; *out_size = msz; return 1; }
            scr_free_buffer(m);
        }
    }
    return 0;
}

extern "C" int mus_is_mus(const uint8_t *data, size_t size) {
    uint8_t *o = nullptr;
    size_t os = 0;
    if (mus_decode_to_scr0(data, size, &o, &os)) {
        free(o);
        return 1;
    }
    return 0;
}
