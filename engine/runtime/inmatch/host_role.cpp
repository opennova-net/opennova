#include <runtime/inmatch/host_role.h>
#include <runtime/inmatch/host_settings.h> // mission_session_title (the session title)

#include <base/gameprofile/game_type.h>
#include <base/gameprofile/gameprofile.h> // the title's version text (the own 0x0B's dword)
#include <base/io/log.h>
#include <base/io/perf_clock.h>
#include <base/vfs/vfs.h> // vfs_country_code (a held player's PLAYER report's GCC)
#include <formats/mission/bms.h>
#include <formats/playersav/player_sav.h>
#include <net/npwire/ingame_encode.h>
#include <net/npwire/ingame_message_id.h>
#include <net/npwire/protocol_message.h>
#include <net/npwire/session_hello.h> // DisconnectEvent
#include <net/title/title_identity.h> // title_in_match (the rules word's bits, DFX2-18)
#include <runtime/inmatch/game_config_block.h> // kSinglePlayerMpAttrib
#include <runtime/inmatch/host_session.h>
#include <runtime/inmatch/joiner_connection.h> // build_mission_status (the own C2S 0x0B)
#include <runtime/inmatch/loadout_submit.h> // resolve_loadout_submit_combo (the own 0x2F pair)
#include <runtime/inmatch/mission_rotation.h>
#include <runtime/inmatch/null_datagram_socket.h>
#include <runtime/inmatch/server_chat.h> // Server_BroadcastDialogLine
#include <runtime/inmatch/server_console.h>
#include <runtime/inmatch/server_initial_state.h>
#include <runtime/inmatch/server_log_recorder.h>
#include <runtime/inmatch/server_message_dispatch.h>
#include <runtime/inmatch/server_loadout_grant.h> // PlayerSlot_SeedOwnLoadoutBuffer (the first round init)
#include <runtime/inmatch/server_powerup.h> // ServerRemoteWeaponTables
#include <runtime/inmatch/server_spawn.h> // Server_SpawnHostSlotBody (the first round init)
#include <runtime/inmatch/server_tick.h>
#include <runtime/mission/mission_kernel.h>
#include <runtime/replication/entity_wire_bridge.h>
#include <runtime/world/player_loadout.h>
#include <runtime/world/collision.h> // the mission start's proximity build (init_local_player_at_mission_start)

#include <string>
#include <string_view>
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

// host_session_pump's own-client hook: the host player's requests, which retail's
// receive pump handles first, inside the server tick after its per-player walk and
// after the previous entity update's sends (D-NET-686).
void drain_own_client(void *context) {
	static_cast<HostRole *>(context)->drain_host_client_gameplay_requests();
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
	session_create_ = CreateSessionResult::Created;
	// The session title's override table is the bring-up's mission text's (the mount's).
	staged_bringup_.host_cfg.text_override = staged_bringup_.mission_text.override_table.get();
	if (staged_bringup_.next_mission)
		bring_up_next_mission(staged_bringup_);
	else
		bring_up(staged_bringup_);
	return false;
}

void HostRole::set_item_catalog(
		std::shared_ptr<const replication::ItemReplicationCatalog> catalog) {
	item_catalog_ = std::move(catalog);
	if (state.client_runtime && item_catalog_)
		state.client_runtime->view().set_item_catalog(item_catalog_);
}

// The is_in_session fact the mission-data block's rules word tests: a socket
// session (a LAN or NovaWorld host, a serve host) is in session, and the
// shell's socketless single player is not, as retail's single player creates
// its session with the network type 0, which leaves is_in_session clear;
// start_host_session's rules.mp_session reads the same fact (D-NET-405).
// [orig: CNapiGameSession_InitRandomSeedOrRequest @0x51E944 - `cmp
//  g_NapiNPCtx.is_in_session, 0` / `jz` @0x51E944..0x51E94B;
//  SinglePlayer_StartMission @0x561AF0 - CNapiNetwork_SetNetworkType(0)
//  @0x561BD4, CNapiGameSession_CreateSession @0x561E65; CNapiNetwork_SetNetworkType
//  @0x4C4A50 - is_in_session = (type in 1..3) @0x4C4A85;
//  CNapiGameSession_CreateSession @0x4C97C0 keeps it clear @0x4C9817]
static bool rules_in_session(const inmatch::HostConfig &host_cfg) {
	return host_cfg.socket_mode != inmatch::SocketMode::Socketless;
}

// The shared bring-up preamble: a fresh loopback + owner over the kernel's
// world and mission, the rule words the world reads at tick time, and the
// is_in_session fact.
void HostRole::reset_state(const inmatch::GameConfig &config, bool serve_and_play, bool in_session,
		const TitleIdentity *title) {
	mission::MissionKernel &kernel = *kernel_;
	state.client_runtime.reset();
    local_round_reset_seen_ = kernel.local.round_reset_revision;
	state.host_loop = replication::LoopbackChannel{};
	state.host_owner = inmatch::HostOwner{};
	state.host_owner.serve_and_play = serve_and_play;
	state.host_owner.ctx.world = &kernel.world;
	state.host_owner.ctx.mission = &kernel.mission;
	state.host_owner.ctx.mission_text_loaded = false;
	state.host_owner.ctx.rotation = rotation_;
	map_change = HostMapChange{};
	apply_rule_words(config, serve_and_play, in_session, title);
}

void HostRole::apply_rule_words(const inmatch::GameConfig &config, bool serve_and_play,
		bool in_session, const TitleIdentity *title) {
	mission::MissionKernel &kernel = *kernel_;
	// The process's loadout name list, whose first name every body the session spawns takes
	// as its equipped weapon (world::player_spawn_equipped_default) [orig:
	//  g_LoadoutWeaponNameList @0x8333F8; PlayerClass_InitEntity @0x4B1100..0x4B1116].
	kernel.world.loadout_weapon_names = &client_process_->loadout_weapon_names;
	// The process's spawn buffer, which no start clears (world::SpawnLoadoutBuffer)
	// [orig: g_SpawnLoadoutBuffer @0x24D4E00].
	kernel.local.loadout.process_buffer = &client_process_->spawn_loadout_buffer;
	kernel.world.rules.fat_bullets = config.fat_bullets != 0;
	kernel.world.rules.one_shot_kill = config.one_shot_kill != 0;
	// The mission-data block's unlimited-vehicles word, rebuilt from the host
	// config at every mission start [orig: Client_BuildMissionDataRequestBlock
	// @0x51E8C5..0x51E8CB from dword_24D2258 = unlimitedVehicles_4D0].
	kernel.world.rules.vehicle_respawns = config.unlimited_vehicles != 0;
	// The rules word every in-match reader tests (g_RulesFlags @0x24D1E34) is the
	// +0x2C dword of the mission-data block the authority builds at every mission
	// start: the host config's mpattrib word IN a session, the literal 0x4204 out
	// of one (SP: TEAM_CHOOSE 0x4, NoFriendlyFire 0x200 and 0x4000), and the S2C
	// 0x64 +44 dword on a joiner. The SP launcher's own mpattrib (0x3A06) never
	// reaches it [orig: CNapiGameSession_InitRandomSeedOrRequest @0x51E94B..
	//  0x51E957; the scope-zero floor's `test g_RulesFlags,10000h` @0x4dbd15]. A DFX2
	// authority then ORs the scope-zero bit 0x10000 into the word, in a session or out
	// (DFX2-18) [orig: CNapiGameSession_InitRandomSeedOrRequest @0x51b89e (dfx2.exe)].
	const uint32_t rules_word = (in_session ? config.mp_attributes : GameConfig::kSpRulesWord) |
			title_in_match(title).rules_word_bits;
	kernel.world.rules.mpattrib = rules_word;
	kernel.world.rules.hit_feedback = config.hit_feedback;
	kernel.world.rules.auto_scope_zero =
			(rules_word & GameConfig::kMpAttribAutoScopeZero) != 0;
	kernel.world.rules.no_friendly_fire =
			(rules_word & GameConfig::kMpAttribNoFriendlyFire) != 0;
	// The is_mp_session_peer bit is the is_client half of the connection
	// mode: set for the SP/listen HostClient, clear for a HostOnly dedicated
	// host. [orig: g_NapiNPCtx +0x64; napi_np_server_ctx.h connection modes]
	kernel.world.rules.mp_session_peer = serve_and_play;
	// The session's sniper-zoom rule (byte_A821F0): the authority's config value in a
	// session, 0 out of one [orig: Game_ApplySessionSettingsToGlobals - the reset
	//  @0x552284, the session test @0x55222A / @0x55228B, the authority test
	//  @0x55228D, the stamp @0x5522A4..0x5522B9].
	kernel.world.rules.allow_sniper_scope_zoom = in_session && config.allow_sniper_scope_zoom;
	// The fill's value: the rule on an in-session peer, else the config's own
	// [orig: WeaponSlot_InitFromDef @0x53EF07..0x53EF25].
	kernel.world.rules.sniper_zoom_fill = in_session && serve_and_play
			? kernel.world.rules.allow_sniper_scope_zoom
			: config.allow_sniper_scope_zoom;
	// Out of a session a fresh start's session settings copy the current
	// profile's +0x564 into the difficulty word, which the player's spawn
	// raises read before the mission start resets it (D-PWR-6); the SP
	// restart's start skips the copy [orig: Game_ApplySessionSettingsToGlobals
	//  @0x551F6F..0x551F75, called from SinglePlayer_StartMission @0x561C28 and
	//  Game_StartMission @0x524662, which `cmp esi, ebx` @0x52465E skips on the
	//  restart's start (Game_RestartRoundSP pushes 1 @0x5263D9)].
	// The restart's spawn raises read the word the previous start left: the same mission's
	// mission-start word (D-PWR-8) [orig: Game_StartMission - the 0 @0x525CDD, the objective
	//  Co-op's -1 @0x525CFA].
	if (!kernel.world.rules.mp_session)
		kernel.world.rules.difficulty = kernel.restart_boot()
				? world::mission_start_difficulty(config.game_type)
				: config.profile_difficulty;
}

// A single player's restart (Game_RestartRoundSP -> Game_StartMission(1)) runs no round-state
// init, so its UP-TIME stamp is the first start's, which ours' restart boot takes back from the
// process words over the one its fresh create stamped; every other start's create keeps its
// own, which the process words remember.
// [orig: Game_RestartRoundSP @0x5263A0 - Game_StartMission(1) @0x5263DB, no
//  Server_InitNewRoundState; its stamp @0x51CBA0 from SinglePlayer_StartMission @0x561C23 and
//  CNapiGameSession_BuildAndCreateSession @0x5695B3 alone]
void HostRole::keep_round_state_init_tick() {
	NapiNPServerCtx &ctx = state.host_owner.ctx;
	if (kernel_ != nullptr && kernel_->restart_boot())
		ctx.round_state_init_tick = client_process_->round_state_init_tick;
	client_process_->round_state_init_tick = ctx.round_state_init_tick;
}

