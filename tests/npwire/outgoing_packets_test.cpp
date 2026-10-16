// The shared BuildOutgoingPackets planner (net/npwire/outgoing_packets.h) and the CS block it
// reads (net/npwire/cs_config.h): a large statement splits into FIRST/MID/FINAL records that each
// fit the field-13 ceiling, small records share a packet, the field-14 budget leaves the rest
// queued, and a full msg_out_max pool either stops the build (client) or drops the refused nodes
// (server). The receive side's reassembly bound (ProtocolReassemblyState::max_bytes, which the
// NovaWorld service's lobby connections set) drops a stream that would pass it.
// [orig: CNapiNPConnection_BuildOutgoingPackets @0x628430; NapiNPMessage_SplitAtLength @0x628350;
//  NapiNPMessage_Create @0x627FC0; CNapiGameSession_InitNPConnection @0x4d3e1f;
//  CNapiNetwork_Init @0x4ca4a0]

#include <net/npwire/cs_config.h>
#include <net/npwire/outgoing_packets.h>
#include <net/npwire/protocol_message.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

using namespace opennova;

int g_failures = 0;

void expect(bool condition, const char *message) {
	if (condition) return;
	std::fprintf(stderr, "FAIL: %s\n", message);
	++g_failures;
}

ProtocolMessage lobby_record(size_t size, uint8_t fill) {
	return make_protocol_message(0, std::vector<uint8_t>(size, fill));
}

size_t packet_bytes(const std::vector<ProtocolMessage> &packet) {
	std::vector<uint8_t> encoded;
	for (const ProtocolMessage &record : packet) append_protocol_message(encoded, record);
	return PROTOCOL_DATAGRAM_OVERHEAD + encoded.size();
}

void check_large_statement_splits_under_the_ceiling() {
	SessionSequencing seq;
	std::vector<uint8_t> payload(4300);
	for (size_t i = 0; i < payload.size(); ++i) payload[i] = static_cast<uint8_t>(i * 7);
	const OutgoingPacketPlan plan = plan_outgoing_packets(seq,
			{make_protocol_message(0, payload)}, 1300, max_packets_per_build(-1),
			OutgoingOverflow::StopBuild);
	expect(!plan.overflow && !plan.encode_failed && plan.unbuilt.empty(), "a 4300-byte statement plans cleanly");
	expect(plan.packets.size() == 4, "a 4300-byte statement takes four 1300-byte packets");
	ProtocolReassemblyState state;
	std::vector<uint8_t> assembled;
	bool complete = false;
	for (size_t p = 0; p < plan.packets.size(); ++p) {
		expect(plan.packets[p].size() == 1, "each packet carries one record of the statement");
		expect(packet_bytes(plan.packets[p]) <= 1300, "every datagram fits the field-13 ceiling");
		const ProtocolMessage &piece = plan.packets[p][0];
		const bool first = p == 0;
		const bool last = p + 1 == plan.packets.size();
		expect(piece.flags.frag_cont == !last && piece.flags.frag_end == !first,
				"FIRST 0x04, MID 0x06, FINAL 0x02");
		expect(piece.reliable, "every piece retains until ACK");
		complete = reassemble_protocol_payload(state, piece, assembled);
		expect(complete == last, "only the FINAL record completes the payload");
	}
	expect(complete && assembled == payload, "the pieces reassemble the statement byte for byte");
	expect(plan.completed_messages == std::vector<size_t>({0, 0, 0, 1}),
			"only the packet with the FINAL record completes the message");
	expect(packet_bytes(plan.packets[0]) == 1300, "the FIRST record fills its packet exactly");
}

void check_small_records_share_a_packet() {
	SessionSequencing seq;
	const OutgoingPacketPlan plan = plan_outgoing_packets(seq,
			{lobby_record(100, 1), lobby_record(200, 2), lobby_record(300, 3)}, 1300,
			max_packets_per_build(-1), OutgoingOverflow::StopBuild);
	expect(plan.packets.size() == 1 && plan.packets[0].size() == 3, "three small records share one packet");
	expect(plan.completed_messages == std::vector<size_t>({3}) && plan.admitted_messages == 3,
			"all three complete and are admitted");
}

void check_budget_leaves_the_rest_queued() {
	SessionSequencing seq;
	const OutgoingPacketPlan plan = plan_outgoing_packets(seq,
			{lobby_record(3000, 4), lobby_record(10, 5)}, 1300, max_packets_per_build(1),
			OutgoingOverflow::StopBuild);
	expect(plan.packets.size() == 1, "a field-14 budget of one builds one packet");
	expect(plan.unbuilt.size() == 3 && plan.unbuilt[0].flags.frag_end && plan.unbuilt[0].flags.frag_cont,
			"the split statement's MID heads the unbuilt queue");
	expect(!plan.unbuilt[1].flags.frag_cont && plan.unbuilt[1].flags.frag_end,
			"then its FINAL record");
	expect(plan.unbuilt[2].payload.size() == 10, "then the next queued statement");
	const OutgoingPacketPlan none = plan_outgoing_packets(seq, {lobby_record(10, 6)}, 1300, 0,
			OutgoingOverflow::DropRefused);
	expect(none.packets.empty() && none.unbuilt.size() == 1, "a zero budget leaves the whole queue");
}

