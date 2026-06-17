#include <novaworld/protocol_message.h>

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

int main() {
	bool ok = true;
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
