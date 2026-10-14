#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace opennova {

// NW-UDP session-channel outer framing + per-session key material, shared by
// every host-side accept path (apps/novaworld_server's listener and the
// in-engine HostSessionAccept). Extracted verbatim from the original listener
// so the two paths can never drift — there is exactly one decode_inbound /
// encode_outbound in the tree, and the key material's two sources sit here: the
// manager LCG the game protocol draws from and the CSPRNG form the NovaWorld
// service legs keep.
//
// "Outer framing" = the NAPI CRC envelope + the SESSION_NWU_KEY layer that
// wraps every 0x41/0x42/0x43/0x46/0x81/0x82/0x83 opcode. The inner SCRK layer
// (0x43 ProtocolMessage region) is handled by protocol_message.h, NOT here.

// The NAPI manager's PRNG, the manager's +4 block {state, multiplier, draws}: a
// 16-bit LCG, each draw `state = u16(state * 0x04B05731 + 1)` and the value
// `min + state % (max - min + 1)` (min itself, drawing nothing, when min is not
// below max). A process creates one manager, at the network init, so every key
// the game protocol draws comes from this one stream.
// [orig: NapiPRNG_NextInRange @0x62e450 - the min >= max return @0x62e45a..0x62e462,
//  the step @0x62e465..0x62e473, the idiv @0x62e475..0x62e47b, the draw count
//  @0x62e47d; NapiSession_GenerateId @0x62e490 - multiplier 0x4B05731 @0x62e4b1,
//  draws 0 @0x62e4bc; NapiNPManager_Create @0x62c5e0, its one caller
//  CNapiNetwork_Init @0x4ca638]
class NapiPrng {
public:
	static constexpr uint32_t kMultiplier = 0x04B05731u;
	explicit NapiPrng(uint32_t seed) : state_(seed) {}
	int32_t next_in_range(int32_t min, int32_t max);
	uint32_t state() const { return state_; }
	uint32_t draws() const { return draws_; }

private:
	uint32_t state_ = 0;
	uint32_t multiplier_ = kMultiplier;
	uint32_t draws_ = 0;
};

// The manager PRNG's seed, the one platform part: retail sums the time-stamp
// counter's two halves and GetTickCount; this sums the two halves of the
// steady clock's nanosecond count and its millisecond count.
// [orig: NapiSession_GenerateId @0x62e490 - rdtsc @0x62e4a0 (sub_61D600), GetTickCount
//  @0x62e4a8, the sum @0x62e4ad..0x62e4c3; called by NapiNPManager_Init @0x62c4a2]
uint32_t napi_prng_platform_seed();

// A session key (the host key, a joined connection's SK, a connecting client's CK):
// four draws in 0..255, the first the most significant byte, drawn again while the
// key is zero. Under the LCG the bytes follow b' = 49b + 1 (mod 256), so a zero key
// cannot occur and the redraw never runs; it is ported as written.
// [orig: NapiNP_GenerateSessionKey @0x61ea70 - the draws @0x61ea7f / @0x61ea92 /
//  @0x61eaa7 / @0x61eabc, the jz @0x61eac3]
uint32_t generate_session_key(NapiPrng &prng);

// The SCRK: 63 draws in 0..31 over the 31-character charset
// "0123456789BCDFGHJKLMNPQRSTVWXYZ", index 31 reading the NUL that ends it and so
// appending nothing. Mod 32 the LCG steps x' = 17x + 1, a full cycle, so an SCRK
// walks the fixed order BCXYGH01LM45QR89VWDFZJK23NP67ST from some rotation and is
// 61 characters, or 62 when the draw before it hit the NUL slot.
// [orig: CNapiNPConnection_GenerateTxKey @0x61dfe0 - `63 - strlen` @0x61e03b..0x61e040,
//  NextInRange(0, g_TxKeyCharsetLen) @0x61e05b, sprintf "%c" @0x61e075, the copy to
//  conn+0xCC @0x61e0c9; the charset @0x7dfa10 via g_TxKeyCharset @0x849f10]
std::string generate_tx_key(NapiPrng &prng);

