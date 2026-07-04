#include <npwire/nw_session_framing.h>

#include <napi/envelope.h>
#include <novacrypto/nwu.h>
#include <npwire/session_keys.h>

#include <random>

namespace opennova {

// 61-char SCRK matching retail captures (notes/retail_capture_findings.md:
// ClientAuth and ServerAuth SCRK are both 61 chars; alphabet = A-Z0-9, 36
// chars). We don't replicate the two-30-char-halves structure (random is
// fine), only the length + alphabet.
namespace {
constexpr int kDevScrkLength = 61;
} // namespace

std::string make_dev_scrk() {
	static constexpr char alphabet[] =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
	static thread_local std::mt19937 gen{std::random_device{}()};
	std::uniform_int_distribution<int> pick(0, 35);
	std::string out;
	out.reserve(kDevScrkLength);
	for (int i = 0; i < kDevScrkLength; ++i) {
		out.push_back(alphabet[pick(gen)]);
	}
	return out;
}

std::string make_dev_nwuid() {
	static constexpr char hex[] = "0123456789abcdef";
	static thread_local std::mt19937 gen{std::random_device{}()};
	std::uniform_int_distribution<int> pick(0, 15);
	std::string out;
	out.reserve(60);
	for (int i = 0; i < 60; ++i) {
		out.push_back(hex[pick(gen)]);
	}
	return out;
}

uint32_t make_random_session_u32() {
	static thread_local std::mt19937 gen{std::random_device{}()};
	return std::uniform_int_distribution<uint32_t>{}(gen);
}

bool nw_decode_inbound(const uint8_t *raw, size_t raw_len,
                       uint8_t &opcode_out, std::vector<uint8_t> &body_out) {
	std::vector<uint8_t> stripped(raw_len);
	size_t out_size = 0;
	if (napi_envelope_decode(raw, raw_len, stripped.data(), stripped.size(),
	                         &out_size) != 0) {
		return false;
	}
	stripped.resize(out_size);
	if (stripped.empty()) return false;

	opcode_out = stripped[0];
	body_out.assign(stripped.begin() + 1, stripped.end());
	// Names swapped vs onnet — server-side decrypt is our nwu_encrypt.
	if (!body_out.empty()) {
		nwu_encrypt(body_out.data(), body_out.size(), SESSION_NWU_KEY);
	}
	return true;
}

std::vector<uint8_t> nw_encode_outbound(uint8_t opcode, std::vector<uint8_t> body) {
	if (!body.empty()) {
		nwu_decrypt(body.data(), body.size(), SESSION_NWU_KEY);
	}
	std::vector<uint8_t> with_opcode;
	with_opcode.reserve(1 + body.size());
	with_opcode.push_back(opcode);
	with_opcode.insert(with_opcode.end(), body.begin(), body.end());

	std::vector<uint8_t> packet(with_opcode.size() + 4);
	size_t out_size = 0;
	if (napi_envelope_encode(with_opcode.data(), with_opcode.size(),
	                         packet.data(), packet.size(), &out_size) != 0) {
		return {};
	}
	packet.resize(out_size);
	return packet;
}

} // namespace opennova