// The single player's start fills the spawn buffer ahead of its mission load: the mission's
// kit (ours' kernel promotes the chunk at the table load, after this), else the profile's
// single-player page (the shell's, which holds the profile), else the static page, which this
// writes; a start in a session fills nothing here (a peer's mission start copies its side's
// page; a dedicated start nothing at all).
// [orig: Game_ApplySessionSettingsToGlobals @0x551500 - `jz loc_5519A2` @0x5517A7 out of a
//  session, the fill @0x5519A2..0x5519E4 (the static page @0x5519DA), from
//  SinglePlayer_StartMission @0x561AF0 (the call @0x561C28); Game_StartMission's restart copy
//  @0x524683..0x5246C8 (the static page @0x5246BE)]
void HostRole::fill_spawn_buffer_out_of_session() {
	mission::MissionKernel &kernel = *kernel_;
	if (kernel.world.rules.mp_session) return;
	(void)seed_single_player_kit_from_profile(kernel.world, nullptr, kernel.local.loadout);
}

// Every body the session spawns from here on (a mid-match join, a map change's
// kept slots, whose context the session keeps) binds its items.def row at its
// spawn, through the kernel the role is bound to at that time; the bodies the
// bring-up spawned are the boot's later steps' (D-NET-395).
// [orig: Entity_SpawnFromAnimSlotProperty @0x43C390 -> Entity_InitFromModel @0x40DC30]
// The vehicle bays' C2S 0x40 spawns through the same kernel (D-NET-332)
// [orig: NapiNPServerMsg_HandleVehicleSpawnRequest @0x51C4C0 -> Entity_SpawnDeployable
//  @0x51C2B0 (the call @0x51C69E)].
void HostRole::install_spawned_body_bind() {
	state.host_owner.ctx.bind_spawned_body = [this](world::EntityHandle body) {
		kernel_->bind_spawned_body(body);
	};
	state.host_owner.ctx.deployable_spawner = [this](world::World &, uint16_t item_id,
			uint8_t team, const int32_t position[6]) {
		return kernel_->spawn_deployable(item_id, team, position);
	};
}

// The HostClient replica pipeline (recv-fold only, 0x0C suppressed): it folds
// the loopback's whole-world 0x0A into the ClientState the host's own view
// renders from.
void HostRole::make_client_runtime(uint32_t game_type) {
	mission::MissionKernel &kernel = *kernel_;
	state.client_runtime = std::make_unique<inmatch::ClientRuntime>(state.host_loop);
	state.client_runtime->set_input_event_queue(&input_events_); // its console's records
	// The 0x4C report's count runs on across the runtimes [orig: g_NetQualityReportTimer
	//  @0xA85B84, process-lived; Client_ProcessNetworkFrame @0x42C239 / @0x42C246].
	state.client_runtime->use_process_state(client_process_);
	state.host_owner.ctx.host_client = state.client_runtime.get();
	// Its mission start ends with the /PROFILE roster pass, then its C2S 0x0B
	// (D-NET-NEW-L4-5) [orig: Game_StartMission - CServerLog_WritePlayerNameRecord
	//  @0x52617B ahead of CNapiNetwork_QueueReliableMessage @0x52624B].
	// The own connection beside every baseline the kernel takes (reset_to_baseline).
	kernel.baseline_captured = [this] {
		own_connection_at_baseline_.reset();
		for (const NapiNPConnection &conn : state.host_owner.ctx.np_protocol.connection_list)
			if (conn.type == NapiNPConnection::kTypeClientSide) own_connection_at_baseline_ = conn;
	};
	kernel.net_mission_start_end = [this] {
		const NapiNPServerCtx &ctx = state.host_owner.ctx;
		if (ctx.logs.profile != nullptr && kernel_ != nullptr)
			ctx.logs.profile->write_mission_roster(ctx, kernel_->world);
		send_own_mission_status();
	};
	// A fresh view counts its 0x0F / 0x50 folds from zero.
	track_seen_ = ReplicaTrackSeen{};
	state.client_runtime->set_profile(kernel.world.profile);
	state.client_runtime->view().set_game_type(game_type);
	state.client_runtime->view().set_mp_session(kernel.world.rules.mp_session);
	// The title the session speaks: the host's own view reads its loopback bodies by it.
	state.client_runtime->view().set_title(state.host_owner.ctx.np_protocol.identity);
	if (item_catalog_)
		state.client_runtime->view().set_item_catalog(item_catalog_);
}

GameConfig singleplayer_game_config(uint32_t game_type,
		const playersav::ProfileRecord *profile) {
	inmatch::GameConfig config;
	config.server_name = "SINGLEPLAYERGAME";
	config.mp_attributes = static_cast<uint32_t>(kSinglePlayerMpAttrib);
	config.max_players = 1;
	config.game_type = game_type;
	// The difficulty word, the S2C 0x08 block's seventh byte: the session
	// settings copy the profile's +0x564 into it (@0x551F6F..0x551F75), but the
	// mission start overwrites it out of a session before any serializer reads
	// it, so the byte is the word that start leaves (a session's is game.cfg's
	// mp_difficulty, host_config.cpp). Our SP stream queues at the bring-up,
	// ahead of the mission start [orig: Game_StartMission @0x525CDD / @0x525CFA;
	// ServerConfig_SerializeToPacket's live read @0x505D1C, from
	// Server_SendInitialGameStateToPlayer @0x51BF95 in Server_TickUpdate].
	config.config_bytes[6] = static_cast<uint8_t>(world::mission_start_difficulty(game_type));
	if (profile != nullptr) {
		// Outside a session the restriction words come from the current
		// profile record: +0x548 -> g_SessionNoCharAbilities, +0x54C ->
		// g_SessionNoWeaponRecoil, +0x554 -> g_SessionNoCrossHairSpread,
		// +0x550 -> g_SessionNoScopeDrift [orig:
		// Game_ApplySessionSettingsToGlobals @0x551F15..0x551F3F], and +0x564
		// -> the difficulty word (g_SessionDifficulty @0x551F6F..0x551F75).
		// The local profile's name (g_LocalProfileName, the body's Name at the mission
		// start) is the current profile record's +4, as the session's is
		// [orig: Game_ApplySessionSettingsToGlobals @0x551680..0x551695, called from
		//  SinglePlayer_StartMission @0x561C28].
		config.player_name = profile->name.substr(0, 31);
		config.no_char_abilities = profile->sp_no_char_abilities;
		config.no_weapon_recoil = profile->sp_no_weapon_recoil;
		config.no_crosshair_spread = profile->sp_no_crosshair_spread;
		config.no_scope_drift = profile->sp_no_scope_drift;
		config.profile_difficulty = profile->sp_difficulty;
	}
	return config;
}

