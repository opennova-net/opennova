#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include <net/npwire/protocol_message.h> // ProtocolMessage, SessionSequencing

namespace opennova {

// What a connection does with a node NapiNPMessage_Create refuses because the msg_out_max pool
// is full. Create latches the MSGCRE record and requests the disconnect either way; what differs
// is where RequestDisconnect lands. A client-side connection (state 5) tears down INLINE, so
// nothing queued in that boundary is built. A server-side connection only marks
// pending_disconnect for the next protocol pump, so every refused non-exempt node is dropped and
// the rest of the boundary still ships.
// [orig: NapiNPMessage_Create @0x627FC0 — the latch @0x6280f9..0x62810a, RequestDisconnect
//  @0x628112 -> CNapiNPConnection_RequestDisconnect @0x61e0f0 (state 5: SetState(6) @0x61e0fa;
//  state 1: pending_disconnect @0x61e107)]
enum class OutgoingOverflow {
	StopBuild,
	DropRefused,
};

// One send boundary's packets, planned but not framed. The owner frames each packet in order
// (frame_session_packet) and applies its own overflow consequence.
struct OutgoingPacketPlan {
	// Each packet's records, in build order; every packet fits the ceiling.
	std::vector<std::vector<ProtocolMessage>> packets;
	// Per packet: how many queue messages it carries the final record of.
	std::vector<std::size_t> completed_messages;
	// What stays queued for the next build: the cs_dir0 field-14 budget's remainder (a split
	// message's remaining pieces first), or the node that failed to encode and everything after it.
	std::vector<ProtocolMessage> unbuilt;
	// Queue messages whose every node passed admission.
	std::size_t admitted_messages = 0;
	// A non-exempt node did not fit the pool. `overflow_message` is the queue index of the message
	// it belongs to; `overflow_count` is the count the MSGCRE record carries (the nodes already
	// out + those admitted ahead of it + 1).
	bool overflow = false;
	std::size_t overflow_message = 0;
	uint32_t overflow_count = 0;
	bool encode_failed = false;
};

// Plan one BuildOutgoingPackets pass over a connection's queue: admit each node against the
// connection's msg_out_max pool in queue order (a SplitAtLength piece is a node too; a
// capacity-exempt node bypasses the pool), split each message at the current packet's remaining
// space (split_protocol_message_to_fill), and pack the records into packets no larger than
// `max_packet_bytes` (the cs_dir0 field-13 ceiling, already floored at 26), at most `max_packets`
// of them (field 14). Under StopBuild the first refused node ends the plan with nothing built;
// under DropRefused refused nodes are dropped and the rest is planned. `max_packets` 0 leaves the
// whole queue unbuilt.
// [orig: CNapiNPConnection_BuildOutgoingPackets @0x628430 — the ceiling @0x628436, the per-message
//  fit and split @0x6284c0..0x6284f5, the packet loop @0x62860b..0x628619;
//  NapiNPMessage_SplitAtLength @0x628350; NapiNPMessage_Create @0x627FC0 — the flag-0x10
//  exemption @0x628031, `msg_out_max >= 0` @0x628048, the count @0x628062..0x62806b]
OutgoingPacketPlan plan_outgoing_packets(const SessionSequencing &seq,
		const std::vector<ProtocolMessage> &queue, std::size_t max_packet_bytes,
		std::size_t max_packets, OutgoingOverflow policy);

} // namespace opennova
