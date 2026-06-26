#pragma once

#include <novaworld/protocol_message.h>
#include <novaworld/replication_min.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace opennova {

// Runtime inputs for the current :64220 game-session emulation. Defaults
// match the current dvxi5 development server flow; callers can override the
// spawn from a parsed .bms mission.
struct GameSessionConfig {
	std::string server_name = "OpenNova Dev";
	std::string mission_name = "AS - Dormant Volcano Isle";
	std::string mission_file = "ASH_I5A.BMS";
	std::string expansion = "jox01";
	std::string player_name = "DevUser";
	uint32_t gametype = 0x00010010u;
	uint32_t mpattrib = 14854u;
	bool spawn_valid = false;
	uint32_t spawn_x = 0xfe56f854u;
	uint32_t spawn_y = 0x0049f5f0u;
	uint32_t spawn_z = 0x003a5e6au;
	uint16_t player_entity_handle = 0;
	std::vector<uint8_t> mission_header_blob;
	std::vector<std::string> spawn_names;
	std::vector<GameEntitySnapshot> replicated_entities;
	std::vector<SpawnPointEntity> spawn_points;
};

enum class GameSessionPhase {
	Handshake,
	MissionReady,
	InitialSync,
	WorldStreaming,
	SpawnRequested,
	Spawned,
};

const char *game_session_phase_name(GameSessionPhase phase);

struct GameSessionState {
	ProtocolReassemblyState reassembly;
	GameSessionPhase phase = GameSessionPhase::Handshake;
	bool spawned = false;
	bool loadout_synced = false;
	bool mission_status_received = false;
	bool spawn_acceptance_sent = false;
	bool world_streaming_armed = false;
	uint16_t ida_initial_state = 0;
	uint16_t ida_initial_subphase = 0;
	size_t ida_mission_replies_pending = 0;
	bool initial_sync_complete = false;
	bool game_start_bundle_sent = false;
	bool mission_disconnect_ack_sent = false;
	size_t spawn_query_count = 0;
	size_t loadout_sync_count = 0;
	size_t world_streaming_ack_count = 0;
	std::vector<ProtocolMessage> queued_replies;
	int ms_since_tag10 = 0;
	int ms_since_tag0a = 0;
	int ms_since_tag57 = 0;
	size_t entity_batch_cursor = 0;
	size_t entity_batch_count = 0;
	bool state4_player_list_sent = false;
	bool state4_player_sync_requested = false;
	bool state4_player_sync_sent = false;
	bool state4_loading_gate_queued = false;
	bool state4_loading_gate_complete = false;
	// Set after we emit our first tag=0x51 PLAYER-SPAWN reply. Used to break
	// the tag=0x29 ↔ tag=0x51 ack loop: NapiNPClientMsg_HandlePlayerSpawn @
	// 0x431BB0 always echoes back tag=0x29 with payload (team+1) after
	// receiving tag=0x51, and re-replying would loop forever.
	bool player_spawn_confirmed = false;
	bool player_binding_valid = false;
	std::string player_name;
	uint8_t player_slot = 0;
	uint16_t player_entity_handle = 0;
	bool client_pos_valid = false;
	uint16_t client_entity_handle = 0;
	uint16_t client_item_type_id = 0;
	uint16_t client_vehicle_handle = 0xFFFF;
	uint32_t client_pos_x = 0;
	uint32_t client_pos_y = 0;
	uint32_t client_pos_z = 0;
	int16_t client_heading = 0;
	int16_t client_pitch = 0;
};

struct GameSessionDispatchResult {
	std::vector<ProtocolMessage> replies;
	std::string label = "in-game ack (empty)";
};

class GameSession {
public:
	explicit GameSession(GameSessionConfig config = {});

	const GameSessionConfig &config() const { return config_; }

	GameSessionDispatchResult handle_messages(GameSessionState &state,
	                                          const std::vector<ProtocolMessage> &messages,
	                                          uint32_t now_tick) const;

	GameSessionDispatchResult tick(GameSessionState &state,
	                               int elapsed_ms,
	                               uint32_t now_tick) const;

	PlayerReplicationState player_replication_state() const;
	PlayerReplicationState player_replication_state(const GameSessionState &state) const;

private:
	GameSessionConfig config_;
};

} // namespace opennova
