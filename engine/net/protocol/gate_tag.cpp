#include <net/protocol/gate_tag.h>

#include <base/resource_index/resource_index.h>
#include <net/novacrypto/pubcrypto.h>
#include <net/protocol/protocol.h>

#include <algorithm>

namespace opennova {

bool decode_gate_tag_file(
		const std::vector<uint8_t> &bytes, const char *key_chain, std::string &tag) {
	// [orig: Mission_LoadEncryptedConfig @0x4cdcd0 - `file_size <= 0x1FFF` @0x4cdd20]
	if (key_chain == nullptr || bytes.size() > kGateTagFileMaxBytes)
		return false;
	// The read buffer is NUL-terminated at the file's size and handed over as a C string.
	// [orig: Mission_LoadEncryptedConfig @0x4cdcd0 - the read @0x4cdd32, the NUL @0x4cdd53]
	const auto end = std::find(bytes.begin(), bytes.end(), uint8_t{ 0 });
	const std::string text(bytes.begin(), end);
	std::vector<uint8_t> plaintext;
	// [orig: Mission_LoadEncryptedConfig @0x4cdcd0 - the decode @0x4cdd65 into the 2047-byte room]
	if (!decode_key_chain(text, key_chain, plaintext, kGateTagDecodeRoom))
		return false;
	// Napi_CopyString: the C string up to its NUL, at most 63 characters.
	// [orig: Mission_LoadEncryptedConfig @0x4cdcd0 - its NUL @0x4cdd79, Napi_CopyString(+76, ..,
	// 64)
	//  @0x4cdd7e]
	const auto stop = std::find(plaintext.begin(), plaintext.end(), uint8_t{ 0 });
	std::string copied(plaintext.begin(), stop);
	if (copied.size() > kGateTagChars)
		copied.resize(kGateTagChars);
	tag = std::move(copied);
	return true;
}

std::string protocol_gate_tag(const Protocol &protocol, const ResourceIndex *mount) {
	// The gate manager's default, then the boot's gt.ssc read loose first.
	// [orig: CNapiGateManager_InitDefaults @0x4d1460 - @0x4d14b6; Mission_LoadEncryptedConfig
	//  @0x4cdcd0 - FileSystem_SetSearchLooseFirst(1) @0x4cdcf4, FileSystem_FileExists("gt.ssc")
	//  @0x4cdcfe]
	std::string tag = protocol.gate_tag != nullptr ? protocol.gate_tag : "";
	if (mount == nullptr)
		return tag;
	std::vector<uint8_t> bytes;
	if (!mount->read_file_raw(kGateTagFile, bytes, VfsLookupPolicy::ForceLooseFirst))
		return tag;
	(void)decode_gate_tag_file(bytes, protocol.gate_tag_keys, tag);
	return tag;
}

} // namespace opennova
