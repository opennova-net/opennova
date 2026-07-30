#include <npwire/protocol_message.h>

#include <novacrypto/nwu.h>

#include <algorithm>
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
	f.settings_update = (raw & PROTOCOL_MSG_FLAG_SETTINGS_UPDATE) != 0;
	f.len16 = (raw & PROTOCOL_MSG_FLAG_LEN16) != 0;
	f.len8 = (raw & PROTOCOL_MSG_FLAG_LEN8) != 0;
	f.skip2 = (raw & PROTOCOL_MSG_FLAG_SKIP2) != 0;
	f.skip1 = (raw & PROTOCOL_MSG_FLAG_SKIP1) != 0;
	f.frag_cont = (raw & PROTOCOL_MSG_FLAG_FRAG_CONT) != 0;
	f.frag_end = (raw & PROTOCOL_MSG_FLAG_FRAG_END) != 0;
	// Deliberately the same 0x80 as settings_update — both views of one bit
	// exist on purpose (see the ProtocolMessageFlags doc block).
	f.msg_type_high_bit = (raw & PROTOCOL_MSG_FLAG_SETTINGS_UPDATE) != 0;
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
	// Retail frames an empty message as the 2-byte zero-flag form [0x00][tag] — no
	// length field at all (the parser reads no-length-flag as length 0). Auto-pick a
	// length flag only when there is a payload to describe, so the empty 0x47/0x09/0x0A
	// legs stay byte-identical to the golden capture.
	// [orig: NapiNP_WriteMessageRecord @0x61da90 length-flag gating;
	//  CNapiNPConnection_ParseMessages @0x625bc0 zero-flag read]
	if (flags_raw == 0 && !msg.payload.empty()) {
		flags_raw = msg.payload.size() > 0xFFu ? PROTOCOL_MSG_FLAG_LEN16 : PROTOCOL_MSG_FLAG_LEN8;
	}
	msg.flags = decode_flags(flags_raw);
	if (msg.flags.msg_type_high_bit) {
		msg.full_tag = static_cast<uint16_t>(PROTOCOL_FULL_TAG_HIGH_BASE | tag);
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
				(msg.flags.msg_type_high_bit ? PROTOCOL_FULL_TAG_HIGH_BASE : 0u) | msg.tag);
		pos += 2;

		// [D-NET-8] On a truncated inner stream the original substitutes 0 for the missing field and
		// STILL dispatches this final (partial, zero-filled) message before stopping — it does not bail
		// [orig: CNapiNPConnection_ParseMessages @0x625bc0: each guarded read falls back to 0 / the value
		// is read from the zero-padded 64 KB buffer]. So on truncation we emit the partial message (its
		// payload is the available bytes zero-padded to the claimed length) and stop, rather than dropping
		// it. Valid packets never hit these branches, so their parse is unchanged.
		auto emit_partial_and_stop = [&] {
			msg.payload.assign(data + pos, data + len);
			msg.payload.resize(msg.length, 0); // zero-pad to the claimed length (length 0 -> empty)
			out.push_back(std::move(msg));
		};
		if (msg.flags.len8) {
			if (pos + 1 > len) { emit_partial_and_stop(); break; }
			msg.length = data[pos];
			pos += 1;
		} else if (msg.flags.len16) {
			if (pos + 2 > len) { emit_partial_and_stop(); break; }
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
			if (pos + 1 > len) { emit_partial_and_stop(); break; } // [D-NET-8]
			msg.skip_bytes.assign(data + pos, data + pos + 1);
			pos += 1;
		} else if (msg.flags.skip2) {
			if (pos + 2 > len) { emit_partial_and_stop(); break; } // [D-NET-8]
			msg.skip_bytes.assign(data + pos, data + pos + 2);
			pos += 2;
		}

		if (pos + msg.length > len) {
			emit_partial_and_stop(); // [D-NET-8] truncated payload -> dispatch available bytes, zero-padded
			break;
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
		flags_raw = msg.payload.size() > 0xFFu ? PROTOCOL_MSG_FLAG_LEN16 : PROTOCOL_MSG_FLAG_LEN8;
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

bool frame_session_packet(SessionSequencing &seq, const SessionCrypto &crypto,
                          const std::vector<ProtocolMessage> &messages,
                          std::vector<uint8_t> &body_out) {
	const bool retain = seq.outbound_message_limit != 0 && !messages.empty();
	if (retain &&
	    (seq.retained_outbound_message_count > seq.outbound_message_limit ||
	     messages.size() > seq.outbound_message_limit -
	             seq.retained_outbound_message_count)) {
		body_out.clear();
		return false;
	}

	ProtocolPacketHeader hdr;
	hdr.session_id = crypto.session_id;
	hdr.seq_num = seq.next_outbound_seq++; // post-increment: the pre-increment value is stamped
	hdr.ack_count = seq.last_inbound_seq;
	hdr.connection_flags = 0; // always 0 on every witnessed encode site
	if (!encode_protocol_packet_plaintext(hdr, messages, crypto.out_scrk, body_out)) {
		return false;
	}
	if (retain) {
		seq.retained_outbound[hdr.seq_num] = messages;
		seq.retained_outbound_message_count += messages.size();
	}
	return true;
}

bool frame_session_packet_for_sequence(SessionSequencing &seq, const SessionCrypto &crypto,
		uint32_t packet_sequence, std::vector<uint8_t> &body_out) {
	if (packet_sequence > seq.next_outbound_seq) {
		body_out.clear();
		return false;
	}
	if (packet_sequence == seq.next_outbound_seq) {
		++seq.next_outbound_seq;
	}

	const auto retained = seq.retained_outbound.find(packet_sequence);
	static const std::vector<ProtocolMessage> no_messages;
	const std::vector<ProtocolMessage> &messages =
			retained != seq.retained_outbound.end() ? retained->second : no_messages;
	const ProtocolPacketHeader hdr{
		crypto.session_id,
		packet_sequence,
		seq.last_inbound_seq,
		0,
	};
	return encode_protocol_packet_plaintext(hdr, messages, crypto.out_scrk, body_out);
}

void acknowledge_session_packets(SessionSequencing &seq, uint32_t ack_sequence) {
	auto retained = seq.retained_outbound.begin();
	while (retained != seq.retained_outbound.end() &&
	       retained->first <= ack_sequence) {
		seq.retained_outbound_message_count -= retained->second.size();
		retained = seq.retained_outbound.erase(retained);
	}
}

std::vector<uint32_t> build_session_missing_sequence_list(
		const SessionSequencing &seq, bool include_zero) {
	std::vector<uint32_t> missing;
	missing.reserve(SESSION_RESEND_LIST_MAX);
	const uint32_t expected = seq.last_inbound_seq + 1;
	missing.push_back(expected);
	if (include_zero && missing.size() < SESSION_RESEND_LIST_MAX) {
		missing.push_back(0);
	}
	if (seq.queued_inbound.empty()) {
		return missing;
	}

	// The witnessed walk is bounded by the queue's HEAD node — the lowest queued
	// sequence for our ordered map — not the highest: gaps between later queued
	// packets are requested on a later pass, after the frontier advances.
	// [orig: CNapiNPConnection_BuildMissingSeqList @0x6234b0 — the
	//  `while (candidate < head_node->seq)` bound @0x623527]
	const uint32_t first_queued = seq.queued_inbound.begin()->first;
	for (uint32_t candidate = expected + 1;
	     candidate < first_queued && missing.size() < SESSION_RESEND_LIST_MAX;
	     ++candidate) {
		if (seq.queued_inbound.find(candidate) == seq.queued_inbound.end()) {
			missing.push_back(candidate);
		}
	}
	return missing;
}

bool encode_session_resend_list(uint32_t remote_key,
		const std::vector<uint32_t> &requested_sequences,
		std::vector<uint8_t> &body_out) {
	if (requested_sequences.size() > SESSION_RESEND_LIST_MAX) {
		body_out.clear();
		return false;
	}
	body_out.clear();
	body_out.reserve(4 + requested_sequences.size() * 4);
	append_u32_le(body_out, remote_key);
	for (uint32_t sequence : requested_sequences) {
		append_u32_le(body_out, sequence);
	}
	return true;
}

bool decode_session_resend_list(const uint8_t *body, size_t body_len,
		uint32_t local_key, std::vector<uint32_t> &requested_sequences_out) {
	requested_sequences_out.clear();
	if (!body || body_len < 4 || body_len > 0x10000u ||
	    read_u32_le(body) != local_key) {
		return false;
	}
	for (size_t pos = 4; pos + 4 <= body_len; pos += 4) {
		requested_sequences_out.push_back(read_u32_le(body + pos));
	}
	return true;
}

bool deframe_session_packet(SessionSequencing &seq, const SessionCrypto &crypto,
                            const uint8_t *body, size_t body_len,
                            ProtocolPacketHeader &hdr_out,
                            std::vector<ProtocolMessage> &messages_out,
                            SessionDeframeAdmission *admission_out) {
	if (admission_out != nullptr) *admission_out = SessionDeframeAdmission{};
	// Retail validates the receiver-local session key before it reads the sequence number or enters
	// ParseMessages. Parse only the plaintext header here so a stale packet from a prior connection
	// cannot mutate ordering/ACK state or make us decrypt and parse its inner stream.
	// [orig: NapiNPProtocol_HandleSessionPacket @0x626b32..0x626b72]
	if (!parse_protocol_packet_header(body, body_len, hdr_out)) {
		return false;
	}
	if (crypto.expected_inbound_session_id.has_value() &&
	    hdr_out.session_id != *crypto.expected_inbound_session_id) {
		messages_out.clear();
		return true; // retail quietly consumes/drops a packet addressed to a different connection
	}
	std::vector<ProtocolMessage> decoded;
	if (!decode_protocol_packet_plaintext(body, body_len, crypto.in_scrk, hdr_out, decoded)) {
		return false; // leave seq untouched; the caller applies its own failure policy
	}
	messages_out.clear();
	if (!seq.ordered_recovery_enabled) {
		// Protocol-only consumers do not own the two-way retained-resend pump needed by the retail
		// contiguous queue. Use a best-effort high-water gate instead: any strictly newer packet is
		// admitted (so a permanent loss cannot deadlock the session), while sequence zero, stale
		// packets, and duplicates are consumed without redispatch or ACK regression.
		if (hdr_out.seq_num == 0 || hdr_out.seq_num <= seq.last_inbound_seq)
			return true;
		messages_out = std::move(decoded);
		seq.last_inbound_seq = hdr_out.seq_num;
		if (admission_out != nullptr) {
			admission_out->admitted = true;
			admission_out->max_ack_count = hdr_out.ack_count;
			admission_out->packets.push_back(
					SessionDeframeAdmission::Packet{hdr_out, messages_out});
		}
		return true;
	}

	// HandleSessionPacket admits only recv_ack_seq+1 to ParseMessages. Sequence zero and
	// seq <= recv_ack_seq are consumed without dispatch; a future packet is retained on
	// the connection queue until its gap closes. [orig: @0x626bcc..0x626c3a]
	if (hdr_out.seq_num == 0 || hdr_out.seq_num <= seq.last_inbound_seq) return true;
	const uint32_t expected = seq.last_inbound_seq + 1;
	if (hdr_out.seq_num != expected) {
		if (admission_out != nullptr) admission_out->future_packet_seen = true;
		seq.missing_request_pending = true;
		const auto existing = seq.queued_inbound.find(hdr_out.seq_num);
		if (existing == seq.queued_inbound.end() &&
		    seq.queued_inbound.size() < SESSION_PACKET_QUEUE_MAX) {
			seq.queued_inbound.emplace(
					hdr_out.seq_num, QueuedSessionPacket{hdr_out, std::move(decoded)});
		}
		return true;
	}

	const ProtocolPacketHeader admitted_header = hdr_out;
	messages_out = std::move(decoded);
	seq.last_inbound_seq = hdr_out.seq_num;
	SessionDeframeAdmission admission;
	admission.admitted = true;
	admission.max_ack_count = hdr_out.ack_count;
	admission.packets.push_back(
			SessionDeframeAdmission::Packet{admitted_header, messages_out});
	for (;;) {
		auto queued = seq.queued_inbound.find(seq.last_inbound_seq + 1);
		if (queued == seq.queued_inbound.end()) break;
		admission.packets.push_back(SessionDeframeAdmission::Packet{
				queued->second.header, queued->second.messages});
		for (ProtocolMessage &message : queued->second.messages)
			messages_out.push_back(std::move(message));
		admission.max_ack_count = std::max(
				admission.max_ack_count, queued->second.header.ack_count);
		seq.last_inbound_seq = queued->first;
		seq.queued_inbound.erase(queued);
	}
	// `hdr_out` describes the datagram passed to this call. Draining queued successors must not
	// silently replace it with the last recovered packet's header; packet-local headers are exposed
	// above for handlers that dispatch the whole recovered run.
	hdr_out = admitted_header;
	if (admission_out != nullptr) *admission_out = admission;
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
	static_assert((PROTOCOL_MSG_FLAG_FRAG_CONT | PROTOCOL_MSG_FLAG_FRAG_END) == 0x06u,
	              "the witnessed fragment-state mask");
	if ((msg.flags.raw & (PROTOCOL_MSG_FLAG_FRAG_CONT | PROTOCOL_MSG_FLAG_FRAG_END)) ==
	    PROTOCOL_MSG_FLAG_FRAG_CONT) {
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