// [orig: SinglePlayer_StartMission @0x561af0]
void HostRole::bring_up_singleplayer(const playersav::ProfileRecord *profile) {
	mission::MissionKernel &kernel = *kernel_;
	const inmatch::GameConfig config = singleplayer_game_config(
			game_type::for_mission_attribs(kernel.mission.header.attrib_flags), profile);
	// The create's reset stops a server an earlier error-arm exit left unstopped; the start
	// closed any socket first, so nothing reaches the wire [orig: SinglePlayer_StartMission -
	// SetTransportMode(1) @0x561BEB, CNapiGameSession_CreateSession called @0x561E65 ->
	// ResetActiveSession @0x4C97CC]
	stop_unstopped_server(client_process_->unstopped_server, ServerLogs{}, nullptr, nullptr);
	reset_state(config, /*serve_and_play=*/true, /*in_session=*/false, /*title=*/nullptr);
	state.host_owner.host_loopback = &state.host_loop;
	// The authority's start steps the process's file-transfer session words, which the context
	// reads [orig: Game_StartMission @0x524360 (the add @0x5244B4, the store @0x5244C6)].
	state.host_owner.ctx.file_transfer_session = &client_process_->file_transfer_session;
	state.host_owner.ctx.client_disconnect_latch = &client_process_->disconnect_latch;
	client_process_->file_transfer_session.start_mission();
	inmatch::HostConfig host_cfg;
	host_cfg.config = config;
	host_cfg.socket_mode = inmatch::SocketMode::Socketless;
	host_cfg.serve_and_play = true;
	host_cfg.host_key = state.host_key;
	host_cfg.host_start_tick = state.host_start_tick;
	host_cfg.session_seed_id = state.session_seed_id;
	session_create_ = inmatch::start_host_session(state.host_owner, host_cfg);
	if (session_create_ == CreateSessionResult::ProcessExit) return; // the process ends
	// The authority's mission start sets the report clocks loading (D-NET-NEW-L9-22).
	inmatch::gate_clocks_mission_start(state.host_owner.ctx);
	keep_round_state_init_tick();
	fill_spawn_buffer_out_of_session();
	install_spawned_body_bind();
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
	if (host_cfg.client_process != nullptr) client_process_ = host_cfg.client_process;
	// A server an earlier error-arm exit left unstopped stops first, over this session's socket
	// (the host screen opened it), ahead of the create
	// [orig: PreMenu_HostSetup @0x5691F0 - CNapiGameSession_ResetActiveSession called @0x5691F5,
	//  ahead of CNapiGameSession_BuildAndCreateSession @0x5694D0 (called @0x56A46A);
	//  UI_HandleHostSessionStart's SetTransportMode(3) @0x556EA6 before; the create's own reset
	//  @0x4C97CC where no pre-game menu runs]
	// A NovaWorld host's setup runs once its session hosts (state 6) and as the authority, so each
	// held player's teardown sends ClientHostPlayerRemoved on it (D-NET-NEW-L9-21)
	// [orig: UI_DispatchScreenEvent @0x54E6A0 - HostDialog_StartSession called @0x54F321
	//  (HostDialog_StartSession @0x558760, its connection mode @0x55878F),
	//  CNapiGameSession_ConnectOrHost called @0x54F33B, the Premenu scene pushed @0x54F371 only
	//  on its success]
	// Each held player's teardown on this authority: the NovaWorld session's removal, then the
	// slot clock to phase 0 and its PLAYER report under what this host's setup left (its game type,
	// the CC.BIN its tick init read again), on a NovaWorld host (D-NET-NEW-L9-22)
	// [orig: Server_HandlePlayerDisconnect @0x51B5C0 - the removal @0x51B5F3, CServerTick_SetState(0)
	//  @0x51B690, Server_SendPlayerMetricsToGate @0x51B696].
	const std::string reset_gcc = vfs_country_code(host_cfg.game_root);
	stop_unstopped_server(client_process_->unstopped_server, host_cfg.logs, socket_,
			host_cfg.local_address_known ? &host_cfg.local_address : nullptr,
			[this, &host_cfg, &reset_gcc](NapiNPConnection &slot) {
				if (client_process_->nwu_player_removed)
					client_process_->nwu_player_removed(slot.reply.player_slot);
				Server_ReportHeldPlayerLeave(client_process_->nwu_gate_metrics,
						host_cfg.network_type == NetworkType::NovaWorld, host_cfg.config.game_type,
						reset_gcc,
						host_cfg.clock_ms ? host_cfg.clock_ms() : host_clock_ms(state.host_owner),
						slot);
			});
	reset_state(host_cfg.config, host_cfg.serve_and_play, rules_in_session(host_cfg),
			host_cfg.title);
	state.host_owner.host_loopback = &state.host_loop;
	NapiNPServerCtx &ctx = state.host_owner.ctx;
	// The authority's mission start steps the process's file-transfer session words, which the
	// context reads [orig: Game_StartMission @0x524360 (the add @0x5244B4, the store @0x5244C6)].
	ctx.file_transfer_session = &client_process_->file_transfer_session;
	ctx.client_disconnect_latch = &client_process_->disconnect_latch;
	client_process_->file_transfer_session.start_mission();
	// The fresh context stands inside the mission load, which holds the spawn gate
	// until its end (finish_mission_load) [orig: Game_StartMission @0x524398].
	ctx.net_spawn_suspended = 1;
	inmatch::set_server_text(ctx, bringup.server_text);
	ctx.terrain_til_data = bringup.terrain_til_data; // S2C 0x45 terrain-tile load source (empty => skipped, §5.37)
	ctx.mission_text_loaded = bringup.mission_text.loaded;
	ctx.mission_briefing3 = bringup.mission_text.briefing3();
	ctx.mission_briefing2 = bringup.mission_text.briefing2();
	ctx.mission_text_title = bringup.mission_text.get_string("info", "title");
	inmatch::install_mission_location_names(ctx, kernel.world, kernel.mission, bringup.mission_text);
	// A host session's start zeroes the round count [orig: UI_HandleHostSessionStart
	// @0x556EAB; HostDialog_StartSession @0x5587BD].
	if (rotation_ != nullptr) rotation_->round_count = 0;
	// The create sets the mission start's session title ahead of the mission-data block
	// that copies it, the mount's text-override table answering first (host_cfg.text_override)
	// [orig: Game_ApplySessionSettingsToGlobals @0x552145..0x55219E;
	//  CNapiGameSession_InitRandomSeedOrRequest @0x51E983].
	session_create_ = inmatch::start_host_session(state.host_owner, host_cfg);
	if (session_create_ == CreateSessionResult::ProcessExit) return; // the process ends
	// The mission start that follows the create sets both report clocks loading
	// (D-NET-NEW-L9-22) [orig: Game_StartMission @0x524360 - CServerTick_SetState(1) @0x5244A4 /
	//  @0x5244AF, after the create's constructs].
	inmatch::gate_clocks_mission_start(ctx);
	keep_round_state_init_tick();
	fill_spawn_buffer_out_of_session();
	install_spawned_body_bind();
	if (host_cfg.serve_and_play) {
		make_client_runtime(host_cfg.config.game_type);
		kernel.local.reset_local_player_input_to_player_facing();
	} else {
		state.client_runtime.reset();
	}
	// Retail's session host stands its session up before its mission load and pumps it
	// through the load, so a remote's hello, connect and admission legs are answered
	// inside the load; ours's session exists from here, ahead of the PreMission pass,
	// where retail's last in-load pumps fall: all of the load's pumps run here, each
	// stepping the connections' send clocks as retail's do (D-NET-116).
	// [orig: Server_PumpNetworkTransport @0x4FD960 from Game_StartMission @0x52589D /
	//  @0x5258E5 / @0x525A5C / @0x525AC5, ahead of the EventTrigger_UpdateAllWithFlag2
	//  call @0x525B86; the session create ahead of the load, CNapiGameSession_CreateSession
	//  @0x4C97C0 from CNapiGameSession_BuildAndCreateSession @0x56997D (the PreMenu's
	//  MultiPlayer_JoinSessionStateMachine @0x56A46A), before GameLoop_Init's
	//  Game_StartMission call @0x526377]
	pump_load(1 + load_window_pumps());
}

// See host_role.h. The map change's next mission start up to its terrain, on the kept
// session alone: the old World is gone (begin_host_map_change) and the next one does not
// exist yet, so nothing here reads one. The load's pumps then run while it loads, where
// retail's do: the reset's 0x25s leave at the first whose send clock is open, and a kept
// joiner's reload legs (its C2S 0x48 / 0x47 / 0x33) are answered from its kept slot at the
// next, ahead of the load end's S2C 0x7B.
// [orig: Game_StartMission's authority arm @0x524492..0x52452B; the two receives ahead of
//  the block, the authority's client-protocol receive @0x524813 and the gate session's
//  update @0x52481D, run no server handler]
void HostRole::begin_next_mission_load(HostBringup head) {
	const inmatch::HostConfig &host_cfg = head.host_cfg;
	if (host_cfg.client_process != nullptr) client_process_ = host_cfg.client_process;
	NapiNPServerCtx &ctx = state.host_owner.ctx;
	// [orig: Game_StartMission @0x524360 (the add @0x5244B4, the store @0x5244C6), as the
	//  bring-up's]
	ctx.file_transfer_session = &client_process_->file_transfer_session;
	ctx.client_disconnect_latch = &client_process_->disconnect_latch;
	// The report clocks loading, just ahead of the add (D-NET-NEW-L9-22) [orig: Game_StartMission
	//  - CServerTick_SetState(1) @0x5244A4 / @0x5244AF].
	ctx.tick_count_ms = inmatch::host_clock_ms(state.host_owner);
	inmatch::gate_clocks_mission_start(ctx);
	client_process_->file_transfer_session.start_mission();
	// The own client's loopback starts the load empty; its view is rebuilt over it at the
	// bring-up (bring_up_next_mission), so what the load's pumps send it reaches the new one.
	state.host_loop.clear();
	inmatch::set_server_text(ctx, head.server_text);
	// The mission text [orig: Game_StartMission - TextResource_LoadMissionTextBin @0x5247CE].
	ctx.mission_text_loaded = head.mission_text.loaded;
	ctx.mission_briefing3 = head.mission_text.briefing3();
	ctx.mission_briefing2 = head.mission_text.briefing2();
	ctx.mission_text_title = head.mission_text.get_string("info", "title");
	// The next mission's start sets the session title again, over a SetServerName's, the
	// mount's override table answering first, ahead of the rebuilt mission-data block
	// [orig: Game_ApplySessionSettingsToGlobals @0x552145..0x55219E;
	//  CNapiGameSession_InitRandomSeedOrRequest @0x51E983]. The integrity tables stay the
	// previous mission's until the load's def loads rebuild them (bring_up_next_mission)
	// [orig: Game_StartMission - AmmoDef_LoadAll @0x525495, WeaponDefs_LoadFile @0x5254BD].
	head.host_cfg.text_override = head.mission_text.override_table.get();
	head.host_cfg.config.integrity_sources = ctx.config.integrity_sources;
	inmatch::continue_host_session(state.host_owner, head.host_cfg);
	// The per-slot reset, after the mission-data block [orig: Game_StartMission
	//  @0x5248AC..0x5248BC].
	if (ctx.is_in_session != 0 && ctx.is_authority != 0)
		inmatch::Server_ResetAllPlayerSlotFrameState(ctx);
	// The chat rings start every mission empty: CHAT GET and the status page list none of
	// the last map's lines [orig: Game_StartMission @0x524A10 -> Chat_ClearAllChannels
	//  @0x497D30].
	server_console_clear(ctx);
	next_mission_head_run_ = true;
	window_pumps_run_ = 0;
	// Then the window's first pumps over the reset slots and the rebuilt streams, the spawn
	// gate still held (D-NET-116), around the BMS header's second read [orig: Game_StartMission
	//  - Server_PumpNetworkTransport @0x524A63, BMS_LoadAndValidateHeader @0x524F66,
	//  Server_PumpNetworkTransport @0x524F76].
	pump_load_stage();
	pump_load_stage();
}

void HostRole::pump_load_stage() {
	pump_load();
	++window_pumps_run_;
}

// The map change's bring-up, inside the next mission's kernel boot where a
// fresh session's starts, past the load's head and stages: the kept session onto
// the new World (the context keeps its connections; the per-mission tables and the
// rule words are the next map's), the CRT stream carried over, the def loads'
// integrity tables, the host's own client view rebuilt over the loopback, then the
// window's remaining pumps. The round init over the kept slots waits for the
// PreMission pass (init_round_after_premission).
// [orig: Game_StartMission - AmmoDef_LoadAll @0x525495, WeaponDefs_LoadFile @0x5254BD,
//  Entity_InitAllFromModels @0x52567F, then the server pumps (Server_PumpNetworkTransport
//  @0x4FD960) @0x52589D, @0x5258E5, @0x5259EC, @0x525A5C and @0x525AC5 ahead of the
//  EventTrigger_UpdateAllWithFlag2 call @0x525B86]
void HostRole::bring_up_next_mission(const HostBringup &bringup) {
	mission::MissionKernel &kernel = *kernel_;
	const inmatch::HostConfig &host_cfg = bringup.host_cfg;
	NapiNPServerCtx &ctx = state.host_owner.ctx;
	state.client_runtime.reset();
	local_round_reset_seen_ = kernel.local.round_reset_revision;
	ctx.world = &kernel.world;
	ctx.mission = &kernel.mission;
	ctx.rotation = rotation_;
	ctx.host_client = nullptr;
	apply_rule_words(host_cfg.config, host_cfg.serve_and_play, rules_in_session(host_cfg),
			host_cfg.title);
	kernel.world.crt_rand = map_change.crt_rand;
	ctx.terrain_til_data = bringup.terrain_til_data;
	inmatch::install_mission_location_names(ctx, kernel.world, kernel.mission, bringup.mission_text);
	// A first load's second chat clear, past the stages' pumps, so a line a joiner sent
	// while the map loaded is gone, then the def loads' integrity tables [orig:
	// Game_StartMission - `is_first_load` (= !is_restart) @0x5243A6, its test @0x525425,
	// Chat_ClearAllChannels @0x52546D, AmmoDef_LoadAll @0x525495, WeaponDefs_LoadFile
	// @0x5254BD].
	server_console_clear(ctx);
	ctx.config.integrity_sources = host_cfg.config.integrity_sources;
	inmatch::configure_continued_session_world(state.host_owner, host_cfg);
	map_change.torn_down = false;
	if (host_cfg.serve_and_play) make_client_runtime(host_cfg.config.game_type);
	// Then the window's pumps the head and the stages left, over the new World, the spawn
	// gate still held (D-NET-116).
	pump_load(load_window_pumps() - window_pumps_run_);
	next_mission_head_run_ = false;
	window_pumps_run_ = 0;
}

