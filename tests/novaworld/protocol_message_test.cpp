#include <net/npwire/protocol_message.h>

#include <cstdio>
#include <string>
#include <vector>

namespace {

bool expect(bool condition, const char *message) {
	if (condition) {
		return true;
	}
	std::fprintf(stderr, "FAIL: %s\n", message);
	return false;
}

bool check_plaintext_encode_decode_roundtrip() {
	opennova::ProtocolPacketHeader hdr;
	hdr.session_id = 0x11223344u;
	hdr.seq_num = 7;
	hdr.ack_count = 6;
	hdr.connection_flags = 0;

	std::vector<opennova::ProtocolMessage> messages;
	messages.push_back(opennova::make_protocol_message(0x10, {0x00, 0x00, 0x00, 0x00}));
	messages.push_back(opennova::make_protocol_message(0x57, {0xEF, 0xBE, 0xAD, 0xDE, 0x01}));

	std::vector<uint8_t> body;
	const std::string scrk = "TESTSCRK0123456789";
	if (!expect(opennova::encode_protocol_packet_plaintext(hdr, messages, scrk, body),
			"encode plaintext protocol body")) return false;
	if (!expect(body.size() > opennova::PROTOCOL_PACKET_HEADER_SIZE,
			"body includes encrypted inner messages")) return false;

	opennova::ProtocolPacketHeader decoded_hdr;
	std::vector<opennova::ProtocolMessage> decoded;
	if (!expect(opennova::decode_protocol_packet_plaintext(
			body.data(), body.size(), scrk, decoded_hdr, decoded),
			"decode plaintext protocol body")) return false;
	if (!expect(decoded_hdr.session_id == hdr.session_id, "session_id round-trips")) return false;
	if (!expect(decoded_hdr.seq_num == hdr.seq_num, "seq_num round-trips")) return false;
	if (!expect(decoded_hdr.ack_count == hdr.ack_count, "ack_count round-trips")) return false;
	if (!expect(decoded.size() == 2, "two inner messages decoded")) return false;
	if (!expect(decoded[0].tag == 0x10 && decoded[0].payload.size() == 4,
			"tag 0x10 payload decoded")) return false;
	if (!expect(decoded[1].tag == 0x57 && decoded[1].payload[4] == 0x01,
			"tag 0x57 payload decoded")) return false;
	return true;
}

// Verify SKIP1 / SKIP2 flag bits round-trip per
// NapiNPConnection_ParseMessages @ 0x625BC0 retail behaviour.
bool check_skip_bytes_round_trip() {
	auto with_skip1 = opennova::make_protocol_message(
			0x42, {0xAA, 0xBB, 0xCC}, /*flags_raw=*/0x28); // LEN8 | SKIP1
	with_skip1.skip_bytes = {0x99};
	auto with_skip2 = opennova::make_protocol_message(
			0x55, {0xDE, 0xAD}, /*flags_raw=*/0x30); // LEN8 | SKIP2
	with_skip2.skip_bytes = {0x77, 0x88};

	std::vector<uint8_t> bytes;
	if (!expect(opennova::encode_protocol_messages({with_skip1, with_skip2}, bytes),
			"encode SKIP1/SKIP2 messages")) return false;

	std::vector<opennova::ProtocolMessage> decoded;
	if (!expect(opennova::parse_protocol_messages(bytes.data(), bytes.size(), decoded),
			"parse SKIP1/SKIP2 messages")) return false;
	if (!expect(decoded.size() == 2, "two messages")) return false;
	if (!expect(decoded[0].flags.skip1 && decoded[0].skip_bytes == std::vector<uint8_t>{0x99},
			"SKIP1 byte preserved")) return false;
	if (!expect(decoded[0].payload == std::vector<uint8_t>({0xAA, 0xBB, 0xCC}),
			"SKIP1 payload preserved")) return false;
	if (!expect(decoded[1].flags.skip2 &&
			decoded[1].skip_bytes == std::vector<uint8_t>({0x77, 0x88}),
			"SKIP2 bytes preserved")) return false;
	if (!expect(decoded[1].payload == std::vector<uint8_t>({0xDE, 0xAD}),
			"SKIP2 payload preserved")) return false;
	return true;
}

bool check_custom_flags_encode() {
	auto msg = opennova::make_protocol_message(0x00, {
			0x00, 0x08, 0x00, 0x00, 0x00,
			0x0C, 0x00, 0x00, 0x00,
	}, 0xA0);
	std::vector<uint8_t> bytes;
	if (!expect(opennova::encode_protocol_messages({msg}, bytes), "encode custom flags")) return false;
	if (!expect(bytes.size() == 12, "custom flags wire size")) return false;
	if (!expect(bytes[0] == 0xA0 && bytes[1] == 0x00 && bytes[2] == 9,
			"flags/tag/len are preserved")) return false;
	return true;
}

bool check_settings_update_selects_high_table() {
	const uint8_t bytes[] = {
		0xA0, 0x2A, 0x01, 0x7F, // 0x80 high table + LEN8
	};
	std::vector<opennova::ProtocolMessage> decoded;
	if (!expect(opennova::parse_protocol_messages(bytes, sizeof(bytes), decoded),
			"parse high-table message")) return false;
	if (!expect(decoded.size() == 1, "one high-table message decoded")) return false;
	if (!expect(decoded[0].flags.settings_update, "flag 0x80 recognized")) return false;
	if (!expect(decoded[0].full_tag == 0x12A,
			"flag 0x80 selects full_tag high table")) return false;

	auto made = opennova::make_protocol_message(0x2A, {0x7F}, 0xA0);
	if (!expect(made.full_tag == 0x12A,
			"make_protocol_message uses 0x80 for high table")) return false;
	return true;
}

bool check_len8_precedes_len16_when_both_bits_set() {
	const uint8_t bytes[] = {
		0x60, 0x33, 0x02, 0xAA, 0xBB, // LEN8 wins even though LEN16 is also set
		0x20, 0x44, 0x01, 0xCC,
	};
	std::vector<opennova::ProtocolMessage> decoded;
	if (!expect(opennova::parse_protocol_messages(bytes, sizeof(bytes), decoded),
			"parse mixed LEN8/LEN16 flags")) return false;
	if (!expect(decoded.size() == 2, "LEN8 precedence preserves following message")) return false;
	if (!expect(decoded[0].tag == 0x33 && decoded[0].length == 2,
			"first message uses one-byte length")) return false;
	if (!expect(decoded[0].payload == std::vector<uint8_t>({0xAA, 0xBB}),
			"first payload decoded via LEN8")) return false;
	if (!expect(decoded[1].tag == 0x44 && decoded[1].payload == std::vector<uint8_t>({0xCC}),
			"second message remains aligned")) return false;
	return true;
}

bool check_fragment_reassembly_flushes_on_no_cont() {
	opennova::ProtocolReassemblyState state;
	auto first = opennova::make_protocol_message(0x00, {0x01, 0x02}, 0x24);
	auto last = opennova::make_protocol_message(0x00, {0x03}, 0x22);
	std::vector<uint8_t> payload;
	bool was_fragmented = false;
	if (!expect(!opennova::reassemble_protocol_payload(state, first, payload, &was_fragmented),
			"FRAG_CONT buffers")) return false;
	if (!expect(was_fragmented, "first fragment reports fragmented")) return false;
	if (!expect(opennova::reassemble_protocol_payload(state, last, payload, &was_fragmented),
			"final fragment flushes")) return false;
	if (!expect(payload.size() == 3, "payload reassembled")) return false;
	if (!expect(payload[0] == 0x01 && payload[2] == 0x03, "payload order preserved")) return false;
	return true;
}

bool check_first_fragment_resets_stale_buffer() {
	opennova::ProtocolReassemblyState state;
	state.buffer = {0x99, 0x88};
	auto first = opennova::make_protocol_message(0x00, {0x01, 0x02}, 0x24);
	auto last = opennova::make_protocol_message(0x00, {0x03}, 0x22);
	std::vector<uint8_t> payload;
	bool was_fragmented = false;
	if (!expect(!opennova::reassemble_protocol_payload(state, first, payload, &was_fragmented),
			"first fragment buffers")) return false;
	if (!expect(state.buffer == std::vector<uint8_t>({0x01, 0x02}),
			"first fragment resets stale reassembly buffer")) return false;
	if (!expect(opennova::reassemble_protocol_payload(state, last, payload, &was_fragmented),
			"final fragment flushes after reset")) return false;
	if (!expect(payload == std::vector<uint8_t>({0x01, 0x02, 0x03}),
			"reassembled payload excludes stale bytes")) return false;
	return true;
}

// Regression: retail's ClientPlayRequest arrives as 0x44 → 0x46 → 0x46 →
// 0x42, where 0x46 (= LEN16 + FRAG_CONT + FRAG_END) is a MID fragment that
// must keep buffering. A previous rewrite used the condition
// `frag_first && !frag_end` which dispatched 0x46 prematurely, losing the
// earlier buffered fragments. See notes/ida_witness_matrix.md for the
// diagnosis trail. Cross-checked against onnet `nw_udp_server.py`
// process_protocol_message.
bool check_fragment_reassembly_three_fragments() {
	opennova::ProtocolReassemblyState state;
	auto first = opennova::make_protocol_message(0x00, {0x01, 0x02, 0x03}, 0x44); // LEN16 + FRAG_CONT
	auto mid   = opennova::make_protocol_message(0x00, {0x04, 0x05, 0x06}, 0x46); // LEN16 + FRAG_CONT + FRAG_END
	auto last  = opennova::make_protocol_message(0x00, {0x07, 0x08, 0x09}, 0x42); // LEN16 + FRAG_END
	std::vector<uint8_t> payload;
	bool was_fragmented = false;
	if (!expect(!opennova::reassemble_protocol_payload(state, first, payload, &was_fragmented),
			"first fragment buffers (flags=0x44)")) return false;
	if (!expect(!opennova::reassemble_protocol_payload(state, mid, payload, &was_fragmented),
			"mid fragment buffers (flags=0x46) — regression case")) return false;
	if (!expect(opennova::reassemble_protocol_payload(state, last, payload, &was_fragmented),
			"final fragment dispatches (flags=0x42)")) return false;
	if (!expect(payload.size() == 9, "all 9 reassembled bytes present")) return false;
	if (!expect(payload[0] == 0x01 && payload[3] == 0x04 && payload[6] == 0x07 && payload[8] == 0x09,
			"payload order preserved across all three fragments")) return false;
	return true;
}

} // namespace

