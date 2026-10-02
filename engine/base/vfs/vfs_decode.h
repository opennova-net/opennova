#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace opennova {

// How the SCR key is chosen when decoding. The values mirror ScrPolicy in
// gameprofile.h one-for-one (same ordinals) so a caller holding a GameProfile can pass
// profile->scr_policy straight through, but the enum is duplicated here to keep engine/base/vfs free
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
// runtime asset loader, so a file that is SCR-wrapped over BFC1 is fully decoded. When
// neither magic is present the data is left untouched (plaintext passes through). Returns true
// on success including the no-op case; false only when an SCR/BFC1 payload is malformed.
//
// This is the payload-codec layer and is intentionally separate from PFF container
// decryption (which pff_extract applies). All runtime consumers share this routine.
bool vfs_decode_payload(std::vector<uint8_t> &data, int scr_policy = VFS_SCR_VERSION_DETECT);

// Whether the loader of the file `name` names takes it as stored: the HLSL effects (.fx), which the
// shader loader takes in the SCR form alone, under a key of its own that it unwraps itself [orig:
// ScriptFile_LoadAndDecrypt @ 0x5AE060: the sniff for 'S','C','R',1 @ 0x5AE0A9, the key
// 0xA55B1EED at 0x5AE0C0]. A read of such a file for the bytes its loader is served (an extract, an
// import) keeps the stored bytes: vfs_decode_payload's version-detected key is the text readers'
// (a version-1 file's is the game's own), which makes such a file noise.
bool vfs_loader_takes_stored(const std::string &name);

} // namespace opennova
