// D-NET-524: the session key material's generator, the NAPI manager's 16-bit LCG. A draw
// steps state = u16(state * 0x04B05731 + 1) and yields min + state % (max - min + 1); a key is
// four byte draws, the first the most significant, so its bytes follow b' = 49b + 1; an SCRK is
// 63 draws over the 31-character charset whose NUL slot appends nothing, so it walks the fixed
// order BCXYGH01LM45QR89VWDFZJK23NP67ST from a rotation, 61 or 62 characters, and a connection
// draws its key before its SCRK. The pinned vectors are an independent model's; the two captured
// keys are retail's (net/lan-parity-verdict.md, D-NET-524).
// [orig: NapiPRNG_NextInRange @0x62e450; NapiSession_GenerateId @0x62e490;
//  NapiNP_GenerateSessionKey @0x61ea70; CNapiNPConnection_GenerateTxKey @0x61dfe0;
//  CNapiNPConnection_InitFromSession @0x6263f4 / @0x626401]

#include <net/npwire/nw_session_framing.h>

#include "../common/test_expect.h"

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>

using opennova::ConnectionKeys;
using opennova::NapiPrng;

namespace {

constexpr char kCharset[] = "0123456789BCDFGHJKLMNPQRSTVWXYZ";

// A key whose four bytes each follow the one before as b' = 49b + 1 (mod 256).
bool bytes_chain(uint32_t key) {
	for (int shift = 24; shift > 0; shift -= 8) {
		const uint32_t b = (key >> shift) & 0xFFu;
		const uint32_t next = (key >> (shift - 8)) & 0xFFu;
		if (((49u * b + 1u) & 0xFFu) != next)
			return false;
	}
	return true;
}

// The SCRK the mod-32 walk gives after a draw whose state was `last` mod 32:
// x' = 17x + 1 (mod 32), 63 steps, slot 31 appending nothing.
std::string scrk_after(uint32_t last) {
	std::string out;
	uint32_t x = last & 31u;
	for (int i = 0; i < 63; ++i) {
		x = (17u * x + 1u) & 31u;
		if (x != 31u)
			out.push_back(kCharset[x]);
	}
	return out;
}

int test_next_in_range_steps_the_sixteen_bit_lcg() {
	NapiPrng prng(0x12345678u);
	// min >= max returns min and draws nothing [orig: @0x62e45a..0x62e462].
	TEST_EXPECT(prng.next_in_range(5, 5) == 5 && prng.draws() == 0 && prng.state() == 0x12345678u);
	TEST_EXPECT(prng.next_in_range(9, 2) == 9 && prng.draws() == 0);
	// The first draw multiplies the whole seed and keeps the low 16 bits.
	TEST_EXPECT(prng.next_in_range(0, 9) == 3 && prng.state() == 0x54F9u && prng.draws() == 1);
	TEST_EXPECT(prng.next_in_range(-3, 3) == 0 && prng.state() == 0xE2AAu && prng.draws() == 2);
	NapiPrng bytes(0x12345678u);
	TEST_EXPECT(bytes.next_in_range(0, 255) == 249 && bytes.next_in_range(0, 255) == 170 &&
			bytes.next_in_range(0, 255) == 139 && bytes.next_in_range(0, 255) == 156 &&
			bytes.state() == 0xFF9Cu);
	TEST_EXPECT(NapiPrng::kMultiplier == 0x04B05731u);
	return 0;
}

int test_keys_and_scrks_follow_the_lcg() {
	// Pinned against an independent model of the generator.
	struct Vector {
		uint32_t seed;
		uint32_t key;
		const char *scrk;
	};
	const Vector vectors[] = {
		{ 0x12345678u, 0xF9AA8B9Cu,
				"YGH01LM45QR89VWDFZJK23NP67STBCXYGH01LM45QR89VWDFZJK23NP67STBC" },
		{ 0u, 0x01329324u, "5QR89VWDFZJK23NP67STBCXYGH01LM45QR89VWDFZJK23NP67STBCXYGH01LM" },
	};
	for (const Vector &v : vectors) {
		NapiPrng prng(v.seed);
		const ConnectionKeys keys = opennova::generate_connection_keys(prng);
		TEST_EXPECT(keys.key == v.key);
		TEST_EXPECT(keys.scrk == v.scrk);
		TEST_EXPECT(prng.draws() == 4u + 63u); // the key's four, then the SCRK's 63
		TEST_EXPECT(bytes_chain(keys.key));
		TEST_EXPECT(keys.scrk == scrk_after(keys.key));
	}
	// The key then the SCRK: the two calls in order give generate_connection_keys's pair.
	NapiPrng a(0x00C0FFEEu);
	NapiPrng b(0x00C0FFEEu);
	const ConnectionKeys pair = opennova::generate_connection_keys(a);
	const uint32_t key = opennova::generate_session_key(b);
	TEST_EXPECT(pair.key == key && pair.scrk == opennova::generate_tx_key(b));
	return 0;
}

// The captured keys: RR's CK 0x4667b839 and SK 0xaced5eff. Any state whose low byte begins
// the key reproduces it, and the SCRK that follows: 61 characters from 'B' after the CK, and
// 62 from 'J' after the SK, whose last byte sits in the NUL slot (the one 63-byte 0x82 SCRK).
int test_captured_keys_reproduce() {
	TEST_EXPECT(bytes_chain(0x4667B839u) && bytes_chain(0xACED5EFFu));
	NapiPrng ck(0x55u);
	const ConnectionKeys client = opennova::generate_connection_keys(ck);
	TEST_EXPECT(client.key == 0x4667B839u);
	TEST_EXPECT(client.scrk == "BCXYGH01LM45QR89VWDFZJK23NP67STBCXYGH01LM45QR89VWDFZJK23NP67S");
	TEST_EXPECT(client.scrk.size() == 61);
	NapiPrng sk(0x9Bu);
	const ConnectionKeys server = opennova::generate_connection_keys(sk);
	TEST_EXPECT(server.key == 0xACED5EFFu);
	TEST_EXPECT(server.scrk == "JK23NP67STBCXYGH01LM45QR89VWDFZJK23NP67STBCXYGH01LM45QR89VWDFZ");
	TEST_EXPECT(server.scrk.size() == 62);
	return 0;
}

// Every SCRK is 61 or 62 characters, 62 exactly when the draw before it hit the NUL slot.
int test_every_scrk_is_sixty_one_or_sixty_two() {
	for (uint32_t seed = 0; seed < 0x10000u; seed += 0x0101u) {
		NapiPrng prng(seed);
		const ConnectionKeys keys = opennova::generate_connection_keys(prng);
		const bool nul_before = (keys.key & 31u) == 31u;
		TEST_EXPECT(keys.scrk.size() == (nul_before ? 62u : 61u));
		TEST_EXPECT(keys.key != 0 && bytes_chain(keys.key));
	}
	return 0;
}

// `b` stepped `steps` times along the low byte's own cycle b' = 49b + 1 (mod 256), a single
// 256-cycle: every draw's low byte follows the one before it, whatever the range drawn.
uint32_t low_byte_after(uint32_t b, int steps) {
	b &= 0xFFu;
	for (int i = 0; i < steps; ++i)
		b = (49u * b + 1u) & 0xFFu;
	return b;
}

// The process's manager is one stream. Its keys and SCRKs keep their shapes, and each key's
// first byte is fixed by the last byte drawn before it: one step after a host key's last byte,
// 64 after a connection's key (its SCRK's 63 draws between them), so f^64(SK & 0xFF) is the
// next connection's SK, exactly. A generator seeded afresh per connection fails this.
int test_manager_draws_are_one_stream() {
	const uint32_t host_key = opennova::manager_session_key();
	TEST_EXPECT(host_key != 0 && bytes_chain(host_key));
	const ConnectionKeys first = opennova::manager_connection_keys();
	TEST_EXPECT(bytes_chain(first.key));
	TEST_EXPECT(first.scrk == scrk_after(first.key));
	TEST_EXPECT((first.key >> 24) == low_byte_after(host_key, 1));
	const ConnectionKeys second = opennova::manager_connection_keys();
	TEST_EXPECT(bytes_chain(second.key) && second.scrk == scrk_after(second.key));
	TEST_EXPECT((second.key >> 24) == low_byte_after(first.key, 64));
	const uint32_t next_key = opennova::manager_session_key();
	TEST_EXPECT((next_key >> 24) == low_byte_after(second.key, 64));
	return 0;
}

// The committed retail captures' play keys, the first 16 characters of each session's client
// and server SCRK (fixtures/novaworld/run_*/manifest.txt): each is how the generator's SCRK
// begins after some state, a run of the fixed order, the NUL slot's skip included.
int test_retail_play_keys_are_runs_of_the_cycle() {
	const char *const runs[] = { "run_20260426_113242", "run_20260426_120102",
		"run_20260426_120859" };
	const std::string names[] = { "client_play_key = ", "server_play_key = " };
	int seen = 0;
	for (const char *run : runs) {
		std::ifstream in(std::string(FIXTURE_DIR) + "/" + run + "/manifest.txt");
		TEST_EXPECT(in.good());
		std::string line;
		while (std::getline(in, line)) {
			if (!line.empty() && line.back() == '\r')
				line.pop_back();
			for (const std::string &name : names) {
				if (line.compare(0, name.size(), name) != 0)
					continue;
				const std::string prefix = line.substr(name.size());
				TEST_EXPECT(prefix.size() == 16);
				bool found = false;
				// Seeds 0..31 start the SCRK at each of the 32 slots (17 is invertible mod 32).
				for (uint32_t seed = 0; seed < 32 && !found; ++seed) {
					NapiPrng prng(seed);
					found = opennova::generate_tx_key(prng).compare(0, prefix.size(), prefix) == 0;
				}
				TEST_EXPECT(found);
				++seen;
			}
		}
	}
	TEST_EXPECT(seen == 6);
	return 0;
}

} // namespace

int main() {
	if (test_next_in_range_steps_the_sixteen_bit_lcg() != 0)
		return 1;
	if (test_keys_and_scrks_follow_the_lcg() != 0)
		return 1;
	if (test_captured_keys_reproduce() != 0)
		return 1;
	if (test_every_scrk_is_sixty_one_or_sixty_two() != 0)
		return 1;
	if (test_manager_draws_are_one_stream() != 0)
		return 1;
	if (test_retail_play_keys_are_runs_of_the_cycle() != 0)
		return 1;
	std::printf("OK: the NAPI manager LCG's keys and SCRKs\n");
	return 0;
}
