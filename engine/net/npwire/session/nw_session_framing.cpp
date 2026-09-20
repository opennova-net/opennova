#include <net/npwire/nw_session_framing.h>

#include <net/napi/envelope.h>
#include <net/novacrypto/nwu.h>
#include <net/npwire/session_keys.h>

#include <random>

namespace opennova {

// The SCRK generator. Retail makes 63 draws, each an INCLUSIVE
// NapiPRNG_NextInRange(0, strlen(charset)) — `min + state % (max - min + 1)`
// — over the 31-char vowel-free charset "0123456789BCDFGHJKLMNPQRSTVWXYZ".
// Index 31 lands on the charset's NUL, and `sprintf("%c", 0)` appends nothing
// while the draw counter still decrements, so the key is 63 minus the number
// of NUL picks: 61.03 chars on average — the "61 chars" every retail capture
// shows is that expectation, not a fixed length. The random source is the
// only substitution (retail seeds its 16-bit LCG from the manager).
// [orig: CNapiNPConnection_GenerateTxKey @0x61dfe0 — `chars_remaining = 63`
//  @0x61e040, NextInRange(0, g_txkey_charset_len) @0x61e05b, sprintf "%c"
//  @0x61e075, Napi_CopyString(conn+204, key, 64) @0x61e0c9; charset
//  g_txkey_charset @0x849f10 -> 0x7dfa10; NapiPRNG_NextInRange @0x62e450
//  `min + state % (max - min + 1)` @0x62e460]
namespace {
constexpr int kScrkDraws = 63;
constexpr char kScrkCharset[] = "0123456789BCDFGHJKLMNPQRSTVWXYZ";
constexpr int kScrkCharsetLen = static_cast<int>(sizeof(kScrkCharset) - 1); // 31
} // namespace

std::string make_dev_scrk() {
	static thread_local std::mt19937 gen{std::random_device{}()};
	std::uniform_int_distribution<int> pick(0, kScrkCharsetLen); // inclusive: 32 outcomes
	std::string out;
	out.reserve(kScrkDraws);
	for (int draw = 0; draw < kScrkDraws; ++draw) {
		const int index = pick(gen);
		if (index == kScrkCharsetLen) continue; // the NUL slot appends nothing
		out.push_back(kScrkCharset[index]);
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
