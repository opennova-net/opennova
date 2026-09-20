#pragma once

// The outer-namespace connection ping body shared by the 0x45 (client -> host)
// and 0x85 (host -> client) opcodes: `[u32 receiver-local key][WR u8][MS u32]`
// as flat TLVs. The receiver drops the datagram unless the leading dword equals
// its own key; a set WR asks for a pong that echoes MS with WR clear, and a
// clear WR lands `now - MS` as the session RTT.
// [orig: CNapiNPConnection_SendPing @0x61F080 — the peer's key dword @0x61F179,
//  NapiNP_WriteTLV("WR", &wants_reply, 1) @0x61F19B, NapiNP_WriteTLV("MS",
//  &timestamp, 4) @0x61F1B4; Nwu_HandlePing @0x623A70 — the key compare
//  @0x623BC2, the walk with Napi_StrCaseEqual on "WR" @0x623BFF and "MS"
//  @0x623C1A stopping at an empty name]
// Both endpoints (joiner_connection_disconnect.cpp, napi_np_protocol.cpp) use
// this one codec; the opcodes live in session_keys.h.

#include <base/io/le.h>
#include <base/io/strutil.h>
#include <net/npwire/flat_tlv.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace opennova {

struct SessionPingBody {
	uint32_t receiver_local_key = 0;
	bool wants_reply = false;
	uint32_t timestamp_ms = 0;
};

// False only when the body cannot carry the key dword. The TLV walk is
// lenient like retail's: a field that does not fit ends the walk with the
// fields read so far, and a value shorter than its width reads as zero (the
// bounds check stands in for retail's unguarded `*(_DWORD *)value`).
inline bool parse_session_ping_body(const uint8_t *data, size_t len, SessionPingBody &out) {
	if (data == nullptr || len < 4) return false;
	out.receiver_local_key = io::read_u32_le(data);
	out.wants_reply = false;
	out.timestamp_ms = 0;
	size_t pos = 4;
	while (pos < len) {
		FlatTlvField field;
		const size_t next = read_flat_tlv(data, len, pos, field);
		if (next == kFlatTlvEnd || field.name.empty()) break;
		if (strutil::iequals(field.name, "WR")) {
			out.wants_reply = field.size >= 1 && field.value[0] != 0;
		} else if (strutil::iequals(field.name, "MS")) {
			out.timestamp_ms = field.size >= 4 ? io::read_u32_le(field.value) : 0u;
		}
		pos = next;
	}
	return true;
}

inline std::vector<uint8_t> build_session_ping_body(uint32_t remote_key, bool wants_reply,
		uint32_t timestamp_ms) {
	std::vector<uint8_t> body;
	io::append_u32_le(body, remote_key);
	const uint8_t wr = wants_reply ? 1 : 0;
	append_flat_tlv(body, "WR", &wr, 1);
	uint8_t ms[4];
	ms[0] = static_cast<uint8_t>(timestamp_ms & 0xFFu);
	ms[1] = static_cast<uint8_t>((timestamp_ms >> 8) & 0xFFu);
	ms[2] = static_cast<uint8_t>((timestamp_ms >> 16) & 0xFFu);
	ms[3] = static_cast<uint8_t>((timestamp_ms >> 24) & 0xFFu);
	append_flat_tlv(body, "MS", ms, 4);
	return body;
}

} // namespace opennova
