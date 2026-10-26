#include <runtime/inmatch/host_boot.h>

#include <base/gameprofile/game_type.h> // host_loading_band's auto game type
#include <base/io/fixed.h>
#include <base/io/log.h>
#include <formats/avatars/avatars.h>
#include <formats/mission/bms_edit.h>
#include <formats/rtxt/rtxt.h>
#include <net/npwire/entity_class.h>
#include <runtime/environment/water_frame.h>
#include <runtime/hud/game_text_lookup.h>
#include <runtime/inmatch/host_settings.h> // mission_session_title (the loading band)
#include <runtime/inmatch/client_runtime.h>
#include <runtime/inmatch/integrity_sources.h>
#include <runtime/inmatch/join_character_profile.h> // fresh_profile_character_vars
#include <runtime/inmatch/server_log_recorder.h>
#include <runtime/inmatch/server_message_dispatch.h> // session_mission_name
#include <runtime/inmatch/server_vehicle_spawn.h> // Server_InitEntityLimitTable
#include <runtime/inmatch/session_status.h>
#include <runtime/mission/mission_sidecars.h> // mission_base_name

#include <cstring>
#include <string_view>
#include <utility>

namespace opennova::inmatch {

ServerTextTable read_host_server_text(const mission::BootFileSource &files,
		const rtxt::File *text_override) {
	ServerTextTable text;
	std::vector<uint8_t> bytes;
	rtxt::File gametext;
	std::string parse_error;
	// No gametext table answers "" before any table is searched
	// [orig: GameText_GetString @0x51EBD0 -- the g_TextGameText test @0x51EBD7].
	if (!files.valid() || !files.read_file(hud::kGameTextTable, bytes) ||
			!rtxt::parse(bytes.data(), bytes.size(), gametext, parse_error))
		return text;
	// Each lookup searches the override table first (D-NET-675)
	// [orig: GameText_GetString -- the call @0x51EBF6 into
	//  TextResource_FindEntryBySectionAndKey @0x75D250, @0x75D27B].
	const auto get = [&](const char *key) {
		const rtxt::Entry *entry = rtxt::find_with_override(text_override, gametext, "Server", key);
		return entry != nullptr ? entry->text : std::string();
	};
	text.medic_request_format = get("STRSRV_MEDREQ");
	text.change_to_blue_format = get("C2Blue");
	text.change_to_red_format = get("C2Red");
	text.host_name = get("STRSRV19");
	text.kicked = get("STRSRV18");
	return text;
}

namespace {

// The mission start's head on the authority, past the render device's mission inits and
// the load's last pump: the kernel's PreMission pass and its tail, which ends in the role's
// round init (MissionKernel::begin_mission_start -> HostRole::run_round_init: the round
// counters' charattr tail, the round init over every active slot, a fresh session's own
// and a map change's kept slots alike, then Player_InitPlayer; D-NET-354,
// D-NET-NEW-L7-39); a no-op when the boot ran it.
// [orig: Game_StartMission @0x524360 -- the EventTrigger_UpdateAllWithFlag2 call @0x525B86,
//  the Server_ResetRoundCounters call @0x525B90, the Server_InitAllPlayerEntitiesForRound
//  call @0x525BAF, the Player_InitPlayer call @0x525BBC]
void run_host_mission_start_head(HostBoot &boot) {
	(void)boot.kernel->begin_mission_start();
}

// The mission's environment (its .trn, overcast.def and its .env) with the BMS
// override layer, and the water plane by its witnessed precedence (the BMS
// override, the .env, the .trn), resolved onto the world before the boot's
// PreMission pass.
void load_environment_and_water(const mission::BootFileSource &files,
		mission::MissionKernel &kernel, HostBoot &boot) {
	const mission::MissionInfo info = mission::mission_info(kernel.mission);
	// The time-of-day load's three files under the header's override layer; a
	// .env that is not there is skipped and the mission starts on the earlier
	// passes' globals (env::load_mission_env_config carries the witness).
	const env::EnvTextReader read = [&files](const std::string &name, std::string &text) {
		std::vector<uint8_t> bytes;
		if (!files.valid() || !files.read_file(name, bytes)) return false;
		text.assign(bytes.begin(), bytes.end());
		return true;
	};
	const env::BmsEnvOverrides overrides = env::bms_env_overrides_from_header(
			static_cast<uint32_t>(info.attrib_flags), info.water_override, info.fog_override,
			info.fog_color, info.water_color, info.water_murk);
	env::MissionEnv loaded;
	const bool env_loaded =
			env::load_mission_env_config(read, info.terrain, info.environment, overrides, loaded);
	boot.env_config = std::move(loaded.config);
	boot.environment.set_config(&boot.env_config, env_loaded);
	// The occupant clamp, the vehicle grounding and the footstep water pick
	// read the plane [orig: g_EnvWaterHeightFixed @0x26C6454]; the rungs and
	// their witnesses are env::mission_water_rung's.
	const float water = env::mission_water_rung(
			overrides, boot.env_config, kernel.terrain_store.terrain_water_height())
								.height;
	boot.water_z_q16 = static_cast<int32_t>(water * io::kFp16One);
	kernel.world.env.water_z = boot.water_z_q16;
	kernel.sync_water_plane();
	// The class the load's timeofday lines left (0 with none), which the server-info update lists.
	kernel.world.env.time_of_day_enum = boot.env_config.time_of_day_enum;
}

// The loose score.ini over the game type's default table, into the session
// config before the bring-up: start_host_session copies the score values
// and the FIELD rows into world.match. Retail opens it from the game
// directory, never from a PFF.
// [orig: the load gated on File_IsSingleFile("score.ini") @0x436ED0, a
//  FindFirstFileA check on disk; GameType_CreateDefaultSettings @0x52DD00;
//  ScoreConfig_LoadFile @0x52D8A0]
void apply_session_score_ini(const mission::BootFileSource &files, GameConfig &config) {
	// The table's fanfare pair first, the title's, which the file only replaces
	// (GameType_CreateDefaultSettings, world::default_score_exp_fanfare).
	config.exp_fanfare = world::default_score_exp_fanfare(config.title);
	std::vector<uint8_t> score_bytes;
	if (files.read_loose("score.ini", score_bytes) && !score_bytes.empty() &&
			!load_session_score_config(config,
					std::string_view(reinterpret_cast<const char *>(score_bytes.data()),
							score_bytes.size())))
		io::logf(io::LogLevel::kWarn, "host boot: score.ini rejected; the default table stands");
}

// The process's own reads a host's config takes ahead of its session create: the loose score.ini
// over the session config (apply_session_score_ini), the host's character table and the own
// slot's character selection.
void apply_process_host_config(const mission::BootFileSource &files, HostConfig &host_cfg,
		bool session_score_ini) {
	if (session_score_ini) apply_session_score_ini(files, host_cfg.config);

	// The host's character table, g_AvatarDefs, which a joiner's add validates its
	// ids against (D-NET-137): the embedder's when it installed one, else the
	// mounted Avatars.def (empty without the file). A shell boot whose simulation
	// was created after the world mounted its Avatars.def carries none.
	// [orig: CAvatarDefs_Init @0x57B180 - File_ParseASCIIFile("Avatars.def") @0x57B1D5]
	if (host_cfg.characters.empty()) {
		std::vector<uint8_t> avatars_bytes;
		if (files.valid() && files.read_file("Avatars.def", avatars_bytes)) {
			avatars::AvatarsFile avatars = {};
			if (avatars::avatars_parse_memory(avatars_bytes.data(), avatars_bytes.size(),
						&avatars) == 0)
				host_cfg.characters = CharacterRegistry::from_file(avatars);
			avatars::avatars_free(&avatars);
		}
	}

	// The host's own slot's character selection when the embedder installed none
	// (a headless host): a fresh profile's over that table, as the boot's fresh
	// record packs each side's first combo; an install with no Avatars.def takes
	// the empty table's (D-NET-NEW-L2-7).
	// [orig: PlayerProfile_InitDefaults @0x54BB40 -> CAvatarDefs_PackFirstComboOfAlignment
	//  @0x57AD40 over g_AvatarDefs]
	if (!host_cfg.local_character_vars)
		host_cfg.local_character_vars = fresh_profile_character_vars(host_cfg.characters);
}

} // namespace

CreateSessionResult create_host_session(HostRole &host, const mission::BootFileSource &files,
		const std::string &mission_basename, const std::vector<uint8_t> &text_override_table,
		HostConfig host_cfg, bool session_score_ini) {
	apply_process_host_config(files, host_cfg, session_score_ini);
	HostBringup create;
	create.mission_text = read_host_mission_text(files, mission_basename, text_override_table);
	create.server_text = read_host_server_text(files, create.mission_text.override_table.get());
	create.host_cfg = std::move(host_cfg);
	return host.create_session(create);
}

void begin_next_host_mission_load(HostRole &host, const mission::BootFileSource &files,
		const std::string &mission_basename, const std::vector<uint8_t> &text_override_table,
		HostConfig host_cfg, bool session_score_ini) {
	if (session_score_ini) apply_session_score_ini(files, host_cfg.config);
	HostBringup head;
	head.mission_text = read_host_mission_text(files, mission_basename, text_override_table);
	head.server_text = read_host_server_text(files, head.mission_text.override_table.get());
	head.host_cfg = std::move(host_cfg);
	head.next_mission = true;
	host.begin_next_mission_load(std::move(head));
}

mission::MissionText read_host_mission_text(const mission::BootFileSource &files,
		const std::string &mission_basename, const std::vector<uint8_t> &text_override_table) {
	mission::MissionText out;
	std::vector<uint8_t> text;
	(void)mission::resolve_mission_text(files, mission_basename, text);
	std::string text_error;
	if (!text.empty() && !mission::parse_mission_text(text.data(), text.size(), out, text_error))
		io::logf(io::LogLevel::kWarn, "host boot: the mission text did not parse: %s",
				text_error.c_str());
	if (!text_override_table.empty()) {
		auto override_table = std::make_shared<rtxt::File>();
		if (rtxt::parse(text_override_table.data(), text_override_table.size(), *override_table,
					text_error))
			out.override_table = std::move(override_table);
		else
			io::logf(io::LogLevel::kWarn, "host boot: the text-override table did not parse: %s",
					text_error.c_str());
	}
	return out;
}

HostLoadingBand host_loading_band(const mission::BootFileSource &files,
		const std::string &mission_file, uint32_t game_type, bool game_type_auto,
		const std::vector<uint8_t> &text_override_table) {
	HostLoadingBand band;
	// The header's name and mode word (an unreadable header leaves both zero, as
	// the scan's zeroed header does).
	bms::Header header{};
	std::vector<uint8_t> bytes;
	std::string error;
	if (files.valid() && files.read_file(mission_file, bytes) && bytes.size() >= bms::kHeaderSize)
		(void)bms::parse_header_blob(bytes.data(), bms::kHeaderSize, header, error);
	band.game_type = game_type_auto ? game_type::for_mission_attribs(header.attrib_flags) : game_type;
	const mission::MissionText text = read_host_mission_text(
			files, mission::mission_base_name(mission_file), text_override_table);
	band.mission_name = session_mission_name(band.game_type,
			mission_session_title(text.override_table.get(), mission_file),
			text.get_string("info", "title"),
			std::string(header.mission_name, ::strnlen(header.mission_name, sizeof(header.mission_name))));
	return band;
}

bool boot_host_mission(HostBootRequest request, HostBoot &boot, std::string &error) {
	boot.kernel = nullptr;
	boot.session = request.session;
	boot.host = request.host;
	boot.session_create = CreateSessionResult::Created;
	boot.mission_text = mission::MissionText{};
	boot.terrain_til.clear();
	boot.server_text = ServerTextTable{};
	boot.item_catalog.reset();
	boot.load_end_deferred = request.host != nullptr && request.render_arm_deferred;
	boot.load_end_owed = false;
	boot.mission_start_head_owed = false;
	if (request.session == nullptr || request.role == nullptr || !request.fresh_kernel) {
		error = "the host boot needs a session, a role and a fresh kernel";
		return false;
	}
	const mission::BootFileSource &files = request.files;
	const std::string &basename = request.mission_basename;
	HostConfig host_cfg = std::move(request.host_cfg);
	// The session's charattr restriction words, as the apply copies them from
	// the session config [orig: Game_ApplySessionSettingsToGlobals
	// @0x551E2B..0x551E4D -> g_SessionNoCharAbilities .. g_SessionNoScopeDrift].
	CharAttrRestrictions charattr_restrictions;
	charattr_restrictions.no_char_abilities = host_cfg.config.no_char_abilities != 0;
	charattr_restrictions.no_weapon_recoil = host_cfg.config.no_weapon_recoil != 0;
	charattr_restrictions.no_crosshair_spread = host_cfg.config.no_crosshair_spread != 0;
	charattr_restrictions.no_scope_drift = host_cfg.config.no_scope_drift != 0;

	// The process's own reads the host config takes (score.ini, Avatars.def, the own slot's
	// character selection: apply_process_host_config).
	if (request.host != nullptr) apply_process_host_config(files, host_cfg, request.session_score_ini);

	// The mission's placed tiles: one shared array serving the S2C 0x45
	// stream, the render overlay and the surface walk. The authority (every
	// role but a joiner) loads <mission>.til, else the terrain's
	// polytrn_tileinfo, loose first (mission::read_placed_tiles); a joiner's
	// are the host's stream, never a same-named local file.
	// [orig: PolyTrn_LoadTerrainConfig @0x60E6C9..0x60E6E5;
	//  Terrain_LoadTileInfoFile @0x60A740, the policy force @0x60A74E;
	//  Terrain_SerializeTiles @0x6080F0]
	if (request.terrain_til) {
		boot.terrain_til = std::move(*request.terrain_til);
	} else if (request.role->kind() != RoleKind::Joiner) {
		(void)mission::read_placed_tiles(files, basename, request.mission, boot.terrain_til);
	}

	// The mission text ahead of the kernel: the briefings and location names
	// feed the bring-up (S2C 0x7E / 0x0F) and [PeopleNames] the promote-time
	// name resolver (resolve_mission_text carries the fallback witness). Its
	// lookups search the mount's override table first, with or without a
	// mission table (mission_text.h).
	boot.mission_text = read_host_mission_text(files, basename, request.text_override_table);

	// The gametext "Server" strings the host's handlers print through, which
	// every host carries (D-NET-344) [orig: Game_InitSubsystems @0x4A6CD0
	// loads gametext.bin; Server_BroadcastMedicRequest @0x5153C9..0x5153D0;
	// GameText_GetString("server", "C2Blue" / "C2Red") @0x51902E / @0x51909C].
	if (request.host != nullptr)
		boot.server_text = read_host_server_text(files, boot.mission_text.override_table.get());

	// A fresh session's create, ahead of its mission start: the round-state init seeds the
	// process's CRT stream and draws the slot table's offset, GameServerBaffleKey and the PCIDKey
	// ring, then the create, before any World exists, so the mission start's server-info update
	// already carries both keys and every CRT draw of the load follows the create's seed
	// (HostRole::create_session, D-NET-NEW-L1-50). A set `mpreset` word ends the process there,
	// ahead of the mission start's every leg: the session never enters its load.
	// [orig: CNapiGameSession_BuildAndCreateSession @0x5694D0 - Server_InitNewRoundState called
	//  @0x5695B3, CNapiGameSession_CreateSession called @0x56997D (@0x4C97E7..0x4C97F0 ->
	//  crt_exit(0)); Game_HostMultiplayerSession @0x4A65A0 - the call @0x4A6759, then
	//  Game_MainLoop(&g_GameModeGameLoop) @0x4A6767; MultiPlayer_JoinSessionStateMachine
	//  @0x56A320 - state 2's call @0x56A46A; SinglePlayer_StartMission @0x561AF0 -
	//  @0x561C23, @0x561E65, UI_NavHistoryPush(&g_GameModeGameLoop) @0x561E85; GameLoop_Init
	//  @0x526370 - Game_StartMission(0) called @0x526377]
	if (request.host != nullptr && !request.next_mission && !request.host->create_ran()) {
		HostBringup create;
		create.host_cfg = host_cfg;
		create.host_cfg.mission_type = static_cast<uint8_t>(request.mission.header.mission_type);
		create.mission_text = boot.mission_text;
		create.server_text = boot.server_text;
		if (request.host->create_session(create) == CreateSessionResult::ProcessExit) {
			boot.session_create = CreateSessionResult::ProcessExit;
			error = "mpreset is set: the session create ends the process (exit code 0)";
			return false;
		}
	}

	// The /PROFILE log opens a fresh file for this mission, ahead of its load,
	// headed by the map file name [orig: Game_StartMission @0x524475..0x524487
	//  -> ServerLog_OpenForWrite(&g_ServerLog, g_ProfileLogPath), BEGN over
	//  g_MapFileName @0x4e2067].
	if (request.host != nullptr && host_cfg.logs.profile != nullptr)
		(void)host_cfg.logs.profile->open(host_cfg.config.mission_file);

	// The load's head, when the embedder ran none ahead of its own loads (a headless host): a
	// fresh session's past its create (its spawn gate, report clocks, file-transfer step and
	// head pump) or a map change's world-free legs, then the window's first pumps, then the
	// load's stage pumps below (HostRole::begin_mission_load / begin_next_mission_load,
	// open_mission_load_window, pump_load_stage). A headless host has no client of its own, so
	// no effect system loads between the head and the window [orig: Game_StartMission - the gate
	//  @0x524958..0x524979 skips CEffectSystem_Init @0x524980 in session on the authority with
	//  no own client].
	const bool stage_pumps = request.host != nullptr && !request.host->mission_load_begun();
	if (stage_pumps && request.next_mission) {
		HostBringup head;
		head.host_cfg = host_cfg;
		head.mission_text = boot.mission_text;
		head.server_text = boot.server_text;
		head.next_mission = true;
		request.host->begin_next_mission_load(std::move(head));
	} else if (stage_pumps) {
		request.host->begin_mission_load();
	}
	// The mission start's server-info update and its periodic update, past the head and ahead of
	// the window and the loads (HostBootRequest::mission_start_update carries the witness).
	if (request.host != nullptr && request.mission_start_update) request.mission_start_update();
	if (stage_pumps) request.host->open_mission_load_window();

	if (!request.session->begin_load().applied()) {
		error = "the session did not enter its load";
		return false;
	}

	// One fresh kernel per load (ADR 0042 d3), the role bound over it, then
	// the embedder's sources and the document.
	mission::MissionKernel &kernel = request.fresh_kernel();
	boot.kernel = &kernel;
	request.role->bind(kernel);
	// The mount's override table every section lookup searches first (the
	// dialog subtitles' [Mission Dialog] among them, D-NET-675).
	kernel.text_override_table = boot.mission_text.override_table;
	kernel.set_assets(request.assets);
	if (request.items != nullptr) kernel.set_items_table(request.items);
	kernel.wire_header_world = request.wire_header_world;
	kernel.open_document(std::move(request.mission), basename, files);

	// The per-class ATTRIBUTES words (Medic, KnifeBonus) the authority's medic
	// heal, knife reach and medic-filtered sends read, and the classes'
	// camouflage items the player spawns bind (the own player's in the
	// bring-up below, D-NET-348). Retail loads charattr.def once, at boot on
	// every peer, ahead of any mission; a missing or empty file leaves the
	// cleared all-zero table (D-NET-345). A joiner's copy is its own, taken
	// before it connects and carried by its runtime.
	// [orig: Game_Run @0x4A7FE3 -> CharAttr_LoadFromDef @0x412140, which
	//  memsets the 0x7C0-byte table first]
	if (request.host != nullptr) {
		if (!boot.charattr_read) {
			std::vector<uint8_t> charattr_bytes;
			if (!files.valid() || !files.read_file("charattr.def", charattr_bytes)) charattr_bytes.clear();
			charattr_load(boot.charattr, charattr_bytes.data(), charattr_bytes.size());
			boot.charattr_read = true;
		}
		install_class_attribute_flags(kernel.world, nullptr, boot.charattr);
		// The chat filter's filter.txt, read off the disk once at boot ahead of the archives' mount
		// (an optional file: none leaves the filter empty), its rules on every session's context.
		// [orig: Game_InitSubsystems @0x4A6DEC -> ChatFilter_LoadFromFile @0x4FD640 ->
		//  ChatFilter_LoadDictionary("filter.txt") @0x68AAF0, ahead of PFF_OpenAllArchives @0x4A6F44]
		if (!boot.chat_filter_read) {
			auto filter = std::make_shared<ChatFilter>();
			std::vector<uint8_t> filter_bytes;
			if (files.valid() && files.read_loose("filter.txt", filter_bytes))
				filter->load(filter_bytes.data(), filter_bytes.size());
			boot.chat_filter = std::move(filter);
			boot.chat_filter_read = true;
		}
		request.host->state.host_owner.ctx.chat_filter = boot.chat_filter;
	}

	// The terrain field (the embedder's parsed documents, else the kernel's
	// file entry), the placed tiles over it and the tileset's .TSD table: the
	// store carries them on the surface view every wiring copies.
	if (request.before_terrain) request.before_terrain(kernel);
	if (request.boot_options.terrain && !kernel.terrain_store.valid()) (void)kernel.load_terrain_field();
	kernel.set_placed_tiles(boot.terrain_til);
	kernel.resolve_tile_surface_table();

	// The environment and the water plane stand before the class inits and
	// the PreMission pass the boot runs: retail's mission start loads the
	// environment with the terrain and stores the water plane first
	// [orig: Game_StartMission @0x524B26..0x5251CD -> Game_LoadTerrainDuringConnect
	//  @0x520710 -> Terrain_LoadEnvironmentConfig @0x52073B, Terrain_Init
	//  @0x520765 / @0x52076F (the g_EnvWaterHeightFixed store @0x60FCBA),
	//  ahead of the Entity_InitAllFromModels call @0x52567F and the
	//  EventTrigger_UpdateAllWithFlag2 call @0x525B86].
	load_environment_and_water(files, kernel, boot);

	// The load's stage pumps the boot owes (stage_pumps above): after the terrain with its
	// environment, after the mission's document, after the avatars and the environment clock,
	// all ahead of the def loads and the objects [orig: Game_StartMission -
	//  Game_LoadTerrainDuringConnect @0x524F7C, Server_PumpNetworkTransport @0x524F8C,
	//  Mission_LoadBMSFile @0x524FFA, @0x525256, Avatars_LoadComboModels @0x525267, @0x5253EA,
	//  AmmoDef_LoadAll @0x525495, Entity_InitAllFromModels @0x52567F].
	if (stage_pumps) {
		request.host->pump_load_stage();
		request.host->pump_load_stage();
		request.host->pump_load_stage();
	}

	// The anti-cheat challenge sources from this peer's own corpus, built as
	// the mission load builds retail's ammo.def and weapon.def tables
	// (integrity_sources.h): a host validates C2S 0x20/0x21 against them and
	// a joiner answers S2C 0x30/0x31 from them, every mission.
	// [orig: Game_StartMission @0x525495 (AmmoDef_LoadAll), @0x5254B3 /
	//  @0x5254BD (AnimDef_InitAll, WeaponDefs_LoadFile), both roles]
	const IntegritySources integrity = compute_integrity_sources(files, &kernel.assets());
	if (request.host != nullptr) host_cfg.config.integrity_sources = integrity;

	// The host's bring-up record, staged for the boot hook: the config with
	// its score rows, the .til, the mission text and the Server strings, and the
	// round counters' charattr tail its round init follows: each mission start on
	// the authority zeroes and disables the properties the session's mp_No* words
	// name (D-NET-374).
	// [orig: Game_StartMission @0x525b90 -> Server_ResetRoundCounters
	//  @0x516C50, its tail @0x516DBD -> @0x4FCF10, the body of
	//  CharAttr_ApplyMpRestrictions @0x4247D0; single player's
	//  SinglePlayer_StartMission @0x561e7b runs the same step]
	if (request.host != nullptr) {
		HostBringup bringup;
		bringup.host_cfg = std::move(host_cfg);
		bringup.terrain_til_data = boot.terrain_til;
		bringup.mission_text = boot.mission_text;
		bringup.server_text = boot.server_text;
		bringup.next_mission = request.next_mission;
		bringup.round_counters_tail = [&boot, charattr_restrictions] {
			charattr_apply_restrictions(boot.charattr, charattr_restrictions);
			install_class_attribute_flags(boot.kernel->world, nullptr, boot.charattr);
		};
		request.host->stage_bringup(std::move(bringup));
		// The bring-up's window leaves the render arm's pumps to an embedder that runs its
		// render device's mission inits after this phase.
		request.host->set_render_arm_deferred(request.render_arm_deferred);
	}

	// The kernel's ordered boot; the role's session comes up inside it, between
	// the world wiring and the system registration [orig:
	// SinglePlayer_StartMission @0x561AF0]. The mission start (the eager WAC,
	// the weather settle) is phase B's.
	mission::KernelBootOptions options = std::move(request.boot_options);
	options.defer_mission_start = true;
	// An embedder with a render device: the PreMission pass waits for the head phase B runs
	// past the device stages (run_host_mission_start_head).
	options.defer_premission = boot.load_end_deferred;
	// A host's player is an active slot's: the role's round init past the PreMission
	// pass spawns it and Player_InitPlayer finds it, so the kernel's own spawn step stands
	// down for every host boot (MissionKernel::round_init, which the bring-up installs).
	// [orig: Player_InitPlayer @0x4E15F0 -> Player_FindLocalPlayerEntity @0x4E0090 (the
	//  Flags 0x100 and ownerConnectionId test @0x4E0124); Game_StartMission @0x524360 calls
	//  the round init @0x525BAF, then the Player_InitPlayer call @0x525BBC]
	options.people_name_resolver = [names = boot.mission_text](int32_t index) {
		return names.people_name(index);
	};
	Role &role = *request.role;
	options.bringup_net_session = [&role, integrity, after = std::move(request.after_bringup)] {
		const bool fresh_runtime = role.bring_up();
		if (ClientRuntime *runtime = role.client_runtime())
			runtime->set_integrity_sources(integrity);
		if (after) after(fresh_runtime);
	};
	std::string boot_error;
	const bool booted = kernel.boot(options, boot_error);
	if (!booted) {
		error = "the mission boot failed: " + boot_error;
		return false;
	}

	// The wire entity class each items.def row stamps, from the replication
	// catalog the host's HostClient view decodes with.
	if (const def::DefItemsFile *items = kernel.items_table()) {
		boot.item_catalog = request.item_catalog
				? request.item_catalog
				: std::make_shared<const replication::ItemReplicationCatalog>(
						  replication::ItemReplicationCatalog::from_items_def(*items));
		if (request.host != nullptr) request.host->set_item_catalog(boot.item_catalog);
		kernel.resolve_item_traits([catalog = boot.item_catalog](int def_id) {
			// A missing or unresolved definition fails closed as Unknown.
			const replication::ItemReplicationProfile *profile = catalog->by_definition_id(def_id);
			return static_cast<uint8_t>(profile != nullptr ? profile->wire_entity_class()
														   : EntityClass::Unknown);
		});
	}

	if (request.host != nullptr) {
		// The mission start's head, now, or past the embedder's device stages, which hold
		// its render arm (phase B).
		if (boot.load_end_deferred) boot.mission_start_head_owed = true;
		else run_host_mission_start_head(boot);
	}
	return true;
}

bool start_host_mission(HostBoot &boot, const HostStartDevice &device, std::string &error) {
	if (boot.kernel == nullptr || boot.session == nullptr) {
		error = "the host boot did not run";
		return false;
	}
	mission::MissionKernel &kernel = *boot.kernel;
	// The water plane the boundary's vehicle initialization grounds hulls
	// against (VehicleSystem::initialize_mission_vehicles), re-stamped over
	// whatever the embedder's device stages left between the phases.
	kernel.world.env.water_z = boot.water_z_q16;
	kernel.sync_water_plane();

	// A deferred authority boot's mission-start head, past the embedder's device stages:
	// the render arm's last pump, after the mission's textures and the fonts, then the
	// PreMission pass, the round init and Player_InitPlayer, so a leg the arm's pumps read
	// meets the slots as the load left them [orig: Game_StartMission @0x524360 -
	// Render_InitMissionTextures @0x525A6E, HUD_InitAllFonts @0x525A9A,
	// Server_PumpNetworkTransport @0x525AC5, then the EventTrigger_UpdateAllWithFlag2 call
	// @0x525B86, Server_InitAllPlayerEntitiesForRound @0x525BAF, Player_InitPlayer @0x525BBC,
	// ahead of the Environment_MissionStartInit call @0x525CB8].
	if (boot.mission_start_head_owed) {
		boot.mission_start_head_owed = false;
		boot.host->pump_render_arm_owed();
		run_host_mission_start_head(boot);
	}

	// The mission-start environment boundary: the weather seed from the .env
	// and the BMS clock, the embedder's render bind, then the kernel's
	// complete_mission_start (the eager WAC, the initializer and the 255-tick
	// settle, the unit census, the vehicles, the baseline)
	// [orig: Environment_SnapStateToTargets @0x57D1E0; Game_StartMission
	//  @0x525371 (the clock), @0x525CB8 -> Environment_MissionStartInit
	//  @0x57F878]. A headless host runs it on a weather owner of its own over
	// the boot's environment, prepared as the game prepares its Weather node
	// at the load.
	env::WeatherRuntime *weather = device.weather;
	env::EnvironmentState *environment = device.environment;
	if (weather == nullptr) {
		if (!boot.weather) boot.weather = std::make_unique<env::WeatherRuntime>();
		weather = boot.weather.get();
		environment = &boot.environment;
		weather->prepare_world_driven(environment);
	}
	const std::function<void()> no_render_bind = [] {};
	weather->run_mission_start_boundary(environment, &kernel.world.weather, kernel.mission.header,
			device.bind_render ? device.bind_render : no_render_bind,
			[&kernel] { (void)kernel.complete_mission_start(); });

	// Past the load the authority's report clocks wait to begin while the pre-round delay runs,
	// else play (D-NET-NEW-L9-22) [orig: Game_StartMission - @0x525E19..0x525E4D].
	if (boot.host != nullptr) {
		NapiNPServerCtx &clock_ctx = boot.host->state.host_owner.ctx;
		clock_ctx.tick_count_ms = host_clock_ms(boot.host->state.host_owner);
		gate_clocks_mission_loaded(clock_ctx, kernel.world.preround_delay_seconds);
	}
	// The /PROFILE roster of the started mission's pool 0, at the start's
	// tail [orig: Game_StartMission @0x526135..0x52617b, after
	//  SpawnWaveList_BuildFromMission @0x526130]. A host with its own client
	// writes it inside the start's end, ahead of its own C2S 0x0B
	// (HostRole's net_mission_start_end); one without writes it here.
	if (boot.host != nullptr && !kernel.net_mission_start_end) {
		const NapiNPServerCtx &ctx = boot.host->state.host_owner.ctx;
		if (ctx.logs.profile != nullptr) ctx.logs.profile->write_mission_roster(ctx, kernel.world);
	}
	// Past the roster the start rebuilds the vehicle EntityLimit table, so a map change or a
	// restart also restores the consumed per-team slots (the S2C 0x70 rows and the C2S 0x40
	// gate read it) [orig: Game_StartMission @0x526182 -> EntityLimit_InitTable @0x509A70].
	if (boot.host != nullptr) Server_InitEntityLimitTable(boot.host->state.host_owner.ctx);

	// The load end: the authority's S2C 0x7B to every slot, then the load's spawn
	// gate clears (D-NET-116) [orig: Game_StartMission @0x52625F..0x526267, @0x52631B].
	// An embedder with a render device runs it after its device stages, which hold the
	// render arm's pumps (end_host_mission_load).
	if (boot.host != nullptr && !boot.load_end_deferred) boot.host->finish_mission_load();
	// The load ends with the mission start: the Game Loop reads its clock only
	// after Game_StartMission returns (Session::complete_load carries it).
	if (!boot.session->complete_load().applied()) {
		error = "the session did not leave its load";
		return false;
	}
	boot.load_end_owed = boot.host != nullptr && boot.load_end_deferred;
	return true;
}

void end_host_mission_load(HostBoot &boot) {
	if (boot.host == nullptr || !boot.load_end_owed) return;
	boot.load_end_owed = false;
	boot.host->finish_mission_load();
}

} // namespace opennova::inmatch