// The map change's round init, after the kernel boot's PreMission pass and
// the round counters' reset: a new entity for every kept slot, the host's own
// included, each bound to its items.def row at its spawn as the boot's steps
// bound the bodies that existed then (the context's bind_spawned_body:
// MissionKernel::bind_spawned_body, the traits, the sound-profile pair, the
// organic weapons, the collision instance) [orig: Entity_SpawnFromAnimSlotProperty
// @0x43C390 -> Entity_InitFromModel @0x40DC30], then on serve-and-play Player_InitPlayer's
// weapon leg over the host's own new entity, its client's load-time stream
// over the loopback (as a fresh session's bring-up queues it) and its look
// heading.
// [orig: Game_StartMission @0x524360 -- the EventTrigger_UpdateAllWithFlag2
//  call @0x525B86, the Server_ResetRoundCounters call @0x525B90, the tick
//  zeroed @0x525B9F, the Server_InitAllPlayerEntitiesForRound call
//  @0x525BAF, the Player_InitPlayer call @0x525BBC]
void HostRole::init_round_after_premission() {
	mission::MissionKernel &kernel = *kernel_;
	kernel.boot_trace.emplace_back("round_init");
	init_all_player_entities_for_round(*this);
	if (!staged_bringup_.host_cfg.serve_and_play) return;
	world::local_loadout_rebuild(kernel.world, kernel.local.loadout, kernel.local.weapon,
			kernel.local.inventory, kernel.local.inventory_valid, /*select_spawn_default=*/true);
	(void)tick_connections(state.host_owner.ctx, /*elapsed_ms=*/0, state.host_owner.now_tick);
	kernel.local.reset_local_player_input_to_player_facing();
}

void HostRole::seed_own_slot_at_first_round_init() {
	if (kernel_ == nullptr) return;
	for (NapiNPConnection &conn : state.host_owner.ctx.np_protocol.connection_list)
		if (conn.type == NapiNPConnection::kTypeClientSide)
			PlayerSlot_SeedOwnLoadoutBuffer(conn, kernel_->world);
	// A Serve Only host's own row's hidden body, slot 0's at this round init (D-NET-352).
	(void)Server_SpawnHostSlotBody(state.host_owner.ctx, kernel_->world);
}

// See host_role.h. Player_InitPlayer re-stamps the local body's class word and animSlot by
// its team from the session start's profile words (the row body of a Serve Only host
// included: it spawned with the side requirement's class), names it after the local profile
// and clears its hidden bit; the mission start's own local-player block then clears the
// indoors bit and the medic-revive latch, clears the hidden bit, rebuilds every proximity list
// (the unhidden body in the person table; the rebuild runs with no local body too) and sets
// the hidden bit again, so every host's local body leaves the mission start hidden. A playing
// host's own join then clears it inside frame 1's receive pump and its 0x0A writer sets it
// again for an undeployed slot, both ahead of the slot lists' count; a Serve Only host's row
// body, which no join reaches, stays hidden.
// [orig: Game_StartMission @0x525BAF / @0x525BBC; Player_InitPlayer @0x4E15F0 - the class and
//  animSlot @0x4E1809..0x4E1843, the Name from g_LocalProfileName @0x4E184F..0x4E186A,
//  `Flags &= ~1` @0x4E19C2; Game_StartMission - the null tests @0x525C70 / @0x525C9A,
//  `and [eax+24h], 0FF7FFFFFh` @0x525C74, +0x1E0 = 0 @0x525C81, `and [eax+24h], 0FFFFFFFEh`
//  @0x525C8C, Entity_BuildAllProximityLists @0x525C90 (outside both tests), `or [eax+24h],
//  edi` @0x525C9E (edi = 1 from @0x5255B9); Server_OnPlayerJoin's clear @0x51A7DA and its 0x0A
//  @0x51A82D -> NetPacket_WritePlayerState @0x4FF7B8; Server_BuildEntitySlotLists @0x4F9815 /
//  @0x4F9820]
void HostRole::init_local_player_at_mission_start() {
	world::World &world = kernel_->world;
	world::Entity *player = world.registry.get(world.cached.local_player);
	if (player != nullptr) {
		Player_InitPlayerSideWords(state.host_owner.ctx, *player);
		// The local profile's name: the session config's player name, the name a playing
		// host's own add gives its body too (g_LocalProfileName).
		player->name = state.host_owner.ctx.config.player_name;
		player->display_name = player->name;
		player->flags &= ~world::kEntityFlagCarried;
		player->flags &= ~world::kEntityFlagIndoors;
		player->medic_reviving = false;
		player->flags &= ~world::kEntityFlagCarried;
	}
	// A Serve Only host's Player_InitPlayer weapon leg over its row body (a playing host's runs
	// at the table load and the round init): the display list from the spawn buffer as the
	// process left it, the slot table, then the select of 195, which over an empty buffer (a
	// fresh process: a dedicated start fills nothing) finds no slot and leaves the body's
	// equipped byte, and over a buffer an earlier single player mission filled equips its slot
	// (D-NET-352).
	// [orig: Player_InitPlayer @0x4E15F0 - g_CurrentWeaponSlot = 195 @0x4E17FF, the display
	//  list from g_SpawnLoadoutBuffer @0x4E1872..0x4E1882, Player_SelectWeaponSlot @0x4E1995;
	//  Game_ApplySessionSettingsToGlobals @0x551500 (the dedicated start's jz @0x5517A7)]
	if (!state.host_owner.serve_and_play)
		world::local_loadout_rebuild(world, kernel_->local.loadout, kernel_->local.weapon,
				kernel_->local.inventory, kernel_->local.inventory_valid,
				/*select_spawn_default=*/true);
	if (world.collision != nullptr) world.collision->refresh_after_registry_change(world);
	if (player != nullptr) player->flags |= world::kEntityFlagCarried;
}

// The 0x0F handler's local-player block has no authority test, so on a playing host its
// own client writes the host's own body, the one g_LocalPlayerEntity the host and its
// client share, as a joiner's does (JoinerRole::apply_world_state_load): the Position, the
// Yaw (and the look yaw), the Pitch and the Roll from the 0x0F words, and the hidden bit
// cleared unless the death screen is up. The own join's 0x0F reaches the client at the
// head of the next frame, ahead of that frame's server tick, so a held host's body counts
// as a human at that tick's slot lists and the frame's entity update runs once (at the
// second frame's tick) before that frame's 0x0A writer hides the undeployed body again.
// [orig: NapiNPClientMsg_0x00F @0x42E200 - the g_LocalPlayerEntity test @0x42E3AF, the
//  stores @0x42E3B9..0x42E3F2 (g_LocalPlayerLookYaw @0x42E3E3), `cmp g_DeathScreenActive`
//  @0x42E3F5, `and [eax+24h], 0FFFFFFFEh` @0x42E403; its authority tests gate only the
//  spawn globals @0x42E369 and the ack burst @0x42E5B7; Game_ProcessMainFrame -
//  Client_ProcessNetworkFrame @0x526692 ahead of Server_TickUpdate @0x5266B6 (the slot
//  lists Server_BuildEntitySlotLists @0x4F97A0, called @0x51D89A), the gate
//  @0x52671C..0x52672A, Entity_UpdateAllEntities @0x52674B; Server_OnPlayerJoin's 0x0F
//  @0x51A869; NetPacket_WritePlayerState's re-hide @0x4FF7B8]
void HostRole::apply_own_world_state_load(mission::MissionKernel &kernel,
		const replication::ClientState &own) {
	world::World &world = kernel.world;
	world::Entity *local = world.registry.get(world.cached.local_player);
	world::AiEntity *local_ai = world.ai.for_handle(world.cached.local_player);
	if (local == nullptr || local_ai == nullptr) return;
	const replication::ClientWorldStateLoad &ws = own.world_state;
	local->position.set_q16({ws.pos_x, ws.pos_y, ws.pos_z});
	local->heading = ws.yaw_bam;
	local->pitch_bam = ws.pitch_bam;
	local->roll_bam = ws.roll_bam;
	local_ai->heading = ws.yaw_bam;
	local_ai->pitch = ws.pitch_bam;
	local_ai->roll = ws.roll_bam;
	local_ai->inf.target_heading = ws.yaw_bam;
	local_ai->inf.look_pitch = ws.pitch_bam;
	kernel.local.input.look_heading = ws.yaw_bam;
	kernel.local.input.look_pitch = ws.pitch_bam;
	if (!own.death_screen_active) {
		local->hidden = false;
		local->flags &= ~world::kEntityFlagCarried;
	}
}