// [D-NET-8] A truncated inner stream still dispatches the final partial message, zero-padded to its
// claimed length — matching CNapiNPConnection_ParseMessages @0x625bc0 (it substitutes 0 for missing
// bytes rather than bailing). Earlier our parser dropped the partial.
bool check_truncated_message_is_dispatched_zero_padded() {
	const uint8_t bytes[] = {0x20, 0x42, 0x05, 0xAA, 0xBB}; // flags=LEN8, tag=0x42, len=5, only 2 body bytes
	std::vector<opennova::ProtocolMessage> decoded;
	if (!expect(opennova::parse_protocol_messages(bytes, sizeof(bytes), decoded),
	            "truncated stream still parses")) return false;
	if (!expect(decoded.size() == 1, "the truncated partial message is dispatched, not dropped")) return false;
	if (!expect(decoded[0].tag == 0x42, "truncated message keeps its tag")) return false;
	const std::vector<uint8_t> want = {0xAA, 0xBB, 0x00, 0x00, 0x00};
	if (!expect(decoded[0].payload == want, "payload = available bytes zero-padded to the claimed length"))
		return false;
	return true;
}

// ADR 0013: the shared SessionSequencing / SessionCrypto framing helpers — frame == the manual
// header-fill + encode (byte-identical) with a seq post-increment; deframe round-trips + latches the
// inbound ack; and the out_scrk is genuinely applied (direction wiring).
bool check_session_packet_frame_deframe() {
	const std::string scrk = "TESTSCRK0123456789";
	std::vector<opennova::ProtocolMessage> messages;
	messages.push_back(opennova::make_protocol_message(0x10, {0x00, 0x00, 0x00, 0x00}));
	messages.push_back(opennova::make_protocol_message(0x57, {0xEF, 0xBE, 0xAD, 0xDE, 0x01}));

	opennova::SessionSequencing seq{7, 3};
	opennova::SessionCrypto crypto{scrk, {}, 0x11223344u};
	std::vector<uint8_t> framed;
	if (!expect(opennova::frame_session_packet(seq, crypto, messages, framed),
	            "frame_session_packet encodes")) return false;
	if (!expect(seq.next_outbound_seq == 8, "frame post-increments next_outbound_seq (7 -> 8)")) return false;

	opennova::ProtocolPacketHeader manual_hdr;
	manual_hdr.session_id = 0x11223344u;
	manual_hdr.seq_num = 7;
	manual_hdr.ack_count = 3;
	manual_hdr.connection_flags = 0;
	std::vector<uint8_t> manual;
	if (!expect(opennova::encode_protocol_packet_plaintext(manual_hdr, messages, scrk, manual),
	            "manual header-fill + encode")) return false;
	if (!expect(framed == manual, "frame body == manual header-fill + encode (byte-identical)")) return false;

	// Retail's outer HandleSessionPacket admits only the next contiguous sequence into
	// ParseMessages. Seed the receiver immediately before this captured seq=7 packet.
	opennova::SessionSequencing rx{1, 6};
	rx.ordered_recovery_enabled = true;
	opennova::SessionCrypto rx_crypto{{}, scrk, 0, 0x11223344u};
	opennova::ProtocolPacketHeader got_hdr;
	std::vector<opennova::ProtocolMessage> got;
	opennova::SessionDeframeAdmission admission;
	if (!expect(opennova::deframe_session_packet(
			rx, rx_crypto, framed.data(), framed.size(), got_hdr, got, &admission),
	            "deframe_session_packet decodes")) return false;
	if (!expect(got_hdr.seq_num == 7 && got_hdr.session_id == 0x11223344u, "deframed header round-trips")) return false;
	if (!expect(rx.last_inbound_seq == 7, "deframe latches last_inbound_seq = hdr.seq_num")) return false;
	if (!expect(got.size() == 2 && got[0].tag == 0x10 && got[1].tag == 0x57, "inner messages round-trip")) return false;
	if (!expect(admission.admitted && admission.max_ack_count == 3,
	            "contiguous packet reports its ACK through the admission gate")) return false;

	// The receiver-local key is checked before sequence admission and inner parsing. A delayed
	// datagram from a prior connection can share this endpoint and SCRK-shaped bytes, but retail
	// drops it before it can poison the new connection's contiguous frontier.
	opennova::ProtocolPacketHeader wrong_session_hdr = manual_hdr;
	wrong_session_hdr.session_id = 0x55667788u;
	wrong_session_hdr.seq_num = 8;
	wrong_session_hdr.ack_count = 99;
	std::vector<uint8_t> wrong_session;
	if (!expect(opennova::encode_protocol_packet_plaintext(
			wrong_session_hdr, messages, scrk, wrong_session),
	            "encode wrong-session packet")) return false;
	got = {opennova::make_protocol_message(0x7F, {0xAA})};
	if (!expect(opennova::deframe_session_packet(
			rx, rx_crypto, wrong_session.data(), wrong_session.size(),
			got_hdr, got, &admission),
	            "wrong-session packet is quietly consumed")) return false;
	if (!expect(got.empty() && !admission.admitted &&
	                    rx.last_inbound_seq == 7 && rx.queued_inbound.empty(),
	            "wrong-session packet cannot parse, dispatch, or mutate sequencing"))
		return false;

	// The exact same UDP datagram can be observed more than once. Retail returns success for
	// seq <= recv_ack_seq without calling ParseMessages, so one-shot gameplay records must not be
	// dispatched twice and the contiguous ACK latch must not move backwards.
	opennova::ProtocolPacketHeader duplicate_hdr = manual_hdr;
	duplicate_hdr.ack_count = 41;
	std::vector<uint8_t> duplicate;
	if (!expect(opennova::encode_protocol_packet_plaintext(duplicate_hdr, messages, scrk, duplicate),
	            "encode duplicate session packet with a newer peer ACK")) return false;
	got.clear();
	if (!expect(opennova::deframe_session_packet(
			rx, rx_crypto, duplicate.data(), duplicate.size(), got_hdr, got, &admission),
	            "duplicate session packet is consumed")) return false;
	if (!expect(got.empty(), "duplicate session packet is not redispatched")) return false;
	if (!expect(rx.last_inbound_seq == 7, "duplicate leaves the contiguous inbound ACK at 7")) return false;
	if (!expect(!admission.admitted && admission.max_ack_count == 0,
	            "duplicate packet cannot advance the peer ACK")) return false;

	opennova::ProtocolPacketHeader stale_hdr = manual_hdr;
	stale_hdr.seq_num = 5;
	stale_hdr.ack_count = 42;
	std::vector<uint8_t> stale;
	if (!expect(opennova::encode_protocol_packet_plaintext(stale_hdr, messages, scrk, stale),
	            "encode stale session packet")) return false;
	got.clear();
	if (!expect(opennova::deframe_session_packet(
			rx, rx_crypto, stale.data(), stale.size(), got_hdr, got, &admission),
	            "stale session packet is consumed")) return false;
	if (!expect(got.empty(), "stale session packet is not redispatched")) return false;
	if (!expect(rx.last_inbound_seq == 7, "stale packet cannot regress the inbound ACK")) return false;
	if (!expect(!admission.admitted && admission.max_ack_count == 0,
	            "stale packet cannot advance the peer ACK")) return false;

	opennova::ProtocolPacketHeader gap_hdr = manual_hdr;
	gap_hdr.seq_num = 9;
	gap_hdr.ack_count = 15;
	const std::vector<opennova::ProtocolMessage> gap_messages = {
		opennova::make_protocol_message(0x31, {0x99}),
	};
	std::vector<uint8_t> gap;
	if (!expect(opennova::encode_protocol_packet_plaintext(gap_hdr, gap_messages, scrk, gap),
	            "encode future-gap session packet")) return false;
	got.clear();
	if (!expect(opennova::deframe_session_packet(
			rx, rx_crypto, gap.data(), gap.size(), got_hdr, got, &admission),
	            "future-gap session packet is consumed")) return false;
	if (!expect(got.empty(), "future-gap packet waits for the missing sequence")) return false;
	if (!expect(rx.last_inbound_seq == 7, "future-gap packet cannot skip the contiguous ACK")) return false;
	if (!expect(!admission.admitted && admission.max_ack_count == 0,
	            "future-gap packet cannot advance the peer ACK before admission")) return false;
	if (!expect(admission.future_packet_seen,
	            "future-gap packet raises the one-pump retail NACK latch")) return false;
	if (!expect(rx.missing_request_pending,
	            "future-gap packet leaves the receive-batch missing check pending")) return false;

	got.clear();
	if (!expect(opennova::deframe_session_packet(
			rx, rx_crypto, gap.data(), gap.size(), got_hdr, got, &admission),
	            "duplicate queued future packet is consumed")) return false;
	if (!expect(got.empty() && admission.future_packet_seen &&
	                    rx.queued_inbound.size() == 1,
	            "duplicate future packet re-raises NACK without duplicating queue storage"))
		return false;

	opennova::ProtocolPacketHeader next_hdr = manual_hdr;
	next_hdr.seq_num = 8;
	next_hdr.ack_count = 20;
	const std::vector<opennova::ProtocolMessage> next_messages = {
		opennova::make_protocol_message(0x30, {0x88}),
	};
	std::vector<uint8_t> next;
	if (!expect(opennova::encode_protocol_packet_plaintext(next_hdr, next_messages, scrk, next),
	            "encode next contiguous session packet")) return false;
	got.clear();
	if (!expect(opennova::deframe_session_packet(
			rx, rx_crypto, next.data(), next.size(), got_hdr, got, &admission),
	            "next contiguous session packet decodes")) return false;
	if (!expect(got.size() == 2,
	            "next contiguous packet dispatches itself and the queued future packet")) return false;
	if (!expect(got[0].tag == 0x30 && got[0].payload == std::vector<uint8_t>({0x88}) &&
	            got[1].tag == 0x31 && got[1].payload == std::vector<uint8_t>({0x99}),
	            "gap close dispatches the next packet before the queued packet")) return false;
	if (!expect(got_hdr.seq_num == 8 && got_hdr.ack_count == 20,
	            "gap recovery preserves the triggering packet header instead of "
	            "overwriting it with the last queued packet")) return false;
	if (!expect(rx.last_inbound_seq == 9,
	            "closing the gap advances the inbound ACK through the queued packet")) return false;
	if (!expect(admission.admitted && admission.max_ack_count == 20,
	            "gap close reports the greatest ACK across every admitted packet")) return false;
	if (!expect(admission.packets.size() == 2 &&
	                    admission.packets[0].header.seq_num == 8 &&
	                    admission.packets[0].header.ack_count == 20 &&
	                    admission.packets[0].messages.size() == 1 &&
	                    admission.packets[0].messages[0].tag == 0x30 &&
	                    admission.packets[1].header.seq_num == 9 &&
	                    admission.packets[1].header.ack_count == 15 &&
	                    admission.packets[1].messages.size() == 1 &&
	                    admission.packets[1].messages[0].tag == 0x31,
	            "gap recovery retains each admitted packet's own header/message association"))
		return false;
	if (!expect(rx.missing_request_pending,
	            "gap close leaves boundary code to clear the earlier batch latch")) return false;

	// Retail bounds each connection's future-packet queue at packet_queue_max=100.
	// Keep sequence 1 missing and offer one packet more than the queue can retain.
	opennova::SessionSequencing bounded_rx{1, 0};
	bounded_rx.ordered_recovery_enabled = true;
	for (uint32_t future_seq = 2;
	     future_seq <= static_cast<uint32_t>(opennova::SESSION_PACKET_QUEUE_MAX) + 2;
	     ++future_seq) {
		opennova::ProtocolPacketHeader future_hdr = manual_hdr;
		future_hdr.seq_num = future_seq;
		future_hdr.ack_count = 1000 + future_seq;
		std::vector<uint8_t> future;
		if (!expect(opennova::encode_protocol_packet_plaintext(
				future_hdr, next_messages, scrk, future),
		            "encode bounded future session packet")) return false;
		got.clear();
		if (!expect(opennova::deframe_session_packet(
				bounded_rx, rx_crypto, future.data(), future.size(), got_hdr, got, &admission),
		            "bounded future session packet is consumed")) return false;
		if (!expect(!admission.admitted && admission.max_ack_count == 0,
		            "queued future packet cannot advance the peer ACK")) return false;
		if (!expect(admission.future_packet_seen,
		            "even a cap-dropped future packet raises the retail NACK latch")) return false;
	}
	if (!expect(bounded_rx.queued_inbound.size() == opennova::SESSION_PACKET_QUEUE_MAX,
	            "future packet queue stays bounded at SESSION_PACKET_QUEUE_MAX")) return false;
	if (!expect(bounded_rx.queued_inbound.count(
			static_cast<uint32_t>(opennova::SESSION_PACKET_QUEUE_MAX) + 1) == 1 &&
	            bounded_rx.queued_inbound.count(
			static_cast<uint32_t>(opennova::SESSION_PACKET_QUEUE_MAX) + 2) == 0,
	            "the first 100 future packets are retained and excess packets are dropped")) return false;

	// out_scrk is genuinely applied: a different key yields a different encrypted body.
	opennova::SessionSequencing seq2{7, 3};
	opennova::SessionCrypto crypto2{"DIFFERENTSCRK98765", {}, 0x11223344u};
	std::vector<uint8_t> framed2;
	if (!expect(opennova::frame_session_packet(seq2, crypto2, messages, framed2),
	            "frame with a different out_scrk")) return false;
	if (!expect(framed2 != framed, "out_scrk is applied: a different key changes the encrypted body")) return false;
	return true;
}