// A connection's key material, the key then the SCRK, as both connect sites draw
// them. [orig: CNapiNPConnection_InitFromSession @0x6263f4 / @0x626401 (CK, then the
//  SCRK); NapiNPProtocol_HandleClientJoin @0x62bf5e / @0x62bf99 (SK, then the SCRK)]
struct ConnectionKeys {
	uint32_t key = 0;
	std::string scrk;
};
ConnectionKeys generate_connection_keys(NapiPrng &prng);

// The same draws from the process's one manager PRNG, seeded at first use
// (napi_prng_platform_seed) and serialized, a connection's key and SCRK under one
// hold. The game protocol's host key, SK / SCRK and CK / SCRK draw here.
// [orig: NapiNPProtocol_StartServer @0x62b5f5 (the host key); HandleClientJoin and
//  InitFromSession above]
uint32_t manager_session_key();
ConnectionKeys manager_connection_keys();

// An SCRK from the OS CSPRNG, in the generator's charset and 63-draw shape but in
// no fixed order or length (63 at most): NovaLogic's master service's SCRK, which is
// not Jointops.exe's code, and the NovaWorld lobby client's, which has not moved
// to the manager PRNG yet (D-NET-524's residual). The game protocol draws generate_tx_key.
std::string make_dev_scrk();

// 60-char lowercase-hex NWUID (retail format). Random per session.
std::string make_dev_nwuid();

// Per-connection ServerAuth.SK. Retail's value is unique per session; a single
// hardcoded constant aliases two concurrent sessions to outside observers.
uint32_t make_random_session_u32();

// A session key over an arbitrary 32-bit draw, drawn until nonzero, zero being no key:
// the NovaWorld master service's SK and the lobby client's CK, which do not draw from
// the game's manager PRNG (generate_session_key); `draw` unset is
// make_random_session_u32, a test injecting its own.
// [orig: NapiNP_GenerateSessionKey @0x61EA70 - the jz @0x61EAC3]
uint32_t make_session_key(const std::function<uint32_t()> &draw = {});

// The connection counter: the game protocol's, pre-incremented at every connection
// create on the protocol, a step to 0 taking 1 instead, the new connection keeping it
// as its index. Retail steps it for the client side's connections (type 2) and the
// host's server-side ones (type 1) alike; a client connection's index is the CI its
// 0x41s and 0x42 carry. `advance_connection_index` steps the given counter;
// `next_connection_index` steps the process's one counter. A LAN browse's enumerator
// connection takes one, and the join from a row it discovered goes on with that same
// connection, so the browse's 0x41s and the join's 0x42 carry one CI; a dial without
// a row creates its own connection and takes the next. Only our client connections
// step it: the host's server-side creates do not (D-NET-713; a server-side index never
// reaches the wire, the 0x82 echoing the client's CI, so only a later client
// connection in the same process numbers differently).
// [orig: CNapiNPConnection_Create @0x62acb0 - `add [esi+0ECCh], edx` @0x62b282 on both
//  arms (the type test @0x62b1d1..0x62b1d5), the 0 -> 1 wrap @0x62b288..0x62b28a,
//  conn+0x14 @0x62b296; a type-2 connection's enumerator is created with it @0x62b1dc;
//  the creates: type 2 @0x4c9b9c / @0x4c9d9c, type 1 in NapiNPProtocol_HandleClientJoin
//  @0x62bf06; CNapiNPConnection_SendSessionInit's CI is the client's, conn+0x154
//  @0x620fa7; CNapiNetwork_StartClientConnection @0x4ca160 ->
//  CNapiNPConnection_InitFromSession @0x626320 on the enumerator's connection]
uint32_t advance_connection_index(uint32_t &counter);
uint32_t next_connection_index();

// Decode an inbound NW-UDP datagram. On success, populates `opcode_out`
// (byte 0 of the CRC-stripped packet, plaintext) and `body_out` (the
// NWU-decrypted payload after byte 0). Names are swapped vs onnet —
// server-side decrypt is our nwu_encrypt.
bool nw_decode_inbound(const uint8_t *raw, size_t raw_len,
                       uint8_t &opcode_out, std::vector<uint8_t> &body_out);

// Encode an outbound reply: prepend `opcode`, NWU-encrypt the body part
// (server-side encrypt is our nwu_decrypt), then NAPI envelope-wrap.
std::vector<uint8_t> nw_encode_outbound(uint8_t opcode, std::vector<uint8_t> body);

} // namespace opennova