// See host_role.h. The 0x0B is the side password (g_SidePassword, the game
// config's own entry, which a host's join menu never writes), the two map
// checksums Mission_GetMapChecksums leaves zero and the packed build version;
// the receiver reads only the checksums, past the string's NUL.
// [orig: Game_StartMission - is_mp_session_peer @0x526189 / @0x52619A,
//  Mission_GetMapChecksums @0x526209, NetPacket_WriteStringAndThreeInts
//  @0x52622E (g_SidePassword @0x52621F, copied from g_GameConfigState+0x372 by
//  Game_ApplySessionSettingsToGlobals @0x551D20..0x551D30),
//  CNapiNetwork_QueueReliableMessage(0x0B) @0x52624B,
//  CNapiNetwork_PumpClientProtocolSend @0x526255; the receiver
//  NapiNPServerMsg_PlayerJoinRequest @0x51AB10]
void HostRole::send_own_mission_status() {
	if (state.host_owner.ctx.is_mp_session_peer == 0 || !state.client_runtime) return;
	const TitleIdentity *title = state.host_owner.ctx.np_protocol.identity;
	// In a session the own client's mission start submits its profile's kit twice
	// ahead of the 0x0B, as a joiner's does: the side block the local team names, its
	// class byte and the kit page it selects, first with the fixed slot 195, then
	// (after Player_InitPlayer) with g_CurrentWeaponSlot, the current slot word (195 unless
	// a later store moved it) resolved against the local table; all three leave in the 0x0B's pump, in queue
	// order (D-NET-695). The page copy into the spawn buffer is the shell's
	// (seed_session_kit_from_profile). A title whose submit ignores the class sends 8,
	// or 7 for a mission of type 2 (DFX2-12). The session test is retail's is_in_session
	// fact, the rules' mp_session (a socket session; the socketless single player is out
	// of one, as SinglePlayer_StartMission's network type 0 leaves it clear): our
	// context's is_in_session word is set for every session create, the single player's
	// too, so it cannot stand for it.
	// [orig: SinglePlayer_StartMission @0x561AF0 - CNapiNetwork_SetNetworkType(0) @0x561BD4;
	//  CNapiNetwork_SetNetworkType @0x4C4A50 - is_in_session = (type in 1..3) @0x4C4A85;
	//  Game_StartMission - is_in_session @0x525767, is_mp_session_peer @0x525773,
	//  g_LocalTeam @0x525793..0x5257C0, the class byte @0x5257C6, the page
	//  @0x5257C9..0x525818, NetPacket_SendLoadoutSubmit(.., 195) @0x525836; is_in_session
	//  @0x525BC4, is_mp_session_peer @0x525BCC, the class @0x525BE8..0x525C20,
	//  g_CurrentWeaponSlot @0x525C1A, the call @0x525C2E; NetPacket_SendLoadoutSubmit
	//  @0x42cdc0 - the slot resolve @0x42ce2d..0x42ce8b; dfx2.exe @0x42ec1f..0x42ec3e]
	if (kernel_ != nullptr && kernel_->world.rules.mp_session) {
		const mission::MissionKernel &kernel = *kernel_;
		uint8_t team = 0;
		for (const NapiNPConnection &conn : state.host_owner.ctx.np_protocol.connection_list)
			if (conn.type == NapiNPConnection::kTypeClientSide && conn.link.transport == &state.host_loop)
				team = conn.assigned_team;
		const JoinerConnection::LoadoutKit *kit = state.client_runtime->host_loadout_kit();
		const world::WeaponTable &weapons = kernel.world.tables.weapons;
		auto submit_for = [&](bool alternate) {
			LoadoutSubmit submit = kit != nullptr
					? profile_loadout_submit(*kit, team, false)
					: fresh_profile_loadout_submit(&weapons, team);
			if (alternate && kernel.local.inventory_valid)
				submit.weapon_slot_index = static_cast<uint32_t>(resolve_loadout_submit_combo(
						weapons, &kernel.local.inventory, team,
						kernel.local.inventory.current_slot));
			if (title_in_match(title).loadout_class_fixed)
				submit.player_class =
						kernel.mission.header.mission_type == bms::MissionType::CombatVehicleMission
						? uint8_t{7}
						: uint8_t{8};
			return submit;
		};
		(void)state.client_runtime->queue_host_message(c2s::LOADOUT_SUBMIT,
				encode_loadout_submit(submit_for(/*alternate=*/false)));
		(void)state.client_runtime->queue_host_message(c2s::LOADOUT_SUBMIT,
				encode_loadout_submit(submit_for(/*alternate=*/true)));
	}
	const gameprofile::GameProfile *profile =
			title != nullptr ? gameprofile::gameprofile_by_id(title->game_id) : nullptr;
	if (!state.client_runtime->queue_host_message(c2s::MISSION_FILE_STATUS,
				build_mission_status(std::string_view{},
						packed_game_version(profile != nullptr ? profile->version_text : nullptr))))
		return;
	state.client_runtime->flush_host_sends();
}

void HostRole::pump_load(int count) {
	opennova::IDatagramSocket &socket =
			socket_ != nullptr ? *socket_ : null_datagram_socket();
	for (int i = 0; i < count; ++i) inmatch::host_session_load_pump(state.host_owner, socket);
}

// The session host's load pumps from the round-over clear to the PreMission pass: nine on
// a dedicated host, ten with its own client, whose render arm pumps once more.
// [orig: Game_StartMission - Server_PumpNetworkTransport @0x524A63, @0x524F76, @0x524F8C
//  (the .bms arm; @0x5251C7 / @0x5251DD the other), @0x525256, @0x5253EA, @0x52589D,
//  @0x5258E5, @0x5259EC (skipped by the dedicated test @0x5259B4: in session, the
//  authority, no own client), @0x525A5C, @0x525AC5]
int HostRole::load_window_pumps() const {
	return 9 + (state.host_owner.ctx.is_mp_session_peer != 0 ? 1 : 0);
}

void HostRole::finish_mission_load() {
	NapiNPServerCtx &ctx = state.host_owner.ctx;
	if (ctx.is_authority != 0) inmatch::Server_BroadcastPlayerInfoToAll(ctx);
	ctx.net_spawn_suspended = 0;
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
		// Its death screen's team change rides it too
		// [orig: DeathScreen_OnSwapTeams @0x5535BA -> NetPacket_SendPingRequest
		//  @0x42DD90 (a misnomer), QueueReliableMessage(0x4D) @0x42DDB1].
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
		// So does its deploy pick: the deploy keys and the DEATH screen's list
		// queue the C2S 0x0E with no authority test, and the server's
		// respawn handler deploys the host's own player like a joiner
		// [orig: Input_HandleActionBinding case 12 @0x49b17b;
		//  Server_ProcessClientRequestRespawn @0x519AF0].
		// So does the same action's spectator convert, the reliable 0x51 a
		// dead player under permanent death queues, whose handler reads the
		// host context [orig: case 12 @0x49B0B9;
		//  Server_ProcessClientRequestSpectatorRespawn @0x51C840].
		// So does its stance key's 0x1D: the handler latches the host's own
		// player on the frame after the press, ahead of that frame's body
		// update. The frame's pack runs before it and is overwritten by it.
		// [orig: NapiNPServerMsg_HandleStanceChange @0x501c60 from
		//  Server_TickUpdate's receive pump @0x51d895]
		// So does its Respawn key's bodiless 0x41, which the handler applies
		// to the host's own slot and body [orig: NetPacket_SendRequestRespawn
		//  @0x42DBA0, its queue call @0x42DBBC; NapiNPServerMsg_DeathTimeoutReset
		//  @0x510540].
		// So does its 0x4C quality report: the 310-frame leg has no authority
		// test, and the protocol keeps one server message table, installed at
		// init, for every connection's messages, the local one's included, so
		// the handler stores the host's level on its own slot (its gates: the
		// authority, the slot pointer, a short read of 0, then the clamp to 4).
		// [orig: Client_ProcessNetworkFrame @0x42C279 ->
		//  CNapiNetwork_QueueReliableMessage @0x4c4fa0; CNapiNetwork_Init
		//  @0x4CA8E7 (g_NPMsgInfoServer @0x82B5D8, its 0x4C row @0x82B9E8) ->
		//  NapiNPServerMsg_0x04C @0x5111B0]
		// So does its 0x34 keepalive: the leg has no authority test either, and
		// the same table's row raises the host slot's ack high-water (D-NET-604).
		// [orig: Client_ProcessNetworkFrame @0x42C1E1; g_NPMsgInfoServer's 0x34
		//  row @0x82B898 -> NapiNPServerMsg_UpdateAck @0x5024B0]
		// So does its mission start's C2S 0x0B, whose receiver runs the join for
		// the host's own slot as for a joiner's (D-NET-NEW-L4-5)
		// [orig: Game_StartMission @0x52624B; NapiNPServerMsg_PlayerJoinRequest
		//  @0x51AB10, from the receive pump @0x51D895].
		// So do its mission start's two C2S 0x2F, ahead of its 0x0B: the accept
		// grants the own slot, whose table is the local one (D-NET-695)
		// [orig: Game_StartMission @0x525836 / @0x525C2E;
		//  NapiNPServerMsg_HandlePlayerLoadout @0x515790].
		// So do its console line (a session peer's CMD queues the reliable 0x04,
		// whose handler echoes an untaken line back as S2C 0x06) and the 0x24 its
		// own client's SERVER / BAN commands send back over the walk; both
		// handlers read the host context (D-NET-710).
		// [orig: Client_ProcessConsoleCommand @0x520F29; NapiNPServerMsg_HandleConsoleLine
		//  @0x5199D0; NapiClientCmd_HandleDebugCommand @0x42c7ec;
		//  NapiNPServerMsg_0x024 @0x514DC0]
		// [orig: NetPacket_SendUserWaypointCreate @0x42dde6 (0x17),
		//  NetPacket_SendWeaponSlotSwitch @0x42dc20 (0x43),
		//  NetPacket_SendCommandType44 @0x42dca0 (0x44),
		//  NetPacket_SendWeaponAction @0x42dceb (0x45), NetPacket_SendTeamChange
		//  @0x42dd19 (0x46), NetPacket_SendVoteKick @0x42dd69 (0x4B),
		//  NetPacket_SendUserWaypointRemove @0x42de21 (0x4F),
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
				dg.tag != c2s::EMOTE_REQUEST && dg.tag != c2s::RADIO_CALL_REQUEST &&
				dg.tag != c2s::TEAM_CHANGE_REQUEST && dg.tag != c2s::STANCE_CHANGE &&
				dg.tag != c2s::RESPAWN_REQUEST && dg.tag != c2s::DEATH_TIMEOUT_RESET &&
				dg.tag != c2s::SPECTATOR_RESPAWN && dg.tag != c2s::CLIENT_QUALITY &&
				dg.tag != c2s::KEEPALIVE && dg.tag != c2s::ADMIN_NETLOG_COMMAND &&
				dg.tag != c2s::BAN_PUNT_COMMAND && dg.tag != c2s::MISSION_FILE_STATUS &&
				dg.tag != c2s::LOADOUT_SUBMIT) {
			deferred.push_back(std::move(dg));
			continue;
		}
		// The 0x0B's receiver reads the host context too, and the 0x2F accept its
		// availability table and title.
		// The chat handler, the snapshot builder, the radio call (its
		// designation table and zone test), the squad handlers, the team
		// change, the Respawn key's 0x41 (the authority and the respawn
		// timeout) and the 0x51 convert (its authority test) read the host
		// context.
		const bool reads_ctx = squad || dg.tag == c2s::CHAT_MESSAGE ||
				dg.tag == c2s::VISIBLE_PLAYERS_REQUEST || dg.tag == c2s::RADIO_CALL_REQUEST ||
				dg.tag == c2s::TEAM_CHANGE_REQUEST || dg.tag == c2s::DEATH_TIMEOUT_RESET ||
				dg.tag == c2s::SPECTATOR_RESPAWN || dg.tag == c2s::ADMIN_NETLOG_COMMAND ||
				dg.tag == c2s::BAN_PUNT_COMMAND || dg.tag == c2s::MISSION_FILE_STATUS ||
				dg.tag == c2s::LOADOUT_SUBMIT;
		std::vector<ProtocolMessage> messages;
		messages.push_back(make_protocol_message(dg.tag, std::move(dg.body)));
		inmatch::ServerDispatchInputs inputs;
		if (reads_ctx) inputs.server_ctx = &state.host_owner.ctx;
		std::vector<ProtocolMessage> replies = inmatch::dispatch_session_replies(
				state.host_owner.ctx.config, *local, messages, state.host_owner.now_tick,
				state.host_owner.ctx.np_protocol.connection_list, &kernel.world, inputs);
		for (ProtocolMessage &reply : replies) state.host_loop.host_send(reply.tag, std::move(reply.payload));
		// The records its handler raised go out behind it, as a remote's do (D-NET-NEW-V3-1)
		// [orig: CNapiNPConnection_ParseMessages @0x625BC0, record by record].
		inmatch::route_entity_pass_records(state.host_owner.ctx, kernel.world);
	}
	for (replication::Datagram &preserved : deferred) state.host_loop.deliver_c2s(preserved.tag, std::move(preserved.body));
}

