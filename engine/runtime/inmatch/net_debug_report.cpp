#include <runtime/inmatch/net_debug_report.h>

#include <runtime/inmatch/client_runtime.h>
#include <runtime/inmatch/joiner_role.h>
#include <runtime/inmatch/napi_np_connection.h>
#include <runtime/inmatch/napi_np_server_ctx.h>

#include <net/npwire/peer_addr.h>

namespace opennova::inmatch {

const char *connection_phase_name(int32_t phase) {
	switch (static_cast<ConnectionPhase>(phase)) {
		case ConnectionPhase::New:
			return "new";
		case ConnectionPhase::ValidateJoin:
			return "validate join";
		case ConnectionPhase::Joined:
			return "joined";
		case ConnectionPhase::NewConnection:
			return "new connection";
		case ConnectionPhase::InitRound:
			return "init round";
		case ConnectionPhase::PendingSpawn:
			return "pending spawn";
		case ConnectionPhase::PlayerAdded:
			return "player added";
		case ConnectionPhase::SendingInitial:
			return "sending initial";
		case ConnectionPhase::Spawned:
			return "spawned";
		case ConnectionPhase::InMatch:
			return "in match";
		case ConnectionPhase::Goodbye:
			return "goodbye";
	}
	return "?";
}

JoinerNetworkDiagnostics joiner_network_diagnostics(const ClientRuntime *runtime, const JoinerRole *role) {
	JoinerNetworkDiagnostics out;
	if (role != nullptr) {
		out.flat_seconds = role->flat_seconds();
		out.freeze_suspected = role->freeze_suspected();
	}
	if (runtime == nullptr) return out;
	out.present = true;
	out.frontier_seq = runtime->inbound_frontier_seq();
	out.outbound_seq = runtime->outbound_seq();
	out.records_applied = runtime->state().compact_records_applied;
	out.gap_depth = static_cast<uint32_t>(runtime->inbound_gap_depth());
	out.retained_outbound = static_cast<uint32_t>(runtime->retained_outbound_depth());
	out.in_match = runtime->in_match();
	out.deployed = runtime->is_deployed();
	out.stage = runtime->admission_stage_name();
	const JoinerConnection::ChallengeDiagnostics challenges = runtime->challenge_diagnostics();
	out.entity_checksum_seen = challenges.entity_checksum_seen;
	out.entity_checksum_answered = challenges.entity_checksum_answered;
	out.loadout_crc_seen = challenges.loadout_crc_seen;
	out.loadout_crc_answered = challenges.loadout_crc_answered;
	out.charattr_seen = challenges.charattr_seen;
	out.charattr_row_missing = challenges.charattr_row_missing;
	out.property_clears = challenges.property_clears;
	const JoinerConnection::JoinRejectRecord reject = runtime->last_join_reject();
	if (reject.set) {
		out.reject_set = true;
		out.reject_jfc = reject.jfc;
		out.reject_jfp = reject.jfp;
		out.reject_jfs = reject.jfs;
	}
	if (runtime->has_disconnect_event()) {
		const DisconnectEvent event = runtime->last_disconnect_event();
		out.disconnect_set = true;
		out.disconnect_dc = event.dc;
		out.disconnect_dpc = event.dpc;
		out.disconnect_ddstr = event.ddstr;
		out.disconnect_dstr = event.dstr;
	}
	out.ping_ms = runtime->client_ping_ms();
	out.average_ping_ms = runtime->client_average_ping_ms();
	out.session_ping_ms = runtime->session_ping_ms();
	out.quality = runtime->net_quality_level();
	out.send_holdoff_ticks = runtime->send_holdoff_ticks();
	out.send_holdoff_countdown = runtime->send_holdoff_countdown();
	return out;
}

NetDebugReport net_debug_report(const Session &session, const NapiNPServerCtx *server,
		const ClientRuntime *runtime, const JoinerRole *joiner, const CountingDatagramSocket *socket) {
	NetDebugReport out;
	out.role = session.kind();
	out.state = session.state();
	out.last_perf = session.last_perf();
	if (socket != nullptr) {
		out.traffic_valid = true;
		out.traffic = socket->totals();
	}
	if (server != nullptr) {
		for (const NapiNPConnection &c : server->np_protocol.connection_list) {
			if (c.type != NapiNPConnection::kTypeServerSide) continue;
			NetPeerRow row;
			row.slot = static_cast<int32_t>(c.reply.player_slot);
			row.name = c.player_name;
			row.address = peer_addr_to_string(c.peer);
			row.phase = static_cast<int32_t>(c.phase);
			row.rtt_ms = c.reply.rtt_ms;
			// The ten-sample ring (slot 10 is the cursor): the mean of the
			// samples recorded so far.
			uint64_t sum = 0;
			uint32_t filled = 0;
			for (size_t i = 0; i < 10; ++i) {
				if (c.reply.rtt_ring[i] != 0) {
					sum += c.reply.rtt_ring[i];
					++filled;
				}
			}
			row.rtt_average_ms = filled > 0 ? static_cast<uint32_t>(sum / filled) : 0;
			row.session_ping_ms = c.session_ping_rtt_ms;
			row.quality = c.reply.client_quality;
			row.min_ping_strikes = c.reply.min_ping_strikes;
			row.max_ping_strikes = c.reply.max_ping_strikes;
			row.send_holdoff_ticks = c.s2c_send_holdoff_ticks;
			row.send_holdoff_countdown = c.s2c_send_holdoff_countdown;
			row.receive_inactive_ms = c.receive_inactive_ms;
			if (socket != nullptr) {
				if (const DatagramTraffic *traffic = socket->peer(c.peer)) {
					row.traffic_valid = true;
					row.traffic = *traffic;
				}
			}
			out.peers.push_back(std::move(row));
		}
	}
	if (session.kind() == RoleKind::Joiner) {
		out.joiner = joiner_network_diagnostics(runtime, joiner);
	}
	return out;
}

}  // namespace opennova::inmatch
