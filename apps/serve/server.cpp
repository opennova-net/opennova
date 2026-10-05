#include "server.h"

#include <base/gameprofile/gameprofile.h>
#include <base/io/log.h>
#include <base/io/strutil.h>
#include <base/vfs/vfs.h>
#include <formats/env/env.h>
#include <formats/mission/bms.h>
#include <formats/mission/bms_edit.h>
#include <formats/rtxt/rtxt.h>
#include <formats/trn/trn.h>
#include <formats/trn/trn_io.h>
#include <net/npwire/entity_class.h>
#include <net/npwire/net_ports.h>
#include <runtime/environment/environment_state.h>
#include <runtime/environment/water_frame.h>
#include <runtime/environment/weather_seed.h>
#include <runtime/inmatch/host_settings.h>
#include <runtime/inmatch/session_status.h>
#include <runtime/mission/mission_text.h>
#include <runtime/mission/runtime_boot.h>
#include <runtime/replication/item_replication_catalog.h>
#include <runtime/terrain_query/surface_tiles.h>

#include <cstdlib>
#include <fstream>
#include <iterator>
#include <sstream>
#include <utility>

namespace opennova::serve {

namespace {

const char kUsage[] =
		"usage: opennova-serve --resource-dir <game dir> /HOST <host file> [/exp <name>] [/d]\n"
		"                      [/game <code>] [--loose-root] [--lan-port <n>] [--log-debug]\n"
		"  --resource-dir   the game install to serve from (its PFF set, as opennova.exe takes it)\n"
		"  /HOST            the host file: retail's `/HOST <file>` format, one `Key value` per line\n"
		"                   (GameName, MaxPlayers, KillLimit, ...) and one `Mission <file.bms>`\n"
		"                   line per rotation entry; the last Mission line is the starting map\n"
		"  /exp, /d, /game  mount the expansion, prefer loose files, pick the data's game code\n"
		"  --loose-root     mount a directory that holds no game archives as loose files\n"
		"  --lan-port       the first port of the bind scan (default 32768, the retail range\n"
		"                   32768..32787)\n"
		"  --log-debug      print the engine's debug log lines\n";

// The one-token switches, retail-spelled ones matched case-insensitively.
bool parse_port(const std::string &text, uint16_t &out) {
	char *end = nullptr;
	const unsigned long value = std::strtoul(text.c_str(), &end, 0);
	if (text.empty() || end == nullptr || *end != '\0' || value == 0 || value > 0xFFFF) return false;
	out = static_cast<uint16_t>(value);
	return true;
}

// The retail GameText lookup with its default: the string a mounted
// gametext.bin carries for section/key, else the caller's default
// [orig: Config_SetDefaults @0x54D0FF..0x54D11E pushes ("Menu", "UNTITLED",
//  "!Untitled") for the default game name].
std::string game_text(const rtxt::File *table, const char *section, const char *key,
		const std::string &fallback) {
	if (table == nullptr) return fallback;
	const std::string value = table->get_in_section(section, key);
	return value.empty() ? fallback : value;
}

// A headless host drains the presentation half of the world's outbox each
// frame, so nothing a presenter would consume accumulates; the wire half
// (entity events, relays, grants, the round ring, the water crossings) is the
// host tick's own drain.
void drain_presentation_outbox(world::World &world) {
	world.out.tip_events.clear();
	world.out.hud_detail_blank = false;
	world.out.destruction.clear();
	world.out.vehicle_effects.clear();
	world.out.effects.clear();
	world.out.script_effects.clear();
	world.out.script_sounds.clear();
	world.out.slot_sounds.clear();
	world.out.sound_emitters.clear();
}

} // namespace

const char *serve_usage() { return kUsage; }

int parse_serve_options(const std::vector<std::string> &args, ServeOptions &out, std::string &error) {
	for (size_t i = 0; i < args.size(); ++i) {
		const std::string &a = args[i];
		const auto value = [&](std::string &dest) {
			if (i + 1 >= args.size()) {
				error = a + " needs a value";
				return false;
			}
			dest = args[++i];
			return true;
		};
		if (a == "--help" || a == "-h" || a == "/?") return -1;
		if (strutil::iequals(a, "/d")) {
			out.loose_override = true;
		} else if (a == "--loose-root") {
			out.loose_root = true;
		} else if (a == "--log-debug") {
			out.log_debug = true;
		} else if (a == "--resource-dir") {
			if (!value(out.resource_dir)) return 1;
		} else if (strutil::iequals(a, "/HOST")) {
			if (!value(out.host_file)) return 1;
		} else if (strutil::iequals(a, "/exp")) {
			if (!value(out.expansion)) return 1;
		} else if (strutil::iequals(a, "/game")) {
			if (!value(out.game)) return 1;
		} else if (a == "--lan-port") {
			std::string text;
			if (!value(text)) return 1;
			if (!parse_port(text, out.port)) {
				error = "--lan-port must be 1..65535";
				return 1;
			}
		} else {
			error = "unknown option '" + a + "'";
			return 1;
		}
	}
	if (out.resource_dir.empty()) {
		error = "--resource-dir <game dir> is required";
		return 1;
	}
	if (out.host_file.empty()) {
		error = "/HOST <host file> is required";
		return 1;
	}
	return 0;
}

Server::Server(ServeOptions options) : options_(std::move(options)) {}

Server::~Server() { stop(); }

bool Server::start(std::string &error) {
	if (!mount(error) || !read_host_file(error) || !boot_mission(error) || !open_socket(error)) {
		stop();
		return false;
	}
	running_ = true;
	return true;
}

// The mount opennova.exe makes (godot/game/boot_root_mount.gd over
// ResourceRoot::mount_runtime / set_root_dir): the witnessed fixed archive
// table (Packed, PackedWithLooseOverride under /d) with the game code's SCR
// keying, and, only when asked, a directory with no archives mounted as a
// plain loose root with the JO keying and no expansion (ADR 0025).
bool Server::mount(std::string &error) {
	const VfsMountMode mode = options_.loose_override ? VfsMountMode::PackedWithLooseOverride
													  : VfsMountMode::Packed;
	std::string game = options_.game;
	// Retail stops its boot when the fixed table opens no archive
	// [orig: PFF_OpenAllArchives @ 0x4a4310; Game_InitSubsystems @ 0x4a6f44].
	bool mounted = index_.scan(options_.resource_dir, options_.expansion, mode,
			VfsArchiveDiscovery::RetailTable) && index_.has_mounted_archive();
	if (!mounted && options_.loose_root) {
		mounted = index_.scan(options_.resource_dir, std::string(), VfsMountMode::LooseOnly,
				VfsArchiveDiscovery::ScanAll);
		game = "jo";
	}
	if (!mounted) {
		error = "could not mount '" + options_.resource_dir + "': " + index_.last_error();
		return false;
	}
	index_.set_scr_policy(gameprofile::gameprofile_scr_policy_for_code(game.c_str()));
	assets_ = std::make_unique<assets::AssetStore>(&index_);
	catalog_ = mission_catalog::build(index_);
	return true;
}

// The host file over the host screen's defaults, then the server type forced
// to Serve Only, as the dead auto-host does once the file is read
// [orig: Game_HostMultiplayerSession @0x4A65A0 -- the parse @0x4A65A0..0x4A65B4
//  (a file that does not open clears the /HOST flag and returns @0x4A65B6),
//  SERVERTYPE = 1 @0x4A65F9..0x4A65FE]. The cfg table's defaults stand in for
// game.cfg, which this slice does not read (D-NET-335).
bool Server::read_host_file(std::string &error) {
	std::ifstream in(options_.host_file, std::ios::binary);
	if (!in) {
		error = "the host file '" + options_.host_file + "' does not open";
		return false;
	}
	const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	host_ = inmatch::HostScreenState{};
	// The default game name: the gametext Menu/UNTITLED string
	// [orig: Config_SetDefaults @0x54D0FF..0x54D11E].
	std::vector<uint8_t> gametext_bytes;
	rtxt::File gametext;
	std::string gametext_error;
	const bool have_gametext = index_.read_file("gametext.bin", gametext_bytes) &&
			rtxt::parse(gametext_bytes.data(), gametext_bytes.size(), gametext, gametext_error);
	host_.config.server_name = game_text(have_gametext ? &gametext : nullptr, "Menu", "UNTITLED",
			"!Untitled").substr(0, 32);
	report_ = inmatch::read_host_file(text.data(), text.size(), host_, rotation_, catalog_);
	host_.serve_and_play = false;
	for (const std::string &key : report_.unknown_keys)
		io::logf(io::LogLevel::kWarn, "opennova-serve: the host file key '%s' is not a host setting",
				key.c_str());
	for (const std::string &name : report_.unknown_missions)
		io::logf(io::LogLevel::kWarn, "opennova-serve: Mission '%s' is not in the mission catalog",
				name.c_str());
	if (rotation_.current() == nullptr) {
		error = "the host file names no mission the catalog lists";
		return false;
	}
	// The "Server" chat strings the host's handlers print through
	// [orig: Game_InitSubsystems @0x4A6CD0 loads gametext.bin;
	//  Server_BroadcastMedicRequest @0x5153C9; the team change's
	//  GameText_GetString("server", "C2Blue" / "C2Red") @0x51902E / @0x51909C].
	if (have_gametext) {
		server_text_.medic_request_format = gametext.get_in_section("Server", "STRSRV_MEDREQ");
		server_text_.change_to_blue_format = gametext.get_in_section("Server", "C2Blue");
		server_text_.change_to_red_format = gametext.get_in_section("Server", "C2Red");
	}
	return true;
}

bool Server::boot_mission(std::string &error) {
	const mission_catalog::Row &row = catalog_[rotation_.current()->catalog_index];
	// The starting map is the catalog row's file, loaded from disk when the row
	// is a loose find and from the archives otherwise
	// [orig: Game_StartMission @0x524769..0x524774 passes g_MapFileName and
	//  g_MissionSourceIsLoose to the BMS load; Mission_LoadBMSFile
	//  @0x40F51A..0x40F527 fopens a loose one].
	std::vector<uint8_t> bms_bytes;
	if (!index_.read_file(row.file, bms_bytes,
				row.loose ? VfsLookupPolicy::ForceLooseFirst : VfsLookupPolicy::ForceArchiveOnly)) {
		error = "the mission '" + row.file + "' does not read";
		return false;
	}
	bms::File doc;
	std::string parse_error;
	if (!bms::parse(bms_bytes.data(), bms_bytes.size(), doc, parse_error)) {
		error = "the mission '" + row.file + "' did not parse: " + parse_error;
		return false;
	}
	const mission::MissionInfo info = mission::mission_info(doc);
	std::string basename = row.file;
	const size_t dot = basename.rfind('.');
	if (dot != std::string::npos) basename.resize(dot);

	// The session config: the host screen's, the published player cap with the
	// dedicated slot, the session game type from the starting row
	// [orig: Game_StartMission @0x5244DE..0x52452B sets g_GameType from the
	//  rotation's current catalog entry +0x1128 on the authority, over the
	//  file's GameType], the mission identity and the expansion check.
	inmatch::GameConfig config = host_.config;
	config.max_players = inmatch::host_player_slot_limit(host_.player_limit, host_.serve_and_play);
	config.game_type = rotation_.map_game_type;
	config.mission_file = rotation_.map_file;
	config.mission_name = doc.get_mission_name();
	config.expansion = index_.mounted_expansion();
	config.expansion_version_checksum =
			vfs_expansion_version_checksum(options_.resource_dir, config.expansion);
	config.session_channel = inmatch::GameSessionChannel::Lan;
	// The loose score.ini over the game type's default table, before the
	// bring-up: start_host_session copies the score values and FIELD rows into
	// world.match (inmatch/host_session.cpp). The Godot host orders it the same
	// way (mission_root.cpp -> Simulation::set_score_config_data).
	// [orig: the load gated on File_IsSingleFile("score.ini") @0x436ED0;
	//  GameType_CreateDefaultSettings @0x52DD00; ScoreConfig_LoadFile @0x52D8A0]
	{
		std::vector<uint8_t> score_bytes;
		if (index_.read_file("score.ini", score_bytes, VfsLookupPolicy::ForceLooseFirst) &&
				!score_bytes.empty() &&
				!inmatch::load_session_score_config(config,
						std::string_view(reinterpret_cast<const char *>(score_bytes.data()),
								score_bytes.size())))
			io::logf(io::LogLevel::kWarn, "opennova-serve: score.ini rejected; the default table stands");
	}

	kernel_ = std::make_unique<mission::MissionKernel>();
	kernel_->set_assets(assets_.get());
	const mission::BootFileSource files = mission::boot_files_from_index(index_);
	kernel_->open_document(std::move(doc), basename, files);

	// The bring-up record the boot hook consumes: the config, the mission text
	// (S2C 0x7E / 0x0F) and the raw .til the S2C 0x45 stream pages out.
	inmatch::HostBringup bringup;
	bringup.host_cfg.config = config;
	bringup.host_cfg.socket_mode = inmatch::SocketMode::Lan;
	bringup.host_cfg.network_type = inmatch::NetworkType::Lan;
	bringup.host_cfg.serve_and_play = host_.serve_and_play;
	bringup.host_cfg.game_root = options_.resource_dir;
	{
		std::vector<uint8_t> text;
		(void)mission::resolve_mission_text(files, basename, text);
		std::string text_error;
		if (!text.empty() &&
				!mission::parse_mission_text(text.data(), text.size(), bringup.mission_text, text_error))
			io::logf(io::LogLevel::kWarn, "opennova-serve: the mission text did not parse: %s",
					text_error.c_str());
	}
	// The mission's placed tiles, read loose-first [orig: Terrain_LoadTileInfoFile
	// @0x60A740, the policy force @0x60A74E].
	(void)index_.read_file(basename + ".til", bringup.terrain_til_data,
			VfsLookupPolicy::ForceLooseFirst);
	surface_tiles_ = terrain::surface_tiles_from_til_bytes(bringup.terrain_til_data);
	mission_text_ = bringup.mission_text;

	// The .trn the water rung and the .TSD tile table read (the kernel's own
	// terrain load builds the height field from the same pair).
	TrnConfig trn;
	bool have_trn = false;
	{
		std::vector<uint8_t> trn_bytes;
		if (index_.read_file(info.terrain + ".trn", trn_bytes)) {
			std::istringstream stream(std::string(trn_bytes.begin(), trn_bytes.end()));
			std::string trn_error;
			have_trn = load_trn(stream, trn, trn_error);
		}
	}
	tile_surface_table_.fill(0);
	if (have_trn) {
		terrain::SurfaceTileFileSource tile_files;
		tile_files.has_file = files.has_file;
		tile_files.read_file = files.read_file;
		terrain::resolve_tileset_surface_table(tile_files,
				trn_mission_tilestrip(trn, info.tile_set), tile_surface_table_.data());
	}

	role_ = std::make_unique<inmatch::HostRole>(inmatch::RoleKind::DedicatedHost);
	role_->bind(*kernel_);
	role_->stage_bringup(std::move(bringup));
	session_ = std::make_unique<inmatch::Session>(*role_);
	if (!session_->begin_load().applied()) {
		error = "the session did not enter its load";
		return false;
	}

	mission::KernelBootOptions options;
	options.playable = false; // Serve Only: no player of the host's own
	options.mp_session = true;
	options.terrain = true;
	options.wac = true;
	options.defer_mission_start = true;
	options.game_type = config.game_type;
	options.player_limit = static_cast<int32_t>(config.max_players);
	options.team_count = config.num_teams;
	options.people_name_resolver = [this](int32_t index) {
		return mission_text_.people_name(index);
	};
	options.bringup_net_session = [this] { role_->bring_up(); };
	std::string boot_error;
	if (!kernel_->boot(options, boot_error)) {
		error = "the mission boot failed: " + boot_error;
		return false;
	}

	// The wire entity class each items.def row stamps, from the replication
	// catalog the game builds off the same rows.
	if (const def::DefItemsFile *items = kernel_->items_table()) {
		auto catalog = std::make_shared<const replication::ItemReplicationCatalog>(
				replication::ItemReplicationCatalog::from_items_def(*items));
		role_->set_item_catalog(catalog);
		kernel_->resolve_item_traits([catalog](int def_id) {
			const replication::ItemReplicationProfile *profile = catalog->by_definition_id(def_id);
			return static_cast<uint8_t>(profile != nullptr ? profile->wire_entity_class()
														   : EntityClass::Unknown);
		});
	}
	// The placed tiles over the charmap the boot's terrain step wired.
	kernel_->world.tables.surface_map.tiles = surface_tiles_.empty() ? nullptr : surface_tiles_.data();
	kernel_->world.tables.surface_map.tile_count = static_cast<int32_t>(surface_tiles_.size());
	kernel_->world.tables.surface_map.tile_surface = tile_surface_table_.data();
	// The gametext "Server" strings the host's handlers print through.
	inmatch::set_server_text(role_->state.host_owner.ctx, server_text_);

	// The environment: the mission's .env with the BMS override layer, the
	// water plane by its witnessed precedence, then the weather seed and the
	// mission start (the initial WAC run, the 255-tick settle, the vehicles).
	env::Config env_config;
	bool env_loaded = false;
	{
		const std::string env_name = info.environment + ".env";
		std::vector<uint8_t> env_bytes;
		const bool exists = !info.environment.empty() && index_.read_file(env_name, env_bytes);
		const std::string env_text(env_bytes.begin(), env_bytes.end());
		env_loaded = env::load_mission_env(exists ? &env_text : nullptr, env_config);
	}
	const env::BmsEnvOverrides overrides = env::bms_env_overrides_from_header(
			static_cast<uint32_t>(info.attrib_flags), info.water_override, info.fog_override,
			info.fog_color, info.water_color, info.water_murk);
	env::apply_bms_overrides(env_config, overrides);
	env::EnvironmentState env_state;
	env_state.set_config(&env_config, env_loaded);
	env::WaterHeightRungs rungs;
	rungs.has_mission_override = overrides.has_water_height;
	rungs.mission_override = overrides.water_height * 0.5f;
	rungs.terrain_height = have_trn && trn.water_height != 0 ? trn.water_height * 0.5f : 0.0f;
	rungs.has_loaded_terrain = kernel_->terrain_store.valid();
	const float water = env::resolve_water_height(rungs, &env_state, 0.0f);
	kernel_->world.env.water_z = static_cast<int32_t>(water * 65536.0f);
	kernel_->sync_water_plane();
	kernel_->world.weather.seed(env::weather_seed_from_config(env_config, kernel_->mission.header));
	kernel_->complete_mission_start();

	if (!session_->complete_load().applied()) {
		error = "the session did not leave its load";
		return false;
	}
	io::logf(io::LogLevel::kInfo,
			"opennova-serve: '%s' (%s) up: %d entities, terrain %s, WAC %s, %u player slot(s)",
			row.file.c_str(), config.mission_name.c_str(), kernel_->promo.spawned,
			kernel_->terrain_store.valid() ? "loaded" : "absent",
			kernel_->wac_loaded ? "loaded" : "absent", config.max_players);
	return true;
}

// The authority's bind scan over the retail LAN server range from its first
// port, stepping by one and wrapping [orig: CNapiNetwork_OpenTransportSocket
// @0x4C6A40, the authority arm @0x4C6AA2; NapiUdpSocket_CreateAndBind
// @0x62D2A0; net_ports.h lan_host_bind_ports].
bool Server::open_socket(std::string &error) {
	if (net::startup() != 0) {
		error = "the socket layer did not start";
		return false;
	}
	net_started_ = true;
	const uint16_t first = options_.port != 0 ? options_.port : kRetailLanPortMin;
	for (const uint16_t port : lan_host_bind_ports(first)) {
		socket_ = net::udp_bind(port, &bound_port_);
		if (socket_.is_valid()) break;
	}
	if (!socket_.is_valid()) {
		error = "no port of the bind scan from " + std::to_string(first) + " is free";
		return false;
	}
	datagrams_ = std::make_unique<net::NetDatagramSocket>(socket_);
	role_->set_socket(datagrams_.get());
	return true;
}

bool Server::frame(double delta_seconds) {
	if (!running_) return false;
	inmatch::FrameInput input;
	input.delta_seconds = delta_seconds;
	const inmatch::FrameOutcome outcome = session_->advance(input);
	drain_presentation_outbox(kernel_->world);
	if (outcome.terminal()) {
		end_message_ = outcome.error.message;
		running_ = false;
		return false;
	}
	return true;
}

void Server::stop() {
	if (session_) {
		(void)session_->close();
		session_.reset();
	}
	running_ = false;
	if (role_) role_->set_socket(nullptr);
	datagrams_.reset();
	if (socket_.is_valid()) net::close_socket(socket_);
	if (net_started_) {
		net::shutdown();
		net_started_ = false;
	}
}

} // namespace opennova::serve
