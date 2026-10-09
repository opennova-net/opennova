#include "server.h"

#include "lister.h"
#include "serve_listing.h"

#include <base/gameprofile/game_type.h>
#include <base/gameprofile/gameprofile.h>
#include <base/io/log.h>
#include <base/io/strutil.h>
#include <base/resource_index/boot_policy.h>
#include <base/vfs/vfs.h>
#include <formats/admincfg/admin_cfg.h>
#include <formats/avatars/avatars.h>
#include <formats/def/def.h>
#include <formats/mission/bms.h>
#include <formats/rtxt/rtxt.h>
#include <net/npwire/net_ports.h>
#include <runtime/inmatch/host_config.h>
#include <runtime/inmatch/map_change.h>
#include <runtime/inmatch/mission_exit.h>
#include <runtime/inmatch/server_ban_lists.h>
#include <runtime/mission/mission_sidecars.h>
#include <runtime/mission/runtime_boot.h>

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <thread>
#include <utility>

namespace opennova::serve {

namespace {

const char kUsage[] =
		"usage: opennova-serve --resource-dir <game dir> /HOST <host file> [/exp <name>] [/d]\n"
		"                      [/game <code>] [--loose-root] [--lan-port <n>] [--log-debug]\n"
		"                      [--master-host <gate>] [--master-gate-port <n>]\n"
		"                      [--credentials <file>] [--allow-public]\n"
		"  --resource-dir   the game install to serve from (its PFF set, as opennova.exe takes it)\n"
		"  /HOST            the host file: retail's `/HOST <file>` format, one `Key value` per line\n"
		"                   (GameName, MaxPlayers, KillLimit, ...) and one `Mission <file.bms>`\n"
		"                   line per rotation entry; the last Mission line is the starting map\n"
		"  /exp, /d, /game  mount the expansion (/mod is /exp), prefer loose files, pick the\n"
		"                   data's game code\n"
		"  --loose-root     mount a directory that holds no game archives as loose files\n"
		"  --lan-port       the first port of the bind scan (default: game.cfg mplanserverportmin,\n"
		"                   the head of the retail LAN server range; mpnovaworldportmin when\n"
		"                   listing on NovaWorld)\n"
		"  --log-debug      print the engine's debug log lines\n"
		"  --master-host    the NovaWorld gate to list on (127.0.0.1 for an\n"
		"                   opennova-novaworld-server on this machine). With it, game.cfg's\n"
		"                   networkconnecttype picks the network: 1 (the default) lists on\n"
		"                   NovaWorld, 2 serves LAN only. Without it the server serves LAN\n"
		"  --master-gate-port  the gate's UDP port (default: the NovaWorld gate port)\n"
		"  --credentials    KEY=VALUE file: NOVAWORLD_USER / NOVAWORLD_PASS, the account the\n"
		"                   listing logs in with for its HOSTKEY (none: no HOSTKEY, which an\n"
		"                   opennova-novaworld-server accepts)\n"
		"  --allow-public   allow NovaLogic's NovaWorld (novaworld.net and its hosts), a live\n"
		"                   shared service: use it sparingly. Loopback and any other host are\n"
		"                   allowed without it\n"
		"  /PROFILE <path>  record each mission to <path>.sph (retail's server log)\n"
		"  /PUNTLOG, /PUNT.TXT, /CHEATLOG\n"
		"                   log punts to _PUNT.TXT, start _CHEAT.TXT (the working directory)\n"
		"The server reads and writes game.cfg in the directory it runs from (the process's\n"
		"working directory, as retail), with the host file's settings over it, and marks\n"
		"itself running there with activesrvr.txt, which a clean exit deletes. A nonzero\n"
		"remote_admin_port in game.cfg opens retail's remote-admin console on that TCP port,\n"
		"its users in admin.cfg there and its log in admin_log.txt; banned.txt and\n"
		"banlist.txt there are the ban lists.\n";

// The admin server's log, by bare name in the working directory [orig: "admin_log.txt"
// @0x7C0860, CAdminServer_Construct @0x402C39].
constexpr const char *kAdminLogFileName = "admin_log.txt";

// The one-token switches, retail-spelled ones matched case-insensitively.
bool parse_port(const std::string &text, uint16_t &out) {
	char *end = nullptr;
	const unsigned long value = std::strtoul(text.c_str(), &end, 0);
	if (text.empty() || end == nullptr || *end != '\0' || value == 0 || value > 0xFFFF) return false;
	out = static_cast<uint16_t>(value);
	return true;
}

// The retail GameText lookup with its default: the string a mounted
// gametext.bin carries for section/key, else the caller's default.
std::string game_text(const rtxt::File *table, const char *section, const char *key,
		const std::string &fallback) {
	if (table == nullptr) return fallback;
	const std::string value = table->get_in_section(section, key);
	return value.empty() ? fallback : value;
}

// The wall clock the NovaWorld session runs on (the session's GetTickCount).
uint32_t wall_ms() {
	return static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now().time_since_epoch())
					.count());
}