// The listen host's own call queues on its own client's local connection,
// which its client frame sends: the server handler then broadcasts the 0x1E
// line to everyone including this client.
// [orig: Input_HandleActionBinding case 217 -> QueueReliableMessage(0x2E)
//  @0x49b50c, no is_authority branch]
bool HostRole::send_medic_request() {
	if (!state.host_owner.serve_and_play || !state.client_runtime) return false;
	opennova::MedicRequest request;
	request.entity_index = kernel_->world.cached.local_player.packed;
	return state.client_runtime->queue_host_message(opennova::c2s::MEDIC_REQUEST,
			opennova::encode_medic_request(request));
}

// The listen host's own Respawn key: its 0x41 rides its local connection, and
// the drain above hands it to the server's handler for its own slot.
// [orig: Input_HandleActionBinding case 55 -> NetPacket_SendRequestRespawn
//  @0x49B22A, no is_authority branch]
bool HostRole::send_respawn_request() {
	return state.host_owner.serve_and_play && state.client_runtime &&
			state.client_runtime->queue_respawn_request();
}

bool HostRole::send_stance_request(int stance) {
	if (kernel_ == nullptr || stance < 0 || stance > 2 || !state.client_runtime) return false;
	if (!kernel_->local.stance_request_allowed()) return false;
	return state.client_runtime->queue_stance_change(world::LocalPlayer::kStanceActionIds[stance]);
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
				kernel.world.out.rounds.frame_clock, ctx.config.effective_send_holdoff_ticks());
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
	// Server_SendLoadedModelPageRequest's S2C 0x68 cursor wraps at the host's frozen
	// loaded-model list count [orig: Server_SendLoadedModelPageRequest @0x511375 /
	// @0x511391].
	state.host_owner.ctx.loaded_model_list_count = input.loaded_model_list_count;
	const uint32_t now = state.host_owner.now_tick;
	// The stats board's host-prep row: the view/viewport setup before the
	// portable host pump.
	if (kernel.world.profile != nullptr)
		kernel.world.profile->add(devtools::Slot::SIM_HOST_PREP,
				static_cast<int64_t>(io::perf_now_us()) - prep_start);
	// The host dictates its own client's local connection (no peer address) a
	// one-tick holdoff, so that client frame's send block -- and the input
	// pack inside it -- opens every frame [orig: Client_ProcessNetworkFrame
	//  @0x42C3DD -> @0x42C3E9; NapiNPServer_UpdateHoldoffTicks @0x4c5f53 ->
	//  1 @0x4c5f59..0x4c5f69; the period arrives as a CS field-3 update,
	//  HandleCSConfigUpdate @0x621940].
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
	// The gate's pre-round half is the same client's copy of the pre-round byte,
	// folded from its loopback 0x0A at the end of the previous frame; a host
	// that does not play receives no 0x0A, so its copy stays 0 and its entity
	// update runs through the pre-round.
	// [orig: Game_ProcessMainFrame -- `cmp g_ClientPreRoundTimer` @0x52672C;
	//  its one store NapiNPClientMsg_0x00A @0x430064]
	kernel.world.cached.client_preround_timer =
			state.host_owner.ctx.is_mp_session_peer != 0 && state.client_runtime != nullptr
			? state.client_runtime->state().preround_delay_seconds
			: 0u;
	// The own client's spectate walk starts from and skips the host's player
	// [orig: Spectator_CycleTarget_0 @0x52ac56 / @0x52ad09].
	if (state.client_runtime != nullptr)
		state.client_runtime->view().set_spectate_local_handle(kernel.world.cached.local_player.packed);
	inmatch::host_session_pump(state.host_owner, socket, &before_server_tick, &kernel,
			nullptr, nullptr, &drain_own_client, this);
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
	{
		// A remote player's own slot reads its pools through the session's tables
		// (D-NET-336) [orig: Entity_GetScoreValueBySlotType @0x5406E0].
		ServerRemoteWeaponTables remote_tables(state.host_owner.ctx.np_protocol.connection_list);
		kernel.world.remote_weapon_tables = &remote_tables;
		kernel.world.pump_weapon_actions();
		kernel.world.remote_weapon_tables = nullptr;
	}
	tail.restart();
	kernel.resolve_new_infantry_adm_ids();
	tail.mark(devtools::Slot::SIM_ADM_RESOLVE);
	kernel.local.tick_medic_cooldown(kernel.local.local_player_dead()); // Player_UpdatePerFrame's cooldown leg
	// The walk's reload requests, the local player's at its own pool-0 visit among
	// every other row's, ride the host's own client onto the loopback in walk order,
	// so the shared dispatcher broadcasts each S2C 0x49 next frame in that order
	// (the authority already refilled where the echo would; the server handler's
	// local-connection gate prevents a second refill) (D-NET-336). A dedicated
	// host has no client connection, so its queue sends nothing, as retail's.
	// [orig: WeaponAction_Reload @0x5430FF / @0x543103 -> NetPacket_SendReloadRequest
	//  @0x432930 -> CNapiNetwork_QueueReliableMessage @0x4C4FA0 (the client
	//  connection test @0x4C4FA8), the connection made only for a host that is also a
	//  client, CNapiGameSession_CreateSession @0x4C97C0 (@0x4C9B73..0x4C9BA6)]
	if (state.host_owner.serve_and_play) {
		for (const world::WeaponReloadRequest &request : kernel.world.out.weapon_reloads) {
			opennova::WeaponReload reload;
			reload.entity_handle = request.entity_handle;
			reload.reload_param = request.reload_param;
			state.host_loop.client_send(0x25, opennova::encode_weapon_reload(reload));
		}
	}
	kernel.world.out.weapon_reloads.clear();
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
		state.client_runtime->set_local_player_position(kernel.world);
		state.client_runtime->Client_ProcessNetworkFrame(now);
		state.client_runtime->apply_received_effects(kernel.world);
		state.client_runtime->flush_host_sends(); // the receive handlers' own sends
		// The vehicle screen's poll after the client net frame, its 0x42 left
		// for the next frame's send (vehicle_menu.h) [orig: Game_ProcessMainFrame --
		//  VehicleMenu_PollAvailability @0x5266A1, ahead of Server_TickUpdate @0x5266B6].
		state.client_runtime->poll_vehicle_menu(kernel.world, kernel.items_table());
		state.client_runtime->raise_net_quality_link_errors(ctx.net_quality_link_errors);
		// The own client's 0x0F copies its location names, each cut to its
		// slot, into the one g_LocationNames the spawn filled, so the host's
		// later reads (a later joiner's 0x0F, the chat's location labels) read
		// that copy [orig: NapiNPClientMsg_0x00F: the count @0x42e51b and the
		// copy @0x42e526..0x42e5ad, no authority test on them (the handler's
		// tests gate the spawn-position store @0x42e360..0x42e369 and the ack
		// burst @0x42e5b7)].
		const replication::ClientState &own = state.client_runtime->state();
		if (own.world_state.revision != track_seen_.world_state_revision) {
			ctx.mission_location_names = own.location_names;
			apply_own_world_state_load(kernel, own);
		}
		// The own client's 0x0F / 0x50 track legs: the map POI list outside
		// the waypoint gametypes (the route stays the promotion's).
		apply_replica_track(kernel.world, state.client_runtime->state(),
				state.client_runtime->game_type(), /*route_from_wire=*/false, track_seen_);
	} else {
		// A Serve Only host has no client runtime, but its main frame runs the same g_NetQuality
		// legs on the process's object (its context's): the sample's level stored, the
		// indicators stepped in a session, then this tick's server link-error callbacks
		// raised, which its status page draws (D-NET-NEW-L8-24).
		// [orig: Game_ProcessMainFrame @0x5263F0 - CNetQuality_SetLevel @0x52659B (the level
		//  @0x526590), `cmp is_in_session` @0x526680 -> CNetQuality_UpdateIndicators @0x52668D
		//  (no client test), Server_TickUpdate @0x5266B6; CNetQuality_SetLinkErrorFlag @0x4C34F0]
		hud::NetQualityIndicators &indicators = ctx.net_quality_indicators;
		hud::net_quality_set_level(indicators, ctx.net_quality_level);
		if (ctx.is_in_session != 0) {
			hud::NovaWorldLinkFacts nw;
			nw.novaworld = ctx.transport_mode == NetworkType::NovaWorld;
			nw.nwu_in_use = ctx.nwu_in_use;
			nw.nwu_session_flags = ctx.nwu_session_flags;
			nw.nwu_session_role = ctx.nwu_session_role;
			hud::net_quality_update_indicators(indicators, nw);
		}
		// Its main frame runs the client net frame all the same, which steps the process's
		// 0x4C count (no report: not a session peer). [orig: Game_ProcessMainFrame @0x526692
		//  (no role test); Client_ProcessNetworkFrame @0x42C239 / @0x42C246]
		(void)client_process_->step_net_quality_report();
		const uint32_t now_ms = host_clock_ms(state.host_owner);
		if ((ctx.net_quality_link_errors & kNetQualityLinkErrorOutgoing) != 0)
			hud::net_quality_set_flag(indicators, hud::kNetLinkErrorOutgoing, now_ms);
		if ((ctx.net_quality_link_errors & kNetQualityLinkErrorIncoming) != 0)
			hud::net_quality_set_flag(indicators, hud::kNetLinkErrorIncoming, now_ms);
	}
	ctx.net_quality_link_errors = 0;
	last_net_us_ = static_cast<int64_t>(io::perf_now_us()) - net_start;
	if (kernel.world.profile != nullptr)
		kernel.world.profile->add(devtools::Slot::SIM_NET, last_net_us_);
	write_profile_frame();
}

