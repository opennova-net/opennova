#include <net/novaworld/join_identity.h>

#include <base/io/strutil.h>
#include <net/novacrypto/ap_alphabet.h>
#include <net/novacrypto/pubcrypto.h>

#include <algorithm>
#include <cstddef>

// [orig: Server_ValidatePlayerJoinRequest @0x512100 — the retail consumer of these plaintexts:
//  NAMEINFO @0x512622, PCID @0x5126d8, SQUADINFO @0x5128b4, each decrypted with
//  NapiPacket_DecryptAndVerify @0x4c2ad0 keyed on the local address + tag; the producer is the
//  NovaWorld web service (NWJoin.dll PUB* cookies), not the client image]

namespace opennova {
namespace {

void append_cstr(std::vector<uint8_t> &out, std::string_view s) {
	out.insert(out.end(), s.begin(), s.end());
	out.push_back(0);
}

} // namespace

std::vector<uint8_t> build_pub_pcid_plaintext(std::string_view pcid) {
	if (pcid.empty()) return {};
	std::vector<uint8_t> out;
	out.reserve(pcid.size() + 1);
	append_cstr(out, pcid);
	return out;
}

std::vector<uint8_t> build_pub_nameinfo_plaintext(std::string_view nwhandle) {
	if (nwhandle.empty()) return {};
	std::vector<uint8_t> out;
	out.reserve(nwhandle.size() + 1);
	append_cstr(out, nwhandle);
	return out;
}

std::vector<uint8_t> build_pub_squadinfo_plaintext(std::string_view nwhandle) {
	if (nwhandle.empty()) return {};
	const std::size_t short_len = std::min<std::size_t>(8, nwhandle.size());
	std::vector<uint8_t> out;
	out.reserve(4 + nwhandle.size() + 1 + short_len + 1);
	out.push_back(0);
	out.push_back(0);
	out.push_back(0);
	out.push_back(0);
	append_cstr(out, nwhandle);
	append_cstr(out, nwhandle.substr(0, short_len));
	return out;
}

PubJoinIdentityPlaintexts build_pub_join_identity_plaintexts(
		std::string_view pcid,
		std::string_view nwhandle) {
	PubJoinIdentityPlaintexts out;
	out.pcid = build_pub_pcid_plaintext(pcid);
	out.name_info = build_pub_nameinfo_plaintext(nwhandle);
	out.squad_info = build_pub_squadinfo_plaintext(nwhandle);
	return out;
}

// --- The host side: the JOIN handler reads these back ----------------------

namespace {

// KeyValueBuffer_FindValue's value buffer and the decoder's out buffer.
// [orig: NapiPacket_DecryptAndVerify @0x4c2ad0 — base64Value[2048] @0x4c2b50,
//  decodedData[2048] @0x4c2b71]
constexpr std::size_t kCookieValueCapacity = 2048;
// The two decrypt buffers of the JOIN handler. [orig: pcid_buf[512] @0x512d29,
//  the SQUADINFO buffer (dword + 2044) @0x512d85]
constexpr std::size_t kPcidOutCapacity = 512;
constexpr std::size_t kSquadOutCapacity = 2048;
constexpr std::size_t kPcidFieldCapacity = 32;      // [orig: @0x4c2734]
constexpr std::size_t kSquadNameChars = 64;          // [orig: i = 65 .. > 1 @0x512dca]
constexpr std::size_t kSquadTagChars = 8;            // [orig: j = 9 .. > 1 @0x512e0b]

// Napi_CopyString(dst, src, N): at most N-1 characters, stopping at a NUL.
std::string copy_capped(std::string_view src, std::size_t capacity) {
	const std::size_t nul = src.find('\0');
	if (nul != std::string_view::npos) src = src.substr(0, nul);
	const std::size_t cap = capacity > 0 ? capacity - 1 : 0;
	return std::string(src.substr(0, std::min(src.size(), cap)));
}

// The bounded string copy of the SQUADINFO walk: up to `chars` bytes from
// `pos`, stopping at a NUL or at `end`.
std::string bounded_string(const std::vector<uint8_t> &buf, std::size_t pos, std::size_t end,
		std::size_t chars) {
	std::string out;
	while (out.size() < chars && pos < end && buf[pos] != 0) out.push_back(static_cast<char>(buf[pos++]));
	return out;
}

} // namespace

void set_join_account_pcid(JoinAccount &account, std::string_view pcid) {
	account.pcid = copy_capped(pcid, kPcidFieldCapacity);
}

bool decrypt_join_cookie_field(const JoinCookiePairs &cookie, const std::string &field_name,
		const SessionIdRing &key_table, std::size_t out_capacity, std::vector<uint8_t> &out) {
	out.clear();
	const std::string *value = nullptr;
	for (const auto &pair : cookie) {
		if (strutil::iequals(pair.first, field_name)) {
			value = &pair.second;
			break;
		}
	}
	if (value == nullptr) return false;
	std::vector<uint8_t> decoded;
	if (!try_decode_ap(copy_capped(*value, kCookieValueCapacity), decoded) ||
			decoded.size() > kCookieValueCapacity || decoded.size() <= 4)
		return false;
	std::vector<uint8_t> plaintext;
	uint32_t key_index = 0;
	bool more_keys = key_table.count != 0;
	for (;;) {
		const uint32_t key = more_keys ? key_table.keys[key_index] : 0u;
		if (try_decrypt_pub_bytes(decoded, std::to_string(static_cast<int32_t>(key)), plaintext))
			break;
		more_keys = ++key_index < key_table.count;
		if (key_index >= key_table.count) return false;
	}
	if (decoded.size() > out_capacity) return false;
	out = std::move(plaintext);
	out.push_back(0);
	return true;
}

void load_join_account(const JoinCookiePairs &cookie, const std::string &local_address,
		const SessionIdRing &key_table, JoinAccount &account) {
	std::vector<uint8_t> buf;
	if (decrypt_join_cookie_field(cookie, local_address + "PCID", key_table, kPcidOutCapacity, buf)) {
		set_join_account_pcid(account, std::string_view(reinterpret_cast<const char *>(buf.data()),
				buf.size()));
	}
	if (!decrypt_join_cookie_field(cookie, local_address + "SQUADINFO", key_table,
				kSquadOutCapacity, buf))
		return;
	// The out-length bounds both strings; it counts the NUL after the plaintext.
	const std::size_t end = buf.size();
	std::size_t names = 0;
	uint32_t squad_id = 0;
	if (end >= 4) {
		squad_id = static_cast<uint32_t>(buf[0]) | (static_cast<uint32_t>(buf[1]) << 8) |
				(static_cast<uint32_t>(buf[2]) << 16) | (static_cast<uint32_t>(buf[3]) << 24);
		names = 4;
	}
	const std::string squad_name = bounded_string(buf, names, end, kSquadNameChars);
	std::size_t tag = names;
	while (tag < end && buf[tag] != 0) ++tag;
	tag = std::min(tag + 1, end);
	account.squad_id = squad_id;
	account.squad_name = squad_name;
	account.squad_tag = bounded_string(buf, tag, end, kSquadTagChars);
}

} // namespace opennova