// The lister's outcome as start's error text.
std::string listing_failure(int code) {
	const char *what = code == nw_lister::kExitNetwork ? "a NovaWorld socket did not open"
			: code == nw_lister::kExitStoppedByService ? "the NovaWorld service stopped the hosting"
			: "the NovaWorld session, the login, the HOSTKEY or the host request failed";
	return std::string("the NovaWorld listing did not host: ") + what +
			" (game.cfg networkconnecttype = 2 serves LAN only, as does a start without --master-host)";
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
		} else if (strutil::iequals(a, "/exp") || strutil::iequals(a, "/mod")) {
			// The game's one `/mod`/`/exp` arm: the last one wins, its first 32
			// bytes kept [orig: Game_ParseCommandLineAndInit @ 0x4a76ac].
			std::string name;
			if (!value(name)) return 1;
			out.expansion = launch_expansion_name(name);
		} else if (strutil::iequals(a, "/game")) {
			if (!value(out.game)) return 1;
		} else if (const int log = parse_log_switch(args, i, out.log_switches, error); log != 0) {
			if (log < 0) return 1;
			i += static_cast<size_t>(log - 1);
		} else if (a == "--lan-port") {
			std::string text;
			if (!value(text)) return 1;
			if (!parse_port(text, out.port)) {
				error = "--lan-port must be 1..65535";
				return 1;
			}
		} else if (a == "--master-host") {
			if (!value(out.master_host)) return 1;
		} else if (a == "--master-gate-port") {
			std::string text;
			if (!value(text)) return 1;
			if (!parse_port(text, out.master_gate_port)) {
				error = "--master-gate-port must be 1..65535";
				return 1;
			}
		} else if (a == "--credentials") {
			if (!value(out.credentials_path)) return 1;
		} else if (a == "--allow-public") {
			out.allow_public = true;
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

bool Server::start(std::string &error, const std::atomic<bool> *cancel) {
	// The admin server's log opens for writing at the process's static
	// construction, ahead of everything, so every launch truncates it whether
	// or not the listener ever opens [orig: CAdminServer_Construct @0x402C10,
	// fopen("admin_log.txt", "w") @0x402C84].
	(void)files_.write(kAdminLogFileName, std::string_view(), /*append=*/false);
	// The log switches arm at the process start, as retail's command-line
	// parse arms them (server_logs.h).
	log_devices_.arm(options_.log_switches);
	// Retail's boot order: game.cfg at Game_Run's start, the subsystems (the
	// mount, weapon.def and game.cfg again), then the dead /HOST path (the host
	// file, the lock, the save, the network type, the socket, on NovaWorld the
	// hosting, then the session). The socket binds before the mission loads,
	// as the dead path creates the session before its Game Loop starts the
	// mission [orig: Game_HostMultiplayerSession @0x4A6760
	// CNapiGameSession_BuildAndCreateSession before Game_MainLoop] and the
	// game's host listens before its boot (mission_root.cpp enable_host_listen);
	// a NovaWorld host hosts on its session before either does
	// [orig: @0x4A66A5..0x4A674F; the live NovaWorld host, D-NET-221,
	//  UI_DispatchScreenEvent @0x54F2CA..0x54F379].
	if (!read_boot_config(error) || !mount(error) || !read_config_over_weapons(error)) {
		stop();
		return false;
	}
	// The subsystems' tail: admin.cfg and the listener [orig: Game_InitSubsystems
	// @0x4A72B8..0x4A72D9, after the game.cfg read @0x4A70AB].
	open_admin();
	if (!read_host_file(error) || !open_socket(error) || !host_on_novaworld(error, cancel) ||
			!boot_mission(/*next_mission=*/false, error)) {
		stop();
		return false;
	}
	if (listing_ != nullptr) bind_listing();
	running_ = true;
	return true;
}

// The listing meets the mission that just started: the live source reads the
// new kernel and the kept server context, takes the map's columns, and the
// Host list goes out at once, as every mission start on a NovaWorld authority
// in session runs the server-info update outside its 1860-tick timer
// [orig: Game_StartMission @0x5248c6..0x5248f5 -> Lobby_UpdateServerInfo ->
//  CPlayerManager_RebuildLists @0x4d45b5 -> CNapiGameSession_SendHostUpdate].
void Server::bind_listing() {
	listing_->set_base(listing_columns());
	listing_->bind(*role_, *kernel_, *lister_);
	lister_->publish_server_info();
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
	// The avatar registry, which the admin console's PETERRABBIT SEXCHANGE walks; an install
	// without Avatars.def leaves it empty [orig: CAvatarDefs_Init @0x57B180 opens
	// "Avatars.def"].
	characters_ = inmatch::CharacterRegistry{};
	std::vector<uint8_t> avatars_bytes;
	if (index_.read_file("Avatars.def", avatars_bytes)) {
		avatars::AvatarsFile avatars = {};
		if (avatars::avatars_parse_memory(avatars_bytes.data(), avatars_bytes.size(), &avatars) == 0)
			characters_ = inmatch::CharacterRegistry::from_file(avatars);
		avatars::avatars_free(&avatars);
	}
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
	gametext_ = rtxt::File{};
	std::string gametext_error;
	bytes.clear();
	have_gametext_ = index_.read_file("gametext.bin", bytes) &&
			rtxt::parse(bytes.data(), bytes.size(), gametext_, gametext_error);
	gamecfg::LoadOptions load;
	load.texts = inmatch::game_cfg_default_texts(have_gametext_ ? &gametext_ : nullptr);
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
// current rotation entry, @0x5695AD..0x5695B1). The network type is the cfg's
// networkconnecttype, which the menus write (1 the NovaWorld screen, 2 the LAN
// one), 1 by default [orig: SetNetworkType(networkConnectType_480)
// @0x4A6609..0x4A6614; UI_InitLANMultiplayerScreen @0x5569E1 (2), @0x556AA0
// (1); Config_SetDefaults @0x54D050 / @0x54D1D4 (1); Config_ParseSettingsLine
// `networkconnecttype` @0x550CA1..0x550CC4]. Without a gate host on the command
// line the server serves LAN whatever the cfg says, where retail's default would
// host on NovaLogic's gs.novaworld.net (D-NET-358, ADR 0051 d6).
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
	novaworld_ = !options_.master_host.empty() && cfg_.networkconnecttype == 1;
	if (options_.master_host.empty() && cfg_.networkconnecttype == 1)
		io::logf(io::LogLevel::kInfo,
				"opennova-serve: serving LAN; NovaWorld listing needs --master-host <gate>");
	if (rotation_.list.current() == nullptr) {
		error = "the host file names no mission the catalog lists";
		return false;
	}
	return true;
}

bool Server::boot_mission(bool next_mission, std::string &error) {
	const inmatch::MissionRotation &list = rotation_.list;
	// The map is the rotation's output, loaded from disk when its catalog row
	// is a loose find and from the archives otherwise
	// [orig: Game_StartMission @0x524769..0x524774 passes g_MapFileName and
	//  g_MissionSourceIsLoose to the BMS load; Mission_LoadBMSFile
	//  @0x40F51A..0x40F527 fopens a loose one].
	const std::string map_file = list.map_file;
	std::vector<uint8_t> bms_bytes;
	if (!index_.read_file(map_file, bms_bytes,
				list.map_source_is_loose ? VfsLookupPolicy::ForceLooseFirst
										 : VfsLookupPolicy::ForceArchiveOnly)) {
		error = "the mission '" + map_file + "' does not read";
		return false;
	}
	bms::File doc;
	std::string parse_error;
	if (!bms::parse(bms_bytes.data(), bms_bytes.size(), doc, parse_error)) {
		error = "the mission '" + map_file + "' did not parse: " + parse_error;
		return false;
	}
	// The mission's base name as every by-name reader takes it: cut at its
	// FIRST '.' [orig: Path_ReplaceOrAppendExtension @0x53C780, the scan
	// @0x53C7C4].
	const std::string basename = mission::mission_base_name(map_file);

	// The session config: the cfg block's (the published player cap with the
	// dedicated slot among it), the session game type from the rotation's row
	// [orig: Game_StartMission @0x5244DE..0x52452B sets g_GameType from the
	//  rotation's current catalog entry +0x1128 on the authority, over the
	//  file's GameType], the mission identity and the expansion check.
	inmatch::GameConfig config = host_.config;
	config.game_type = list.map_game_type;
	config.mission_file = map_file;
	config.mission_name = doc.get_mission_name();
	config.expansion = index_.mounted_expansion();
	config.expansion_version_checksum =
			vfs_expansion_version_checksum(options_.resource_dir, config.expansion);
	config.session_channel = novaworld_ ? inmatch::GameSessionChannel::NovaWorld
										: inmatch::GameSessionChannel::Lan;

	// Serve Only: the dedicated role over the bound socket's game view, no
	// player of the host's own, the session coming up inside the boot. The
	// role, its session and the rotation live for the whole run; a map change
	// keeps them, and the socket with them.
	if (!next_mission) {
		role_ = std::make_unique<inmatch::HostRole>(inmatch::RoleKind::DedicatedHost);
		role_->set_socket(&demux_->game());
		role_->set_rotation(&rotation_);
		session_ = std::make_unique<inmatch::Session>(*role_);
		// The game's protocol joins the socket now: what reached it while the
		// NovaWorld session hosted had no handler and was dropped.
		demux_->set_game_attached(true);
		bind_admin_console();
	}

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
	// A listed server's session runs on the NovaWorld network type, whose
	// session already hosts, as SetNetworkType(1) stands through the match
	// [orig: Game_HostMultiplayerSession @0x4A6614; UI_ProcessLANSessionStateMachine
	//  @0x558e85..0x558e8c].
	request.host_cfg.network_type =
			novaworld_ ? inmatch::NetworkType::NovaWorld : inmatch::NetworkType::Lan;
	request.host_cfg.serve_and_play = host_.serve_and_play;
	request.host_cfg.game_root = options_.resource_dir;
	// The log devices and the socket's address (bound on every interface) for
	// the session's logs (server_logs.h; inmatch/server_console.h).
	request.host_cfg.logs = log_devices_.logs();
	request.host_cfg.local_address = PeerAddr{0, bound_port_};
	request.host_cfg.local_address_known = true;
	// banned.txt and banlist.txt by bare name in the working directory, as retail opens them;
	// the session's round init loads both (server_ban_lists.h).
	request.host_cfg.ban_directory = std::string();
	request.session_score_ini = true;
	request.next_mission = next_mission;
	mission::KernelBootOptions &options = request.boot_options;
	options.playable = false; // Serve Only: no player of the host's own
	options.mp_session = true;
	options.terrain = true;
	options.wac = true;
	options.game_type = config.game_type;
	options.player_limit = static_cast<int32_t>(config.max_players);
	options.team_count = config.num_teams;
	request.fresh_kernel = [this]() -> mission::MissionKernel & {
		auto fresh = std::make_unique<mission::MissionKernel>();
		if (kernel_) fresh->carry_across_load_from(*kernel_);
		kernel_ = std::move(fresh);
		return *kernel_;
	};
	if (!inmatch::boot_host_mission(std::move(request), boot_, error)) return false;
	// No device stages run between the phases on a headless host.
	if (!inmatch::start_host_mission(boot_, inmatch::HostStartDevice{}, error)) return false;
	++missions_played_;
	io::logf(io::LogLevel::kInfo,
			"opennova-serve: '%s' (%s) up: %d entities, terrain %s, WAC %s, %u player slot(s)",
			map_file.c_str(), config.mission_name.c_str(), kernel_->promo.spawned,
			kernel_->terrain_store.valid() ? "loaded" : "absent",
			kernel_->wac_loaded ? "loaded" : "absent", config.max_players);
	return true;
}

// The authority's mission exit, as the main frame and the Post Menu route it:
// a round end in the session (3, or 4 under REPLAY with LASTGAME off) is the
// map change; the end of the rotation, and any other exit, ends the session.
// The map change re-applies the cfg block to the session settings, and the
// PreMenu's init saves game.cfg before the next map loads.
// [orig: Game_ProcessMainFrame @0x526806..0x526867; PostMenu_RouteMissionExit
//  @0x5685C2..0x56864F; PreMenu_Init @0x5693E0 -> Game_SaveConfig @0x5693F4;
//  Game_StartMission @0x524662]
bool Server::route_mission_exit(int32_t reason) {
	if (!role_) return false;
	inmatch::NapiNPServerCtx &ctx = role_->state.host_owner.ctx;
	const bool in_session = ctx.is_in_session != 0;
	if (inmatch::main_frame_exit(reason, in_session, /*authority=*/true) !=
					inmatch::MainFrameExit::PostMenu ||
			!in_session ||
			(reason != inmatch::kMissionExitMapCycle && reason != inmatch::kMissionExitRoundOver))
		return false;
	if (inmatch::begin_host_map_change(*role_, catalog_, &cfg_, &host_) ==
			inmatch::MapChangeStep::RotationEnded) {
		rotation_ended_ = true;
		end_message_ = "the map rotation ended";
		return false;
	}
	(void)save_config();
	std::string error;
	if (!boot_mission(/*next_mission=*/true, error)) {
		end_message_ = "the next mission did not boot: " + error;
		return false;
	}
	// A listed server keeps its NovaWorld session and socket through the map
	// change; the listing follows the new map at the mission start.
	if (listing_ != nullptr) bind_listing();
	return true;
}

// The one socket, opened once for the network type: the authority's bind scan
// over the cfg's LAN server range on the LAN type, and on the NovaWorld type the
// mpnovaworld range whatever the authority, so the NWU session, the game
// traffic and the joiners share it (D-NET-346). Each scans from its first port,
// stepping by its delta and wrapping; --lan-port replaces the first port
// [orig: CNapiNetwork_OpenTransportSocket @0x4C6A40 — the one open @0x4C6A7C,
// the authority arm @0x4C6AA2 over mplanserverportmin / max / delta
// (g_GameConfigState+0x244 / +0x248 / +0x24C), the NovaWorld arm
// @0x4C6AF6..0x4C6B08 over mpnovaworldportmin / max / delta (+0x228 / +0x22C /
// +0x230); NapiUdpSocket_CreateAndBind @0x62D2A0; net_ports.h]. The
// mpnovaworldportrandom start (+0x234, off by default) is not ported. The
// embedder owns the socket layer (net::startup), which is process-wide.
bool Server::open_socket(std::string &error) {
	const uint32_t first = options_.port != 0 ? options_.port
			: static_cast<uint32_t>(novaworld_ ? cfg_.mp_novaworld_port_min : cfg_.mp_lan_server_port_min);
	const uint32_t max = static_cast<uint32_t>(
			novaworld_ ? cfg_.mp_novaworld_port_max : cfg_.mp_lan_server_port_max);
	const uint32_t delta = static_cast<uint32_t>(
			novaworld_ ? cfg_.mp_novaworld_port_delta : cfg_.mp_lan_server_port_delta);
	for (const uint16_t port : lan_host_bind_ports(first, max, delta)) {
		socket_ = net::udp_bind(port, &bound_port_);
		if (socket_.is_valid()) break;
	}
	if (!socket_.is_valid()) {
		error = "no port of the bind scan from " + std::to_string(first) + " is free";
		return false;
	}
	datagrams_ = std::make_unique<net::NetDatagramSocket>(socket_);
	demux_ = std::make_unique<DatagramDemux>(*datagrams_);
	return true;
}

// What the listing carries before the mission boots: the session settings'
// name, message, cap, password and rules, the cfg's LAN-only, ping, player-list
// and country settings, the mount's expansion, and the starting map's game type
// and title. The live match's config replaces the session's own columns once it
// is up (ServeListing::registration).
// [orig: CNapiGameSession_BuildHostVarLists @0x4d0b50 reads the cfg block
//  (serverName_3A5, serverMessage_560, maxPlayers_3F4, hostLanOnly_340);
//  Lobby_UpdateServerInfo @0x4fe8c0 — GameType_GetAbbreviation(g_GameType, 1),
//  the mission's title, AllowPing from g_NWAllowPing @0x4FEF72]
HostRegistration Server::listing_columns() const {
	HostRegistration r;
	const inmatch::GameConfig &config = host_.config;
	r.server_name = config.server_name;
	r.server_message = config.custom_text;
	r.max_players = host_.player_limit;
	r.published_cap = static_cast<int>(config.max_players);
	r.password = !config.server_password.empty();
	r.listen_host = false;
	r.lan_only = cfg_.mp_novaworld_host_lan_only != 0 ? 1 : 0;
	r.allow_ping = cfg_.ping != 0;
	r.send_player_list = cfg_.sendplayerlist != 0;
	r.country = cfg_.country;
	r.expansion = index_.mounted_expansion();
	r.tracers = (config.mp_attributes & inmatch::GameConfig::kMpAttribNoTracers) == 0;
	r.game_type = game_text(have_gametext_ ? &gametext_ : nullptr, "GateTypeAbbrev",
			game_type::host_abbreviation_key(rotation_.list.map_game_type), std::string());
	if (const inmatch::MissionRotationEntry *entry = rotation_.list.current()) {
		const mission_catalog::Row &row = catalog_[entry->catalog_index];
		r.mission_name = row.title.empty() ? row.file : row.title;
	}
	return r;
}

// The NovaWorld leg of the dead auto-host on the one socket: the lister's gate
// probe, the NWU session's verify (the login and the HOSTKEY with an account),
// the host request and the wait for state 6, pumped here until it hosts or
// fails [orig: Game_HostMultiplayerSession @0x4A66BD..0x4A674F —
// WaitForStateChange(gate), ConnectAndWaitForValidation @0x4A66E2,
// BuildHostVarLists @0x4A66FD, StartHostingSession(0) @0x4A6709, the
// ProcessPeriodicUpdate loop while state 5 @0x4A6725..0x4A6735, a reset when it
// is not 6 @0x4A6718]. The session's datagrams ride the demux's session view;
// the gate probe keeps a socket of its own, as retail's gate worker does.
bool Server::host_on_novaworld(std::string &error, const std::atomic<bool> *cancel) {
	if (!novaworld_) return true;
	demux_->set_game_attached(false);
	listing_ = std::make_unique<ServeListing>(listing_columns());
	nw_lister::ListerOptions lister_options;
	lister_options.master_host = options_.master_host;
	lister_options.master_gate_port = options_.master_gate_port;
	lister_options.allow_public = options_.allow_public;
	// Loopback and the OpenNova service by default, NovaLogic's NovaWorld behind --allow-public.
	lister_options.destinations = nw_lister::DestinationPolicy::NovaLogicGated;
	lister_options.credentials = options_.credentials;
	lister_ = std::make_unique<nw_lister::Lister>(lister_options, *listing_, &demux_->session());
	const NwuLobbySession &lobby = lister_->lobby();
	demux_->set_session_claim([&lobby](const PeerAddr &from, const uint8_t *data, std::size_t len) {
		return lobby.claims(from, data, len);
	});
	// The Host list's tokens are the mounted gametext's NovaWorld and TimeOfDay strings.
	lister_->host_role().set_lobby_text(make_host_lobby_text(
			[this](const char *section, const char *key, std::string &out) {
				if (!have_gametext_) return false;
				out = gametext_.get_in_section(section, key);
				return !out.empty();
			}));
	if (!lister_->start()) {
		error = listing_failure(lister_->exit_code());
		return false;
	}
	while (!lister_->hosting()) {
		if (cancel != nullptr && cancel->load()) {
			error = "stopped before the NovaWorld session hosted";
			return false;
		}
		if (!lister_->tick(wall_ms())) {
			error = listing_failure(lister_->exit_code());
			return false;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	io::logf(io::LogLevel::kInfo, "opennova-serve: listed on NovaWorld (%s), the game on UDP %u",
			lister_options.master_host.c_str(), bound_port_);
	return true;
}

bool Server::frame(double delta_seconds) {
	if (!running_) return false;
	// The per-main-frame counter [orig: Game_ProcessMainFrame @0x5265D5 ->
	// Game_TickHudFrameCounters: ++g_MainFrameCounter (ex dword_A8705C)].
	++main_frame_;
	// The NovaWorld session's pass first, as the main frame pumps it ahead of
	// the server tick [orig: Game_ProcessMainFrame @0x526532
	// CNapiGameSession_ProcessPeriodicUpdate]; then its facts reach the match,
	// whose NovaWorld exit reads them, even once the lister has finished.
	if (lister_ != nullptr) {
		if (!lister_->finished()) (void)lister_->tick(wall_ms());
		listing_->sync_session();
	}
	inmatch::FrameInput input;
	input.delta_seconds = delta_seconds;
	const inmatch::FrameOutcome outcome = session_->advance(input);
	// No presenter drains the presentation half of the outbox.
	kernel_->world.out.discard_presentation();
	if (!outcome.terminal()) {
		// The admin pump ends a game frame; a frame whose mission exit pushes the
		// Post Menu returns before it [orig: Game_ProcessMainFrame @0x526872..0x526878
		// (the exit's return), @0x5268F1 (CAdminServer_ProcessFrame)].
		pump_admin();
		return true;
	}
	end_message_ = outcome.error.message;
	const int32_t reason = role_->state.host_owner.ctx.mission_exit_reason;
	if (route_mission_exit(reason)) return true;
	// The admin's quit (exit reason 1, which only GOTO MENUSTATE stores on this host's
	// context) goes straight to the router's teardown [orig: PostMenu_RouteMissionExit
	// @0x5684A8..0x5684AB -> @0x568654].
	if (reason == inmatch::kMissionExitQuit) {
		quit_ = true;
		end_message_ = "the remote admin quit the session (GOTO MENUSTATE)";
	}
	// The session ends here: the rotation's end and the quit take StopServer's
	// goodbye, as the router's teardown does through CNapiGameSession_FullDestroy
	// [orig: PostMenu_RouteMissionExit @0x568683].
	stop();
	return false;
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
	if (role_ && (rotation_ended_ || quit_)) {
		// The rotation's end and the quit left through CNapiGameSession_FullDestroy, whose
		// round init frees banlist.txt's list (off a session it is not reloaded) and re-reads
		// banned.txt with its dirty word cleared, so the exit below saves nothing an in-game ban
		// added
		// [orig: PostMenu_RouteMissionExit @0x568683 -> CNapiGameSession_FullDestroy
		//  @0x4C96A0 -> Server_InitNewRoundState @0x4C9780 (@0x51C92F, @0x51CB25..0x51CB3B)].
		inmatch::NapiNPServerCtx &ctx = role_->state.host_owner.ctx;
		ctx.bans.pcids.reset();
		inmatch::Server_ReloadAddressBanList(ctx);
	}
	if (exit_tail) (void)save_config();
	// The shutdown's banned.txt save, gated on the list and its dirty word
	// [orig: Game_ShutdownSubsystems @0x4A539D -> j_BanList_SaveToFile @0x508E20].
	if (exit_tail && role_) inmatch::Server_SaveAddressBanList(role_->state.host_owner.ctx);
	// The admin server's sockets close with the process.
	if (admin_tcp_) admin_tcp_->close();
	if (role_) role_->set_socket(nullptr);
	if (listing_) listing_->unbind();
	// The NovaWorld deregistration: ClientStopHosting, then the goodbye burst,
	// on the same socket before it closes.
	if (lister_) {
		lister_->stop();
		lister_.reset();
	}
	listing_.reset();
	demux_.reset();
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

bool Server::AdminForward::dispatch(const AdminSession &session, std::string_view line,
		std::vector<std::string> &replies) {
	return console != nullptr ? console->dispatch(session, line, replies) : true;
}

std::string Server::AdminForward::status_report() {
	return console != nullptr ? console->status_report() : std::string();
}

// admin.cfg from the working directory (a missing file is no users and no whitelist), then the
// listener on game.cfg's 16-bit remote_admin_port when it is nonzero, on every interface; a
// listen that fails is ignored, as retail ignores Listen's result.
// [orig: Game_InitSubsystems — CAdminServer_LoadConfig("admin.cfg") @0x4A72C2, the port's
//  `movzx` and nonzero test @0x4A72C7..0x4A72D1, CAdminServer_Listen @0x4A72D9 (its result
//  unread)]
void Server::open_admin() {
	admincfg::AdminConfig config;
	(void)admincfg::load_file(admincfg::kFileName, config);
	admin_server_ = std::make_unique<AdminServer>(std::move(config), admin_forward_, admin_rand_,
			[this](std::string_view line) {
				// The log is a text-mode stream: each "\n" reaches the disk as CR LF.
				std::string text;
				for (const char c : line) {
					if (c == '\n') text += '\r';
					text += c;
				}
				(void)files_.write(kAdminLogFileName, text, /*append=*/true);
			});
	admin_tcp_ = std::make_unique<net::AdminTcpServer>(*admin_server_);
	const uint16_t port = static_cast<uint16_t>(cfg_.remote_admin_port);
	if (port == 0) return;
	if (admin_tcp_->listen(port))
		io::logf(io::LogLevel::kInfo, "opennova-serve: remote admin on TCP %u", admin_tcp_->port());
	else
		io::logf(io::LogLevel::kWarn, "opennova-serve: remote admin did not listen on TCP %u", port);
}

// The console over the session's context, for the whole run: the cfg block SET writes and
// Game_SaveConfig saves, the host's rotation, the mounted gametext and avatars, and GOTO
// MENUSTATE's quit. The chat seams are a listen host's: this host's CHAT SEND and CHAT GET run
// over the context's CHAT ring and flood table (server_console.h).
void Server::bind_admin_console() {
	rotation_admin_ = std::make_unique<inmatch::HostRotationAdmin>(rotation_, catalog_);
	inmatch::AdminConsole::Seams seams;
	seams.config_block = &cfg_;
	seams.save_config = [this] { (void)save_config(); };
	seams.rotation = rotation_admin_.get();
	seams.game_text = [this](std::string_view section, std::string_view key) {
		return have_gametext_ ? gametext_.get_in_section(std::string(section), std::string(key))
							  : std::string();
	};
	seams.characters = &characters_;
	// GOTO MENUSTATE's input action 3 on this host: exit reason 1; the active connection's
	// disconnect drops nothing, as a Serve Only host has no client connection. The next frame's
	// mission exit takes the router's reason-1 arm, which destroys the session; with no menu
	// to land on, the session's end is the process's (frame, quit()).
	// [orig: Input_HandleActionBinding case 3 @0x49AF26 (g_MissionExitReason = 1),
	//  CNapiNetwork_DisconnectActiveConnection @0x4C918F (no connection, no record);
	//  PostMenu_RouteMissionExit @0x5684AB -> CNapiGameSession_FullDestroy @0x568683]
	seams.quit_to_menu = [this] {
		role_->state.host_owner.ctx.mission_exit_reason = inmatch::kMissionExitQuit;
	};
	admin_console_ = std::make_unique<inmatch::AdminConsole>(role_->state.host_owner.ctx, std::move(seams));
	admin_forward_.console = admin_console_.get();
}

// The scene is the Game Loop whenever this runs: a map change runs inside one frame, so the
// pre and post menus' frames, which retail spends between the missions, never reach the pump.
// The challenge's rand() is the process's one CRT stream, the one the game's draws share on the
// main thread; the port keeps that stream on the world (World::crt_rand, D-NET-115), so the pump
// draws from it and hands it back, and an accepted connection advances the session's draws as
// retail's does. [orig: CAdminServer_AcceptConnection @0x405783..0x4057AC (32 draws of
//  rand() % 255 + 1); srand @0x51C1AA, the session's one seed]
void Server::pump_admin() {
	if (!admin_tcp_ || !admin_tcp_->listening() || !admin_console_) return;
	admin_console_->set_scene(inmatch::AdminScene::GameLoop);
	admin_console_->set_main_frame(main_frame_);
	admin_rand_ = kernel_->world.crt_rand;
	admin_tcp_->pump();
	kernel_->world.crt_rand = admin_rand_;
}

} // namespace opennova::serve