void HostRole::write_profile_frame() {
	// The /PROFILE frame leg closes the update, on a frame no mission exit
	// cut short [orig: Game_ProcessMainFrame — the exit tests @0x526806..0x526860,
	//  then the write @0x526879..0x5268e7]. The leg sits past the pause word's
	// tests, so a paused update writes too, at the tick the pause froze
	// (++g_CurrentTick only inside the gate @0x5265A0..0x5265B4): FBEG and the
	// PDATs again on each paused update while that tick has tick & 7 == 7.
	mission::MissionKernel &kernel = *kernel_;
	NapiNPServerCtx &ctx = state.host_owner.ctx;
	if (ctx.logs.profile != nullptr && ctx.mission_exit_reason == 0 &&
			kernel.world.mission_exit_reason == 0)
		ctx.logs.profile->write_frame(ctx, kernel.world, kernel.world.logic_tick);
}

void HostRole::observe_frame_rate(int32_t fps) {
	Role::observe_frame_rate(fps);
	state.host_owner.ctx.stats_avg_fps = fps;
}

void HostRole::observe_cpu_share(int32_t cpu_percent) {
	// Published before the drain, so every Server_TickUpdate of the frame
	// copies this window's share [orig: Game_MainLoop @0x52B948, the drain
	// @0x52BA08; Server_TickUpdate @0x51D7EA].
	state.host_owner.ctx.stats_cpu_percent = cpu_percent;
}

void HostRole::observe_frame_statistics(int32_t frames_last_second, int32_t cpu_percent) {
	state.host_owner.ctx.stats_frames_last_second = frames_last_second;
	state.host_owner.ctx.stats_cpu_percent = cpu_percent;
}

bool HostRole::session_lost(SessionError &error) const {
	const NapiNPServerCtx &ctx = state.host_owner.ctx;
	// The NovaWorld session's end exits the mission on either host kind; the
	// world-side writers (the SP end screens, the round-over keys, the in-game
	// RESTART) store theirs on the world [orig: g_MissionExitReason, read by
	// Game_ProcessMainFrame @0x526806..0x526867 after the frame's update].
	const int32_t reason = ctx.mission_exit_reason != 0 ? ctx.mission_exit_reason
			: kernel_ != nullptr ? kernel_->world.mission_exit_reason : 0;
	if (reason == 0) return false;
	error = {SessionErrorCode::SessionLost, "mission exit " + std::to_string(reason)};
	return true;
}

// The host's mission exit. Retail's authority teardown walks every active
// player slot in the in-match states 2..7 and queues each one S2C 0x25 (empty
// body, one-send to that slot), sets its net player to game state 8 and its
// slot state back to 1; the session reset then stops the server: while
// host_running is still set, every connection has the record {ds 1, dc 9, dp
// 0, dstr "", dpc 0, ddstr "NP.C:SH:STOP"} latched (first cause wins) and is
// destroyed — the destroy sends the 0x86 SERVER_GOODBYE burst (four identical
// keyed datagrams carrying that record) to the peer, then runs the player
// teardown — and only after the walk is host_running cleared. A joiner (retail
// or ours) therefore leaves the match at once instead of sitting through the
// 120 s silence reap; the burst, not a reliable description record, is what
// retail puts on the wire here. The queued 0x25s never leave: no pump runs after the
// teardown's own (the Post Menu's router is the next update), the destroy drains a
// connection's message queues after its burst, and an error arm closes the socket.
// [orig: Game_TeardownMission @0x522350 -> Server_DisconnectAndResetAllPlayerSlots
//  @0x516160 (the six `push 25h` / NapiNPServer_SendFiltered @0x5161bb..0x516385;
//  NapiNPServer_SendToConn @0x4C4F20 -> CNapiNPConnection_QueueMessage @0x628640);
//  CNapiGameSession_ResetActiveSession @0x4C8A70 -> NapiNPProtocol_StopServer
//  @0x62A820 (the record @0x62a8c3..0x62a8f6 latched-if-invalid @0x62a904..0x62a919,
//  CNapiNPConnection_Destroy @0x62a924 per connection, host_running cleared
//  @0x62a95c) -> TeardownActiveConnection @0x6253C0 -> SendDisconnectPacket @0x61F2A0;
//  CNapiNPConnection_Destroy @0x62A4B0 - the teardown called @0x62A4F9, then
//  CNapiNPConnection_DrainMessageQueues called @0x62A50A]
void HostRole::close() {
	opennova::IDatagramSocket &socket =
			socket_ != nullptr ? *socket_ : null_datagram_socket();
	NapiNPServerCtx &ctx = state.host_owner.ctx;
	// The teardown's head: pools 0..2 go [orig: Game_TeardownMission — Entity_Destroy over
	// pools 0, 1 and 2 @0x52236F / @0x52239A / @0x5223C0]. A map change's teardown already
	// ran it, its pump and its PostMission sweep on this kernel (map_change.h).
	const bool tear_down = kernel_ != nullptr && !map_change.torn_down;
	// The last frame's entity pass queued its records at their calls, so they lead the
	// teardown's destroys and its pump, which sends them; ours raised them for the next tick's
	// head, which never comes: they are routed first.
	// [orig: Game_ProcessMainFrame - Entity_UpdateAllEntities called @0x52674B ahead of the
	//  exit tests @0x526806; Game_TeardownMission - the Entity_Destroy call @0x52236F, then
	//  Server_PumpNetworkTransport called @0x522588]
	if (tear_down && ctx.is_authority != 0 && kernel_->world.rules.mp_session)
		route_entity_pass_records(ctx, kernel_->world);
	if (tear_down) {
		kernel_->destroy_mission_pools();
		// A carrier's destroy drops its flag, whose 0x2F state is queued at the destroy on an
		// in-session authority: it leads everything the teardown's pump sends (D-NET-NEW-L1-25).
		route_teardown_drops(state.host_owner.ctx, kernel_->world);
	}
	// The /PROFILE log closes next, before the teardown's pump, the post-mission pass and the
	// slot disconnects, so their markers never reach the file (a closed log's marker writes
	// nothing: CServerLog_WriteDisconnectMarker @0x4E1C50 tests the file @0x4E1C53..0x4E1C57)
	// [orig: Game_TeardownMission @0x5223e2..0x5223f0 -> CServerLog_CloseAndFree].
	if (ctx.logs.profile != nullptr) ctx.logs.profile->close();
	// The teardown's server pump, on a session's authority only (single player, out of a
	// session, runs none, so an exit from its input pass leaves the latched record alone): a
	// held own goodbye removes the host's player over the destroyed bodies, its leave records
	// sent here (D-NET-NEW-L8-21 path B).
	// [orig: Game_TeardownMission — Server_PumpNetworkTransport called @0x522588;
	//  Server_PumpNetworkTransport @0x4FD960 — `cmp is_authority` @0x4FD960, `cmp
	//  is_in_session` @0x4FD969, jz @0x4FD970]
	if (tear_down && kernel_->world.rules.mp_session)
		inmatch::host_session_teardown_pump(state.host_owner, socket);
	// Then the authority's one-shot PostMission sweep over the destroyed pools, ahead of the
	// per-slot 0x25 walk below [orig: Game_TeardownMission — EventTrigger_UpdateAllWithFlag4
	// call @0x52266C precedes Server_DisconnectAndResetAllPlayerSlots @0x52269B].
	if (tear_down) Server_RunPostMissionSweep(ctx, *kernel_); // its actions send at their calls
	for (NapiNPConnection &conn : ctx.np_protocol.connection_list) {
		if (conn.type != NapiNPConnection::kTypeServerSide || conn.link.transport == nullptr)
			continue;
		if (conn.slot_state >= PlayerSlotState::PlayerSync)
			conn.link.transport->host_send(s2c::GAME_RESET, std::vector<uint8_t>{},
					/*reliable=*/false);
	}
	// An orphaned row in states 2..7 takes its 0x25 through its live net player too
	// (D-NET-NEW-L1-24; none past that player's leave, Q7-36 (a)) [orig:
	//  Server_DisconnectAndResetAllPlayerSlots @0x516160 - the active byte @0x516190, the 0x25 to
	//  the row @0x5161C2..0x5161D4; NapiNPServer_SendFiltered's 0x20 arm, the row's +0x1C
	//  @0x4C8A1F].
	for (OrphanedPlayerRow &orphan : ctx.orphaned_player_rows) {
		NapiNPConnection *net_player = orphan_net_player(ctx, orphan);
		if (net_player == nullptr || net_player->type != NapiNPConnection::kTypeServerSide ||
				net_player->link.transport == nullptr)
			continue;
		if (orphan.row.slot_state >= PlayerSlotState::PlayerSync)
			net_player->link.transport->host_send(s2c::GAME_RESET, std::vector<uint8_t>{},
					/*reliable=*/false);
	}
	// Nothing transmits the walk's 0x25s: the teardown's pump ran ahead of the walk and its
	// tail runs none, the main loop pumps nothing across the mode change, and the Post Menu's
	// router neither, on the destroy arm (Server_EndMissionSession, then the destroy) or an
	// error arm (the modes cleared, the socket closed). So a stock joiner gets the STOP burst
	// below, or nothing.
	// [orig: Game_TeardownMission - Server_PumpNetworkTransport called @0x522588, the walk
	//  called @0x52269B, the tail @0x5226AA..0x5227A2; Game_MainLoop @0x52B630 - the shutdown
	//  @0x52BAFF, the next mode's init @0x52B70D and update @0x52BA5A; PostMenu_RouteMissionExit
	//  @0x568460 - Server_EndMissionSession called @0x568672 (@0x51E850: no pump), the destroy
	//  @0x568683, the error arms' SetTransportMode(0) @0x5686F9]
	// The post-mission router's error arms (5, 6, 7, 20 and the disconnect reasons 9..19) clear
	// the network, connection and transport modes and skip CNapiGameSession_FullDestroy, whose
	// session reset stops the server: such an exit leaves the server unstopped and its joiners
	// with no goodbye burst. The reason is the one the teardown's pump left: a listen host's own
	// leave maps the process latch's record there, a stale kick's turning exit 1 into 7 or 9..19
	// (D-NET-NEW-L1-26); a NovaWorld session's end stores 12.
	// [orig: PostMenu_RouteMissionExit @0x568460 - 7 @0x568500..0x568526, 9..19
	//  @0x56852B..0x568588 -> loc_5686DA (@0x5686DA..0x5686F9), the destroy @0x568683;
	//  CNapiGameSession_FullDestroy @0x4C96A0 - CNapiGameSession_ResetActiveSession (the call
	//  @0x4C96E2)]
	const int32_t exit_reason = ctx.mission_exit_reason != 0 ? ctx.mission_exit_reason
			: kernel_ != nullptr ? kernel_->world.mission_exit_reason : 0;
	const PostMissionRoute route = route_mission_exit(exit_reason, /*novaworld=*/false);
	if (route.error != PostMissionError::None) {
		// The unstopped server outlives the session in the process (ctx+0x68 and the protocol's
		// list are g_NapiNPCtx's): the browse's and a join's pumps step it
		// (pump_unstopped_server) and the next session reset stops it (stop_unstopped_server).
		// The nodes' transports go with this session; each node keeps its keys, sequence,
		// records, names and clocks, and its queue as it stands: the packets framed for its
		// boundary, then every message not yet built, the walk's 0x25 among them, in the
		// order the boundary would have packed them (the owner's held messages ahead of what
		// its transport holds) (D-NET-NEW-L1-26).
		// [orig: CNapiNPConnection_QueueMessage @0x628640 (the 0x25s queued, nothing built);
		//  CNapiNPConnection_PumpEnumeratorAndSend @0x6290C0 - BuildOutgoingPackets called
		//  @0x6292B4 at the browse's first due pump]
		UnstoppedServer &unstopped = client_process_->unstopped_server;
		unstopped = UnstoppedServer{};
		unstopped.running = ctx.np_protocol.host_running != 0;
		unstopped.session_name = ctx.np_protocol.session_name;
		// The one game protocol's name, which the manager's PROTOCOL line prints for a held
		// node's datagram (receive_unstopped_server).
		const TitleIdentity *identity = ctx.np_protocol.identity;
		unstopped.protocol_name =
				identity != nullptr && identity->pn != nullptr ? identity->pn : "";
		// The protocol's words the held hello handler answers from: its identity, its start
		// stamp and the 0x81's protocol fields as they stand (receive_unstopped_server).
		unstopped.identity = identity;
		unstopped.host_start_tick = ctx.np_protocol.host_start_tick;
		if (identity != nullptr) unstopped.hello = host_server_hello(ctx, ClientHello{}, PeerAddr{});
		// And the words a 0x42 the held protocol queues meets: its own gates' (the password, the
		// refusal byte, the node template) and the validate callback's (the join lock, the
		// address bans, the capacity), none of which the error arm resets (receive_unstopped_server).
		unstopped.server_password = ctx.config.server_password;
		unstopped.reject_new_connections = ctx.np_protocol.reject_new_connections;
		unstopped.node_template = server_node_template(ctx);
		unstopped.join_locked = ctx.join_locked;
		for (const banlist::AddressBan &ban : ctx.bans.addresses.entries)
			unstopped.banned_addresses.push_back(ban.address);
		unstopped.join_capacity = ctx.config.join_capacity();
		unstopped.lineup_queue_open = ctx.config.lineup_queue_open();
		// A listen host whose own client never left (an exit that is not the own leave's, the
		// NovaWorld session's end among them) holds its own pair too: the session's own client
		// connection (+0xE60, type 2) and the type-1 twin its own 0x42 created, at address 0, the
		// first server-side node of the list. Our one own node stands for the pair: the client
		// as it is, the twin a server-side copy with no record of its own.
		// (D-NET-NEW-L1-26, L9-s11's own-pair hand-over)
		// [orig: CNapiGameSession_CreateSession @0x4C97C0 - the type-2 create @0x4C9B9C ahead of
		//  its 0x42's type-1 create (NapiNPProtocol_HandleClientJoin @0x62BF06, the address word
		//  @0x62BF28); CNapiGameSession_ResetActiveSession @0x4C8A70 - the +0xE60 destroy
		//  @0x4C8A93 ahead of StopServer @0x4C8AA1]
		for (const NapiNPConnection &conn : ctx.np_protocol.connection_list) {
			if (conn.type != NapiNPConnection::kTypeClientSide) continue;
			unstopped.own_client = conn;
			unstopped.own_client->link.transport = nullptr;
			UnstoppedNode twin;
			twin.conn = conn;
			twin.conn.type = NapiNPConnection::kTypeServerSide;
			twin.conn.link.transport = nullptr;
			twin.conn.disconnect_event_valid = false;
			twin.conn.disconnect_event = DisconnectEvent{};
			unstopped.nodes.push_back(std::move(twin));
			break;
		}
		for (const NapiNPConnection &conn : ctx.np_protocol.connection_list) {
			if (conn.type != NapiNPConnection::kTypeServerSide) continue;
			UnstoppedNode node;
			node.conn = conn;
			node.conn.link.transport = nullptr;
			auto staged = state.host_owner.pending_session_datagrams.find(conn.peer);
			if (staged != state.host_owner.pending_session_datagrams.end())
				node.staged = std::move(staged->second);
			auto pending = state.host_owner.pending_session_messages.find(conn.peer);
			if (pending != state.host_owner.pending_session_messages.end())
				node.queued = std::move(pending->second);
			if (conn.link.transport != nullptr) {
				auto *udp = static_cast<replication::UdpSessionTransport *>(conn.link.transport);
				replication::Datagram held;
				while (udp->pop_outbound(held)) {
					ProtocolMessage message = make_protocol_message(
							held.tag, std::move(held.body), held.protocol_flags_raw);
					message.reliable = held.reliable;
					message.capacity_exempt = held.capacity_exempt;
					message.retention_flushes = held.retention_flushes;
					node.queued.push_back(std::move(message));
				}
			}
			unstopped.nodes.push_back(std::move(node));
		}
		return;
	}
	// The destroy arm runs Server_EndMissionSession ahead of the destroy: on the authority, the
	// METPROTOCOL SERVER report and the report clocks' reset, so the players the stop below
	// removes report GCC "" (D-NET-NEW-L9-22).
	// [orig: PostMenu_RouteMissionExit @0x568460 - Server_EndMissionSession called @0x568672,
	//  CNapiGameSession_FullDestroy called @0x568683; Server_EndMissionSession @0x51E850 -
	//  is_authority @0x51E865, Server_ProcessPeriodicTimerUpdate called @0x51E867]
	if (route.destroys_session && ctx.is_authority != 0) {
		ctx.tick_count_ms = inmatch::host_clock_ms(state.host_owner);
		inmatch::Server_ProcessPeriodicTimerUpdate(ctx);
	}
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
	// [orig: NapiNPProtocol_StopServer -> CNapiNPConnection_LogHostStopped
	//  @0x62a97b, after host_running clears @0x62a95c]
	inmatch::write_inout_line(ctx, inmatch::inout_host_line(ctx, /*started=*/false));
}

