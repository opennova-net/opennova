// inmatch::net_debug_report — the in-match network's live state for the F3
// Net window (ADR 0039 dev tooling; ADR 0042 d5, the one engine function per
// fact): the session's role, state and tick banking, the listen server's
// per-connection health (round trip, the client's reported quality, ping
// strikes, the send holdoff), the joiner's own view (its pings, quality,
// sequence frontier, admission stage, the CRC challenges, the last reject or
// disconnect), and the datagram traffic the counting socket saw. The retail
// engine's network debug screen showed the frame rate, the CPU load and the
// local/remote quantum size [orig: Network_DrawDebugScreen @0x500EF0]; this
// report carries the facts behind those lines and the port's own.
#pragma once

#include <runtime/inmatch/session.h>
#include <net/npwire/counting_datagram_socket.h>

#include <cstdint>
#include <string>
#include <vector>

namespace opennova::inmatch {

class ClientRuntime;
class JoinerRole;
struct NapiNPServerCtx;

// The joiner's diagnostics (the MCP game_debug net_joiner_diagnostics trace
// and the Net window's joiner block).
struct JoinerNetworkDiagnostics {
	bool present = false;
	uint32_t frontier_seq = 0;
	uint32_t outbound_seq = 0;
	uint64_t records_applied = 0;
	uint32_t gap_depth = 0;
	uint32_t retained_outbound = 0;
	int32_t flat_seconds = 0;
	bool freeze_suspected = false;
	bool in_match = false;
	bool deployed = false;
	std::string stage;
	// The CRC / attribute challenges the server issued and the answers.
	uint32_t entity_checksum_seen = 0;
	uint32_t entity_checksum_answered = 0;
	uint32_t loadout_crc_seen = 0;
	uint32_t loadout_crc_answered = 0;
	uint32_t charattr_seen = 0;
	uint32_t charattr_row_missing = 0;
	uint32_t property_clears = 0;
	bool reject_set = false;
	int64_t reject_jfc = 0;
	int64_t reject_jfp = 0;
	std::string reject_jfs;
	bool disconnect_set = false;
	int64_t disconnect_dc = 0;
	int64_t disconnect_dpc = 0;
	std::string disconnect_ddstr;
	std::string disconnect_dstr;
	// The client's own link readings.
	uint32_t ping_ms = 0;
	uint32_t average_ping_ms = 0;
	uint32_t session_ping_ms = 0;
	uint8_t quality = 0; // 0..4, the C2S 0x4C report
	uint32_t send_holdoff_ticks = 0;
	uint32_t send_holdoff_countdown = 0;
};

JoinerNetworkDiagnostics joiner_network_diagnostics(const ClientRuntime *runtime, const JoinerRole *role);

// One listen-server connection as the Net window lists it.
struct NetPeerRow {
	int32_t slot = -1;
	std::string name;
	std::string address;
	int32_t phase = 0; // ConnectionPhase
	uint32_t rtt_ms = 0;
	uint32_t rtt_average_ms = 0; // the ten-sample ring's mean over its filled entries
	uint32_t session_ping_ms = 0;
	uint8_t quality = 0;
	uint16_t min_ping_strikes = 0;
	uint16_t max_ping_strikes = 0;
	uint32_t send_holdoff_ticks = 0;
	uint32_t send_holdoff_countdown = 0;
	uint32_t receive_inactive_ms = 0;
	bool traffic_valid = false;
	DatagramTraffic traffic{};
};

struct NetDebugReport {
	RoleKind role = RoleKind::SinglePlayer;
	State state = State::Unloaded;
	world::TickBankPolicy bank_policy = world::TickBankPolicy::WallClock;
	FramePerf last_perf{};
	std::vector<NetPeerRow> peers;
	JoinerNetworkDiagnostics joiner;
	bool traffic_valid = false;
	DatagramTraffic traffic{};
};

// Any argument may be null: a single-player session carries no server
// context, no joiner and no socket.
NetDebugReport net_debug_report(const Session &session, const NapiNPServerCtx *server,
		const ClientRuntime *runtime, const JoinerRole *joiner, const CountingDatagramSocket *socket);

const char *connection_phase_name(int32_t phase);

}  // namespace opennova::inmatch