bool check_session_resend_list_wire_and_gap_selection() {
	opennova::SessionSequencing rx{1, 3};
	rx.queued_inbound.emplace(5, opennova::QueuedSessionPacket{});
	rx.queued_inbound.emplace(7, opennova::QueuedSessionPacket{});
	rx.queued_inbound.emplace(10, opennova::QueuedSessionPacket{});

	const std::vector<uint32_t> missing =
			opennova::build_session_missing_sequence_list(rx, false);
	// The witnessed walk is bounded by the queue TAIL (the highest queued sequence,
	// 10 here): every hole below it is requested in this pass — the frontier gap
	// (4) and the holes between queued packets (6, 8, 9); queued 5 and 7 are not.
	// [orig: CNapiNPConnection_BuildMissingSeqList @0x6234b0 — tail link read
	//  @0x623503 (conn+0x7A4), the `candidate < tail->seq` bound @0x623527]
	if (!expect(missing == std::vector<uint32_t>({4, 6, 8, 9}),
	            "retail missing list starts at expected and stops at the queue tail"))
		return false;
	const std::vector<uint32_t> forced =
			opennova::build_session_missing_sequence_list(rx, true);
	if (!expect(forced == std::vector<uint32_t>({4, 0, 6, 8, 9}),
	            "forced retail missing list places the zero sentinel after expected"))
		return false;

	std::vector<uint8_t> body;
	if (!expect(opennova::encode_session_resend_list(0x11223344u, forced, body),
	            "encode resend-list plaintext"))
		return false;
	const std::vector<uint8_t> expected_wire = {
		0x44, 0x33, 0x22, 0x11,
		0x04, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00,
		0x06, 0x00, 0x00, 0x00,
		0x08, 0x00, 0x00, 0x00,
		0x09, 0x00, 0x00, 0x00,
	};
	if (!expect(body == expected_wire,
	            "resend-list plaintext is key then LE dwords with no count or terminator"))
		return false;

	std::vector<uint32_t> decoded;
	if (!expect(opennova::decode_session_resend_list(
			body.data(), body.size(), 0x11223344u, decoded) &&
		            decoded == forced,
	            "matching local key decodes every complete resend dword"))
		return false;
	if (!expect(!opennova::decode_session_resend_list(
			body.data(), body.size(), 0x55667788u, decoded),
	            "resend-list key mismatch is rejected"))
		return false;
	const uint8_t short_body[] = {0x44, 0x33, 0x22};
	if (!expect(!opennova::decode_session_resend_list(
			short_body, sizeof(short_body), 0x11223344u, decoded),
	            "resend-list shorter than its key is rejected"))
		return false;
	body.push_back(0xAA);
	body.push_back(0xBB);
	if (!expect(opennova::decode_session_resend_list(
			body.data(), body.size(), 0x11223344u, decoded) &&
		            decoded == forced,
	            "retail ignores one to three trailing resend-list bytes"))
		return false;

	opennova::SessionSequencing capped{1, 10};
	capped.queued_inbound.emplace(40, opennova::QueuedSessionPacket{});
	const std::vector<uint32_t> capped_missing =
			opennova::build_session_missing_sequence_list(capped, false);
	if (!expect(capped_missing.size() == opennova::SESSION_RESEND_LIST_MAX &&
	                    capped_missing.front() == 11 && capped_missing.back() == 26,
	            "retail missing-list builder caps the ascending request list at sixteen"))
		return false;
	return true;
}

