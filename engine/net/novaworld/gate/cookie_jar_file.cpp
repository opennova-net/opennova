#include <net/novaworld/cookie_jar_file.h>

#include <base/io/file_io.h>
#include <base/io/strutil.h>
#include <net/novacrypto/nwu.h> // nwu_decrypt / nwu_encrypt: Crypto_EncryptBuffer / Crypto_DecryptBuffer

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <utility>

namespace opennova {

namespace {

void xor_word(std::array<uint8_t, 16> &fingerprint, size_t at, uint32_t word) {
	for (size_t i = 0; i < 4; ++i) fingerprint[at + i] ^= static_cast<uint8_t>(word >> (8u * i));
}

// A name's first 16 characters, to its first NUL, into the leading bytes.
void xor_name(std::array<uint8_t, 16> &fingerprint, const std::string &name) {
	const size_t length = std::min(std::strlen(name.c_str()), fingerprint.size());
	for (size_t i = 0; i < length; ++i) fingerprint[i] ^= static_cast<uint8_t>(name[i]);
}

void put_u32(std::vector<uint8_t> &out, uint32_t value) {
	for (size_t i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(value >> (8u * i)));
}

uint32_t get_u32(const uint8_t *at) {
	return static_cast<uint32_t>(at[0]) | (static_cast<uint32_t>(at[1]) << 8u) |
	       (static_cast<uint32_t>(at[2]) << 16u) | (static_cast<uint32_t>(at[3]) << 24u);
}

// One string of a record: its length to its first NUL, then its bytes under the key (an empty
// one, its cipher skipped, no bytes).
void put_string(std::vector<uint8_t> &out, const char *text, std::string_view key) {
	std::string bytes(text);
	put_u32(out, static_cast<uint32_t>(bytes.size()));
	nwu_decrypt(reinterpret_cast<uint8_t *>(bytes.data()), bytes.size(), key);
	out.insert(out.end(), bytes.begin(), bytes.end());
}

void put_string(std::vector<uint8_t> &out, const std::optional<std::string> &text, std::string_view key) {
	if (!text) {
		put_u32(out, 0);
		return;
	}
	put_string(out, text->c_str(), key);
}

} // namespace

// [orig: CDKey_GenerateVolumeFingerprint @0x651ED0 - the text's XORs @0x651F41..0x651FB1, the
//  serial's words @0x651FB4..0x651FC9, the names @0x651FCC..0x652046]
std::array<uint8_t, 16> volume_fingerprint(const RetailMachineInputs &volume) {
	std::array<uint8_t, 16> fingerprint{};
	char text[9] = {};
	std::snprintf(text, sizeof(text), "%08X", static_cast<unsigned>(volume.volume_serial));
	const auto ch = [&text](size_t i) { return static_cast<uint8_t>(text[i]); };
	for (size_t i = 0; i < 8; ++i) {
		fingerprint[i] ^= ch(i);
		fingerprint[15 - i] ^= ch(i);
	}
	xor_word(fingerprint, 0, volume.volume_serial);
	xor_word(fingerprint, 12, volume.volume_serial);
	xor_word(fingerprint, 4, volume.volume_serial ^ volume.maximum_component_length);
	xor_word(fingerprint, 8, volume.volume_serial ^ volume.filesystem_flags);
	xor_name(fingerprint, volume.volume_name);
	xor_name(fingerprint, volume.filesystem_name);
	return fingerprint;
}

// [orig: CookieJar_SaveToFile @0x64EE60 - the key's byte 16 cleared @0x64EE8A]
std::string cookie_jar_key(const RetailMachineInputs &volume) {
	const std::array<uint8_t, 16> fingerprint = volume_fingerprint(volume);
	std::string key;
	for (const uint8_t byte : fingerprint) {
		if (byte == 0) break;
		key.push_back(static_cast<char>(byte));
	}
	return key;
}

// [orig: CookieJar_SaveToFile @0x64EE60 - the magic @0x64EE8F, "PERSISTENT" @0x64EEDE, the express
//  test @0x64EEFA / @0x64EF06, the node word @0x64EF16, Crypto_EncryptBuffer called @0x64EF71, the
//  entry's word @0x64F23B]
std::vector<uint8_t> write_cookie_jar(const CookieJar &jar, bool remember_login_unchecked,
		std::string_view key) {
	const uint32_t include_express_login = remember_login_unchecked ? 0u : 1u;
	std::vector<uint8_t> out;
	put_u32(out, include_express_login);
	put_u32(out, kCookieJarFileMagic);
	for (const CookieEntry &entry : jar.entries()) {
		const std::string_view name(entry.name.c_str());
		if (!strutil::starts_with_icase(name, "PERSISTENT")) continue;
		if (strutil::iequals(name, "PERSISTENTEXPRESSLOGINDATA") && include_express_login == 0) continue;
		put_u32(out, 0); // the node's word: every jar access uses the node whose word is 0
		put_string(out, entry.name.c_str(), key);
		put_string(out, entry.value.c_str(), key);
		put_string(out, entry.expires, key);
		put_string(out, entry.path, key);
		put_string(out, entry.domain, key);
		put_u32(out, entry.secure);
	}
	return out;
}

// [orig: CookieJar_LoadFromFile @0x64F290 - the magic @0x64F30B, the bounds from @0x64F33A,
//  Crypto_DecryptBuffer called @0x64F373, the record's word @0x64F49F, its node @0x64F4D0]
void read_cookie_jar(const uint8_t *data, size_t size, std::string_view key, CookieJar &jar) {
	if (data == nullptr || size < 8) return;
	// The loader's buffer starts past the leading word.
	const uint8_t *buffer = data + 4;
	const size_t length = size - 4;
	if (get_u32(buffer) != kCookieJarFileMagic || length <= 4) return;
	size_t at = 4;
	while (at + 4 <= length) {
		const uint32_t node_word = get_u32(buffer + at);
		at += 4;
		std::string strings[5];
		bool whole = true;
		for (std::string &text : strings) {
			if (at + 4 > length) {
				whole = false;
				break;
			}
			const uint32_t count = get_u32(buffer + at);
			at += 4;
			if (count > length - at) {
				whole = false;
				break;
			}
			std::string bytes(reinterpret_cast<const char *>(buffer + at), count);
			nwu_encrypt(reinterpret_cast<uint8_t *>(bytes.data()), bytes.size(), key);
			text = bytes.c_str(); // the string is read to its first NUL
			at += count;
		}
		if (!whole || at + 4 > length) return;
		const uint32_t word = get_u32(buffer + at);
		at += 4;
		if (node_word != 0) continue;
		CookieEntry entry;
		entry.name = std::move(strings[0]);
		entry.value = std::move(strings[1]);
		entry.expires = std::move(strings[2]);
		entry.path = std::move(strings[3]);
		entry.domain = std::move(strings[4]);
		entry.secure = word;
		jar.append_loaded(std::move(entry));
	}
}

// [orig: Menu_InitShellResources @0x552500 - CUIManager_Create called @0x5525D8,
//  CookieJar_OpenAndLoadFile called @0x552630]
void MenuBrowserState::create(std::string path, std::string jar_key) {
	remember_login_checked.reset();
	jar.clear();
	file_path = std::move(path);
	key = std::move(jar_key);
	if (file_path.empty()) return;
	std::vector<uint8_t> bytes;
	std::string error;
	if (!io::read_file_bytes(file_path, bytes, error)) return;
	read_cookie_jar(bytes.data(), bytes.size(), key, jar);
}

// [orig: CUIScene_SaveCookieJarFile @0x63BA60 - the property @0x63BA66..0x63BA88]
bool MenuBrowserState::save(std::string &error) const {
	if (file_path.empty()) return true;
	const bool unchecked = remember_login_checked.has_value() && !*remember_login_checked;
	const std::vector<uint8_t> bytes = write_cookie_jar(jar, unchecked, key);
	return io::write_file_atomic(file_path, bytes.data(), bytes.size(), error);
}

MenuBrowserState &process_menu_browser() {
	static MenuBrowserState state;
	return state;
}

} // namespace opennova
