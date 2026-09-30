#include <net/npwire/emote_wire.h>

#include <base/io/byte_reader.h>
#include <base/io/le.h>

namespace opennova {

void decode_emote_request(const uint8_t *body, size_t len, EmoteRequest &out) {
	out = EmoteRequest{};
	io::ByteReader r(body, len);
	// The host consumes the low byte only [orig: @0x501e74..0x501e7a]; the
	// full i16 is what the sender writes.
	if (len >= 2) {
		out.value = r.read_i16();
	} else {
		out.value = static_cast<int16_t>(r.read_u8());
	}
}

std::vector<uint8_t> encode_emote_request(const EmoteRequest &request) {
	std::vector<uint8_t> out;
	io::append_i16_le(out, request.value); // [orig: @0x42c137]
	return out;
}

void decode_emote_broadcast(const uint8_t *body, size_t len, EmoteBroadcast &out) {
	out = EmoteBroadcast{};
	io::ByteReader r(body, len);
	out.emote = r.read_u8();        // [orig: @0x427eca..0x427ed2]
	out.player_index = r.read_u8(); // [orig: @0x427ed8..0x427ee4]
}

std::vector<uint8_t> encode_emote_broadcast(const EmoteBroadcast &broadcast) {
	// [orig: @0x501e70..0x501e92 — the zeroed dword, byte 0 the emote, byte 1
	//  the sender's pool-0 index]
	return {broadcast.emote, broadcast.player_index, 0, 0};
}

void decode_radio_call_request(const uint8_t *body, size_t len, RadioCallRequest &out) {
	out = RadioCallRequest{};
	io::ByteReader r(body, len);
	// The host consumes the low byte only, 0 when the body is empty
	// [orig: @0x5143aa..0x5143b2]; the full i16 is what the sender writes.
	if (len >= 2) {
		out.value = r.read_i16();
	} else {
		out.value = static_cast<int16_t>(r.read_u8());
	}
}

std::vector<uint8_t> encode_radio_call_request(const RadioCallRequest &request) {
	std::vector<uint8_t> out;
	io::append_i16_le(out, request.value); // [orig: @0x42c167]
	return out;
}

} // namespace opennova
