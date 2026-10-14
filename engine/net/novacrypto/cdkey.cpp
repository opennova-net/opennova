#include <net/novacrypto/cdkey.h>

#include <net/novacrypto/nwu.h>
#include <net/novacrypto/pubcrypto.h>

#include <base/io/strutil.h>

#include <cstdint>

namespace opennova::cdkey {

namespace {

// The PRNG the mix steps, {state, multiplier, counter}: the state seeded with
// the fingerprint's key seed, the multiplier NWU's.
// [orig: NapiPRNG_Init @ 0x62e430; @0x716560 (dfx2.exe)]
struct Prng {
	uint32_t state = 0;
	uint32_t multiplier = 0;
	uint32_t counter = 0;
};

// [orig: NapiNP_EncryptStringWithPRNG @0x700df0 (dfx2.exe): NapiNP_ComputeKeySeed of the key
//  @0x700e01, NapiPRNG_Init @0x700e0e]
Prng prng_for(std::string_view fingerprint) {
	Prng prng;
	prng.state = nwu_compute_seed(fingerprint);
	prng.multiplier = NWU_LCG_MAGIC;
	return prng;
}

// One step: the 16-bit product of the state's and the multiplier's low words,
// plus one, stored back zero-extended; the counter counts it.
// [orig: NapiNP_EncryptCharWithCipher @0x7007db..0x7007eb (dfx2.exe)]
uint32_t prng_step(Prng &prng) {
	const auto product = static_cast<uint16_t>(
			static_cast<uint16_t>(prng.state) * static_cast<uint16_t>(prng.multiplier));
	++prng.counter;
	prng.state = static_cast<uint16_t>(product + 1u);
	return prng.state;
}

// The class a character shifts within, tested in this order: its first
// character and its size, or a size of 0 for a byte in none (left alone, and
// no step taken).
// [orig: NapiNP_EncryptCharWithCipher @0x7007c6..0x700857 (dfx2.exe): 'a'..'z', 'A'..'Z',
//  '0'..'9', compared as signed chars]
struct CharClass {
	char first = 0;
	int size = 0;
};

CharClass class_of(char c) {
	if (c >= 'a' && c <= 'z')
		return { 'a', 26 };
	if (c >= 'A' && c <= 'Z')
		return { 'A', 26 };
	if (c >= '0' && c <= '9')
		return { '0', 10 };
	return {};
}

// One character of the mix. Down is the original's: the position less the
// step modulo the size, the size added back when that went below zero. Up
// undoes it.
// [orig: NapiNP_EncryptCharWithCipher @ 0x618690; @0x7007ff..0x700807 (dfx2.exe), the
//  `ch - first + size * (state / size) - state` form]
void shift(char &c, Prng &prng, bool up) {
	const CharClass cls = class_of(c);
	if (cls.size == 0)
		return;
	const int step = static_cast<int>(prng_step(prng) % static_cast<uint32_t>(cls.size));
	int position = (c - cls.first) + (up ? step : -step);
	if (position < 0)
		position += cls.size;
	if (position >= cls.size)
		position -= cls.size;
	c = static_cast<char>(cls.first + position);
}

// The string walk: one PRNG for the whole string, each character in turn up
// to the first NUL.
// [orig: NapiNP_EncryptStringWithPRNG @ 0x618cc0; @0x700e13..0x700e29 (dfx2.exe)]
void mix(std::string &text, std::string_view fingerprint, bool up) {
	Prng prng = prng_for(fingerprint);
	for (char &c : text) {
		if (c == '\0')
			break;
		shift(c, prng, up);
	}
}

std::vector<uint8_t> bytes_of(std::string_view text) {
	return std::vector<uint8_t>(text.begin(), text.end());
}

// A decoded value as the client keeps it: a C string, so up to its first NUL.
// [orig: CDKey_WriteActivationFromNWACT @0x411f37 (dfx2.exe), Napi_CopyString to +16]
std::string c_string_of(const std::vector<uint8_t> &bytes) {
	std::string out;
	for (const uint8_t b : bytes) {
		if (b == 0)
			break;
		out.push_back(static_cast<char>(b));
	}
	return out;
}

} // namespace

bool is_fingerprint(std::string_view text) {
	if (text.size() != kFingerprintLength)
		return false;
	for (const char c : text) {
		if (c < 'A' || c > 'Z')
			return false;
	}
	return true;
}

void mix_down(std::string &text, std::string_view fingerprint) {
	mix(text, fingerprint, false);
}

void mix_up(std::string &text, std::string_view fingerprint) {
	mix(text, fingerprint, true);
}

std::string make_wire_key(std::string_view serial) {
	return encode_key_chain(bytes_of(serial), kDfx2WireChain);
}

// [orig: CDKey_DecodeWireJoinChain @0x411b40 (dfx2.exe): NapiNP_DecodeEncryptedString under
//  the descriptor's +16 chain @0x411b88]
bool decode_wire_key(std::string_view wire_key, std::string &serial) {
	serial.clear();
	std::vector<uint8_t> plaintext;
	if (!decode_key_chain(std::string(wire_key), kDfx2WireChain, plaintext))
		return false;
	serial.assign(plaintext.begin(), plaintext.end());
	return true;
}

std::string make_activation(std::string_view wire_key, std::string_view fingerprint) {
	std::string registry = encode_key_chain(bytes_of(wire_key), kDfx2RegistryChain);
	mix_up(registry, fingerprint);
	return registry;
}

bool read_activation(std::string_view nwact, std::string_view fingerprint, std::string &wire_key) {
	wire_key.clear();
	std::string registry(nwact);
	mix_down(registry, fingerprint);
	// [orig: CDKey_Decode @0x411af0 (dfx2.exe): NapiNP_DecodeEncryptedString under the
	//  descriptor's +20 chain @0x411b38]
	std::vector<uint8_t> plaintext;
	if (!decode_key_chain(registry, kDfx2RegistryChain, plaintext))
		return false;
	wire_key = c_string_of(plaintext);
	return true;
}

LanJoinVerdict check_lan_join(
		const WireKeys &joiner, const WireKeys &host, const std::vector<WireKeys> &others) {
	std::vector<uint8_t> plaintext;
	// [orig: @0x50f3fc..0x50f407 (dfx2.exe): a NULL or empty CDKIID, code 10 @0x50f5ba]
	if (joiner.cdkiid.empty())
		return LanJoinVerdict::MissingKey;
	// [orig: @0x50f40d..0x50f42c (dfx2.exe): CDKey_DecodeWireJoinChain, code 11 @0x50f433]
	if (!decode_key_chain(joiner.cdkiid, kDfx2WireChain, plaintext))
		return LanJoinVerdict::UndecodableKey;
	// [orig: @0x50f449..0x50f462 (dfx2.exe): the host's own, when it has one, code 12 @0x50f469]
	if (!host.cdkiid.empty() && strutil::iequals(host.cdkiid, joiner.cdkiid))
		return LanJoinVerdict::HostsKey;
	// [orig: @0x50f47f..0x50f4c8 (dfx2.exe): every other player's +724 (a NULL one read as ""),
	//  code 13 @0x50f59f]
	for (const WireKeys &other : others) {
		if (strutil::iequals(joiner.cdkiid, other.cdkiid))
			return LanJoinVerdict::AnotherPlayersKey;
	}
	// [orig: @0x50f4ca..0x50f4d6 (dfx2.exe): no CDKIIDEXP1 (NULL or empty) ends the leg]
	if (joiner.exp1.empty())
		return LanJoinVerdict::Accepted;
	// [orig: @0x50f4dc..0x50f4fb (dfx2.exe): the decode under off_812F00, code 11]
	if (!decode_key_chain(joiner.exp1, kDfx2Exp1Chain, plaintext))
		return LanJoinVerdict::UndecodableKey;
	// [orig: @0x50f501..0x50f51a (dfx2.exe): the host's own CDKIIDEXP1, code 12]
	if (!host.exp1.empty() && strutil::iequals(host.exp1, joiner.exp1))
		return LanJoinVerdict::HostsKey;
	// [orig: @0x50f520..0x50f565 (dfx2.exe): every other player's +728, code 13]
	for (const WireKeys &other : others) {
		if (strutil::iequals(joiner.exp1, other.exp1))
			return LanJoinVerdict::AnotherPlayersKey;
	}
	return LanJoinVerdict::Accepted;
}

} // namespace opennova::cdkey
