#include <novaworld/gate_probe.h>

#include <novacrypto/nwu.h>

namespace opennova {

std::vector<uint8_t> gate_probe_build(std::string_view tag, std::string_view nwu_key) {
	// Match the binary's ping-thread layout: the tag string PLUS its
	// trailing NUL is the exact byte range fed to the cipher.
	std::vector<uint8_t> buf;
	buf.reserve(tag.size() + 1);
	buf.insert(buf.end(), tag.begin(), tag.end());
	buf.push_back(0x00);
	// Crypto_DecryptBuffer is called on the send side; we mirror that.
	nwu_decrypt(buf.data(), buf.size(), nwu_key);
	return buf;
}

bool gate_response_decrypt_and_parse(const uint8_t *data, size_t len,
                                     GateResponse &out,
                                     std::string_view nwu_key) {
	if (!data || len == 0 || nwu_key.empty()) {
		out = GateResponse{};
		return false;
	}
	std::vector<uint8_t> plain(data, data + len);
	nwu_encrypt(plain.data(), plain.size(), nwu_key); // PFF_EncryptBuffer
	std::string body(reinterpret_cast<const char *>(plain.data()), plain.size());
	return gate_response_parse(body, out);
}

} // namespace opennova