bool check_session_retransmit_retention_and_current_ack() {
	const std::string scrk = "RETRANSMIT-SCRK";
	const opennova::SessionCrypto crypto{scrk, {}, 0x11223344u};
	const std::vector<opennova::ProtocolMessage> original_messages = {
		opennova::make_protocol_message(0x49, {0x34, 0x12, 0xC5, 0x00}),
	};
	opennova::SessionSequencing tx{7, 3};
	tx.outbound_message_limit = 16;

	std::vector<uint8_t> original;
	if (!expect(opennova::frame_session_packet(
			tx, crypto, original_messages, original),
	            "first send retains the records assigned to sequence seven"))
		return false;
	tx.last_inbound_seq = 9;
	std::vector<uint8_t> resent;
	if (!expect(opennova::frame_session_packet_for_sequence(
			tx, crypto, 7, resent),
	            "retained packet can be reframed at its original sequence"))
		return false;
	if (!expect(tx.next_outbound_seq == 8,
	            "reframing an old sequence does not advance the outbound counter"))
		return false;

	opennova::ProtocolPacketHeader resent_header;
	std::vector<opennova::ProtocolMessage> resent_messages;
	if (!expect(opennova::decode_protocol_packet_plaintext(
			resent.data(), resent.size(), scrk, resent_header, resent_messages),
	            "decode reframed retained packet"))
		return false;
	if (!expect(resent_header.seq_num == 7 && resent_header.ack_count == 9,
	            "retransmit keeps its sequence but carries the sender's current ACK"))
		return false;
	if (!expect(resent_messages.size() == 1 && resent_messages[0].tag == 0x49 &&
	                    resent_messages[0].payload == original_messages[0].payload,
	            "retransmit reconstructs the original reliable message records"))
		return false;
	if (!expect(resent != original,
	            "current ACK makes the reconstructed retransmit differ from the original datagram body"))
		return false;

	if (!expect(!opennova::frame_session_packet_for_sequence(
			tx, crypto, 9, resent),
	            "a resend request beyond the next outbound sequence is skipped"))
		return false;
	if (!expect(opennova::frame_session_packet_for_sequence(
			tx, crypto, 8, resent),
	            "requesting exactly the next sequence emits a new empty session packet"))
		return false;
	if (!expect(tx.next_outbound_seq == 9,
	            "emitting the requested next sequence advances the outbound counter"))
		return false;
	resent_messages.clear();
	if (!expect(opennova::decode_protocol_packet_plaintext(
			resent.data(), resent.size(), scrk, resent_header, resent_messages) &&
		            resent_header.seq_num == 8 && resent_header.ack_count == 9 &&
		            resent_messages.empty(),
	            "requested next sequence has the current ACK and no retained records"))
		return false;

	opennova::acknowledge_session_packets(tx, 7);
	if (!expect(opennova::frame_session_packet_for_sequence(
			tx, crypto, 7, resent),
	            "retail can still reconstruct an acknowledged old sequence as an empty packet"))
		return false;
	resent_messages.clear();
	if (!expect(opennova::decode_protocol_packet_plaintext(
			resent.data(), resent.size(), scrk, resent_header, resent_messages) &&
		            resent_header.seq_num == 7 && resent_messages.empty(),
	            "admitted peer ACK retires the retained message records"))
		return false;

	opennova::SessionSequencing opt_out{1, 0};
	if (!expect(opennova::frame_session_packet(
			opt_out, crypto, original_messages, original) &&
		            opt_out.retained_outbound.empty(),
	            "sessions that do not opt into retransmission retain no outbound records"))
		return false;

	opennova::SessionSequencing bounded{1, 0};
	bounded.outbound_message_limit = 1;
	if (!expect(opennova::frame_session_packet(
			bounded, crypto, original_messages, original),
	            "bounded retransmission queue admits its first record"))
		return false;
	if (!expect(!opennova::frame_session_packet(
			bounded, crypto, original_messages, original) &&
		            bounded.next_outbound_seq == 2,
	            "witnessed outbound message cap rejects growth before assigning another sequence"))
		return false;
	opennova::acknowledge_session_packets(bounded, 1);
	if (!expect(opennova::frame_session_packet(
			bounded, crypto, original_messages, original) &&
		            bounded.next_outbound_seq == 3,
	            "an admitted ACK frees retention capacity for the next message"))
		return false;

	opennova::ProtocolMessage invalid =
			opennova::make_protocol_message(0x49, std::vector<uint8_t>(256, 0xAA), 0x20);
	opennova::SessionSequencing transactional{20, 0};
	transactional.outbound_message_limit = 4;
	std::vector<uint8_t> invalid_body = {0xCC};
	if (!expect(!opennova::frame_session_packet(
			transactional, crypto, {invalid}, invalid_body) &&
		            transactional.next_outbound_seq == 21 &&
		            transactional.retained_outbound.empty() &&
		            transactional.retained_outbound_message_count == 0 &&
		            invalid_body.empty(),
	            "encode failure consumes the assigned sequence but leaves no phantom retained record"))
		return false;

	// Retail queues reliable message nodes, not whole session datagrams. An
	// unreliable record can share the first physical packet, but must be absent
	// when that packet sequence is reconstructed after a NACK.
	opennova::ProtocolMessage unreliable =
			opennova::make_protocol_message(0x42, {0x00, 0x00});
	unreliable.reliable = false;
	opennova::SessionSequencing mixed_tx{30, 0};
	mixed_tx.outbound_message_limit = 16;
	std::vector<uint8_t> mixed_original;
	if (!expect(opennova::frame_session_packet(
			mixed_tx, crypto, {original_messages[0], unreliable}, mixed_original) &&
		            mixed_tx.retained_outbound_message_count == 1,
	            "mixed packet retains only its reliable message node"))
		return false;
	std::vector<opennova::ProtocolMessage> mixed_first_messages;
	if (!expect(opennova::decode_protocol_packet_plaintext(
			mixed_original.data(), mixed_original.size(), scrk,
			resent_header, mixed_first_messages) &&
		            mixed_first_messages.size() == 2 &&
		            mixed_first_messages[0].tag == original_messages[0].tag &&
		            mixed_first_messages[1].tag == unreliable.tag,
	            "unreliable record remains present on the first physical send"))
		return false;
	std::vector<uint8_t> mixed_resent;
	if (!expect(opennova::frame_session_packet_for_sequence(
			mixed_tx, crypto, 30, mixed_resent),
	            "mixed packet sequence can be reconstructed"))
		return false;
	resent_messages.clear();
	if (!expect(opennova::decode_protocol_packet_plaintext(
			mixed_resent.data(), mixed_resent.size(), scrk,
			resent_header, resent_messages) &&
		            resent_messages.size() == 1 &&
		            resent_messages[0].tag == original_messages[0].tag,
			"NACK retransmit omits the unreliable record from the original packet"))
		return false;

	// userParam=310 is a finite, per-message lifetime measured in OPEN send
	// boundaries. Every packet in one MTU-split flush shares counter C; pruning
	// sees C, then the owner increments once. A record stamped at C survives
	// through C+308 and is removed at C+309, without removing an indefinitely
	// retained sibling assigned to the same sequence.
	opennova::ProtocolMessage finite =
			opennova::make_protocol_message(0x4C, {0x01});
	finite.retention_flushes = 310;
	opennova::SessionSequencing finite_tx{50, 0};
	finite_tx.outbound_message_limit = 16;
	std::vector<uint8_t> finite_original;
	if (!expect(opennova::frame_session_packet(
			finite_tx, crypto, {original_messages[0], finite}, finite_original) &&
		            finite_tx.retained_outbound_message_count == 2 &&
		            finite_tx.retained_outbound.at(50)[1].retention_deadline_flush == 309,
	            "finite node stamps deadline C+310-1 beside a reliable sibling"))
		return false;
	opennova::complete_session_send_flush(finite_tx); // prune C, then C -> C+1
	for (uint32_t i = 0; i < 308; ++i)
		opennova::complete_session_send_flush(finite_tx);
	if (!expect(finite_tx.send_flush_counter == 309 &&
	                    finite_tx.retained_outbound_message_count == 2,
	            "userParam 310 survives through send-flush counter C+308"))
		return false;
	std::vector<uint8_t> finite_before_expiry;
	if (!expect(opennova::frame_session_packet_for_sequence(
			finite_tx, crypto, 50, finite_before_expiry),
	            "finite mixed sequence reconstructs before its deadline"))
		return false;
	resent_messages.clear();
	if (!expect(opennova::decode_protocol_packet_plaintext(
			finite_before_expiry.data(), finite_before_expiry.size(), scrk,
			resent_header, resent_messages) && resent_messages.size() == 2,
	            "pre-expiry replay still includes the finite node"))
		return false;
	opennova::complete_session_send_flush(finite_tx); // prune C+309, then increment
	if (!expect(finite_tx.send_flush_counter == 310 &&
	                    finite_tx.retained_outbound_message_count == 1,
	            "userParam 310 expires exactly at send-flush counter C+309"))
		return false;
	std::vector<uint8_t> finite_after_expiry;
	if (!expect(opennova::frame_session_packet_for_sequence(
			finite_tx, crypto, 50, finite_after_expiry),
	            "mixed sequence remains reconstructable after one node expires"))
		return false;
	resent_messages.clear();
	if (!expect(opennova::decode_protocol_packet_plaintext(
			finite_after_expiry.data(), finite_after_expiry.size(), scrk,
			resent_header, resent_messages) && resent_messages.size() == 1 &&
		            resent_messages[0].tag == original_messages[0].tag,
	            "post-expiry replay preserves only the indefinitely retained sibling"))
		return false;

	opennova::SessionSequencing cap_tx{40, 0};
	cap_tx.outbound_message_limit = 2;
	if (!expect(opennova::frame_session_packet(
			cap_tx, crypto, original_messages, original) &&
		            cap_tx.retained_outbound_message_count == 1,
	            "capacity fixture retains one reliable node"))
		return false;
	if (!expect(!opennova::frame_session_packet(
			cap_tx, crypto, {original_messages[0], unreliable}, original) &&
		            cap_tx.next_outbound_seq == 41 &&
		            cap_tx.retained_outbound_message_count == 1,
	            "msg_out_max counts transient nodes before first-send pruning"))
		return false;
	if (!expect(opennova::frame_session_packet(
			cap_tx, crypto, {unreliable}, original) &&
		            cap_tx.next_outbound_seq == 42 &&
		            cap_tx.retained_outbound_message_count == 1,
	            "one transient node can send at the last slot"))
		return false;
	if (!expect(!opennova::frame_session_packet(
			cap_tx, crypto, {unreliable}, original) &&
		            cap_tx.next_outbound_seq == 42,
	            "an earlier transient packet still occupies its node through the shared boundary"))
		return false;
	opennova::complete_session_send_flush(cap_tx);
	if (!expect(opennova::frame_session_packet(
			cap_tx, crypto, {unreliable}, original) &&
		            cap_tx.next_outbound_seq == 43,
	            "boundary completion prunes transient node capacity"))
		return false;
	return true;
}

