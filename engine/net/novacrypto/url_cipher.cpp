#include <net/novacrypto/url_cipher.h>

namespace opennova {

namespace {

constexpr char TERMINATOR = '&';

} // namespace

namespace {

// The key byte for position i. Retail indexes a 128-byte stack buffer the
// 21-char key was sprintf'd into LINEARLY — no modulo — so position 21 reads
// the key's NUL terminator and every later position reads whatever the
// stack held, which no two runs share. The contract is therefore "at most
// key length + 1 characters": the NUL slot is modeled, anything past it is
// refused (both directions stop there).
// [orig: URL_ParseConnectionQueryString @0x54dfb0 — `char cipher_key[128]`,
//  sprintf(cipher_key, "diheijefhgcdjcgcjcfbd") / "cfhdcegjigecjehcgjdhe";
//  NK loop `*ni_src = ni_ptr - ni_src[cipher_key - g_NkBuf] + 48` @0x54e173,
//  CK loop @0x54e1fd]
bool key_byte_at(std::string_view key, size_t i, uint8_t &out) {
	if (i < key.size()) {
		out = static_cast<uint8_t>(key[i]);
		return true;
	}
	if (i == key.size()) {
		out = 0; // the terminator slot
		return true;
	}
	return false;
}

} // namespace

// [orig: URL_ParseConnectionQueryString @ 0x54dfb0 (retail) — NK=/CK= loops: plain[i]=cipher[i]-key[i]+'0',
//        '&'(38)-terminated. Keys NK@0x7d3f30, CK@0x7d3f04. grill wave 3 NW-C4, MATCHING.
//        (jodemo: Auth_ParseRegistrationURL@0x514c40, keys 0x74d8a0/0x74d874.)]
std::string url_cipher_decode(std::string_view cipher, std::string_view key) {
	std::string out;
	if (key.empty()) {
		return out;
	}
	out.reserve(cipher.size());
	for (size_t i = 0; i < cipher.size(); ++i) {
		const char c = cipher[i];
		if (c == TERMINATOR) {
			break;
		}
		uint8_t k = 0;
		if (!key_byte_at(key, i, k)) {
			break;
		}
		const uint8_t src = static_cast<uint8_t>(c);
		out.push_back(static_cast<char>(static_cast<uint8_t>(src - k + '0')));
	}
	return out;
}

std::string url_cipher_encode(std::string_view plain, std::string_view key) {
	std::string out;
	if (key.empty()) {
		return out;
	}
	out.reserve(plain.size());
	for (size_t i = 0; i < plain.size(); ++i) {
		uint8_t k = 0;
		if (!key_byte_at(key, i, k)) {
			break;
		}
		const uint8_t p = static_cast<uint8_t>(plain[i]);
		out.push_back(static_cast<char>(static_cast<uint8_t>(p + k - '0')));
	}
	return out;
}

} // namespace opennova
