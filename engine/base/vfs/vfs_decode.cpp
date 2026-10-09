#include <base/vfs/vfs_decode.h>

#include <base/io/strutil.h>
#include <formats/bfc1/bfc1.h>
#include <formats/scr/scr.h>

using namespace opennova::bfc1;
using namespace opennova::scr;

namespace opennova {
namespace {

uint32_t scr_key_for_version(uint8_t version) {
    // SCR version-byte key table shared by every runtime asset path.
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

} // namespace

bool vfs_decode_payload(std::vector<uint8_t> &data, int scr_policy) {
    if (!decode_scr(data, scr_policy)) return false; // SCR container first
    if (!bfc1_unpack(data)) return false;            // then BFC1 (possibly over the decrypted bytes)
    return true;
}

bool vfs_loader_takes_stored(const std::string &name) {
    return strutil::ends_with_icase(name, ".fx");
}

} // namespace opennova
