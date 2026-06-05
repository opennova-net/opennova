#ifndef OPENNOVA_VFS_DECODE_H
#define OPENNOVA_VFS_DECODE_H

#include <cstdint>
#include <vector>

namespace opennova {

// How the SCR key is chosen when decoding. The values mirror ScrPolicy in
// gameprofile.h one-for-one (same ordinals) so a caller holding a NovaGameProfile can pass
// profile->scr_policy straight through, but the enum is duplicated here to keep libs/vfs free
// of a gameprofile dependency (vfs is the lower layer). The version byte alone cannot tell the
// JO Demo (DEFAULT-keyed) from retail JO/DFX2 (JO_DFX2-keyed) — both stamp version 1 — so a
// caller that knows the game forces the key with one of the FORCE_* policies.
enum VfsScrPolicy {
    VFS_SCR_VERSION_DETECT = 0, // pick the key from the SCR version byte (default)
    VFS_SCR_FORCE_DEFAULT  = 1, // always SCR_KEY_DEFAULT (0xABEEFACE) — JO Demo
    VFS_SCR_FORCE_JO_DFX2  = 2, // always SCR_KEY_JO_DFX2 (0x2A5A8EAD)
    VFS_SCR_FORCE_SHADERS  = 3, // always SCR_KEY_SHADERS (0xA55B1EED)
};

// Decode a stored file payload in place. If the data carries SCR encryption it is decrypted
// with the key selected by `scr_policy` (version-detected by default); if it carries BFC1
// compression it is decompressed. Both are applied in that order (SCR then BFC1), matching the
// importer's asset_resolver, so a file that is SCR-wrapped over BFC1 is fully decoded. When
// neither magic is present the data is left untouched (plaintext passes through). Returns true
// on success including the no-op case; false only when an SCR/BFC1 payload is malformed.
//
// This is the payload-codec layer and is intentionally separate from PFF container
// decryption (which pff_extract applies). Keeping both Godot and the Python importer on this
// one routine is what makes their decoded bytes identical.
bool vfs_decode_payload(std::vector<uint8_t> &data, int scr_policy = VFS_SCR_VERSION_DETECT);

} // namespace opennova

#endif // OPENNOVA_VFS_DECODE_H
