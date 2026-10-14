#pragma once

// The in-game body codec registry: one entry per (direction, tag) that pairs the
// catalog's structured decoder with the struct encoder that writes the same
// body, so a body can be proven against itself — decode it to the byte, then
// re-encode the decoded fields and compare. The retail-corpus parity gate
// (tests/novaworld/nwmsg_codec_parity_test.cpp) runs every committed retail body
// through it; an entry with no encoder is decode-only (the host still writes
// those bytes inline from its own state).
//
// Entries cover the catalog-Decoded tags witnessed in the committed retail
// corpus (fixtures/novaworld/run_*); every key is a catalog Decoded tag, which
// nw_message_coverage checks. Adapters only copy fields between the decoded
// struct and the encoder's input: no raw bytes pass through (ADR 0003).
// [orig: the dispatch tables g_NPMsgInfoClient @0x82AE28 (S2C, client side) and
//  g_NPMsgInfoServer @0x82B5D8 (C2S, host side)]

#include <net/npwire/entity_class.h>
#include <net/npwire/ingame_decode.h> // FrameObjectiveBody

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace opennova {

struct TitleIdentity; // net/title/title_identity.h

// Session state a body's layout depends on but the body does not carry.
struct IngameBodyCodecContext {
	// S2C 0x0A / C2S 0x0C compact records: wire type id -> record class
	// (built from items.def). Empty resolves nothing, so those bodies fail closed.
	std::function<EntityClass(uint16_t)> class_of;
	// g_GameType & 0x20000 and the title's co-op checkpoint: the 0x0A sub-block-3
	// objective body.
	FrameObjectiveBody objective_body = FrameObjectiveBody::None;
	// (g_GameType & ~0x20000) == 0x10020: the 0x0F waypoint tail.
	bool waypoint_gametype = false;

	// The session a capture or a host runs: its game type and the title it speaks
	// (null runs JO's, title_in_match).
	static IngameBodyCodecContext for_session(uint32_t game_type, const TitleIdentity *title);
};

enum class BodyDecode : uint8_t {
	Exact,        // the decoder accepted the body and read every byte
	Rejected,     // the decoder refused the body
	ShortConsume, // accepted, but bytes remain the decoder does not model
};

struct IngameBodyCodecResult {
	BodyDecode decode = BodyDecode::Rejected;
	size_t consumed = 0;
	bool reencoded = false;       // an encoder ran on the decoded fields
	std::vector<uint8_t> bytes;   // its output, when `reencoded`
};

struct IngameBodyCodec {
	char dir;            // 'S' = server->client, 'C' = client->server
	uint8_t tag;
	const char *decoder; // the decode_* the entry calls
	const char *encoder; // the encode_* it calls; nullptr = decode-only
	IngameBodyCodecResult (*run)(const uint8_t *body, size_t len,
			const IngameBodyCodecContext &ctx);
};

const IngameBodyCodec *ingame_body_codecs(size_t *count);
const IngameBodyCodec *lookup_ingame_body_codec(char dir, uint8_t tag);

} // namespace opennova