void check_overflow_policies() {
	SessionSequencing seq;
	seq.outbound_message_limit = 2;
	const std::vector<ProtocolMessage> queue = {lobby_record(10, 1), lobby_record(10, 2),
	                                            lobby_record(10, 3)};
	const OutgoingPacketPlan client = plan_outgoing_packets(seq, queue, 1300,
			max_packets_per_build(-1), OutgoingOverflow::StopBuild);
	expect(client.overflow && client.overflow_message == 2 && client.overflow_count == 3,
			"the third node overflows a two-node pool (count = 0 out + 2 admitted + 1)");
	expect(client.packets.empty() && client.admitted_messages == 2,
			"a client-side overflow builds nothing this boundary");

	std::vector<ProtocolMessage> with_exempt = queue;
	with_exempt.push_back(lobby_record(10, 4));
	with_exempt.back().capacity_exempt = true;
	const OutgoingPacketPlan server = plan_outgoing_packets(seq, with_exempt, 1300,
			max_packets_per_build(-1), OutgoingOverflow::DropRefused);
	expect(server.overflow && server.overflow_message == 2 && server.overflow_count == 3,
			"the server plan records the same first overflow");
	expect(server.packets.size() == 1 && server.packets[0].size() == 3,
			"the refused node is dropped while the admitted and exempt ones ship");
	expect(server.packets[0][2].capacity_exempt, "the exempt record bypasses the full pool");
}

void check_cs_templates() {
	const CsConfig lobby = novaworld_service_cs_config();
	expect(lobby.timeout_ms == 240000 && lobby.recv_max_per_tick == 4 &&
	               lobby.idle_send_interval_ms == 60000 && lobby.active_send_interval_ms == 1000 &&
	               lobby.packet_queue_interval_ms == -1 && lobby.packet_queue_max == 100 &&
	               lobby.msg_out_max == 500 && lobby.max_packet_bytes == 1300 &&
	               lobby.max_packets_per_tick == -1,
			"the NOVAWORLDUDP template matches InitNPConnection @0x4d3e1f");
	const CsConfig game = jointoperations_cs_config();
	const CsConfig defaults;
	expect(game.timeout_ms == defaults.timeout_ms && game.idle_send_interval_ms == defaults.idle_send_interval_ms &&
	               game.active_send_interval_ms == defaults.active_send_interval_ms &&
	               game.packet_queue_interval_ms == defaults.packet_queue_interval_ms &&
	               game.packet_queue_max == defaults.packet_queue_max &&
	               game.msg_out_max == defaults.msg_out_max &&
	               game.max_packet_bytes == defaults.max_packet_bytes &&
	               game.max_packets_per_tick == defaults.max_packets_per_tick,
			"the CsConfig defaults are the JOINTOPERATIONS template");
	expect(novaworld_service_cs_config(50).max_packet_bytes == 100 &&
	               novaworld_service_cs_config(0x20000).max_packet_bytes == 0x10000,
			"field 13 runs the service template's clamp ladder");
	CsConfig overlaid = lobby;
	apply_cs_field(overlaid, 13, 600);
	apply_cs_field(overlaid, 6, 2000);
	expect(overlaid.max_packet_bytes == 600 && overlaid.packet_queue_interval_ms == 2000,
			"an overlay stores slots 6 and 13");
	expect(packet_ceiling_bytes(10) == 26 && packet_ceiling_bytes(1300) == 1300,
			"the ceiling is floored at 26");
}

ProtocolMessage fragment(uint8_t frag_bits, size_t size, uint8_t fill) {
	return make_protocol_message(0, std::vector<uint8_t>(size, fill),
			static_cast<uint8_t>(frag_bits | PROTOCOL_MSG_FLAG_LEN16));
}

