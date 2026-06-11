#include <novacrypto/url_cipher.h>

namespace opennova {

namespace {

constexpr char TERMINATOR = '&';

} // namespace

// [orig: parse_connection_query_string @ 0x54dfb0 (retail) — NK=/CK= loops: plain[i]=cipher[i]-key[i]+'0',
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
		const uint8_t src = static_cast<uint8_t>(c);
		const uint8_t k = static_cast<uint8_t>(key[i % key.size()]);
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
		const uint8_t p = static_cast<uint8_t>(plain[i]);
		const uint8_t k = static_cast<uint8_t>(key[i % key.size()]);
		out.push_back(static_cast<char>(static_cast<uint8_t>(p + k - '0')));
	}
	return out;
}

} // namespace opennova