void HostRole::stop_unstopped_server_at_exit(const ServerLogs &logs) {
	stop_unstopped_server(client_process_->unstopped_server, logs, nullptr, nullptr);
}

// The own type-2 is our one own node while it is in the list; past its leave the record it
// held stays on the context (NapiNPServerCtx::own_client_left_record). Its join never failed,
// so the join-failure words read zero.
std::optional<ConnectionErrorRecord> HostRole::own_client_error_record() const {
	const NapiNPServerCtx &ctx = state.host_owner.ctx;
	std::optional<DisconnectEvent> event;
	for (const NapiNPConnection &conn : ctx.np_protocol.connection_list) {
		if (conn.type != NapiNPConnection::kTypeClientSide) continue;
		event = conn.disconnect_event_valid ? conn.disconnect_event : DisconnectEvent{};
		break;
	}
	if (!event && ctx.own_client_left) event = ctx.own_client_left_record;
	if (!event) return std::nullopt;
	ConnectionErrorRecord record;
	record.disconnect_code = event->dc;
	record.disconnect_text = event->dstr;
	record.disconnect_param = event->dpc;
	return record;
}

// The editor Stop/Start rewind: the kernel's own restore with the own connection as
// the baseline's capture left it, then a fresh HostClient view re-fed from the
// restored world's pools (the static, spawn, organic and marker batches) and the
// minimap's initial scan re-armed. A baseline the mission start took holds the own
// slot short of its join: that restore is a mission start, as retail's
// single-player restart is, and ends as the start did, with the own client's 0x0B,
// so the next frame's receive pump runs the own join, whose clear unhides the body
// the start's local-player block hid (out of a session no 0x0A re-sets it).
// [orig: Game_RestartRoundSP @0x5263A0 - Server_DisconnectAndResetAllPlayerSlots
//  @0x5263D4, Game_StartMission(1) @0x5263DB; Game_StartMission - the hide @0x525C9E,
//  the 0x0B @0x52624B / @0x526255; Server_OnPlayerJoin - the clear @0x51A7DA, the
//  0x0A behind is_in_session @0x51A824]
bool HostRole::reset_to_baseline(SessionError &error) {
	mission::MissionKernel &kernel = *kernel_;
	if (!kernel.restore_baseline()) {
		error = {SessionErrorCode::TickFailed, "mission baseline is unavailable"};
		return false;
	}
	if (state.client_runtime) {
		state.host_loop.clear();
		if (own_connection_at_baseline_)
			for (NapiNPConnection &conn : state.host_owner.ctx.np_protocol.connection_list)
				if (conn.type == NapiNPConnection::kTypeClientSide) conn = *own_connection_at_baseline_;
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
		if (own_connection_at_baseline_ &&
				own_connection_at_baseline_->slot_state == PlayerSlotState::AwaitJoin)
			send_own_mission_status();
	}
	return true;
}

void authority_dialog_line_loaded(NapiNPServerCtx &ctx, ClientRuntime *own_client,
		const world::World &world, const audio::DialogLineRef &line) {
	if (own_client != nullptr) own_client->post_authority_dialog_line(world, line);
	Server_BroadcastDialogLine(ctx, line.dialog_name, static_cast<int16_t>(line.line));
}

void HostRole::dialog_line_loaded(const audio::DialogLineRef &line) {
	if (kernel() == nullptr) return;
	authority_dialog_line_loaded(state.host_owner.ctx, state.client_runtime.get(), kernel()->world, line);
}

} // namespace opennova::inmatch
