#include "npruntime/client_runtime.h"

#include <utility>

namespace opennova::np {

ClientRuntime::ClientRuntime(ClientSession::Config config, std::string player_name)
		: role_(Role::Joiner),
		  joiner_(std::make_unique<JoinerConnection>(std::move(config), std::move(player_name))) {}

ClientRuntime::ClientRuntime(netsim::ISessionTransport &host_loopback)
		: role_(Role::HostClient), loopback_(&host_loopback) {}

std::vector<uint8_t> ClientRuntime::start() {
	if (role_ != Role::Joiner) return {};
	return joiner_->start();
}

void ClientRuntime::receive(const uint8_t *raw, std::size_t len) {
	if (role_ != Role::Joiner || raw == nullptr || len == 0) return;
	recv_fifo_.emplace_back(raw, raw + len);
}

void ClientRuntime::seed_session(uint32_t session_id, std::string client_scrk,
                                 std::string server_scrk, uint32_t next_seq, uint32_t last_ack,
                                 uint16_t self_handle, uint16_t self_type) {
	if (role_ != Role::Joiner) return;
	joiner_->seed_in_match(session_id, std::move(client_scrk), std::move(server_scrk), next_seq,
	                       last_ack, self_handle, self_type);
	deployed_ = true; // a seeded replay is post-deploy (the captured client was uplinking)
}

std::vector<std::vector<uint8_t>> ClientRuntime::run_frame(const PlayerExtendedUplink *uplink,
                                                           uint32_t now_tick) {
	std::vector<std::vector<uint8_t>> outbound;

	// (1) RECV pump — fold S2C into ClientState, recv-before-send [orig: PumpClientProtocolRecv
	// @0x42c228 runs before the SEND block].
	if (role_ == Role::HostClient) {
		// The SP host's own loopback carries inner {tag,body} (ADR 0011 §3 SP crypto bypass).
		if (loopback_ != nullptr) view_.pump(*loopback_);
	} else {
		// A remote joiner: framed datagrams. JoinerConnection decodes the 0x83 SESSION envelope and
		// surfaces the inner bodies, which we fold via NetClientView::apply (the single remote-wire
		// fold path; pump(transport) is the loopback path — exactly one is active per role).
		while (!recv_fifo_.empty()) {
			std::vector<uint8_t> dg = std::move(recv_fifo_.front());
			recv_fifo_.pop_front();
			JoinerConnection::PollResult pr = joiner_->handle_datagram(dg.data(), dg.size());
			for (std::vector<uint8_t> &reply : pr.outbound) outbound.push_back(std::move(reply));
			for (const auto &tb : pr.inbound_world) view_.apply(tb.first, tb.second); // 0x0C/0x0D/0x10/0x20
			for (const std::vector<uint8_t> &a : pr.inbound_0a) view_.apply(0x0A, a);  // per-frame 0x0A
			if (pr.reached_in_match) deployed_ = true; // deploy on the spawn name-match (§5.44 gate)
		}
	}

	if (role_ == Role::HostClient) return outbound; // host: no connect-drive, no 0x0C uplink

	// (1b) CONNECT-DRIVE — while Driving, advance the in-match spawn-gate burst one stage per frame
	// [the original drives the burst from the same per-frame pump loop, via NapiClient_WaitForGameStart].
	if (joiner_->phase() == JoinerConnection::Phase::Driving) {
		for (std::vector<uint8_t> &d : joiner_->pump(now_tick)) outbound.push_back(std::move(d));
	}

	// (2) SEND — the C2S 0x0C player uplink, gated InMatch && deployed (the witnessed
	// is_in_session && !is_authority && !dword_81474C && !g_spawn_success_gate; a Joiner is always
	// !is_authority). The host (HostClient) returned above without ever reaching here.
	if (uplink != nullptr && joiner_->in_match() && deployed_ && joiner_->has_self_handle()) {
		outbound.push_back(joiner_->frame_c2s_uplink(joiner_->self_handle(),
		                                             joiner_->spawn_pose().item_type_id, *uplink));
	}
	return outbound;
}

std::vector<std::vector<uint8_t>>
ClientRuntime::Client_ProcessNetworkFrame(const PlayerExtendedUplink &uplink, uint32_t now_tick) {
	return run_frame(&uplink, now_tick);
}

std::vector<std::vector<uint8_t>> ClientRuntime::Client_ProcessNetworkFrame(uint32_t now_tick) {
	return run_frame(nullptr, now_tick);
}

} // namespace opennova::np
