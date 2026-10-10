#include <runtime/inmatch/host_boot.h>

#include <base/io/fixed.h>
#include <base/io/log.h>
#include <formats/mission/bms_edit.h>
#include <formats/rtxt/rtxt.h>
#include <net/npwire/entity_class.h>
#include <runtime/environment/water_frame.h>
#include <runtime/hud/game_text_lookup.h>
#include <runtime/inmatch/server_log_recorder.h>
#include <runtime/inmatch/session_status.h>

#include <string_view>
#include <utility>

namespace opennova::inmatch {

ServerTextTable read_host_server_text(const mission::BootFileSource &files) {
	ServerTextTable text;
	std::vector<uint8_t> bytes;
	rtxt::File gametext;
	std::string parse_error;
	if (!files.valid() || !files.read_file(hud::kGameTextTable, bytes) ||
			!rtxt::parse(bytes.data(), bytes.size(), gametext, parse_error))
		return text;
	text.medic_request_format = gametext.get_in_section("Server", "STRSRV_MEDREQ");
	text.change_to_blue_format = gametext.get_in_section("Server", "C2Blue");
	text.change_to_red_format = gametext.get_in_section("Server", "C2Red");
	return text;
}

namespace {

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
	// their witnesses are env::resolve_water_height's.
	const env::WaterHeightRungs rungs = env::mission_water_rungs(overrides,
			kernel.terrain_store.terrain_water_height(), kernel.terrain_store.valid());
	const float water = env::resolve_water_height(rungs, &boot.environment, 0.0f);
	boot.water_z_q16 = static_cast<int32_t>(water * io::kFp16One);
	kernel.world.env.water_z = boot.water_z_q16;
	kernel.sync_water_plane();
}

} // namespace