// The reassembly bound a NovaWorld service lobby connection sets
// (ProtocolReassemblyState::max_bytes; service policy, retail's buffer has none): a
// stream exactly at it reassembles; the fragment that would carry one past it drops
// the stream, its buffer freed, and every later fragment of it is dropped through
// the FINAL, which dispatches nothing; an unfragmented record between still
// dispatches; the next stream reassembles, and a FIRST record mid-drop starts one.
// The hostile shape, MID records without end, never grows the buffer past the
// lobby's 64 KiB. Unbounded (0, the default) keeps retail's growth.
void check_reassembly_bound() {
	const uint8_t first = PROTOCOL_MSG_FLAG_FRAG_CONT;
	const uint8_t mid = PROTOCOL_MSG_FLAG_FRAG_CONT | PROTOCOL_MSG_FLAG_FRAG_END;
	const uint8_t final_piece = PROTOCOL_MSG_FLAG_FRAG_END;
	std::vector<uint8_t> out;
	bool dropped = false;

	ProtocolReassemblyState state{{}, 3000};
	expect(!reassemble_protocol_payload(state, fragment(first, 1000, 1), out, nullptr, &dropped) &&
			!dropped, "a FIRST under the bound buffers");
	expect(!reassemble_protocol_payload(state, fragment(mid, 1000, 2), out, nullptr, &dropped) &&
			!dropped, "a MID under the bound buffers");
	expect(reassemble_protocol_payload(state, fragment(final_piece, 1000, 3), out, nullptr, &dropped) &&
			!dropped && out.size() == 3000, "a stream exactly at the bound reassembles");

	expect(!reassemble_protocol_payload(state, fragment(first, 2000, 4), out, nullptr, &dropped) &&
			!dropped, "the next FIRST buffers");
	expect(!reassemble_protocol_payload(state, fragment(mid, 1001, 5), out, nullptr, &dropped) &&
			dropped, "the MID that would pass the bound is dropped");
	expect(state.buffer.empty() && state.buffer.capacity() == 0 && state.discarding,
			"the dropped stream's buffer is freed");
	expect(!reassemble_protocol_payload(state, fragment(mid, 10, 6), out, nullptr, &dropped) &&
			dropped && state.buffer.empty(), "a later MID of the dropped stream is dropped");
	expect(reassemble_protocol_payload(state, fragment(0, 50, 7), out, nullptr, &dropped) &&
			!dropped && out.size() == 50, "an unfragmented record still dispatches");
	expect(!reassemble_protocol_payload(state, fragment(final_piece, 10, 8), out, nullptr, &dropped) &&
			dropped && out.empty(), "the dropped stream's FINAL dispatches nothing");
	expect(!state.discarding, "the FINAL ends the drop");

	expect(!reassemble_protocol_payload(state, fragment(first, 100, 9), out) &&
			reassemble_protocol_payload(state, fragment(final_piece, 100, 10), out) &&
			out.size() == 200, "the next stream reassembles");

	reassemble_protocol_payload(state, fragment(first, 2999, 11), out);
	expect(!reassemble_protocol_payload(state, fragment(mid, 2, 12), out, nullptr, &dropped) &&
			dropped, "a MID past the bound starts a drop");
	expect(!reassemble_protocol_payload(state, fragment(first, 5, 13), out, nullptr, &dropped) &&
			!dropped && !state.discarding, "a FIRST mid-drop starts a new stream");
	expect(reassemble_protocol_payload(state, fragment(final_piece, 5, 14), out) &&
			out == std::vector<uint8_t>({13, 13, 13, 13, 13, 14, 14, 14, 14, 14}),
			"the new stream reassembles alone");

	expect(!reassemble_protocol_payload(state, fragment(first, 3001, 15), out, nullptr, &dropped) &&
			dropped && state.discarding, "a FIRST past the bound is dropped");
	expect(!reassemble_protocol_payload(state, fragment(final_piece, 1, 16), out, nullptr, &dropped) &&
			dropped && !state.discarding, "its FINAL ends the drop");

	ProtocolReassemblyState lobby{{}, 64 * 1024};
	size_t peak = 0;
	int drops = 0;
	for (int i = 0; i < 100; ++i) {
		reassemble_protocol_payload(lobby, fragment(mid, 1000, 0x41), out, nullptr, &dropped);
		peak = std::max(peak, lobby.buffer.size());
		drops += dropped ? 1 : 0;
	}
	expect(peak == 65000 && drops == 35,
			"MID records past 64 KiB are dropped and the buffer never passes it");
	expect(!reassemble_protocol_payload(lobby, fragment(final_piece, 3, 0x03), out, nullptr, &dropped) &&
			dropped && out.empty(), "the oversized stream's FINAL dispatches nothing");

	ProtocolReassemblyState unbounded;
	for (int i = 0; i < 100; ++i) reassemble_protocol_payload(unbounded, fragment(mid, 1000, 0x41), out);
	expect(unbounded.buffer.size() == 100000, "with no bound the buffer takes every fragment");
	expect(reassemble_protocol_payload(unbounded, fragment(final_piece, 3, 0x03), out) &&
			out.size() == 100003, "and its FINAL dispatches the whole stream");
}

} // namespace

int main() {
	check_large_statement_splits_under_the_ceiling();
	check_small_records_share_a_packet();
	check_budget_leaves_the_rest_queued();
	check_overflow_policies();
	check_cs_templates();
	check_reassembly_bound();
	if (g_failures != 0) {
		std::fprintf(stderr, "%d failure(s)\n", g_failures);
		return 1;
	}
	std::printf("npwire_outgoing_packets: OK\n");
	return 0;
}
