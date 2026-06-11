#include <novacrypto/url_cipher.h>

namespace opennova {

namespace {

constexpr char TERMINATOR = '&';

} // namespace

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