bool boot_host_mission(HostBootRequest request, HostBoot &boot, std::string &error) {
	boot.kernel = nullptr;
	boot.session = request.session;
	boot.host = request.host;
	boot.mission_text = mission::MissionText{};
	boot.terrain_til.clear();
	boot.server_text = ServerTextTable{};
	boot.item_catalog.reset();
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

	// The /PROFILE log opens a fresh file for this mission, ahead of its load,
	// headed by the map file name [orig: Game_StartMission @0x524475..0x524487
	//  -> ServerLog_OpenForWrite(&g_ServerLog, g_ProfileLogPath), BEGN over
	//  g_MapFileName @0x4e2067].
	if (request.host != nullptr && host_cfg.logs.profile != nullptr)
		(void)host_cfg.logs.profile->open(host_cfg.config.mission_file);

	// The loose score.ini over the game type's default table, into the session
	// config before the bring-up: start_host_session copies the score values
	// and the FIELD rows into world.match. Retail opens it from the game
	// directory, never from a PFF.
	// [orig: the load gated on File_IsSingleFile("score.ini") @0x436ED0, a
	//  FindFirstFileA check on disk; GameType_CreateDefaultSettings @0x52DD00;
	//  ScoreConfig_LoadFile @0x52D8A0]
	if (request.host != nullptr && request.session_score_ini) {
		std::vector<uint8_t> score_bytes;
		if (files.read_loose("score.ini", score_bytes) && !score_bytes.empty() &&
				!load_session_score_config(host_cfg.config,
						std::string_view(reinterpret_cast<const char *>(score_bytes.data()),
								score_bytes.size())))
			io::logf(io::LogLevel::kWarn, "host boot: score.ini rejected; the default table stands");
	}

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
	// name resolver (resolve_mission_text carries the fallback witness).
	{
		std::vector<uint8_t> text;
		(void)mission::resolve_mission_text(files, basename, text);
		std::string text_error;
		if (!text.empty() &&
				!mission::parse_mission_text(text.data(), text.size(), boot.mission_text, text_error))
			io::logf(io::LogLevel::kWarn, "host boot: the mission text did not parse: %s",
					text_error.c_str());
	}

	// The gametext "Server" strings the host's handlers print through, which
	// every host carries (D-NET-344) [orig: Game_InitSubsystems @0x4A6CD0
	// loads gametext.bin; Server_BroadcastMedicRequest @0x5153C9..0x5153D0;
	// GameText_GetString("server", "C2Blue" / "C2Red") @0x51902E / @0x51909C].
	if (request.host != nullptr) boot.server_text = read_host_server_text(files);

	if (!request.session->begin_load().applied()) {
		error = "the session did not enter its load";
		return false;
	}

	// One fresh kernel per load (ADR 0042 d3), the role bound over it, then
	// the embedder's sources and the document.
	mission::MissionKernel &kernel = request.fresh_kernel();
	boot.kernel = &kernel;
	request.role->bind(kernel);
	kernel.set_assets(request.assets);
	if (request.items != nullptr) kernel.set_items_table(request.items);
	kernel.wire_header_world = request.wire_header_world;
	kernel.open_document(std::move(request.mission), basename, files);

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

	// The host's bring-up record, staged for the boot hook: the config with
	// its score rows, the .til, the mission text and the Server strings.
	if (request.host != nullptr) {
		HostBringup bringup;
		bringup.host_cfg = std::move(host_cfg);
		bringup.terrain_til_data = boot.terrain_til;
		bringup.mission_text = boot.mission_text;
		bringup.server_text = boot.server_text;
		bringup.next_mission = request.next_mission;
		request.host->stage_bringup(std::move(bringup));
	}

	// The kernel's ordered boot; the role's session comes up inside it, between
	// the world wiring and the system registration [orig:
	// SinglePlayer_StartMission @0x561AF0]. The mission start (the eager WAC,
	// the weather settle) is phase B's.
	mission::KernelBootOptions options = std::move(request.boot_options);
	options.defer_mission_start = true;
	options.people_name_resolver = [names = boot.mission_text](int32_t index) {
		return names.people_name(index);
	};
	Role &role = *request.role;
	options.bringup_net_session = [&role, after = std::move(request.after_bringup)] {
		const bool fresh_runtime = role.bring_up();
		if (after) after(fresh_runtime);
	};
	std::string boot_error;
	if (!kernel.boot(options, boot_error)) {
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

	// The per-class ATTRIBUTES words (Medic, KnifeBonus) the authority's medic
	// heal, knife reach and medic-filtered sends read. Retail loads charattr.def
	// once, at boot on every peer; a missing or empty file leaves the cleared
	// all-zero table (D-NET-345). A joiner's copy is its own, taken before it
	// connects and carried by its runtime.
	// [orig: Game_Run @0x4A7FE3 -> CharAttr_LoadFromDef @0x412140, which
	//  memsets the 0x7C0-byte table first]
	if (request.host != nullptr) {
		if (!boot.charattr_read) {
			std::vector<uint8_t> charattr_bytes;
			if (!files.valid() || !files.read_file("charattr.def", charattr_bytes)) charattr_bytes.clear();
			charattr_load(boot.charattr, charattr_bytes.data(), charattr_bytes.size());
			boot.charattr_read = true;
		}
		// Each mission start on the authority zeroes and disables the
		// properties the session's mp_No* words name (D-NET-374).
		// [orig: Game_StartMission @0x525b90 -> Server_ResetRoundCounters
		//  @0x516C50, its tail @0x516DBD -> @0x4FCF10, the body of
		//  CharAttr_ApplyMpRestrictions @0x4247D0; single player's
		//  SinglePlayer_StartMission @0x561e7b runs the same step]
		charattr_apply_restrictions(boot.charattr, charattr_restrictions);
		kernel.world.tables.class_attribute_flags = charattr_class_attribute_rows(boot.charattr);
		kernel.world.tables.charattr_disabled_word = charattr_pack_disabled(boot.charattr);
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

	// The /PROFILE roster of the started mission's pool 0, at the start's
	// tail [orig: Game_StartMission @0x526135..0x52617b, after
	//  SpawnWaveList_BuildFromMission @0x526130].
	if (boot.host != nullptr) {
		const NapiNPServerCtx &ctx = boot.host->state.host_owner.ctx;
		if (ctx.logs.profile != nullptr) ctx.logs.profile->write_mission_roster(ctx, kernel.world);
	}

	// The authority's load end: the S2C 0x7B to every slot, and a map change's
	// last pump [orig: Game_StartMission @0x52625F..0x526267].
	if (boot.host != nullptr) boot.host->finish_mission_load();
	// The load ends with the mission start: the Game Loop reads its clock only
	// after Game_StartMission returns (Session::complete_load carries it).
	if (!boot.session->complete_load().applied()) {
		error = "the session did not leave its load";
		return false;
	}
	return true;
}

} // namespace opennova::inmatch
