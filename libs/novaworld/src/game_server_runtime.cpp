#include <novaworld/game_server_runtime.h>

#include <algorithm>
#include <utility>

namespace opennova {

GameServerRuntime::GameServerRuntime(GameServerRuntimeConfig config)
		: config_(std::move(config)),
		  session_(config_.session) {}

void GameServerRuntime::configure(GameServerRuntimeConfig config) {
	const bool was_running = running_;
	config_ = std::move(config);
	session_ = GameSession(config_.session);
	sessions_.clear();
	running_ = was_running;
	last_event_ = "configured";
	last_reply_count_ = 0;
}

void GameServerRuntime::start() {
	if (running_) {
		return;
	}
	running_ = true;
	last_event_ = "started";
	last_reply_count_ = 0;
}

void GameServerRuntime::stop() {
	if (!running_) {
		return;
	}
	running_ = false;
	last_event_ = "stopped";
	last_reply_count_ = 0;
}

void GameServerRuntime::reset() {
	sessions_.clear();
	tick_ms_ = 0;
	tick_counter_ = 0;
	messages_rx_ = 0;
	messages_tx_ = 0;
	last_event_ = running_ ? "reset" : "stopped";
	last_reply_count_ = 0;
}

void GameServerRuntime::reset_session(const std::string &session_id) {
	sessions_.erase(session_id);
	last_event_ = "session reset";
	last_reply_count_ = 0;
}

GameServerDispatch GameServerRuntime::handle_message(const std::string &session_id,
                                                     const ProtocolMessage &message,
                                                     uint32_t now_tick) {
	return handle_messages(session_id, std::vector<ProtocolMessage>{message}, now_tick);
}

GameServerDispatch GameServerRuntime::handle_messages(
		const std::string &session_id,
		const std::vector<ProtocolMessage> &messages,
		uint32_t now_tick) {
	GameServerDispatch dispatch;
	dispatch.session_id = session_id;

	if (!running_) {
		dispatch.label = "game server stopped";
		last_event_ = dispatch.label;
		last_reply_count_ = 0;
		return dispatch;
	}

	GameSessionState &state = sessions_[session_id];
	GameSessionDispatchResult result =
			session_.handle_messages(state, messages, resolve_tick(now_tick));
	dispatch.replies = std::move(result.replies);
	dispatch.label = std::move(result.label);

	messages_rx_ += messages.size();
	messages_tx_ += dispatch.replies.size();
	last_event_ = dispatch.label;
	last_reply_count_ = static_cast<int>(dispatch.replies.size());
	return dispatch;
}

std::vector<GameServerDispatch> GameServerRuntime::tick(int elapsed_ms, uint32_t now_tick) {
	std::vector<GameServerDispatch> dispatches;
	if (!running_) {
		return dispatches;
	}

	const int clamped_elapsed = std::max(0, elapsed_ms);
	tick_ms_ += static_cast<uint64_t>(clamped_elapsed);
	++tick_counter_;
	const uint32_t tick_value = resolve_tick(now_tick);

	for (auto &kv : sessions_) {
		GameSessionDispatchResult result = session_.tick(kv.second, clamped_elapsed, tick_value);
		if (result.replies.empty()) {
			continue;
		}
		GameServerDispatch dispatch;
		dispatch.session_id = kv.first;
		dispatch.replies = std::move(result.replies);
		dispatch.label = std::move(result.label);
		messages_tx_ += dispatch.replies.size();
		last_event_ = dispatch.label;
		last_reply_count_ = static_cast<int>(dispatch.replies.size());
		dispatches.push_back(std::move(dispatch));
	}

	return dispatches;
}

GameServerDispatch GameServerRuntime::tick_session(const std::string &session_id,
                                                   int elapsed_ms, uint32_t now_tick) {
	GameServerDispatch dispatch;
	dispatch.session_id = session_id;
	if (!running_) {
		dispatch.label = "game server stopped";
		return dispatch;
	}
	auto it = sessions_.find(session_id);
	if (it == sessions_.end()) {
		dispatch.label = "no such session";
		return dispatch;
	}
	GameSessionDispatchResult result =
			session_.tick(it->second, std::max(0, elapsed_ms), resolve_tick(now_tick));
	dispatch.replies = std::move(result.replies);
	dispatch.label = std::move(result.label);
	messages_tx_ += dispatch.replies.size();
	last_event_ = dispatch.label;
	last_reply_count_ = static_cast<int>(dispatch.replies.size());
	return dispatch;
}

const GameSessionState *GameServerRuntime::session_state(const std::string &session_id) const {
	auto it = sessions_.find(session_id);
	return it == sessions_.end() ? nullptr : &it->second;
}

bool GameServerRuntime::bind_session_player(
		const std::string &session_id,
		std::string player_name,
		uint8_t player_slot,
		uint16_t entity_handle) {
	auto it = sessions_.find(session_id);
	if (it == sessions_.end()) {
		return false;
	}
	it->second.player_binding_valid = true;
	it->second.player_name = std::move(player_name);
	it->second.player_slot = player_slot;
	it->second.player_entity_handle = entity_handle;
	last_event_ = "session player binding installed";
	return true;
}

GameServerRuntimeSnapshot GameServerRuntime::snapshot(const std::string &primary_session_id) const {
	GameServerRuntimeSnapshot out;
	out.running = running_;
	out.bind_port = config_.bind_port;
	out.server_name = config_.session.server_name;
	out.mission_name = config_.session.mission_name;
	out.mission_file = config_.session.mission_file;
	out.player_name = config_.session.player_name;
	out.tick_ms = tick_ms_;
	out.tick_counter = tick_counter_;
	out.messages_rx = messages_rx_;
	out.messages_tx = messages_tx_;
	out.session_count = sessions_.size();
	out.last_event = last_event_;
	out.last_reply_count = last_reply_count_;

	auto it = sessions_.find(primary_session_id);
	if (it != sessions_.end()) {
		out.primary_session = snapshot_for(it->first, it->second);
	} else if (!sessions_.empty()) {
		const auto &first = *sessions_.begin();
		out.primary_session = snapshot_for(first.first, first.second);
	} else {
		out.primary_session.session_id = primary_session_id;
	}
	return out;
}

uint32_t GameServerRuntime::resolve_tick(uint32_t now_tick) const {
	return now_tick != 0 ? now_tick : static_cast<uint32_t>(tick_ms_ & 0xFFFFFFFFu);
}

GameServerSessionSnapshot GameServerRuntime::snapshot_for(
		const std::string &session_id,
		const GameSessionState &state) const {
	GameServerSessionSnapshot out;
	out.session_id = session_id;
	out.phase = game_session_phase_name(state.phase);
	out.spawned = state.spawned;
	out.loadout_synced = state.loadout_synced;
	out.mission_status_received = state.mission_status_received;
	out.spawn_acceptance_sent = state.spawn_acceptance_sent;
	out.world_streaming_armed = state.world_streaming_armed;
	out.ida_initial_state = state.ida_initial_state;
	out.ida_initial_subphase = state.ida_initial_subphase;
	out.initial_sync_complete = state.initial_sync_complete;
	out.game_start_bundle_sent = state.game_start_bundle_sent;
	out.spawn_query_count = state.spawn_query_count;
	out.loadout_sync_count = state.loadout_sync_count;
	out.world_streaming_ack_count = state.world_streaming_ack_count;
	out.queued_reply_count = state.queued_replies.size();
	out.replicated_entity_count = config_.session.replicated_entities.size();
	out.spawn_point_count = config_.session.spawn_points.size();
	out.entity_batch_cursor = state.entity_batch_cursor;
	out.entity_batch_count = state.entity_batch_count;
	out.state4_loading_gate_queued = state.state4_loading_gate_queued;
	out.state4_loading_gate_complete = state.state4_loading_gate_complete;
	out.client_pos_valid = state.client_pos_valid;
	out.client_pos_x = state.client_pos_x;
	out.client_pos_y = state.client_pos_y;
	out.client_pos_z = state.client_pos_z;
	out.ms_since_tag10 = state.ms_since_tag10;
	out.ms_since_tag0a = state.ms_since_tag0a;
	out.ms_since_tag57 = state.ms_since_tag57;
	return out;
}

} // namespace opennova
