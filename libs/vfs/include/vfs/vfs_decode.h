#ifndef OPENNOVA_VFS_DECODE_H
#define OPENNOVA_VFS_DECODE_H

#include <cstdint>
#include <vector>

namespace opennova {

// Decode a stored file payload in place. If the data carries SCR encryption it is decrypted
// with the version-appropriate key first, then known fallback keys when the output is not
// textual (Land Warrior reuses SCR version 1 with a different key); if it carries BFC1
// compression it is decompressed. Both are applied in that order (SCR then BFC1), matching
// the importer's asset_resolver, so a file that is SCR-wrapped over BFC1 is fully decoded.
// When neither magic is present the data is left untouched (plaintext passes through).
// Returns true on success including the no-op case; false only when an SCR/BFC1 payload is
// malformed.
//
// This is the payload-codec layer and is intentionally separate from PFF container
// decryption (which pff_extract applies). Keeping both Godot and the Python importer on this
// one routine is what makes their decoded bytes identical.
bool vfs_decode_payload(std::vector<uint8_t> &data);

} // namespace opennova

#endif // OPENNOVA_VFS_DECODE_H
