#include <novaworld/game_server_runtime.h>

#include <cstdio>
#include <vector>

namespace {

bool expect(bool condition, const char *message) {
	if (condition) {
		return true;
	}
	std::fprintf(stderr, "FAIL: %s\n", message);
	return false;
}

bool has_tag(const std::vector<opennova::ProtocolMessage> &messages, uint8_t tag) {
	for (const opennova::ProtocolMessage &message : messages) {
		if (message.tag == tag) {
			return true;
		}
	}
	return false;
}

void drain_queued(opennova::GameServerRuntime &runtime) {
	for (int i = 0; i < 64 && runtime.snapshot("debug").primary_session.queued_reply_count > 0; ++i) {
		runtime.tick(16, static_cast<uint32_t>(1000 + i));
	}
}

bool check_debug_mission_flow_updates_snapshot() {
	opennova::GameServerRuntimeConfig config;
	config.session.spawn_points = {
		{1, 49, 1359, 1, 1000, 2000, 3000},
	};
	opennova::GameServerRuntime runtime(config);
	runtime.start();

	const auto result = runtime.handle_message(
			"debug", opennova::make_protocol_message(0x37, {}), 100);
	if (!expect(!result.replies.empty(), "mission request produces replies")) return false;

	const auto snapshot = runtime.snapshot("debug");
	if (!expect(snapshot.running, "runtime reports running")) return false;
	if (!expect(snapshot.session_count == 1, "runtime tracks one session")) return false;
	if (!expect(!snapshot.primary_session.spawned, "mission request does not mark session spawned")) return false;
	if (!expect(snapshot.primary_session.phase == "mission_ready", "mission request marks phase mission_ready")) return false;
	if (!expect(snapshot.primary_session.ida_initial_state == 2, "mission request enters IDA initial state 2")) return false;
	if (!expect(snapshot.primary_session.queued_reply_count == 14, "mission request queues IDA mission bootstrap")) return false;
	if (!expect(snapshot.primary_session.spawn_point_count == 1, "runtime snapshot exposes spawn-point config")) return false;
	if (!expect(!snapshot.primary_session.spawn_point_sync_enabled, "spawn-point sync is opt-in")) return false;
	if (!expect(!snapshot.primary_session.spawn_points_synced, "mission request has not synced spawn points yet")) return false;
	if (!expect(snapshot.messages_rx == 1, "runtime counts incoming messages")) return false;
	if (!expect(snapshot.messages_tx == result.replies.size(), "runtime counts outgoing messages")) return false;
	return true;
}

bool check_tick_driver_uses_runtime_sessions() {
	opennova::GameServerRuntimeConfig config;
	config.session.spawn_points = {
		{1, 49, 1359, 1, 1000, 2000, 3000},
	};
	opennova::GameServerRuntime runtime(config);
	runtime.start();
	runtime.handle_message("debug", opennova::make_protocol_message(0x37, {}), 100);
	drain_queued(runtime);
	runtime.handle_message("debug", opennova::make_protocol_message(0x0A, {}), 140);

	const auto first_stream = runtime.tick(300, 160);
	bool saw_tag10 = false;
	for (const opennova::GameServerDispatch &dispatch : first_stream) {
		saw_tag10 = saw_tag10 || has_tag(dispatch.replies, 0x10);
		if (!expect(!has_tag(dispatch.replies, 0x0A),
				"state 4 tick does not emit early world-reference 0x0A")) return false;
		if (!expect(!has_tag(dispatch.replies, 0x57),
				"state 4 tick does not emit early RTT 0x57")) return false;
	}
	if (!expect(saw_tag10,
			"state 4 tick dispatches entity stream without local-player ack")) return false;
	runtime.handle_messages("debug", {}, 170);

	for (int i = 0; i < 4; ++i) {
		const auto dispatches = runtime.tick(300, static_cast<uint32_t>(200 + i));
		for (const opennova::GameServerDispatch &dispatch : dispatches) {
			saw_tag10 = saw_tag10 || has_tag(dispatch.replies, 0x10);
		}
	}
	if (!expect(saw_tag10, "tick dispatches entity stream before spawn acceptance")) return false;

	const auto snapshot = runtime.snapshot("debug");
	if (!expect(!snapshot.primary_session.spawned, "world streaming does not mark runtime session spawned")) return false;
	if (!expect(snapshot.primary_session.phase == "world_streaming", "runtime session enters world_streaming")) return false;
	if (!expect(snapshot.primary_session.world_streaming_armed, "runtime session records streaming arm")) return false;
	if (!expect(snapshot.primary_session.world_streaming_ack_count == 0,
			"server-driven state 4 does not count as a streaming ack")) return false;
	if (!expect(!snapshot.primary_session.spawn_points_synced, "runtime session keeps experimental spawn-point sync disabled")) return false;
	if (!expect(snapshot.primary_session.entity_batch_count > 0, "runtime snapshot counts entity batches")) return false;
	if (!expect(snapshot.tick_ms == 1660, "tick advances runtime clock")) return false;
	if (!expect(snapshot.tick_counter == 15, "tick increments runtime counter")) return false;

	runtime.handle_message("debug", opennova::make_protocol_message(0x0E, {0xFE, 0xFF}), 2000);
	const auto spawned_snapshot = runtime.snapshot("debug");
	if (!expect(!spawned_snapshot.primary_session.spawned,
			"spawn request waits for loadout/status readiness")) return false;
	return true;
}

bool check_post_load_readiness_uses_runtime_session() {
	opennova::GameServerRuntimeConfig config;
	config.session.spawn_points = {
		{1, 49, 1359, 1, 1000, 2000, 3000},
	};
	opennova::GameServerRuntime runtime(config);
	runtime.start();
	runtime.handle_message("debug", opennova::make_protocol_message(0x37, {}), 100);
	drain_queued(runtime);
	runtime.handle_message("debug", opennova::make_protocol_message(0x09, {}), 120);
	for (int i = 0; i < 6; ++i) {
		runtime.tick(300, static_cast<uint32_t>(200 + i));
	}
	runtime.handle_message("debug", opennova::make_protocol_message(0x22, {0x00, 0xF7, 0x1C}), 240);
	runtime.tick(300, 241);

	const auto loadout = runtime.handle_messages("debug", {
			opennova::make_protocol_message(0x2F, std::vector<uint8_t>(35, 0)),
			opennova::make_protocol_message(0x2F, std::vector<uint8_t>(35, 0)),
			opennova::make_protocol_message(0x0B, std::vector<uint8_t>(13, 0)),
	}, 450);
	if (!expect(has_tag(loadout.replies, 0x5A), "runtime loadout emits loadout sync")) return false;
	if (!expect(!has_tag(loadout.replies, 0x0D), "runtime loadout does not emit unsupported local-player 0x0D")) return false;
	if (!expect(has_tag(loadout.replies, 0x42), "runtime loadout/status emits retail tag=0x42")) return false;
	if (!expect(has_tag(loadout.replies, 0x0A), "runtime loadout/status emits game-start world reference")) return false;
	if (!expect(has_tag(loadout.replies, 0x0F), "runtime loadout/status emits game-start")) return false;
	if (!expect(has_tag(loadout.replies, 0x3E), "runtime loadout/status emits retail tag=0x3E")) return false;
	if (!expect(!has_tag(loadout.replies, 0x1E), "runtime loadout/status does not emit old post-spawn probe")) return false;

	const auto snapshot = runtime.snapshot("debug");
	if (!expect(snapshot.primary_session.spawned, "runtime loadout/status marks session spawned")) return false;
	if (!expect(snapshot.primary_session.spawn_acceptance_sent, "runtime loadout/status records spawn acceptance")) return false;
	if (!expect(snapshot.primary_session.game_start_bundle_sent, "runtime loadout/status records game-start bundle")) return false;
	if (!expect(snapshot.primary_session.mission_status_received, "runtime loadout/status records mission status")) return false;
	if (!expect(snapshot.primary_session.spawn_query_count == 0,
			"runtime game-start does not require pre-game spawn queries")) return false;
	if (!expect(snapshot.primary_session.loadout_sync_count == 2, "runtime loadout/status counts loadout syncs")) return false;
	if (!expect(!snapshot.primary_session.spawn_points_synced,
			"runtime readiness does not emit experimental spawn-point sync by default")) return false;
	return true;
}

bool check_stopped_runtime_suppresses_dispatch() {
	opennova::GameServerRuntime runtime;
	const auto result = runtime.handle_message(
			"debug", opennova::make_protocol_message(0x37, {}), 100);
	if (!expect(result.replies.empty(), "stopped runtime produces no replies")) return false;
	if (!expect(runtime.snapshot("debug").session_count == 0, "stopped runtime does not open sessions")) return false;
	return true;
}

} // namespace

int main() {
	bool ok = true;
	ok = check_debug_mission_flow_updates_snapshot() && ok;
	ok = check_tick_driver_uses_runtime_sessions() && ok;
	ok = check_post_load_readiness_uses_runtime_session() && ok;
	ok = check_stopped_runtime_suppresses_dispatch() && ok;
	return ok ? 0 : 1;
}
