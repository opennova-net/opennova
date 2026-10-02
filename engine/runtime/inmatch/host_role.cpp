#include <runtime/inmatch/host_role.h>

#include <base/gameprofile/game_type.h>
#include <base/io/perf_clock.h>
#include <formats/mission/bms.h>
#include <net/npwire/ingame_encode.h>
#include <net/npwire/ingame_message_id.h>
#include <net/npwire/protocol_message.h>
#include <net/npwire/session_hello.h> // DisconnectEvent
#include <runtime/inmatch/host_session.h>
#include <runtime/inmatch/null_datagram_socket.h>
#include <runtime/inmatch/server_initial_state.h>
#include <runtime/inmatch/server_message_dispatch.h>
#include <runtime/inmatch/server_tick.h>
#include <runtime/mission/mission_kernel.h>
#include <runtime/replication/entity_wire_bridge.h>

#include <string>
#include <utility>
#include <vector>

namespace opennova::inmatch {

namespace {

// host_session_pump's before-server-tick hook: ground every soldier the tick
// spawns before its first authoritative update.
void before_server_tick(void *context) {
	if (context != nullptr)
		static_cast<mission::MissionKernel *>(context)->resolve_new_infantry_adm_ids();
}

} // namespace

HostRole::HostRole() = default;

HostRole::HostRole(RoleKind kind,
		std::shared_ptr<const replication::ItemReplicationCatalog> item_catalog)
		: kind_(kind), item_catalog_(std::move(item_catalog)) {}

// The boot hook's bring-up: the staged record through the general bring-up
// (the HostClient view follows on serve-and-play, taking the catalog this
// role holds).
bool HostRole::bring_up() {
	bring_up(staged_bringup_);
	return false;
}

void HostRole::set_item_catalog(
		std::shared_ptr<const replication::ItemReplicationCatalog> catalog) {
	item_catalog_ = std::move(catalog);
	if (state.client_runtime && item_catalog_)
		state.client_runtime->view().set_item_catalog(item_catalog_);
}

// The shared bring-up preamble: a fresh loopback + owner over the kernel's
// world and mission, the rule words the world reads at tick time, and the
// is_in_session fact.
void HostRole::reset_state(const inmatch::GameConfig &config, bool serve_and_play) {
	mission::MissionKernel &kernel = *kernel_;
	state.client_runtime.reset();
    local_round_reset_seen_ = kernel.local.round_reset_revision;
	state.host_loop = replication::LoopbackChannel{};
	state.host_owner = inmatch::HostOwner{};
	state.host_owner.serve_and_play = serve_and_play;
	state.host_owner.ctx.world = &kernel.world;
	state.host_owner.ctx.mission = &kernel.mission;
	state.host_owner.ctx.mission_text_loaded = false;
	kernel.world.rules.fat_bullets = config.fat_bullets;
	kernel.world.rules.one_shot_kill = config.one_shot_kill;
	// The mission-data block's unlimited-vehicles word, rebuilt from the host
	// config at every mission start [orig: Client_BuildMissionDataRequestBlock
	// @0x51E8C5..0x51E8CB from dword_24D2258 = unlimitedVehicles_4D0].
	kernel.world.rules.vehicle_respawns = config.unlimited_vehicles;
	// The mpattrib word's 0x10000 bit the scope-zero -1 floor reads in session
	// [orig: `test g_RulesFlags,10000h` @0x4dbd15; g_RulesFlags @0x24D1E34 is
	// the host's mpattrib word, the S2C 0x64 +44 dword on a joiner].
	kernel.world.rules.mpattrib = config.mp_attributes;
	kernel.world.rules.hit_feedback = config.hit_feedback;
	kernel.world.rules.auto_scope_zero =
			(config.mp_attributes & GameConfig::kMpAttribAutoScopeZero) != 0;
	kernel.world.rules.no_friendly_fire =
			(config.mp_attributes & GameConfig::kMpAttribNoFriendlyFire) != 0;
	// The is_mp_session_peer bit is the is_client half of the connection
	// mode: set for the SP/listen HostClient, clear for a HostOnly dedicated
	// host. [orig: g_NapiNPCtx +0x64; napi_np_server_ctx.h connection modes]
	kernel.world.rules.mp_session_peer = serve_and_play;
}

// The HostClient replica pipeline (recv-fold only, 0x0C suppressed): it folds
// the loopback's whole-world 0x0A into the ClientState the host's own view
// renders from.
void HostRole::make_client_runtime(uint32_t game_type) {
	mission::MissionKernel &kernel = *kernel_;
	state.client_runtime = std::make_unique<inmatch::ClientRuntime>(state.host_loop);
	state.host_owner.ctx.host_client = state.client_runtime.get();
	state.client_runtime->set_profile(kernel.world.profile);
	state.client_runtime->view().set_game_type(game_type);
	state.client_runtime->view().set_mp_session(kernel.world.rules.mp_session);
	if (item_catalog_)
		state.client_runtime->view().set_item_catalog(item_catalog_);
}

GameConfig singleplayer_game_config(uint32_t game_type) {
	inmatch::GameConfig config;
	config.server_name = "SINGLEPLAYERGAME";
	config.mp_attributes = 0x3A06u;
	config.max_players = 1;
	config.game_type = game_type;
	return config;
}

// [orig: SinglePlayer_StartMission @0x561af0]
void HostRole::bring_up_singleplayer() {
	mission::MissionKernel &kernel = *kernel_;
	const inmatch::GameConfig config = singleplayer_game_config(
			game_type::for_mission_attribs(kernel.mission.header.attrib_flags));
	reset_state(config, /*serve_and_play=*/true);
	state.host_owner.host_loopback = &state.host_loop;
	inmatch::HostConfig host_cfg;
	host_cfg.config = config;
	host_cfg.socket_mode = inmatch::SocketMode::Socketless;
	host_cfg.serve_and_play = true;
	host_cfg.host_key = state.host_key;
	host_cfg.host_start_tick = state.host_start_tick;
	host_cfg.session_seed_id = state.session_seed_id;
	inmatch::start_host_session(state.host_owner, host_cfg);
	make_client_runtime(config.game_type);
	// Seed the look heading from the auto-spawned player's facing.
	kernel.local.reset_local_player_input_to_player_facing();
}

// The shell's bring-up: the LAN host from its consolidated server config, or
// its SP listen server; serve_and_play false is the dedicated (no local
// player) form of the same session [orig: the §5.0 mode-3 listen server].
void HostRole::bring_up(const HostBringup &bringup) {
	mission::MissionKernel &kernel = *kernel_;
	const inmatch::HostConfig &host_cfg = bringup.host_cfg;
	reset_state(host_cfg.config, host_cfg.serve_and_play);
	state.host_owner.host_loopback = &state.host_loop;
	NapiNPServerCtx &ctx = state.host_owner.ctx;
	ctx.terrain_til_data = bringup.terrain_til_data; // S2C 0x45 terrain-tile load source (empty => skipped, §5.37)
	ctx.mission_text_loaded = bringup.mission_text.loaded;
	ctx.mission_briefing3 = bringup.mission_text.briefing3;
	ctx.mission_briefing2 = bringup.mission_text.briefing2;
	inmatch::install_mission_location_names(ctx, kernel.mission, bringup.mission_text.location_texts);
	inmatch::start_host_session(state.host_owner, host_cfg);
	if (host_cfg.serve_and_play) {
		make_client_runtime(host_cfg.config.game_type);
		kernel.local.reset_local_player_input_to_player_facing();
	} else {
		state.client_runtime.reset();
	}
}

void HostRole::drain_host_client_gameplay_requests() {
	mission::MissionKernel &kernel = *kernel_;
	inmatch::NapiNPConnection *local = nullptr;
	for (inmatch::NapiNPConnection &conn : state.host_owner.ctx.np_protocol.connection_list) {
		if (conn.type == inmatch::NapiNPConnection::kTypeClientSide && conn.link.transport == &state.host_loop) {
			local = &conn;
			break;
		}
	}
	if (local == nullptr) return;
	// The listen client's own roster slot (retail g_LocalPlayerSlotId): its S2C
	// 0x4D fold tells its own slot's join notice from another's
	// [orig: NapiNPClientMsg_HandleSpawnSlot @0x4317f0].
	if (state.client_runtime)
		state.client_runtime->view().set_local_player_slot(local->reply.player_slot);
	replication::Datagram dg;
	std::vector<replication::Datagram> deferred;
	while (state.host_loop.host_recv(dg)) {
		// Every gameplay request the host's own player produces rides this
		// local client queue: the reload, the action-6 mounted-slot select and
		// the medic call all go through CNapiNetwork_QueueReliableMessage with
		// no is_authority exclusion. Dispatch them before the later
		// ENTITY_UPLINK-only drain consumes this FIFO.
		// [orig: Input_HandleActionBinding_0 @ 0x4E0420 (action 6 @ 0x4E0492,
		//  push 16h @ 0x4E04E4) -> NapiNPServerMsg_HandleWeaponToggle @ 0x511A70;
		//  Input_HandleActionBinding action 217 @ 0x49B4B4..0x49B50C (push 2Eh
		//  @ 0x49B500)]
		// The host player's chat line rides the same queue (the talk senders
		// are a session peer's QueueReliableMessage(0xD) too); its handler
		// reads the server context [orig: NapiNPServer_HandleChatMessage
		// @0x513760].
		// The listen client's visible-players refreshes (the C2S 0x22 / 0x23
		// pair its 0x0F / 0x4D / 0x50 handlers queue) and its emote request
		// ride the same queue [orig: NapiNPClientMsg_0x00F @0x42e66c..0x42e6ab;
		//  NapiNPClientMsg_HandleSpawnSlot @0x43181d / @0x43183e;
		//  NapiNPClientMsg_TeamAssign @0x431ae4 / @0x431b05;
		//  NetPacket_SendEmoteRequest @0x42c14c]; its radio call too
		// [orig: NetPacket_SendRadioCallRequest @0x42c17c].
		// The host player's command-map sends ride it too: each squad /
		// waypoint / punt sender is a bare QueueReliableMessage on the local
		// client's connection, and the server handlers read the host
		// context (server_squad.h).
		// [orig: NetPacket_SendChatMessage @0x42dde6 (0x17),
		//  NetPacket_SendWeaponSlotSwitch @0x42dc20 (0x43),
		//  NetPacket_SendCommandType44 @0x42dca0 (0x44),
		//  NetPacket_SendWeaponAction @0x42dceb (0x45), NetPacket_SendTeamChange
		//  @0x42dd19 (0x46), NetPacket_SendVoteKick @0x42dd69 (0x4B),
		//  NetPacket_SendEntityUpdate @0x42de21 (0x4F),
		//  CMap_HandlePlayerListCallback @0x5488b9 (0x3F) — every sender name
		//  a misnomer; CNapiNetwork_QueueReliableMessage @0x4c4fa0 queues on
		//  the local client connection (this+0xE60)]
		const bool squad = dg.tag == c2s::WAYPOINT_SHARE || dg.tag == c2s::PUNT_VOTE ||
				dg.tag == c2s::SQUAD_JOIN_REQUEST || dg.tag == c2s::SQUAD_ORDER_REQUEST ||
				dg.tag == c2s::FIRETEAM_ASSIGN || dg.tag == c2s::SQUAD_RECRUIT ||
				dg.tag == c2s::GO_CODE || dg.tag == c2s::WAYPOINT_DELETE;
		if (!squad && dg.tag != c2s::WEAPON_RELOAD_REQUEST &&
				dg.tag != c2s::MOUNTED_WEAPON_SLOT_SELECT &&
				dg.tag != c2s::MEDIC_REQUEST && dg.tag != c2s::CHAT_MESSAGE &&
				dg.tag != c2s::PLAYER_SYNC_REQUEST && dg.tag != c2s::VISIBLE_PLAYERS_REQUEST &&
				dg.tag != c2s::EMOTE_REQUEST && dg.tag != c2s::RADIO_CALL_REQUEST) {
			deferred.push_back(std::move(dg));
			continue;
		}
		// The chat handler, the snapshot builder, the radio call (its
		// designation table and zone test) and the squad handlers read the
		// host context.
		const bool reads_ctx = squad || dg.tag == c2s::CHAT_MESSAGE ||
				dg.tag == c2s::VISIBLE_PLAYERS_REQUEST || dg.tag == c2s::RADIO_CALL_REQUEST;
		std::vector<ProtocolMessage> messages;
		messages.push_back(make_protocol_message(dg.tag, std::move(dg.body)));
		inmatch::ServerDispatchInputs inputs;
		if (reads_ctx) inputs.server_ctx = &state.host_owner.ctx;
		std::vector<ProtocolMessage> replies = inmatch::dispatch_session_replies(
				state.host_owner.ctx.config, *local, messages, state.host_owner.now_tick,
				state.host_owner.ctx.np_protocol.connection_list, &kernel.world, inputs);
		for (ProtocolMessage &reply : replies) state.host_loop.host_send(reply.tag, std::move(reply.payload));
	}
	for (replication::Datagram &preserved : deferred) state.host_loop.deliver_c2s(preserved.tag, std::move(preserved.body));
}

// The listen host's own call rides its loopback client like the reload
// request: the server handler broadcasts the 0x1E line to everyone including
// this client.
bool HostRole::send_medic_request() {
	if (!state.host_owner.serve_and_play) return false;
	opennova::MedicRequest request;
	request.entity_index = kernel_->world.cached.local_player.packed;
	state.host_loop.client_send(opennova::c2s::MEDIC_REQUEST, opennova::encode_medic_request(request));
	return true;
}

// The listen frame: input -> the local player's body input, Server_TickUpdate
// (the C2S drain, ONE logic tick, the 0x0A fan) through the shared owner
// loop, the local view/weapon pumps, then the local ClientState fold.
// [orig: Game_ProcessMainFrame @0x5263f0]
bool HostRole::local_fire_admitted() const {
	const mission::MissionKernel &kernel = *kernel_;
	const NapiNPServerCtx &ctx = state.host_owner.ctx;
	// The gate's session tests: the retail is_in_session fact and the
	// is_client bit; the shooter is the local player here by construction.
	// [orig: `cmp is_in_session` @0x42be12, `cmp is_mp_session_peer` @0x42be1e,
	//  `cmp edi, g_LocalPlayerEntity` @0x42be26]
	if (!kernel.world.rules.mp_session || ctx.is_mp_session_peer == 0) return true;
	if (!kernel.world.cached.local_player.valid() || state.client_runtime == nullptr) return true;
	// Entity_ValidatePtr: the player slot that owns the entity; none admits
	// [orig: `cmp eax, ebx; jz` @0x42be2e].
	for (const NapiNPConnection &conn : ctx.np_protocol.connection_list) {
		if (conn.link.owned_entity != kernel.world.cached.local_player) continue;
		return Server_AcceptsPlayerFireTick(conn, state.client_runtime->current_tick(),
				kernel.world.logic_tick, ctx.config.effective_send_holdoff_ticks());
	}
	return true;
}

void HostRole::run_tick(const TickInput &input) {
	mission::MissionKernel &kernel = *kernel_;
	// The socketless host (the SP listen server, the headless test rigs):
	// every datagram dropped.
	opennova::IDatagramSocket &socket =
			socket_ != nullptr ? *socket_ : null_datagram_socket();
	const int64_t prep_start = static_cast<int64_t>(io::perf_now_us());
	// What the view arbiter reads from the session (death screen, end round,
	// the death camera): sampled pre-fold, exactly the value the old inline
	// view tick consumed at this point in the frame.
	kernel.local.view_session_inputs = view_session_inputs_for(
			state.client_runtime.get(), /*joiner=*/false, kernel.local.local_player_dead(),
			kernel.world.rules.mp_session);
	// Server_SendRandomSeedSync's non-dedicated S2C 0x68 cursor wraps against
	// the renderer viewport height [orig: Server_SendRandomSeedSync @0x511360];
	// a missing viewport leaves the seam unset and inmatch suppresses 0x68
	// instead of inventing a screen size (D-NET-206).
	state.host_owner.ctx.loaded_model_viewport_height =
			input.viewport_height > 0 ? static_cast<uint32_t>(input.viewport_height) : 0u;
	const uint32_t now = state.host_owner.now_tick;
	// The stats board's host-prep row: the view/viewport setup before the
	// portable host pump.
	if (kernel.world.profile != nullptr)
		kernel.world.profile->add(devtools::Slot::SIM_HOST_PREP,
				static_cast<int64_t>(io::perf_now_us()) - prep_start);
	drain_host_client_gameplay_requests();
	// The host dictates send holdoffs but receives none, so its own client
	// frame's send block -- and the input pack inside it -- opens every frame
	// [orig: Client_ProcessNetworkFrame @0x42C3DD -> @0x42C3E9; the period
	//  arrives only as a CS field-3 update, HandleCSConfigUpdate @0x621940].
	kernel.local.apply_player_input_pre_tick(/*pack_input=*/true);
	// The pending fire-sound slots count down ahead of the server tick's
	// receive, so a slot this frame's C2S queues starts on the next frame.
	// [orig: Game_ProcessMainFrame -- the Sound_TickPendingSlots call
	//  @0x526697 precedes the Server_TickUpdate call @0x5266B6 (its receive
	//  pump @0x51D895)]
	kernel.world.tick_pending_sound_slots();
	// The entity-update gate's exemption for a host that also plays: its own
	// client's death-screen latch, folded at the end of the previous frame as
	// retail's client receive sets it at the head of this one.
	// [orig: Game_ProcessMainFrame -- `cmp is_mp_session_peer` @0x52670B,
	//  `cmp g_DeathScreenActive,0` @0x526713]
	kernel.world.cached.peer_death_screen = state.host_owner.ctx.is_mp_session_peer != 0 &&
			state.client_runtime != nullptr && state.client_runtime->state().death_screen_active;
	// The own client's spectate walk starts from and skips the host's player
	// [orig: Spectator_CycleTarget_0 @0x52ac56 / @0x52ad09].
	if (state.client_runtime != nullptr)
		state.client_runtime->view().set_spectate_local_handle(kernel.world.cached.local_player.packed);
	inmatch::host_session_pump(state.host_owner, socket, &before_server_tick, &kernel,
			nullptr, nullptr);
	// The frame tail after the server tick laps onto the stats board's
	// player-tail row; the weapon walk keeps its own row.
	devtools::ProfileLap tail(kernel.world.profile);
    if (local_round_reset_seen_ != kernel.local.round_reset_revision) {
        local_round_reset_seen_ = kernel.local.round_reset_revision;
        if (state.client_runtime) state.client_runtime->reset_local_round_state();
    }
	// The weather tick follows the server tick's entity update [orig:
	// Game_ProcessMainFrame @ 0x52674b -> @ 0x526774]; the next frame's 0x0A
	// fan projects the advanced weather.
	kernel.tick_weather();
	kernel.local.run_local_view_tick();
	tail.mark(devtools::Slot::SIM_PLAYER_TAIL);
	// The frame's one weapon-action walk follows the camera compose: the
	// local player's slot pumps at its own pool-0 slot, the gunners around it.
	// [orig: Game_ProcessMainFrame -- Camera_ComputeThirdPersonView @0x526781,
	//  the WeaponAction_ProcessAllEntities call @0x526786]
	kernel.local.authority_fire_admitted = local_fire_admitted();
	kernel.world.pump_weapon_actions();
	tail.restart();
	kernel.resolve_new_infantry_adm_ids();
	tail.mark(devtools::Slot::SIM_ADM_RESOLVE);
	kernel.local.tick_medic_cooldown(kernel.local.local_player_dead()); // Player_UpdatePerFrame's cooldown leg
	// The pump's wire-facing reload outcome relays onto the loopback so the
	// shared dispatcher broadcasts the S2C 0x49 to every client next frame
	// (the authority already performed WeaponSlot_ReloadAmmo inside the pump;
	// the server handler's local-connection gate prevents a second refill).
	if (kernel.local.last_reload.valid && state.host_owner.serve_and_play) {
		opennova::WeaponReload reload;
		reload.entity_handle = kernel.local.last_reload.entity_handle;
		reload.reload_param = kernel.local.last_reload.reload_param;
		state.host_loop.client_send(0x25, opennova::encode_weapon_reload(reload));
		kernel.local.last_reload = world::LocalWeaponReloadWire{};
	}
	tail.mark(devtools::Slot::SIM_PLAYER_TAIL);
	// The host's measurable net leg for the stats board: the ClientState fold
	// (host_loop -> ClientState). The S2C serialize/emit half rides inside
	// host_session_pump, fused with the logic tick.
	const int64_t net_start = static_cast<int64_t>(io::perf_now_us());
	NapiNPServerCtx &ctx = state.host_owner.ctx;
	if (state.client_runtime) {
		// The connection indicators on the authority: the level its send
		// window buckets lands before the indicators step inside the client
		// frame, and the server protocol's link-error callbacks of this tick
		// after it, as retail's Server_TickUpdate runs after that step
		// [orig: Game_ProcessMainFrame — CNetQuality_SetLevel @0x52659b,
		//  CNetQuality_UpdateIndicators @0x52668d, Server_TickUpdate @0x5266b6].
		state.client_runtime->set_net_quality_level(ctx.net_quality_level);
		state.client_runtime->Client_ProcessNetworkFrame(now);
		state.client_runtime->apply_received_effects(kernel.world);
		state.client_runtime->raise_net_quality_link_errors(ctx.net_quality_link_errors);
	}
	ctx.net_quality_link_errors = 0;
	last_net_us_ = static_cast<int64_t>(io::perf_now_us()) - net_start;
	if (kernel.world.profile != nullptr)
		kernel.world.profile->add(devtools::Slot::SIM_NET, last_net_us_);
}

void HostRole::observe_frame_rate(int32_t fps) {
	Role::observe_frame_rate(fps);
	state.host_owner.ctx.stats_avg_fps = fps;
}

void HostRole::observe_frame_statistics(int32_t frames_last_second, int32_t cpu_percent) {
	state.host_owner.ctx.stats_frames_last_second = frames_last_second;
	state.host_owner.ctx.stats_cpu_percent = cpu_percent;
}

bool HostRole::session_lost(SessionError &error) const {
	const NapiNPServerCtx &ctx = state.host_owner.ctx;
	// The NovaWorld session's end exits the mission on either host kind.
	if (ctx.mission_exit_reason != 0) {
		error = {SessionErrorCode::SessionLost,
				"mission exit " + std::to_string(ctx.mission_exit_reason)};
		return true;
	}
	if (ctx.connection_mode != ConnectionMode::HostOnly) return false;
	if (!ctx.round_end_announced || ctx.round_end_linger_ticks != 0 ||
			ctx.is_in_session != 0)
		return false;
	error = {SessionErrorCode::RoundEnded,
			"round ended: the post-round linger expired and the session closed"};
	return true;
}

// The host's mission exit. Retail's authority teardown walks every active
// player slot in the in-match states 2..7 and sends each one S2C 0x25 (empty
// body, one-send to that slot), sets its net player to game state 8 and its
// slot state back to 1; the session reset then stops the server: while
// host_running is still set, every connection has the record {ds 1, dc 9, dp
// 0, dstr "", dpc 0, ddstr "NP.C:SH:STOP"} latched (first cause wins) and is
// destroyed — the destroy sends the 0x86 SERVER_GOODBYE burst (four identical
// keyed datagrams carrying that record) to the peer, then runs the player
// teardown — and only after the walk is host_running cleared. A joiner (retail
// or ours) therefore leaves the match at once instead of sitting through the
// 120 s silence reap; the burst, not a reliable description record, is what
// retail puts on the wire here.
// [orig: Game_TeardownMission @0x522350 -> Server_DisconnectAndResetAllPlayerSlots
//  @0x516160 (the six `push 25h` / NapiNPServer_SendFiltered @0x5161bb..0x516385);
//  CNapiGameSession_ResetActiveSession @0x4C8A70 -> NapiNPProtocol_StopServer
//  @0x62A820 (the record @0x62a8c3..0x62a8f6 latched-if-invalid @0x62a904..0x62a919,
//  CNapiNPConnection_Destroy @0x62a924 per connection, host_running cleared
//  @0x62a95c) -> TeardownActiveConnection @0x6253C0 -> SendDisconnectPacket @0x61F2A0]
void HostRole::close() {
	// The teardown's head: pools 0..2 go, then the authority's one-shot
	// PostMission sweep, both ahead of the per-slot 0x25 walk below
	// [orig: Game_TeardownMission — EventTrigger_UpdateAllWithFlag4 call
	//  @0x52266C precedes Server_DisconnectAndResetAllPlayerSlots @0x52269B].
	if (kernel_ != nullptr) kernel_->run_post_mission_pass(/*is_authority=*/true);
	opennova::IDatagramSocket &socket =
			socket_ != nullptr ? *socket_ : null_datagram_socket();
	NapiNPServerCtx &ctx = state.host_owner.ctx;
	for (NapiNPConnection &conn : ctx.np_protocol.connection_list) {
		if (conn.type != NapiNPConnection::kTypeServerSide || conn.link.transport == nullptr)
			continue;
		if (conn.burst.sync_state >= 2)
			conn.link.transport->host_send(s2c::GAME_RESET, std::vector<uint8_t>{},
					/*reliable=*/false);
	}
	inmatch::host_session_flush_s2c(state.host_owner, socket);
	// NapiNPProtocol_StopServer: latch STOP and destroy every server-side node in
	// list order; the burst rides the socket directly, like retail's SendTo.
	std::vector<PeerAddr> peers;
	for (const NapiNPConnection &conn : ctx.np_protocol.connection_list) {
		if (conn.type == NapiNPConnection::kTypeServerSide) peers.push_back(conn.peer);
	}
	for (const PeerAddr &peer : peers) {
		for (NapiNPConnection &conn : ctx.np_protocol.connection_list) {
			if (conn.peer == peer && conn.type == NapiNPConnection::kTypeServerSide) {
				latch_disconnect_event(conn,
						make_disconnect_event(1, 9, 0, 0, "", 0, "NP.C:SH:STOP"));
				break;
			}
		}
		std::vector<std::vector<uint8_t>> goodbye;
		if (!inmatch::destroy_connection(ctx, peer, &goodbye)) continue;
		for (const std::vector<uint8_t> &datagram : goodbye)
			socket.send_to(peer, datagram.data(), datagram.size());
		state.host_owner.peers.erase(peer);
		state.host_owner.pending_session_messages.erase(peer);
		state.host_owner.pending_session_datagrams.erase(peer);
	}
	ctx.np_protocol.host_running = 0;
}

// The editor Stop/Start rewind: the kernel's own restore, then a fresh
// HostClient view re-fed from the restored world's pools (the static, spawn,
// organic and marker batches) and the minimap's initial scan re-armed.
bool HostRole::reset_to_baseline(SessionError &error) {
	mission::MissionKernel &kernel = *kernel_;
	if (!kernel.restore_baseline()) {
		error = {SessionErrorCode::TickFailed, "mission baseline is unavailable"};
		return false;
	}
	if (state.client_runtime) {
		state.host_loop.clear();
		make_client_runtime(state.host_owner.ctx.config.game_type);
		// The replacement local client starts with zero score. Reset the
		// matching sender cache too, or an equal post-retry award is suppressed.
		// [orig: Server_PlayerAdd @0x51D50A (slot+0x14C zeroed at install; also
		//  @0x51CD00 before the whole-slot memset @0x51CD06);
		//  Server_UpdateCaptureZoneProximity @0x50874E..0x508790 (GetFieldPlusOne
		//  0x1C @0x508749, cmp/store slot+0x14C @0x50874E/@0x50875F, the 0x81
		//  send @0x508790)]
		for (NapiNPConnection &conn : state.host_owner.ctx.np_protocol.connection_list)
			if (conn.link.transport == &state.host_loop)
				conn.reply.score_delta_sound_value = 0;
		replication::ClientReplicaPipeline &view = state.client_runtime->view();
		view.apply(0x10, opennova::encode_static_entity_batch(
				replication::build_pool2_static_batch(kernel.world)));
		view.apply(0x0D, opennova::encode_pool_spawn_batch(
				replication::build_pool1_spawn_batch(kernel.world)));
		view.apply(0x0C, opennova::encode_organic_spawn_batch(
				replication::build_pool0_organic_batch(kernel.world)));
		view.apply(0x20, opennova::encode_pool3_sync_batch(
				replication::build_pool3_marker_batch(kernel.world)));
		inmatch::Server_RearmMinimapInitialScan(state.host_owner.ctx);
	}
	return true;
}

} // namespace opennova::inmatch
