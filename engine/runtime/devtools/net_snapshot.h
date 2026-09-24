// The Net window's record (ADR 0042 d6: records in): the in-match network as
// inmatch::net_debug_report reads it, copied field by field by the embedder
// into this plain devtools struct — the devtools group is net-agnostic
// (include_graph rule 1b), so it never names an inmatch type. Roles and
// states arrive as the Game window's StatusRole / StatusState mirrors.
#pragma once

#include <runtime/devtools/game_status_snapshot.h>

#include <cstdint>
#include <string>
#include <vector>

namespace opennova::devtools {

struct NetTraffic {
	uint64_t tx_packets = 0;
	uint64_t tx_bytes = 0;
	uint64_t rx_packets = 0;
	uint64_t rx_bytes = 0;
};

struct NetPeerSnapshotRow {
	int32_t slot = -1;
	std::string name;
	std::string address;
	std::string phase;
	uint32_t rtt_ms = 0;
	uint32_t rtt_average_ms = 0;
	uint32_t session_ping_ms = 0;
	int32_t quality = 0;
	int32_t min_ping_strikes = 0;
	int32_t max_ping_strikes = 0;
	uint32_t send_holdoff_ticks = 0;
	uint32_t send_holdoff_countdown = 0;
	uint32_t receive_inactive_ms = 0;
	bool traffic_valid = false;
	NetTraffic traffic;
};

struct NetJoinerSnapshot {
	bool present = false;
	std::string stage;
	bool in_match = false;
	bool deployed = false;
	uint32_t frontier_seq = 0;
	uint32_t outbound_seq = 0;
	uint64_t records_applied = 0;
	uint32_t gap_depth = 0;
	uint32_t retained_outbound = 0;
	int32_t flat_seconds = 0;
	bool freeze_suspected = false;
	uint32_t ping_ms = 0;
	uint32_t average_ping_ms = 0;
	uint32_t session_ping_ms = 0;
	int32_t quality = 0;
	uint32_t send_holdoff_ticks = 0;
	uint32_t send_holdoff_countdown = 0;
	uint32_t entity_checksum_seen = 0;
	uint32_t entity_checksum_answered = 0;
	uint32_t loadout_crc_seen = 0;
	uint32_t loadout_crc_answered = 0;
	uint32_t charattr_seen = 0;
	uint32_t charattr_row_missing = 0;
	uint32_t property_clears = 0;
	std::string last_reject;     // "" = none
	std::string last_disconnect; // "" = none
};

struct NetSnapshot {
	bool valid = false;
	uint64_t logic_tick = 0;
	double wall_seconds = 0.0; // the push's clock (the window's rate deltas)
	StatusRole role = StatusRole::SinglePlayer;
	StatusState state = StatusState::Unloaded;
	std::string bank_policy;
	int64_t frame_tick_us = 0; // the last session frame's tick cost
	int32_t frame_ticks = 0;
	double fps = 0.0;
	bool traffic_valid = false;
	NetTraffic traffic;
	std::vector<NetPeerSnapshotRow> peers;
	NetJoinerSnapshot joiner;
};

}  // namespace opennova::devtools
