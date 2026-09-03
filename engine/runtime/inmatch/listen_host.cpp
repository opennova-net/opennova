#include <runtime/inmatch/listen_host.h>

#include <formats/mission/bms.h>
#include <runtime/inmatch/server_message_dispatch.h>
#include <base/gameprofile/game_type.h>
#include <net/npwire/ingame_message_id.h>
#include <net/npwire/protocol_message.h>
#include <runtime/mission/mission_kernel.h>

#include <utility>
#include <vector>

namespace opennova::inmatch::listen_host {

namespace {

// The shared bring-up preamble: a fresh loopback + owner over the kernel's
// world and mission, the rule words the world reads at tick time, and the
// is_in_session fact.
void reset_state(mission::MissionKernel &kernel, ListenHostState &state,
		const inmatch::GameConfig &config, bool serve_and_play) {
	state.client_runtime.reset();
	state.host_loop = replication::LoopbackChannel{};
	state.host_owner = inmatch::HostOwner{};
	state.host_owner.serve_and_play = serve_and_play;
	state.host_owner.ctx.world = &kernel.world;
	state.host_owner.ctx.mission = &kernel.mission;
	state.host_owner.ctx.mission_text_loaded = false;
	kernel.world.rules.fat_bullets = config.fat_bullets;
	kernel.world.rules.one_shot_kill = config.one_shot_kill;
	kernel.session_open = true;
}

// host_session_pump's before-server-tick hook: ground every soldier the tick
// spawns before its first authoritative update.
void before_server_tick(void *context) {
	if (context != nullptr)
		static_cast<mission::MissionKernel *>(context)->resolve_new_infantry_adm_ids();
}

} // namespace

// [orig: SinglePlayer_StartMission @0x561af0]
void bringup(mission::MissionKernel &kernel, ListenHostState &state) {
	inmatch::GameConfig config;
	config.server_name = "SINGLEPLAYERGAME";
	config.max_players = 1;
	config.game_type = game_type::for_mission_attribs(kernel.mission.header.attrib_flags);
	reset_state(kernel, state, config, /*serve_and_play=*/true);
	state.host_owner.host_loopback = &state.host_loop;
	inmatch::HostConfig host_cfg;
	host_cfg.config = config;
	host_cfg.socket_mode = inmatch::SocketMode::Socketless;
	host_cfg.serve_and_play = true;
	host_cfg.host_key = state.host_key;
	host_cfg.host_start_tick = state.host_start_tick;
	host_cfg.session_seed_id = state.session_seed_id;
	inmatch::start_host_session(state.host_owner, host_cfg);
	state.client_runtime = std::make_unique<inmatch::ClientRuntime>(state.host_loop);
	state.client_runtime->set_profile(kernel.world.profile);
	state.client_runtime->view().set_game_type(config.game_type);
	state.client_runtime->view().set_mp_session(kernel.world.rules.mp_session);
	// Seed the look heading from the auto-spawned player's facing.
	kernel.reset_local_player_input_to_player_facing();
}

void bringup_dedicated(mission::MissionKernel &kernel, ListenHostState &state,
		const inmatch::HostConfig &host_cfg) {
	reset_state(kernel, state, host_cfg.config, /*serve_and_play=*/false);
	// HostOnly registers no local-player connection: no type-2 loopback is
	// handed to create_session and no local player spawns.
	inmatch::HostConfig cfg = host_cfg;
	cfg.serve_and_play = false;
	inmatch::start_host_session(state.host_owner, cfg);
}

void drain_host_client_gameplay_requests(mission::MissionKernel &kernel,
		ListenHostState &state) {
	inmatch::NapiNPConnection *local = nullptr;
	for (inmatch::NapiNPConnection &conn : state.host_owner.ctx.np_protocol.connection_list) {
		if (conn.type == inmatch::NapiNPConnection::kTypeClientSide && conn.link.transport == &state.host_loop) {
			local = &conn;
			break;
		}
	}
	if (local == nullptr) return;
	replication::Datagram dg;
	std::vector<replication::Datagram> deferred;
	while (state.host_loop.host_recv(dg)) {
		if (dg.tag != c2s::WEAPON_RELOAD_REQUEST) {
			deferred.push_back(std::move(dg));
			continue;
		}
		std::vector<ProtocolMessage> messages;
		messages.push_back(make_protocol_message(dg.tag, std::move(dg.body)));
		std::vector<ProtocolMessage> replies = inmatch::dispatch_session_replies(
				state.host_owner.ctx.config, *local, messages, state.host_owner.now_tick,
				state.host_owner.ctx.np_protocol.connection_list, &kernel.world);
		for (ProtocolMessage &reply : replies) state.host_loop.host_send(reply.tag, std::move(reply.payload));
	}
	for (replication::Datagram &preserved : deferred) state.host_loop.deliver_c2s(preserved.tag, std::move(preserved.body));
}

// The listen frame: input -> the local player's body input, Server_TickUpdate
// (the C2S drain, ONE logic tick, the 0x0A fan) through the shared owner
// loop, the local view/weapon pumps, then the local ClientState fold.
// [orig: Game_ProcessMainFrame @0x5263f0]
void frame(mission::MissionKernel &kernel, ListenHostState &state,
		opennova::IDatagramSocket &socket, int32_t viewport_height) {
	// Server_SendRandomSeedSync's non-dedicated S2C 0x68 cursor wraps against
	// the renderer viewport height [orig: Server_SendRandomSeedSync @0x511360];
	// a missing viewport leaves the seam unset and npruntime suppresses 0x68
	// instead of inventing a screen size (D-NET-206).
	state.host_owner.ctx.loaded_model_viewport_height =
			viewport_height > 0 ? static_cast<uint32_t>(viewport_height) : 0u;
	const uint32_t now = state.host_owner.now_tick;
	drain_host_client_gameplay_requests(kernel, state);
	kernel.apply_player_input_pre_tick();
	inmatch::host_session_pump(state.host_owner, socket, &before_server_tick, &kernel,
			nullptr, nullptr);
	// The weather tick follows the server tick's entity update [orig:
	// Game_ProcessMainFrame @ 0x52674b -> @ 0x526774]; the next frame's 0x0A
	// fan projects the advanced weather.
	kernel.tick_weather();
	kernel.run_local_player_post_tick();
	if (state.client_runtime)
		state.client_runtime->Client_ProcessNetworkFrame(now); // fold host_loop -> ClientState
	kernel.resolve_new_infantry_adm_ids();
}

} // namespace opennova::inmatch::listen_host