// [orig: CNapiNPConnection_BuildOutgoingPackets @0x628430;
// NapiNPMessage_SplitAtLength @0x628350; WriteMessageRecord @0x61DA90]
bool check_split_fills_partial_packet_and_clears_expiry() {
	using namespace opennova;
	size_t cursor = PROTOCOL_DATAGRAM_OVERHEAD;
	const auto prefix = split_protocol_message_to_fill(make_protocol_message(0x60, std::vector<uint8_t>(10, 1)), 48, cursor);
	if (!expect(prefix.size() == 1 && cursor == 31, "first record uses 13 bytes after the 18-byte packet overhead")) return false;
	ProtocolMessage original = make_protocol_message(0x61, std::vector<uint8_t>(20, 2));
	original.reliable = false; original.retention_flushes = 310;
	const auto pieces = split_protocol_message_to_fill(original, 48, cursor);
	if (!expect(pieces.size() == 2 && pieces[0].payload.size() == 14 && pieces[1].payload.size() == 6 && cursor == 27,
			"a message that fits alone splits 14/6 to fill the preceding packet")) return false;
	if (!expect(pieces[0].flags.raw == 0x24 && pieces[1].flags.raw == 0x22,
			"the two short records encode LEN8 FIRST/FINAL flags")) return false;
	for (const auto &piece : pieces) if (!expect(piece.reliable && piece.retention_flushes == 0,
			"split records retain until ACK even when their original was transient or finite")) return false;
	ProtocolReassemblyState state; std::vector<uint8_t> body;
	if (!expect(!reassemble_protocol_payload(state, pieces[0], body) &&
			reassemble_protocol_payload(state, pieces[1], body) && body == original.payload,
			"split-to-fill dispatches one complete original payload")) return false;
	cursor = 1200;
	const auto narrowed = split_protocol_message_to_fill(make_protocol_message(0x62, std::vector<uint8_t>(300, 3)), 1300, cursor);
	if (!expect(narrowed.size() == 2 && narrowed[0].payload.size() == 96 && narrowed[0].flags.len8 && cursor == 225,
			"the original four-byte prefix charge survives when the split head's wire length narrows")) return false;
	std::vector<uint8_t> encoded; append_protocol_message(encoded, narrowed[0]);
	if (!expect(encoded.size() == 99, "LEN16-to-LEN8 narrowing leaves one unused byte at the packet end")) return false;
	cursor = 47;
	const auto deferred = split_protocol_message_to_fill(make_protocol_message(0x63, {1,2,3}), 48, cursor);
	return expect(deferred.size() == 1 && !deferred[0].flags.frag_cont && cursor == 24,
			"without room for a prefix and one byte, the entire record moves to the next packet");
}

int main() {
	bool ok = true;
	ok = check_split_fills_partial_packet_and_clears_expiry() && ok;
	ok = check_session_retransmit_retention_and_current_ack() && ok;
	ok = check_session_resend_list_wire_and_gap_selection() && ok;
	ok = check_session_packet_frame_deframe() && ok;
	ok = check_truncated_message_is_dispatched_zero_padded() && ok;
	ok = check_plaintext_encode_decode_roundtrip() && ok;
	ok = check_custom_flags_encode() && ok;
	ok = check_settings_update_selects_high_table() && ok;
	ok = check_len8_precedes_len16_when_both_bits_set() && ok;
	ok = check_fragment_reassembly_flushes_on_no_cont() && ok;
	ok = check_first_fragment_resets_stale_buffer() && ok;
	ok = check_fragment_reassembly_three_fragments() && ok;
	ok = check_skip_bytes_round_trip() && ok;
	return ok ? 0 : 1;
}
