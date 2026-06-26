#pragma once

#include <novaworld/game_session.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace opennova {

struct GameServerRuntimeConfig {
	GameSessionConfig session;
	uint16_t bind_port = 64220;
};

struct GameServerSessionSnapshot {
	std::string session_id;
	std::string phase = "handshake";
	bool spawned = false;
	bool loadout_synced = false;
	bool mission_status_received = false;
	bool spawn_acceptance_sent = false;
	bool world_streaming_armed = false;
	uint16_t ida_initial_state = 0;
	uint16_t ida_initial_subphase = 0;
	bool initial_sync_complete = false;
	bool game_start_bundle_sent = false;
	size_t spawn_query_count = 0;
	size_t loadout_sync_count = 0;
	size_t world_streaming_ack_count = 0;
	size_t queued_reply_count = 0;
	size_t replicated_entity_count = 0;
	size_t spawn_point_count = 0;
	size_t entity_batch_cursor = 0;
	size_t entity_batch_count = 0;
	bool state4_loading_gate_queued = false;
	bool state4_loading_gate_complete = false;
	bool client_pos_valid = false;
	uint32_t client_pos_x = 0;
	uint32_t client_pos_y = 0;
	uint32_t client_pos_z = 0;
	int ms_since_tag10 = 0;
	int ms_since_tag0a = 0;
	int ms_since_tag57 = 0;
};

struct GameServerRuntimeSnapshot {
	bool running = false;
	uint16_t bind_port = 64220;
	std::string server_name;
	std::string mission_name;
	std::string mission_file;
	std::string player_name;
	uint64_t tick_ms = 0;
	uint32_t tick_counter = 0;
	uint64_t messages_rx = 0;
	uint64_t messages_tx = 0;
	size_t session_count = 0;
	std::string last_event;
	int last_reply_count = 0;
	GameServerSessionSnapshot primary_session;
};

struct GameServerDispatch {
	std::string session_id;
	std::vector<ProtocolMessage> replies;
	std::string label;
};

// Socket-independent game-server driver. Transports decode packets into
// ProtocolMessage arrays, pass them through this runtime, then encode replies.
// Godot scenes, CLI harnesses, and future tests can share this layer.
class GameServerRuntime {
public:
	explicit GameServerRuntime(GameServerRuntimeConfig config = {});

	void configure(GameServerRuntimeConfig config);
	const GameServerRuntimeConfig &config() const { return config_; }

	void start();
	void stop();
	bool running() const { return running_; }

	void reset();
	void reset_session(const std::string &session_id = "debug");

	GameServerDispatch handle_messages(const std::string &session_id,
	                                   const std::vector<ProtocolMessage> &messages,
	                                   uint32_t now_tick = 0);
	GameServerDispatch handle_message(const std::string &session_id,
	                                  const ProtocolMessage &message,
	                                  uint32_t now_tick = 0);

	std::vector<GameServerDispatch> tick(int elapsed_ms, uint32_t now_tick = 0);

	// Per-session variant of tick(): drive ONE session's periodic emitter
	// (the 0x10/0x0A/0x57 cadence). A listen-server host drives this only
	// while the session is pre-Spawned, then stops the instant it reaches
	// Spawned so NetSystem::emit_s2c can own that connection's per-frame 0x0A
	// (otherwise the static-config tick 0x0A double-emits alongside the live
	// one). Empty dispatch when the session is unknown or the runtime stopped.
	GameServerDispatch tick_session(const std::string &session_id,
	                                int elapsed_ms, uint32_t now_tick = 0);

	// Read a session's live decode state — its phase and the joiner pose
	// cached from the C2S 0x0C uplink (client_pos_*/heading/pitch). Returns
	// nullptr when the session_id is unknown. Lets a host observe the
	// handshake reach Spawned and recover the joiner's pose without a
	// snapshot copy.
	const GameSessionState *session_state(const std::string &session_id) const;

	bool bind_session_player(const std::string &session_id,
	                         std::string player_name,
	                         uint8_t player_slot,
	                         uint16_t entity_handle);

	GameServerRuntimeSnapshot snapshot(const std::string &primary_session_id = "debug") const;

private:
	uint32_t resolve_tick(uint32_t now_tick) const;
	GameServerSessionSnapshot snapshot_for(const std::string &session_id,
	                                       const GameSessionState &state) const;

	GameServerRuntimeConfig config_;
	GameSession session_;
	std::unordered_map<std::string, GameSessionState> sessions_;
	bool running_ = false;
	uint64_t tick_ms_ = 0;
	uint32_t tick_counter_ = 0;
	uint64_t messages_rx_ = 0;
	uint64_t messages_tx_ = 0;
	std::string last_event_ = "stopped";
	int last_reply_count_ = 0;
};

} // namespace opennova
