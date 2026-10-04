#include <net/npwire/outgoing_packets.h>

#include <limits>
#include <utility>

namespace opennova {

OutgoingPacketPlan plan_outgoing_packets(const SessionSequencing &seq,
		const std::vector<ProtocolMessage> &queue, std::size_t max_packet_bytes,
		std::size_t max_packets, OutgoingOverflow policy) {
	OutgoingPacketPlan plan;
	if (max_packets == 0) {
		plan.unbuilt = queue;
		return plan;
	}

	// Admission, per physical node in queue order: every record (a whole message or one split
	// piece) is admitted while `retained + transient + admitted-so-far + 1 <= msg_out_max`; a
	// capacity-exempt node bypasses it. [orig: NapiNPMessage_Create @0x627FC0 — the flag-0x10
	//  exemption @0x628031, `msg_out_max >= 0` @0x628048, the count @0x628062..0x62806b;
	//  SplitAtLength @0x62838f pieces go through Create too]
	std::size_t available = session_outbound_message_prefix_count(
			seq, std::numeric_limits<std::size_t>::max());
	const std::size_t occupied =
			seq.retained_outbound_message_count + seq.transient_outbound_message_count;
	std::size_t admitted_nodes = 0;
	std::size_t planned_packet_bytes = PROTOCOL_DATAGRAM_OVERHEAD;
	std::vector<std::vector<ProtocolMessage>> planned;
	planned.reserve(queue.size());
	for (std::size_t m = 0; m < queue.size(); ++m) {
		std::size_t message_packet_bytes = planned_packet_bytes;
		std::vector<ProtocolMessage> pieces = split_protocol_message_to_fill(
				queue[m], max_packet_bytes, message_packet_bytes);
		std::vector<ProtocolMessage> kept;
		kept.reserve(pieces.size());
		bool refused = false;
		for (ProtocolMessage &piece : pieces) {
			if (!piece.capacity_exempt) {
				if (plan.overflow || available == 0) {
					if (!plan.overflow) {
						plan.overflow = true;
						plan.overflow_message = m;
						plan.overflow_count =
								static_cast<uint32_t>(occupied + admitted_nodes + 1);
					}
					if (policy == OutgoingOverflow::StopBuild) {
						plan.packets.clear();
						plan.completed_messages.clear();
						return plan;
					}
					refused = true;
					continue; // the node is dropped
				}
				--available;
				++admitted_nodes;
			}
			kept.push_back(std::move(piece));
		}
		// The fill planner only advances over nodes that were actually queued.
		if (!refused) {
			planned_packet_bytes = message_packet_bytes;
			++plan.admitted_messages;
		}
		planned.push_back(std::move(kept));
	}

	// Packing: each record joins the open packet while it fits, else the packet is closed; the
	// build stops once its packet budget went out and the rest waits for the next build.
	// [orig: CNapiNPConnection_BuildOutgoingPackets @0x628430 — the fit @0x6284c0..0x6284d3, the
	//  packet loop @0x62860b..0x628619]
	std::vector<ProtocolMessage> packet;
	std::size_t packet_bytes = PROTOCOL_DATAGRAM_OVERHEAD;
	std::size_t completed_in_packet = 0;
	const auto close_packet = [&] {
		if (packet.empty()) return;
		plan.packets.push_back(std::move(packet));
		plan.completed_messages.push_back(completed_in_packet);
		packet.clear();
		completed_in_packet = 0;
		packet_bytes = PROTOCOL_DATAGRAM_OVERHEAD;
	};
	const auto leave_rest = [&](std::size_t message, std::size_t piece) {
		for (std::size_t j = piece; j < planned[message].size(); ++j)
			plan.unbuilt.push_back(std::move(planned[message][j]));
		for (std::size_t later = message + 1; later < planned.size(); ++later)
			for (ProtocolMessage &rest : planned[later]) plan.unbuilt.push_back(std::move(rest));
	};
	for (std::size_t m = 0; m < planned.size(); ++m) {
		std::vector<ProtocolMessage> &pieces = planned[m];
		for (std::size_t i = 0; i < pieces.size(); ++i) {
			std::vector<uint8_t> encoded;
			if (!append_protocol_message(encoded, pieces[i])) {
				close_packet();
				plan.encode_failed = true;
				leave_rest(m, i);
				return plan;
			}
			if (packet_bytes + encoded.size() > max_packet_bytes) {
				const bool closed = !packet.empty();
				close_packet();
				if (closed && plan.packets.size() >= max_packets) {
					leave_rest(m, i);
					return plan;
				}
			}
			packet.push_back(std::move(pieces[i]));
			packet_bytes += encoded.size();
			if (i + 1 == pieces.size()) ++completed_in_packet;
		}
	}
	close_packet();
	return plan;
}

} // namespace opennova
