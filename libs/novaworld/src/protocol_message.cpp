#include <novaworld/protocol_message.h>

#include <novacrypto/nwu.h>

#include <utility>

namespace opennova {

namespace {

inline uint32_t read_u32_le(const uint8_t *p) {
	return static_cast<uint32_t>(p[0]) |
			(static_cast<uint32_t>(p[1]) << 8) |
			(static_cast<uint32_t>(p[2]) << 16) |
			(static_cast<uint32_t>(p[3]) << 24);
}

void append_u32_le(std::vector<uint8_t> &out, uint32_t v) {
	out.push_back(static_cast<uint8_t>(v & 0xFFu));
	out.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
	out.push_back(static_cast<uint8_t>((v >> 16) & 0xFFu));
	out.push_back(static_cast<uint8_t>((v >> 24) & 0xFFu));
}

// Witnessed at NapiNPConnection_ParseMessages @ 0x625BC0 body branches
// (LEN/SKIP bits) and NapiNPConnection_DispatchMessage @ 0x622570 fragment
// flag handling (FRAG bits) in retail Jointops.exe. Cross-checked against
// onnet's `ProtocolMessageFlag` enum (FRAG_CONT/FRAG_END names match).
ProtocolMessageFlags decode_flags(uint8_t raw) {
	ProtocolMessageFlags f;
	f.raw = raw;
	f.settings_update = (raw & 0x80u) != 0;
	f.len16 = (raw & 0x40u) != 0;
	f.len8 = (raw & 0x20u) != 0;
	f.skip2 = (raw & 0x10u) != 0;
	f.skip1 = (raw & 0x08u) != 0;
	f.frag_cont = (raw & 0x04u) != 0;
	f.frag_end = (raw & 0x02u) != 0;
	f.msg_type_high_bit = (raw & 0x80u) != 0;
	return f;
}

} // namespace

ProtocolMessage make_protocol_message(uint8_t tag, std::vector<uint8_t> payload,
                                      uint8_t flags_raw) {
	ProtocolMessage msg;
	msg.tag = tag;
	msg.full_tag = tag;
	msg.payload = std::move(payload);
	msg.length = static_cast<uint32_t>(msg.payload.size());
	if (flags_raw == 0) {
		flags_raw = msg.payload.size() > 0xFFu ? 0x40u : 0x20u;
	}
	msg.flags = decode_flags(flags_raw);
	if (msg.flags.msg_type_high_bit) {
		msg.full_tag = static_cast<uint16_t>(0x100u | tag);
	}
	return msg;
}

// 13-byte session header per NapiNPProtocol_HandleSessionPacket @
// 0x626A00 + NapiNPConnection_ParseMessages @ 0x625BC0:
//   bytes 0..3 = session_id (matches conn->session_keys.local_key)
//   bytes 4..7 = seq_num (per-packet ordering, dedup)
//   bytes 8..11 = ack_count (peer's ack of our outbound seq)
//   byte 12 = connection_flags (cursor advances on read, value discarded)
bool parse_protocol_packet_header(const uint8_t *data, size_t len,
                                  ProtocolPacketHeader &out) {
	if (!data || len < PROTOCOL_PACKET_HEADER_SIZE) {
		return false;
	}
	out.session_id = read_u32_le(data + 0);
	out.seq_num = read_u32_le(data + 4);
	out.ack_count = read_u32_le(data + 8);
	out.connection_flags = data[12];
	return true;
}

// Per-message parsing matches NapiNPConnection_ParseMessages @ 0x625BC0
// inner loop: [flags:u8][tag:u8] then optional length (LEN8/LEN16) then
// optional skip bytes (SKIP1/SKIP2 — read but discarded by retail).
bool parse_protocol_messages(const uint8_t *data, size_t len,
                             std::vector<ProtocolMessage> &out) {
	if (!data) {
		return false;
	}
	out.clear();
	size_t pos = 0;
	while (pos + 2 <= len) {
		ProtocolMessage msg;
		const uint8_t flags_raw = data[pos];
		msg.flags = decode_flags(flags_raw);
		msg.tag = data[pos + 1];
		msg.full_tag = static_cast<uint16_t>(
				(msg.flags.msg_type_high_bit ? 0x100u : 0u) | msg.tag);
		pos += 2;

		if (msg.flags.len8) {
			if (pos + 1 > len) return true;
			msg.length = data[pos];
			pos += 1;
		} else if (msg.flags.len16) {
			if (pos + 2 > len) return true; // tolerant: stop cleanly
			msg.length = static_cast<uint32_t>(data[pos]) |
					(static_cast<uint32_t>(data[pos + 1]) << 8);
			pos += 2;
		} else {
			msg.length = 0;
		}

		// SKIP1/SKIP2: retail's parser reads these bytes off the wire and
		// discards them (cursor advances, value not stored). Honour the
		// same cursor advance so the next message lands at the correct
		// offset; retain the consumed bytes on the message in case a
		// caller wants to re-encode bit-for-bit.
		if (msg.flags.skip1) {
			if (pos + 1 > len) return true;
			msg.skip_bytes.assign(data + pos, data + pos + 1);
			pos += 1;
		} else if (msg.flags.skip2) {
			if (pos + 2 > len) return true;
			msg.skip_bytes.assign(data + pos, data + pos + 2);
			pos += 2;
		}

		if (pos + msg.length > len) {
			// Bad/truncated packet. Keep what we parsed.
			return true;
		}
		if (msg.length > 0) {
			msg.payload.assign(data + pos, data + pos + msg.length);
			pos += msg.length;
		}
		out.push_back(std::move(msg));
	}
	return true;
}

bool append_protocol_message(std::vector<uint8_t> &out,
                             const ProtocolMessage &msg) {
	if (msg.payload.size() > 0xFFFFu) {
		return false;
	}
	uint8_t flags_raw = msg.flags.raw;
	// Retail emits zero-flag messages (no length field, empty payload) —
	// e.g. the S2C 0x1C stub in every BMS-state bundle (witnessed in the
	// 2026-04-26 capture runs, fixtures/novaworld/run_*/bundle_1c_*.nwmsg).
	// Honor that shape; only auto-pick a length flag when there is a
	// payload that needs one.
	if (flags_raw == 0 && !msg.payload.empty()) {
		flags_raw = msg.payload.size() > 0xFFu ? 0x40u : 0x20u;
	}
	ProtocolMessageFlags flags = decode_flags(flags_raw);
	out.push_back(flags_raw);
	out.push_back(msg.tag);
	if (flags.len8) {
		if (msg.payload.size() > 0xFFu) {
			return false;
		}
		out.push_back(static_cast<uint8_t>(msg.payload.size() & 0xFFu));
	} else if (flags.len16) {
		out.push_back(static_cast<uint8_t>(msg.payload.size() & 0xFFu));
		out.push_back(static_cast<uint8_t>((msg.payload.size() >> 8) & 0xFFu));
	} else if (!msg.payload.empty()) {
		return false;
	}
	// SKIP1/SKIP2: emit the same skip byte(s) as the inbound parser
	// consumed (retail discards them, but retains positional alignment).
	// If caller didn't supply skip_bytes but flagged SKIP1/SKIP2, write
	// zeros — retail ignores the values anyway.
	if (flags.skip1) {
		if (!msg.skip_bytes.empty()) {
			out.push_back(msg.skip_bytes[0]);
		} else {
			out.push_back(0);
		}
	} else if (flags.skip2) {
		if (msg.skip_bytes.size() >= 2) {
			out.push_back(msg.skip_bytes[0]);
			out.push_back(msg.skip_bytes[1]);
		} else {
			out.push_back(0);
			out.push_back(0);
		}
	}
	out.insert(out.end(), msg.payload.begin(), msg.payload.end());
	return true;
}

bool encode_protocol_messages(const std::vector<ProtocolMessage> &messages,
                              std::vector<uint8_t> &out) {
	out.clear();
	for (const ProtocolMessage &msg : messages) {
		if (!append_protocol_message(out, msg)) {
			out.clear();
			return false;
		}
	}
	return true;
}

bool decode_protocol_packet_plaintext(const uint8_t *body, size_t body_len,
                                      std::string_view scrk,
                                      ProtocolPacketHeader &hdr_out,
                                      std::vector<ProtocolMessage> &messages_out) {
	if (!body || body_len < PROTOCOL_PACKET_HEADER_SIZE) {
		return false;
	}
	if (!parse_protocol_packet_header(body, body_len, hdr_out)) {
		return false;
	}
	const size_t msgs_len = body_len - PROTOCOL_PACKET_HEADER_SIZE;
	if (msgs_len == 0) {
		messages_out.clear();
		return true;
	}
	std::vector<uint8_t> inner(body + PROTOCOL_PACKET_HEADER_SIZE, body + body_len);
	if (!scrk.empty()) {
		nwu_encrypt(inner.data(), inner.size(), scrk);
	}
	return parse_protocol_messages(inner.data(), inner.size(), messages_out);
}

bool encode_protocol_packet_plaintext(const ProtocolPacketHeader &hdr,
                                      const std::vector<ProtocolMessage> &messages,
                                      std::string_view scrk,
                                      std::vector<uint8_t> &body_out) {
	std::vector<uint8_t> inner;
	if (!encode_protocol_messages(messages, inner)) {
		body_out.clear();
		return false;
	}
	if (!inner.empty() && !scrk.empty()) {
		nwu_decrypt(inner.data(), inner.size(), scrk);
	}
	body_out.clear();
	body_out.reserve(PROTOCOL_PACKET_HEADER_SIZE + inner.size());
	append_u32_le(body_out, hdr.session_id);
	append_u32_le(body_out, hdr.seq_num);
	append_u32_le(body_out, hdr.ack_count);
	body_out.push_back(hdr.connection_flags);
	body_out.insert(body_out.end(), inner.begin(), inner.end());
	return true;
}

// Fragment reassembly per NapiNPConnection_DispatchMessage @ 0x622570
// (three-state model in retail) and onnet's nw_udp_server.py
// process_protocol_message (None/START/MIDDLE/END classification).
//
// Retail's frag bits collapse to a single rule for the dispatch decision:
//   FRAG_CONT (0x04) set    = "more fragments coming"  → BUFFER, no dispatch
//   FRAG_CONT (0x04) clear  = "this is the last/only piece" → DISPATCH
//
// The three-state breakdown for completeness:
//   flags & 0x06 == 0x04 (FIRST)   → reset buffer + append, no dispatch
//   flags & 0x06 == 0x06 (MID)     → append, no dispatch
//   flags & 0x06 == 0x02 (FINAL)   → append + dispatch reassembled
//   flags & 0x06 == 0x00 (NONE)    → dispatch directly (single message)
//
// Regression history: an earlier rewrite used `frag_first && !frag_end`
// (= dispatch on FRAG_CONT+FRAG_END) which broke retail's mid-fragment
// flag=0x46 by dispatching it prematurely. See notes/ida_witness_matrix.md
// for diagnosis trail.
bool reassemble_protocol_payload(ProtocolReassemblyState &state,
                                 const ProtocolMessage &msg,
                                 std::vector<uint8_t> &payload_out,
                                 bool *was_fragmented) {
	if (was_fragmented) {
		*was_fragmented = msg.flags.frag_cont || msg.flags.frag_end ||
				!state.buffer.empty();
	}
	payload_out.clear();
	if ((msg.flags.raw & 0x06u) == 0x04u) {
		state.buffer.clear();
	}
	state.buffer.insert(state.buffer.end(), msg.payload.begin(), msg.payload.end());
	if (msg.flags.frag_cont) {
		return false;
	}
	payload_out = std::move(state.buffer);
	state.buffer.clear();
	return true;
}

bool decode_protocol_packet(uint8_t *body, size_t body_len,
                            std::string_view session_nwu_key,
                            std::string_view client_scrk,
                            ProtocolPacketHeader &hdr_out,
                            std::vector<ProtocolMessage> &messages_out) {
	if (!body || body_len < PROTOCOL_PACKET_HEADER_SIZE || session_nwu_key.empty()) {
		return false;
	}
	// Outer decrypt: matches PFF_EncryptBuffer on the recv side.
	nwu_encrypt(body, body_len, session_nwu_key);
	return decode_protocol_packet_plaintext(body, body_len, client_scrk,
			hdr_out, messages_out);
}

} // namespace opennova
