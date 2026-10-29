#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace opennova {

class ResourceIndex;
struct Protocol; // net/protocol/protocol.h

// gt.ssc's size bound, and the decoder's room: a file larger than 0x1FFF bytes is not read, and
// one whose A-P text decodes to more than 2047 bytes does not decode.
// [orig: Mission_LoadEncryptedConfig @0x4cdcd0 - `file_size <= 0x1FFF` @0x4cdd20, the 2047-byte
//  room @0x4cdd65 -> NapiNP_DecodeEncryptedString @0x619130 (bufSize @0x61920f)]
inline constexpr std::size_t kGateTagFileMaxBytes = 0x1FFF;
inline constexpr std::size_t kGateTagDecodeRoom = 2047;
// The gate tag's own buffer: Napi_CopyString into the gate manager's 64-byte tag keeps 63
// characters. [orig: Mission_LoadEncryptedConfig @0x4cdd7e; CNapiGateManager +76]
inline constexpr std::size_t kGateTagChars = 63;

// gt.ssc's bytes as the boot reads them: the text up to its first NUL (the read buffer is
// NUL-terminated at the file's size), decoded under `key_chain` (pubcrypto.h decode_key_chain),
// and the plaintext up to its first NUL kept at kGateTagChars characters. False, `tag` untouched,
// for a file over kGateTagFileMaxBytes or text that does not decode; an empty plaintext decodes
// to an empty tag, as retail's copy makes one.
// [orig: Mission_LoadEncryptedConfig @0x4cdcd0 - the read @0x4cdd32, the NUL @0x4cdd53, the
//  decode @0x4cdd65 (skipped below zero), its NUL @0x4cdd79, Napi_CopyString(+76, .., 64)
//  @0x4cdd7e]
bool decode_gate_tag_file(
		const std::vector<uint8_t> &bytes, const char *key_chain, std::string &tag);

// The gate tag a protocol probes the gate with and its NovaWorld session carries (the 0x42's NA
// and GateTag CU, the start page's [GT]): the gate manager's default, the protocol's, replaced at
// boot by the mount's gt.ssc when one is there and decodes under the protocol's key chain. The
// boot reads gt.ssc loose first, whatever the session's lookup order: the expansion's folder, the
// game's folder, then the mounted archives (Vfs's ForceLooseFirst). `mount` null reads none.
// `protocol` must be complete.
// [orig: CNapiGateManager_InitDefaults @0x4d1460 (Napi_CopyString(this+76, "jop:cus2", 64)
//  @0x4d14b6); Game_InitSubsystems @0x4a7149..0x4a714e -> Mission_LoadEncryptedConfig
//  @0x4cdcd0 over g_GateManager - FileSystem_SetSearchLooseFirst(1) @0x4cdcf4,
//  FileSystem_FileExists("gt.ssc") @0x4cdcfe]
std::string protocol_gate_tag(const Protocol &protocol, const ResourceIndex *mount);

} // namespace opennova
