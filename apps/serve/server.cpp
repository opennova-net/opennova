#include "server.h"

#include <base/gameprofile/gameprofile.h>
#include <base/io/log.h>
#include <base/io/strutil.h>
#include <base/vfs/vfs.h>
#include <formats/def/def.h>
#include <formats/mission/bms.h>
#include <formats/rtxt/rtxt.h>
#include <net/npwire/net_ports.h>
#include <runtime/inmatch/host_config.h>
#include <runtime/mission/runtime_boot.h>

#include <cstdlib>
#include <fstream>
#include <iterator>
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
		"  --lan-port       the first port of the bind scan (default: game.cfg mplanserverportmin,\n"
		"                   the head of the retail LAN server range)\n"
		"  --log-debug      print the engine's debug log lines\n"
		"The server reads and writes game.cfg in the directory it runs from (the process's\n"
		"working directory, as retail), with the host file's settings over it, and marks\n"
		"itself running there with activesrvr.txt, which a clean exit deletes.\n";

// The one-token switches, retail-spelled ones matched case-insensitively.
bool parse_port(const std::string &text, uint16_t &out) {
	char *end = nullptr;
	const unsigned long value = std::strtoul(text.c_str(), &end, 0);
	if (text.empty() || end == nullptr || *end != '\0' || value == 0 || value > 0xFFFF) return false;
	out = static_cast<uint16_t>(value);
	return true;
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
	// Retail's boot order: game.cfg at Game_Run's start, the subsystems (the
	// mount, weapon.def and game.cfg again), then the dead /HOST path (the host
	// file, the lock, the save, the session). The socket binds before the
	// mission loads, as the dead path creates the session before its Game Loop
	// starts the mission [orig: Game_HostMultiplayerSession @0x4A6760
	// CNapiGameSession_BuildAndCreateSession before Game_MainLoop] and the
	// game's host listens before its boot (mission_root.cpp enable_host_listen).
	if (!read_boot_config(error) || !mount(error) || !read_config_over_weapons(error) ||
			!read_host_file(error) || !open_socket(error) || !boot_mission(error)) {
		stop();
		return false;
	}
	running_ = true;
	return true;
}

