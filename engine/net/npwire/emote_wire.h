#pragma once

// The voice-menu wire: the C2S 0x14 request a client sends from its Emotes
// menu and the S2C 0x2D broadcast the host fans to the players near the
// sender; the C2S 0x13 request its Radio menu sends (the host answers with the
// S2C 0x6D radio call, ingame_encode.h encode_tracked_player_voice).
// See docs/net/novaworld-net-re.md and docs/interface/hud-re.md "The MP legs".

#include <cstddef>
#include <cstdint>
#include <vector>

namespace opennova {

// C2S 0x14: [i16 value], the Emotes-menu digit (1..9, and 10 for the 0 key).
// The host reads only the low byte; a zero-length body reads 0.
// [orig: NetPacket_SendEmoteRequest @0x42C120 (the i16 store @0x42c137, length
//  2 @0x42c13d) from Input_HandleSpecialKeys @0x49c752; the host read
//  NapiNPServerMsg_HandleEmoteRequest @0x501e74..0x501e7a]
struct EmoteRequest {
	int16_t value = 0;
};
void decode_emote_request(const uint8_t *body, size_t len, EmoteRequest &out);
std::vector<uint8_t> encode_emote_request(const EmoteRequest &request);

// S2C 0x2D: [u8 emote][u8 raw pool-0 index][u16 0]. The host packs a 4-byte
// dword whose high half is zero; the client reads the first two bytes, each
// defaulting to 0 when absent.
// [orig: NapiNPServerMsg_HandleEmoteRequest — `payload = 0` @0x501e70, the
//  emote byte @0x501e7d, Pool_GetIndexFromPtr(0, entity) @0x501e87 into
//  byte 1 @0x501e92, length 4 @0x501f38; NapiNPClientMsg_HandleEmote @0x427E90 —
//  the reads @0x427eca..0x427ee4]
struct EmoteBroadcast {
	uint8_t emote = 0;
	uint8_t player_index = 0;
};
void decode_emote_broadcast(const uint8_t *body, size_t len, EmoteBroadcast &out);
std::vector<uint8_t> encode_emote_broadcast(const EmoteBroadcast &broadcast);

// C2S 0x13: [i16 value], the Radio-menu digit (1..9, and 10 for the 0 key).
// The host reads only the low byte; a zero-length body reads 0.
// [orig: NetPacket_SendRadioCallRequest @0x42C150 (ex NetPacket_SendInt16Type13;
//  the i16 store @0x42c167, length 2 @0x42c16d) from Input_HandleSpecialKeys
//  @0x49c79e; the host read NapiNPServerMsg_HandleRadioCall @0x5143aa..0x5143b0]
struct RadioCallRequest {
	int16_t value = 0;
};
void decode_radio_call_request(const uint8_t *body, size_t len, RadioCallRequest &out);
std::vector<uint8_t> encode_radio_call_request(const RadioCallRequest &request);

} // namespace opennova
