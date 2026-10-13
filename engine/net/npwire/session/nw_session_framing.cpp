#include <net/npwire/nw_session_framing.h>

#include <net/napi/envelope.h>
#include <net/novacrypto/nwu.h>
#include <net/npwire/session_keys.h>

#include <base/os_random/os_random.h>

#include <chrono>
#include <mutex>
#include <random>

namespace opennova {

namespace {
// The SCRK's draws and charset: 63 draws, each an inclusive 0..31 over the
// 31-character vowel-free charset, index 31 its NUL.
// [orig: CNapiNPConnection_GenerateTxKey @0x61dfe0 - `63 - strlen` @0x61e040;
//  g_TxKeyCharset @0x849f10 -> 0x7dfa10; g_TxKeyCharsetLen @0x31c1074 = 31]
constexpr int kScrkDraws = 63;
constexpr char kScrkCharset[] = "0123456789BCDFGHJKLMNPQRSTVWXYZ";
constexpr int kScrkCharsetLen = static_cast<int>(sizeof(kScrkCharset) - 1); // 31
} // namespace

// [orig: NapiPRNG_NextInRange @0x62e450]
int32_t NapiPrng::next_in_range(int32_t min, int32_t max) {
	// min >= max: min, no draw (`cmp edi, esi / jl` @0x62e45a, `mov eax, edi` @0x62e45e,
	// `pop edi` @0x62e460, the return @0x62e462).
	if (min >= max)
		return min;
	// imul's low 32 bits, +1, the low 16 kept @0x62e465..0x62e473.
	state_ = (multiplier_ * state_ + 1u) & 0xFFFFu;
	const int32_t span = max - min + 1; // @0x62e475..0x62e478
	const int32_t offset = static_cast<int32_t>(state_) % span; // idiv, state 0..0xFFFF @0x62e47b
	++draws_; // @0x62e47d
	return min + offset;
}

uint32_t napi_prng_platform_seed() {
	// The steady clock stands in for the time-stamp counter (its two 32-bit halves)
	// and its millisecond count for GetTickCount. [orig: NapiSession_GenerateId
	// @0x62e490 - sub_61D600 (rdtsc) @0x62e4a0, GetTickCount @0x62e4a8, the sum
	// @0x62e4ad..0x62e4b8]
	using namespace std::chrono;
	const uint64_t counter = static_cast<uint64_t>(
			duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count());
	const uint32_t tick = static_cast<uint32_t>(counter / 1000000u);
	return tick + static_cast<uint32_t>(counter >> 32) + static_cast<uint32_t>(counter);
}

// [orig: NapiNP_GenerateSessionKey @0x61ea70]
uint32_t generate_session_key(NapiPrng &prng) {
	uint32_t key = 0;
	do {
		uint32_t value = static_cast<uint32_t>(prng.next_in_range(0, 0xFF)); // @0x61ea7f
		value = (value << 8) | static_cast<uint32_t>(prng.next_in_range(0, 0xFF)); // @0x61ea92
		value = (value << 8) | static_cast<uint32_t>(prng.next_in_range(0, 0xFF)); // @0x61eaa7
		key = (value << 8) | static_cast<uint32_t>(prng.next_in_range(0, 0xFF)); // @0x61eabc
	} while (key == 0); // @0x61eac3
	return key;
}

// [orig: CNapiNPConnection_GenerateTxKey @0x61dfe0]
std::string generate_tx_key(NapiPrng &prng) {
	std::string out;
	out.reserve(kScrkDraws);
	for (int remaining = kScrkDraws; remaining > 0; --remaining) {
		// The draw @0x61e05b, then `sprintf("%c", charset[index])` @0x61e075: the NUL
		// at index 31 prints nothing, and the draw still counts.
		const char c = kScrkCharset[prng.next_in_range(0, kScrkCharsetLen)];
		if (c != '\0')
			out.push_back(c);
	}
	return out; // 63 at most: the 64-byte conn+0xCC keeps it whole @0x61e0c9
}

ConnectionKeys generate_connection_keys(NapiPrng &prng) {
	ConnectionKeys keys;
	keys.key = generate_session_key(prng);
	keys.scrk = generate_tx_key(prng);
	return keys;
}

namespace {

// The process's one manager and its PRNG, seeded when first drawn from.
// [orig: NapiNPManager_Create @0x62c5e0 (called once, CNapiNetwork_Init @0x4ca638) ->
//  NapiNPManager_Init @0x62c480 -> NapiSession_GenerateId @0x62c4a2]
struct ManagerPrng {
	std::mutex mutex;
	NapiPrng prng{ napi_prng_platform_seed() };
};

ManagerPrng &manager_prng() {
	static ManagerPrng manager;
	return manager;
}

} // namespace

uint32_t manager_session_key() {
	ManagerPrng &manager = manager_prng();
	const std::lock_guard<std::mutex> lock(manager.mutex);
	return generate_session_key(manager.prng);
}

ConnectionKeys manager_connection_keys() {
	ManagerPrng &manager = manager_prng();
	const std::lock_guard<std::mutex> lock(manager.mutex);
	return generate_connection_keys(manager.prng);
}

// The CSPRNG SCRK (nw_session_framing.h): the generator's 63 inclusive draws over
// its charset, the NUL slot appending nothing, but drawn from base/os_random, so
// its characters follow no order.
std::string make_dev_scrk() {
	OsRandom gen;
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
	OsRandom gen;
	std::uniform_int_distribution<int> pick(0, 15);
	std::string out;
	out.reserve(60);
	for (int i = 0; i < 60; ++i) {
		out.push_back(hex[pick(gen)]);
	}
	return out;
}

uint32_t make_random_session_u32() {
	return os_random_u32();
}

uint32_t make_session_key(const std::function<uint32_t()> &draw) {
	uint32_t key = 0;
	do {
		key = draw ? draw() : make_random_session_u32();
	} while (key == 0); // [orig: NapiNP_GenerateSessionKey @0x61EA70 - jz @0x61EAC3]
	return key;
}

uint32_t advance_connection_index(uint32_t &counter) {
	// [orig: CNapiNPConnection_Create @0x62b282..0x62b296]
	++counter;
	if (counter == 0) counter = 1;
	return counter;
}

uint32_t next_connection_index() {
	static std::mutex mutex;
	static uint32_t counter = 0;
	const std::lock_guard<std::mutex> lock(mutex);
	return advance_connection_index(counter);
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
