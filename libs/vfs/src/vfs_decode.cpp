#include "vfs/vfs_decode.h"

#include <bfc1/bfc1.h>
#include <scr/scr.h>
#include <mus/mus_sniff.h>

#include <cstdlib>

namespace opennova {
namespace {

uint32_t scr_key_for_version(uint8_t version) {
    // Mirrors pyopennova/asset_resolver _SCR_VERSION_KEYS.
    switch (version) {
        case 1:  return SCR_KEY_JO_DFX2;
        case 2:  return SCR_KEY_SHADERS;
        default: return SCR_KEY_DEFAULT; // 0 and anything unrecognized
    }
}

uint32_t scr_key_for_policy(int scr_policy, uint8_t version) {
    // FORCE_* fixes the key regardless of the version byte; VERSION_DETECT (and any unknown
    // value) falls back to keying off the version byte. Values mirror VfsScrPolicy / ScrPolicy.
    switch (scr_policy) {
        case VFS_SCR_FORCE_DEFAULT: return SCR_KEY_DEFAULT;
        case VFS_SCR_FORCE_JO_DFX2: return SCR_KEY_JO_DFX2;
        case VFS_SCR_FORCE_SHADERS: return SCR_KEY_SHADERS;
        default:                    return scr_key_for_version(version);
    }
}

bool decode_scr(std::vector<uint8_t> &data, int scr_policy) {
    if (!scr_is_scr(data.data(), data.size())) {
        return true; // not SCR: no-op
    }
    if (data.size() < SCR_HEADER_SIZE) {
        return false;
    }
    const uint8_t version = scr_get_version(data.data(), data.size());
    const uint32_t key = scr_key_for_policy(scr_policy, version);

    std::vector<uint8_t> out(data.size() - SCR_HEADER_SIZE);
    size_t out_size = out.size();
    uint8_t *out_ptr = out.empty() ? nullptr : out.data();
    if (scr_decrypt_buf(data.data(), data.size(), out_ptr, &out_size, key) != 0) {
        return false;
    }
    out.resize(out_size);
    data.swap(out);
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

// Resolve a MUS script to its plaintext "SCR0" form. Handled BEFORE the generic
// SCR-container decode for two reasons: (1) the headerless retail-disk form has
// no magic the generic path recognizes (scr_decrypt_mus is never reached by
// decode_scr), and (2) a plaintext "SCR0" MUS would otherwise be mis-decrypted
// by decode_scr, whose scr_is_scr() magic check matches the first three bytes
// "SCR" and so also fires on "SCR0". mus_decode_to_scr0 exits cheaply for the
// common forms (plaintext / SCR-header); non-MUS data falls through to the
// generic decoders unchanged.
bool decode_mus(std::vector<uint8_t> &data) {
    uint8_t *out = nullptr;
    size_t out_size = 0;
    if (mus_decode_to_scr0(data.data(), data.size(), &out, &out_size) && out) {
        data.assign(out, out + out_size);
        free(out);
        return true;
    }
    return false;
}

} // namespace

bool vfs_decode_payload(std::vector<uint8_t> &data, int scr_policy) {
    if (decode_mus(data)) return true;                  // MUS (plaintext / SCR-wrapped / headerless) first
    if (!decode_scr(data, scr_policy)) return false;    // then generic SCR container
    if (!decode_bfc1(data)) return false;               // then BFC1 (possibly over the decrypted bytes)
    return true;
}

} // namespace opennova
