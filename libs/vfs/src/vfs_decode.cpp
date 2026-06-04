#include "vfs/vfs_decode.h"

#include <bfc1/bfc1.h>
#include <scr/scr.h>

namespace opennova {
namespace {

uint32_t scr_key_for_version(uint8_t version) {
    switch (version) {
        case 1:  return SCR_KEY_JO_DFX2;
        case 2:  return SCR_KEY_SHADERS;
        default: return SCR_KEY_DEFAULT; // 0 and anything unrecognized
    }
}

bool scr_payload_is_textual(const std::vector<uint8_t> &data) {
    const size_t sample_size = data.size() < 64 ? data.size() : 64;
    if (sample_size == 0) {
        return false;
    }
    size_t printable = 0;
    for (size_t i = 0; i < sample_size; ++i) {
        const uint8_t c = data[i];
        if (c == 9 || c == 10 || c == 13 || (c >= 32 && c < 127)) {
            ++printable;
        }
    }
    return (printable * 10) >= (sample_size * 9);
}

bool decrypt_scr_with_key(const std::vector<uint8_t> &data, uint32_t key, std::vector<uint8_t> &out) {
    out.assign(data.size() - SCR_HEADER_SIZE, 0);
    size_t out_size = out.size();
    uint8_t *out_ptr = out.empty() ? nullptr : out.data();
    if (scr_decrypt_buf(data.data(), data.size(), out_ptr, &out_size, key) != 0) {
        out.clear();
        return false;
    }
    out.resize(out_size);
    return true;
}

bool decode_scr(std::vector<uint8_t> &data) {
    if (!scr_is_scr(data.data(), data.size())) {
        return true; // not SCR: no-op
    }
    if (data.size() < SCR_HEADER_SIZE) {
        return false;
    }
    const uint8_t version = scr_get_version(data.data(), data.size());
    const uint32_t primary = scr_key_for_version(version);
    const uint32_t keys[] = {
        primary,
        SCR_KEY_DEFAULT,
        SCR_KEY_JO_DFX2,
        SCR_KEY_SHADERS,
        SCR_KEY_DFLW,
    };

    std::vector<uint8_t> fallback;
    bool have_fallback = false;
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
        const uint32_t key = keys[i];
        bool already_tried = false;
        for (size_t j = 0; j < i; ++j) {
            if (keys[j] == key) {
                already_tried = true;
                break;
            }
        }
        if (already_tried) {
            continue;
        }

        std::vector<uint8_t> plain;
        if (!decrypt_scr_with_key(data, key, plain)) {
            continue;
        }
        if (!have_fallback) {
            fallback = plain;
            have_fallback = true;
        }
        if (scr_payload_is_textual(plain)) {
            data.swap(plain);
            return true;
        }
    }

    if (!have_fallback) {
        return false;
    }
    data.swap(fallback);
    return true;
}

bool decode_bfc1(std::vector<uint8_t> &data) {
    if (!bfc1_is_bfc1(data.data(), data.size())) {
        return true; // not BFC1: no-op
    }
    uint32_t usize = 0;
    if (bfc1_uncompressed_size(data.data(), data.size(), &usize) != 0) {
        return false;
    }
    std::vector<uint8_t> out(usize);
    size_t out_size = out.size();
    uint8_t *out_ptr = out.empty() ? nullptr : out.data();
    if (bfc1_decompress(data.data(), data.size(), out_ptr, &out_size) != 0) {
        return false;
    }
    out.resize(out_size);
    data.swap(out);
    return true;
}

} // namespace

bool vfs_decode_payload(std::vector<uint8_t> &data) {
    if (!decode_scr(data)) return false;   // SCR first
    if (!decode_bfc1(data)) return false;  // then BFC1 (possibly over the decrypted bytes)
    return true;
}

} // namespace opennova