// Game_Run's read, before the mount: no weapon table yet (every avail_wpn
// line drops) and no gametext (the `!` default texts). A set mpreset exits
// the process there [orig: Game_Run @0x4A7FBB -> Game_LoadConfig @0x551480,
// crt_exit @0x5514A1..0x5514AC]; a missing file is the defaults.
bool Server::read_boot_config(std::string &error) {
	const gamecfg::LoadResult boot = gamecfg::load_file(gamecfg::kFileName, gamecfg::LoadOptions{});
	cfg_ = boot.cfg;
	if (boot.reset_exit) {
		reset_exit_ = true;
		error = "game.cfg sets mpreset: the process exits";
		return false;
	}
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

// Game_InitSubsystems' read, once weapon.def has loaded behind the mount and
// gametext.bin with it: the same file over the defaults again, now with the
// weapon table its avail_wpn lines address and the localized default texts;
// the defaults keep the boot read's player_index and hw3d_deviceno
// [orig: Game_InitSubsystems @0x4A6FED (gametext.bin), @0x4A70A6
// (WeaponDef_LoadAll), @0x4A70AB (Game_LoadConfig); Config_SetDefaults keeps
// the block's first word]. From here every return owes Game_Run's exit tail.
bool Server::read_config_over_weapons(std::string &error) {
	roster_.clear();
	std::vector<uint8_t> bytes;
	if (index_.read_file("weapon.def", bytes)) {
		def::DefWeaponsFile weapons = {};
		if (def::def_parse_weapons_memory(bytes.data(), bytes.size(), &weapons) == 0) {
			roster_ = inmatch::game_cfg_weapon_roster(weapons);
			def::def_free_weapons(&weapons);
		}
	}
	rtxt::File gametext;
	std::string gametext_error;
	bytes.clear();
	const bool have_gametext = index_.read_file("gametext.bin", bytes) &&
			rtxt::parse(bytes.data(), bytes.size(), gametext, gametext_error);
	gamecfg::LoadOptions load;
	load.texts = inmatch::game_cfg_default_texts(have_gametext ? &gametext : nullptr);
	load.weapons = roster_;
	load.player_index = cfg_.player_index;
	load.hw3d_deviceno = cfg_.hw3d_deviceno;
	const gamecfg::LoadResult loaded = gamecfg::load_file(gamecfg::kFileName, load);
	cfg_ = loaded.cfg;
	if (loaded.reset_exit) {
		reset_exit_ = true;
		error = "game.cfg sets mpreset: the process exits";
		return false;
	}
	exit_save_owed_ = true;
	return true;
}

// The dead /HOST path [orig: Game_HostMultiplayerSession @0x4A65A0]: the host
// file over the cfg block (a file that does not open ends the attempt,
// @0x4A65AA..0x4A65C0), the directory lock (@0x4A65C1..0x4A65F1), Serve Only
// saved (@0x4A65F9..0x4A6604), then the session settings from the block
// (CNapiGameSession_BuildAndCreateSession @0x4A6759, which fails with no
// current rotation entry, @0x5695AD..0x5695B1). The session plays LAN whatever
// networkconnecttype says until the NovaWorld listing lands (ADR 0051 PR6).
bool Server::read_host_file(std::string &error) {
	std::ifstream in(options_.host_file, std::ios::binary);
	if (!in) {
		error = "the host file '" + options_.host_file + "' does not open";
		return false;
	}
	const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	report_ = inmatch::read_host_file(text.data(), text.size(), cfg_, rotation_, catalog_);
	for (const std::string &key : report_.unknown_keys)
		io::logf(io::LogLevel::kWarn, "opennova-serve: the host file key '%s' is not a host setting",
				key.c_str());
	for (const std::string &name : report_.unknown_missions)
		io::logf(io::LogLevel::kWarn, "opennova-serve: Mission '%s' is not in the mission catalog",
				name.c_str());
	if (!gamecfg::write_active_server_marker(gamecfg::kActiveServerMarkerFileName))
		io::logf(io::LogLevel::kWarn, "opennova-serve: %s did not open for writing",
				gamecfg::kActiveServerMarkerFileName);
	cfg_.dedicated = 1;
	(void)save_config();
	host_ = inmatch::host_session_settings(cfg_);
	if (rotation_.current() == nullptr) {
		error = "the host file names no mission the catalog lists";
		return false;
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
	std::string basename = row.file;
	const size_t dot = basename.rfind('.');
	if (dot != std::string::npos) basename.resize(dot);

	// The session config: the cfg block's (the published player cap with the
	// dedicated slot among it), the session game type from the starting row
	// [orig: Game_StartMission @0x5244DE..0x52452B sets g_GameType from the
	//  rotation's current catalog entry +0x1128 on the authority, over the
	//  file's GameType], the mission identity and the expansion check.
	inmatch::GameConfig config = host_.config;
	config.game_type = rotation_.map_game_type;
	config.mission_file = rotation_.map_file;
	config.mission_name = doc.get_mission_name();
	config.expansion = index_.mounted_expansion();
	config.expansion_version_checksum =
			vfs_expansion_version_checksum(options_.resource_dir, config.expansion);
	config.session_channel = inmatch::GameSessionChannel::Lan;

	// Serve Only: the dedicated role over the bound socket, no player of the
	// host's own, the session coming up inside the boot.
	role_ = std::make_unique<inmatch::HostRole>(inmatch::RoleKind::DedicatedHost);
	role_->set_socket(datagrams_.get());
	session_ = std::make_unique<inmatch::Session>(*role_);

	// The engine's one host boot (ADR 0051 d4), the game's own order.
	inmatch::HostBootRequest request;
	request.mission = std::move(doc);
	request.mission_basename = basename;
	request.files = mission::boot_files_from_index(index_);
	request.assets = assets_.get();
	request.session = session_.get();
	request.role = role_.get();
	request.host = role_.get();
	request.host_cfg.config = config;
	request.host_cfg.socket_mode = inmatch::SocketMode::Lan;
	request.host_cfg.network_type = inmatch::NetworkType::Lan;
	request.host_cfg.serve_and_play = host_.serve_and_play;
	request.host_cfg.game_root = options_.resource_dir;
	request.session_score_ini = true;
	mission::KernelBootOptions &options = request.boot_options;
	options.playable = false; // Serve Only: no player of the host's own
	options.mp_session = true;
	options.terrain = true;
	options.wac = true;
	options.game_type = config.game_type;
	options.player_limit = static_cast<int32_t>(config.max_players);
	options.team_count = config.num_teams;
	request.fresh_kernel = [this]() -> mission::MissionKernel & {
		kernel_ = std::make_unique<mission::MissionKernel>();
		return *kernel_;
	};
	if (!inmatch::boot_host_mission(std::move(request), boot_, error)) return false;
	// No device stages run between the phases on a headless host.
	if (!inmatch::start_host_mission(boot_, inmatch::HostStartDevice{}, error)) return false;
	io::logf(io::LogLevel::kInfo,
			"opennova-serve: '%s' (%s) up: %d entities, terrain %s, WAC %s, %u player slot(s)",
			row.file.c_str(), config.mission_name.c_str(), kernel_->promo.spawned,
			kernel_->terrain_store.valid() ? "loaded" : "absent",
			kernel_->wac_loaded ? "loaded" : "absent", config.max_players);
	return true;
}

// The authority's bind scan over the cfg's LAN server range from its first
// port, stepping by its delta and wrapping; --lan-port replaces the first port
// [orig: CNapiNetwork_OpenTransportSocket @0x4C6A40, the authority arm
// @0x4C6AA2 over mplanserverportmin / max / delta (g_GameConfigState+0x244 /
// +0x248 / +0x24C); NapiUdpSocket_CreateAndBind @0x62D2A0; net_ports.h
// lan_host_bind_ports]. The embedder owns the socket layer (net::startup),
// which is process-wide.
bool Server::open_socket(std::string &error) {
	const uint32_t first = options_.port != 0 ? options_.port
											  : static_cast<uint32_t>(cfg_.mp_lan_server_port_min);
	for (const uint16_t port : lan_host_bind_ports(first,
				 static_cast<uint32_t>(cfg_.mp_lan_server_port_max),
				 static_cast<uint32_t>(cfg_.mp_lan_server_port_delta))) {
		socket_ = net::udp_bind(port, &bound_port_);
		if (socket_.is_valid()) break;
	}
	if (!socket_.is_valid()) {
		error = "no port of the bind scan from " + std::to_string(first) + " is free";
		return false;
	}
	datagrams_ = std::make_unique<net::NetDatagramSocket>(socket_);
	return true;
}

bool Server::frame(double delta_seconds) {
	if (!running_) return false;
	inmatch::FrameInput input;
	input.delta_seconds = delta_seconds;
	const inmatch::FrameOutcome outcome = session_->advance(input);
	// No presenter drains the presentation half of the outbox.
	kernel_->world.out.discard_presentation();
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
	// Game_Run's exit: game.cfg saved, the subsystems (the socket among them)
	// shut down, then the lock deleted, unconditionally [orig: Game_Run
	// @0x4A7FFF Game_SaveConfig, @0x4A8004 Game_ShutdownSubsystems,
	// @0x4A8009..0x4A800E DeleteFileA("activesrvr.txt")].
	const bool exit_tail = exit_save_owed_;
	exit_save_owed_ = false;
	if (exit_tail) (void)save_config();
	if (role_) role_->set_socket(nullptr);
	datagrams_.reset();
	if (socket_.is_valid()) net::close_socket(socket_);
	if (exit_tail) gamecfg::remove_active_server_marker(gamecfg::kActiveServerMarkerFileName);
}

bool Server::save_config() {
	// [orig: Game_SaveConfig @0x54C490: fopen("game.cfg", "w"); a file that
	//  does not open writes nothing, @0x54C4AF..0x54C4BB]
	std::string error;
	if (gamecfg::save_file(gamecfg::kFileName, cfg_, roster_, error)) return true;
	io::logf(io::LogLevel::kWarn, "opennova-serve: %s", error.c_str());
	return false;
}

} // namespace opennova::serve
