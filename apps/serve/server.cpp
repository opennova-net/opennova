#include "server.h"

#include "destination_policy.h" // resolve_destination (the second listing's gate lookup)
#include "lister.h"
#include "profile_name.h"
#include "serve_listing.h"

#include <base/gameprofile/game_type.h>
#include <base/gameprofile/gameprofile.h>
#include <base/io/file_io.h>
#include <base/io/log.h>
#include <base/io/strutil.h>
#include <base/resource_index/boot_policy.h>
#include <base/vfs/vfs.h>
#include <formats/admincfg/admin_cfg.h>
#include <formats/avatars/avatars.h>
#include <formats/mission/bms.h>
#include <formats/rtxt/rtxt.h>
#include <net/novaworld/lobby_identity.h> // the NWHWI device reads
#include <net/title/title_identity.h>
#include <runtime/hud/hud_math.h> // hud_label_font_choice (HUD_InitAllFonts)
#include <runtime/inmatch/host_config.h>
#include <runtime/inmatch/loaded_model_list.h> // the S2C 0x68 cursor's wrap
#include <runtime/inmatch/map_change.h>
#include <runtime/inmatch/mission_exit.h>
#include <runtime/inmatch/server_ban_lists.h>
#include <runtime/inmatch/punt_log.h> // the connect log's socket teardown line
#include <runtime/inmatch/server_console.h> // the chat display wrap
#include <runtime/inmatch/server_info.h> // read_session_listing_columns (the start's update)
#include <runtime/mission/mission_sidecars.h>
#include <runtime/mission/runtime_boot.h>
#include <runtime/world/present_drains.h> // discard_entity_presentation (no presenter)
#include <runtime/world/tick_count_clock.h> // frame_tick_count_ms (the NovaWorld session's clock)

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <ctime> // the second listing's next try, in the supervisor's clock
#include <iterator>
#include <thread>
#include <utility>

namespace opennova::serve {

namespace {

const char kUsage[] =
		"usage: opennova-serve --resource-dir <game dir> /HOST <host file> [/maxplayers <n>]\n"
		"                      [/exp <name>] [/d]\n"
		"                      [/game <code>] [/LAN] [--loose-root] [--lan-port <n>] "
		"[--log-debug]\n"
		"                      [--master-host <gate>] [--master-gate-port <n>]\n"
		"                      [--credentials <file>] [--allow-public]\n"
		"                      [--also-master-host <gate>] [--also-master-gate-port <n>]\n"
		"                      [--also-credentials <file>]\n"
		"                      [--status-page on|off] [--stats-file <file>] [--exit-file <file>]\n"
		"                      [--second-listing-file <file>]\n"
		"  --resource-dir   the game install to serve from (its PFF set, as opennova.exe takes "
		"it)\n"
		"  /HOST            the host file: retail's `/HOST <file>` format, one `Key value` per "
		"line\n"
		"                   (GameName, MaxPlayers, KillLimit, ...) and one `Mission <file.bms>`\n"
		"                   line per rotation entry; the last Mission line is the starting map\n"
		"  /maxplayers <n>  the cap a host file's MaxPlayers line yields to (retail's switch)\n"
		"  /exp, /d, /game  mount the expansion (/mod is /exp), prefer loose files, pick the\n"
		"                   data's game code: its keying and the title the server speaks\n"
		"                   (default jo)\n"
		"  /LAN             every game.cfg read sets mpnovaworldhostlanonly: a NovaWorld\n"
		"                   listing advertises LAN only (the game's other switches are\n"
		"                   accepted, and those the server has no use for do nothing)\n"
		"  --loose-root     mount a directory that holds no game archives as loose files\n"
		"  --lan-port       the first port of the bind scan (default: game.cfg "
		"mplanserverportmin,\n"
		"                   the head of the retail LAN server range; mpnovaworldportmin when\n"
		"                   listing on NovaWorld); 0 binds a port the OS picks\n"
		"  --log-debug      print the engine's debug log lines\n"
		"  --master-host    the NovaWorld gate to list on (127.0.0.1 for a\n"
		"                   NovaWorld gate on this machine). With it, game.cfg's\n"
		"                   networkconnecttype picks the network: 1 (the default) lists on\n"
		"                   NovaWorld, 2 serves LAN only. Without it the server serves LAN\n"
		"  --master-gate-port  the gate's UDP port (default: the NovaWorld gate port)\n"
		"  --credentials    KEY=VALUE file: NOVAWORLD_USER / NOVAWORLD_PASS, the account the\n"
		"                   listing logs in with for its HOSTKEY (none: no HOSTKEY, which the\n"
		"                   NovaWorld service accepts)\n"
		"  --allow-public   allow NovaLogic's NovaWorld (novaworld.net and its hosts), a live\n"
		"                   shared service: use it sparingly. Loopback and any other host are\n"
		"                   allowed without it\n"
		"  --also-master-host, --also-master-gate-port, --also-credentials\n"
		"                   a second NovaWorld gate to list the same server on, its port and\n"
		"                   its account (needs --master-host); it lists once the first map is\n"
		"                   up, is tried again when it does not host, and never stops the server\n"
		"  --status-page    on (the default) prints the status page on stdout and takes stdin\n"
		"                   as its chat input; off does neither (a supervised server's)\n"
		"  --stats-file     rewrite this file once a minute with the last minute's tick health\n"
		"                   (frame time p50, p99 and longest, late frames), one JSON line\n"
		"  --exit-file      at a non-zero exit, write the error that ended the server there,\n"
		"                   one line\n"
		"  --second-listing-file\n"
		"                   rewrite this file whenever the second listing's state changes (off,\n"
		"                   trying, listed, retrying or not_listed, with why), one JSON line\n"
		"  /PROFILE <path>  record each mission to <path>.sph (retail's server log)\n"
		"  /PUNTLOG, /PUNT.TXT, /CHEATLOG\n"
		"                   log punts to _PUNT.TXT, start _CHEAT.TXT (the working directory)\n"
		"  /NETLOG          start the net log, _NETLOG.TXT (the remote admin's CMD NETLOG\n"
		"                   toggles it)\n"
		"  /INOUT           log the host's start and stop and each player's add and removal\n"
		"                   to _inout.txt\n"
		"The server reads and writes game.cfg in the directory it runs from (the process's\n"
		"working directory, as retail), with the host file's settings over it, and marks\n"
		"itself running there with activesrvr.txt, which a clean exit deletes. A nonzero\n"
		"remote_admin_port in game.cfg opens retail's remote-admin console on that TCP port,\n"
		"its users in admin.cfg there and its log in admin_log.txt; banned.txt and\n"
		"banlist.txt there are the ban lists.\n";

// The one-token switches, retail-spelled ones matched case-insensitively.
bool parse_port(const std::string &text, uint16_t &out, bool allow_zero = false) {
	char *end = nullptr;
	const unsigned long value = std::strtoul(text.c_str(), &end, 0);
	if (text.empty() || end == nullptr || *end != '\0' || (value == 0 && !allow_zero) || value > 0xFFFF)
		return false;
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

// The clock the NovaWorld session runs on, the process's GetTickCount (the main loop's; engine
// world/tick_count_clock.h carries its steps, D-NET-NEW-LF-2)
// [orig: CNapiNPConnection_PumpSendIntervals @0x628FD0 - GetTickCount @0x628FE3].
uint32_t wall_ms() { return world::frame_tick_count_ms(); }

// The lister's outcome as start's error text, naming the cfg word and the gate host the
// listing ran with (a listing runs only for networkconnecttype 1 with a --master-host).
std::string listing_failure(int code, int32_t networkconnecttype, const std::string &master_host) {
	const char *what = code == kExitNetwork ? "a NovaWorld socket did not open"
			: code == kExitStoppedByService
			? "the NovaWorld service stopped the hosting"
			: "the NovaWorld session, the login, the HOSTKEY or the host request failed";
	return std::string("the NovaWorld listing did not host: ") + what +
			" (game.cfg networkconnecttype = " + std::to_string(networkconnecttype) +
			" lists on NovaWorld through --master-host " + master_host +
			"; networkconnecttype = 2, or a start without --master-host, serves LAN only)";
}

// The device facts a Serve Only host's NWHWI prints, sampled once at its boot. A /SERVEONLY boot
// opens the renderer as any other (only the video test is skipped), so they are this machine's by
// retail's reads: the physical memory (the command line's GlobalMemoryStatus), the desktop main()
// sampled (Renderer_Init's), adapter 0's description and the cfg's hw3d_deviceno adapter's DxDiag
// display memory (the device creation's). Off Windows read_display_adapter has no Direct3D, so
// the description is retail's no-device "(null)" and the display memory 0; a failed DxDiag query
// is 0 too (the device keeps the query's result only when it is not negative).
// [orig: Game_ParseCommandLineAndInit @0x4A7310 - g_TotalPhysMemoryMB stored @0x4A7E15;
//  Game_InitSubsystems @0x4A6CD0 - Renderer_Init called @0x4A70F2, the serve-only test @0x4A710C
//  skipping only Game_RunVideoTestDialog, Renderer_SetDisplayModeWithFallback called @0x4A722A;
//  CGfxDevice_CreateDevice @0x67E5D0 - the query called @0x67EBEC, kept @0x67EBFD or 0 @0x67EC25]
HardwareIdentity sample_device_identity(const gamecfg::GameCfg &cfg, const ServeOptions &options) {
	HardwareIdentity hw;
	read_total_physical_memory(hw.total_phys_bytes);
	hw.desktop_width = options.desktop_width;
	hw.desktop_height = options.desktop_height;
	read_display_adapter(static_cast<uint32_t>(cfg.hw3d_deviceno), hw);
	return hw;
}

// Whether `from` is `lobby`'s server: the 0x81 sender it took, or one of its probe targets.
bool from_lobby_server(const NwuLobbySession &lobby, const PeerAddr &from) {
	if (from == lobby.nw_peer()) return true;
	for (const NwuLobbySession::Target &target : lobby.targets())
		if (target.ip == from.ip && static_cast<uint16_t>(target.port) == from.port) return true;
	return false;
}

// A session lane's claim (DatagramDemux::Claim): its lister's session's (NwuLobbySession::
// claims), and while the socket carries a second listing (D-NET-1385), less a datagram from the
// other listing's server that is not its own's too. Both connections have CI 1, so a 0x81 either
// would take by its CI from any sender (retail's rule for its one session) could be the other's
// server answering the other's probe. A lane whose lister is gone takes nothing.
bool lane_claims(const Lister *own, const Lister *other, const PeerAddr &from, const uint8_t *data,
		std::size_t len) {
	if (own == nullptr || !own->lobby().claims(from, data, len)) return false;
	return other == nullptr || !from_lobby_server(other->lobby(), from) ||
			from_lobby_server(own->lobby(), from);
}

} // namespace

const char *serve_usage() { return kUsage; }

int parse_serve_options(const std::vector<std::string> &args, ServeOptions &out, std::string &error,
		const std::string &command_line) {
	// The retail switches, read as the game reads its own command line: one walk over the line's
	// tokens, a value-taking switch stepping onto its value, any token it does not know passed over
	// (walk_retail_switches) [orig: Game_ParseCommandLineAndInit @0x4a7310].
	const RetailSwitches retail = walk_retail_switches(
			retail_command_line_tokens(command_line.empty() ? retail_command_line(args) : command_line));
	out.loose_override = retail.loose_override;
	out.expansion = retail.expansion;
	out.host_file = retail.host_file.value_or(std::string());
	out.max_player_arg = retail.max_players;
	out.lan_switch = retail.lan;
	out.nopunt = retail.nopunt;
	out.log_switches = retail.log_switches;
	// The server's own options; every other token is the walk's (a switch, a switch's value, or
	// a token the game passes over).
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
		if (a == "--loose-root") {
			out.loose_root = true;
		} else if (a == "--log-debug") {
			out.log_debug = true;
		} else if (a == "--resource-dir") {
			if (!value(out.resource_dir)) return 1;
		} else if (strutil::iequals(a, kLaunchFlagGame)) {
			if (!value(out.game)) return 1;
		} else if (a == "--lan-port") {
			std::string text;
			if (!value(text)) return 1;
			uint16_t port = 0;
			if (!parse_port(text, port, /*allow_zero=*/true)) {
				error = "--lan-port must be 0..65535";
				return 1;
			}
			out.port = port;
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
		} else if (a == "--also-master-host") {
			if (!value(out.also_master_host)) return 1;
		} else if (a == "--also-master-gate-port") {
			std::string text;
			if (!value(text)) return 1;
			if (!parse_port(text, out.also_master_gate_port)) {
				error = "--also-master-gate-port must be 1..65535";
				return 1;
			}
		} else if (a == "--also-credentials") {
			if (!value(out.also_credentials_path)) return 1;
		} else if (a == "--allow-public") {
			out.allow_public = true;
		} else if (a == "--status-page") {
			std::string text;
			if (!value(text)) return 1;
			if (text != "on" && text != "off") {
				error = "--status-page takes on or off";
				return 1;
			}
			out.status_page = text == "on";
		} else if (a == "--stats-file") {
			if (!value(out.stats_file)) return 1;
		} else if (a == "--exit-file") {
			if (!value(out.exit_file)) return 1;
		} else if (a == "--second-listing-file") {
			if (!value(out.second_listing_file))
				return 1;
		} else if (a.rfind("--", 0) == 0) {
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
	if (!out.also_master_host.empty() && out.master_host.empty()) {
		error = "--also-master-host needs --master-host: the second listing lists beside the first";
		return 1;
	}
	// Two sessions with one service would share its session server's datagrams on the one socket.
	if (!out.also_master_host.empty() && strutil::iequals(out.also_master_host, out.master_host) &&
			out.also_master_gate_port == out.master_gate_port) {
		error = "--also-master-host names the --master-host gate: the second listing lists on another service";
		return 1;
	}
	return 0;
}

Server::Server(ServeOptions options) : options_(std::move(options)) {
	// The switch every read of the process takes [orig: g_LanSwitch, read by
	// Game_LoadConfig @0x5514B1].
	config_.set_lan_switch(options_.lan_switch);
}

Server::~Server() {
	stop();
	if (chat_font_valid_) fnt::fnt_free(&chat_font_);
}

bool Server::start(std::string &error, const std::atomic<bool> *cancel) {
	return begin(error, cancel) && start_session(error);
}

bool Server::begin(std::string &error, const std::atomic<bool> *cancel) {
	// The admin server's log opens for writing at the process's static
	// construction, ahead of everything, so every launch truncates it whether
	// or not the listener ever opens [orig: CAdminServer_Construct @0x402C10,
	// fopen("admin_log.txt", "w") @0x402C84].
	(void)files_.write(inmatch::kAdminLogFileName, std::string_view(), /*append=*/false);
	// The log switches arm at the process start, as retail's command-line
	// parse arms them (inmatch/server_log_devices.h).
	{
		// The title /game names speaks the header (JO's without one or for an unknown code).
		std::string title_error;
		log_devices_.arm(options_.log_switches,
				inmatch::connect_log_header(resolve_title_identity(
						options_.game.empty() ? std::string("jo") : options_.game, title_error)));
	}
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
	if (!resolve_title(error) || !read_boot_config(error) || !mount(error) ||
			!read_config_over_weapons(error)) {
		stop();
		return false;
	}
	// The renderer's start and its device creation, after that game.cfg read: the device facts
	// NWHWI prints (sample_device_identity), read once.
	device_identity_ = sample_device_identity(config_.cfg(), options_);
	// The subsystems' tail: admin.cfg and the listener [orig: Game_InitSubsystems
	// @0x4A72B8..0x4A72D9, after the game.cfg read @0x4A70AB].
	open_admin();
	if (!read_host_file(error) || !open_socket(error) || !host_on_novaworld(error, cancel)) {
		stop();
		return false;
	}
	begun_ = true;
	return true;
}

// The starting map: the session settings from the cfg block as it stands at the create, which a
// SetMPReset during the hosting wait changed (ServeListing::on_command), then the boot, whose
// session create reads the block's mpreset word
// [orig: CNapiGameSession_BuildAndCreateSession @0x5694D0 builds the session settings from the
//  block at the create, @0x56955D..0x56956C; CNapiGameSession_CreateSession @0x4C97E7 reads
//  dword_25509FC itself].
bool Server::start_session(std::string &error) {
	if (!begun_ || running_) {
		error = "the server has not begun, or its session already stands";
		return false;
	}
	host_ = inmatch::host_session_settings(config_.cfg());
	if (!boot_mission(/*next_mission=*/false, error)) {
		stop();
		return false;
	}
	if (listing_ != nullptr) bind_listing();
	running_ = true;
	// The second listing's first try starts beside the bound match, pumped by the frames.
	if (second_listing_wanted()) begin_second_try();
	return true;
}

// The mission start's server-info update, which the host boot runs past the mission text and a
// map change's head, ahead of the load window and the loads (HostBootRequest::
// mission_start_update). The update reads the globals as they stand there: the session settings
// the start applied, the rotation's game type, the mission text's title or else the header's name,
// the header's climate (the validate read the header ahead of it), TimeOfDay's class the apply
// cleared, and the last round's clock (a fresh process's 0), the session's own columns over them
// (its settings, lock and keys: the create stood it up ahead of the start). Every mission start on
// a NovaWorld authority in session runs it outside the 1860-tick timer, both lists whole; the
// starting map's is the fresh host's first full Host list, its host result having published
// nothing (D-NET-465), and its METPROTOCOL ID report follows (g_SessionRoundCount == 0,
// D-NET-NEW-L9-22). The session's periodic update then sends them, so they leave the load's
// length ahead of the round's first refresh
// [orig: Game_StartMission @0x524360 - Game_ApplySessionSettingsToGlobals called @0x524662
//  (`xor ebp, ebp` @0x552064, the g_EnvTimeOfDayEnum store @0x55207B),
//  BMS_LoadAndValidateHeader called @0x524774 (g_BmsHeaderBlock read @0x40E2A9 / @0x40E2D4),
//  TextResource_LoadMissionTextBin @0x5247CE, the gates @0x5248C6..0x5248DC,
//  Lobby_UpdateServerInfo called @0x5248F5, CPlayerManager_RebuildLists(1) called @0x524903 ->
//  CNapiGameSession_SendHostUpdate @0x4d45b5, Server_SendMetricsToGate called @0x524928,
//  CNapiGameSession_ProcessPeriodicUpdate called @0x52493D; g_RoundTimeRemaining's stores
//  @0x524A5D / @0x525251 follow].
void Server::run_mission_start_update(const std::string &header_name, int32_t climate) {
	if (listing_ == nullptr || lister_ == nullptr || lister_->finished()) return;
	HostRegistration r = listing_columns();
	// The session the boot's head created ahead of this mission start (HostRole::create_session,
	// D-NET-NEW-L1-50): its columns over the settings, as a bound listing reads them, the round-state
	// init's GameServerBaffleKey and the cookie-key ring's first key among them.
	// [orig: CNapiGameSession_BuildAndCreateSession @0x5694D0 - Server_InitNewRoundState called
	//  @0x5695B3 (the g_GameServerBaffleKey store @0x51CB88, CSessionIdRing_Init called @0x51CB8E),
	//  ahead of GameLoop_Init's Game_StartMission call @0x526377; Lobby_UpdateServerInfo @0x4FE8C0 -
	//  the ring's current key @0x4FF191..0x4FF196, g_GameServerBaffleKey read @0x4FF1CC]
	if (role_ != nullptr)
		inmatch::read_session_listing_columns(r, role_->state.host_owner.ctx,
				[this](const char *section, const char *key, const char *fallback) {
					return game_text(have_gametext_ ? &gametext_ : nullptr, section, key, fallback);
				});
	// Then the mission start's own columns (inmatch::apply_mission_start_columns carries the
	// witness): the mission text's title else the header's name, its climate, TimeOfDay 0, the last
	// round's clock.
	inmatch::apply_mission_start_columns(r, boot_.mission_text.get_string("info", "title"),
			header_name, climate, client_process_.round_time_remaining_ticks);
	listing_->set_base(std::move(r));
	lister_->publish_server_info(rotation_.round_count);
	(void)lister_->tick(wall_ms());
	// A hosting second listing publishes the same mission start on its own session.
	if (second_lister_ != nullptr && second_published_ && second_lister_->hosting()) {
		second_lister_->publish_server_info(rotation_.round_count);
		(void)second_lister_->tick(wall_ms());
	}
}

// The listing meets the mission that just started: the live source reads the new kernel and the
// kept server context, its columns the map's from here; its update went out at the mission start
// (run_mission_start_update).
void Server::bind_listing() {
	listing_->set_base(listing_columns());
	listing_->bind(*role_, *kernel_, *lister_);
}

// Game_Run's read, before the mount: no weapon table yet (every avail_wpn
// line drops) and no gametext (the `!` default texts). A set mpreset exits
// the process there [orig: Game_Run @0x4A7FBB -> Game_LoadConfig @0x551480,
// crt_exit @0x5514A1..0x5514AC]; a missing file is the defaults.
bool Server::read_boot_config(std::string &error) {
	if (!config_.load()) {
		reset_exit_ = true;
		error = "game.cfg sets mpreset: the process exits";
		return false;
	}
	return true;
}

bool Server::resolve_title(std::string &error) {
	const std::string code = options_.game.empty() ? std::string("jo") : options_.game;
	title_ = resolve_title_identity(code, error);
	if (title_ == nullptr) {
		error = "/game " + code + ": " + error;
		return false;
	}
	// The listing's login and HOSTKEY fetch request the title's web pages by name, the second
	// listing's as the first's.
	const bool first_logs_in = !options_.master_host.empty() && options_.credentials.present();
	const bool second_logs_in =
			!options_.also_master_host.empty() && options_.also_credentials.present();
	if (first_logs_in || second_logs_in) {
		const std::string refusal = title_identity_refusal(*title_, TitleIdentityPart::WebFlow);
		if (!refusal.empty()) {
			error = "/game " + code + " with " +
					(first_logs_in ? "--credentials" : "--also-credentials") + ": " + refusal;
			title_ = nullptr;
			return false;
		}
	}
	// The title's cfg table, which every game.cfg read and save walks: dfx2.exe's carries the
	// mpdfxcategory its host lists print as DFXCATEGORY1 (DFX2-7).
	config_.set_config_table(title_->host_category_var ? gamecfg::ConfigTable::Dfx2
	                                                   : gamecfg::ConfigTable::Jo);
	return true;
}

// The mount opennova.exe makes (godot/game/boot_root_mount.gd over
// ResourceRoot::mount_runtime / set_root_dir): the witnessed fixed archive
// table (Packed, PackedWithLooseOverride under /d) with the game code's SCR
// keying, and, only when asked, a directory with no archives mounted as a
// plain loose root with no expansion, under the JO keying when /game names no
// title (ADR 0025). Then the gate tag: the title's, or the mount's gt.ssc's.
bool Server::mount(std::string &error) {
	const std::string code = options_.game.empty() ? std::string("jo") : options_.game;
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
		if (game.empty()) game = "jo";
	}
	if (!mounted) {
		error = "could not mount '" + options_.resource_dir + "': " + index_.last_error();
		return false;
	}
	index_.set_scr_policy(gameprofile::gameprofile_scr_policy_for_code(game.c_str()));
	// The boot's gt.ssc over the gate manager's default tag, read once the archives mount
	// [orig: Game_InitSubsystems @0x4a7149..0x4a714e -> Mission_LoadEncryptedConfig @0x4cdcd0].
	gate_tag_ = title_gate_tag(*title_, &index_);
	io::logf(io::LogLevel::kInfo, "opennova-serve: speaking %s, gate tag %s", code.c_str(),
			gate_tag_.c_str());
	// The expansion's text-override table, as the boot's Expansion_LoadAssets
	// reads it with no archive open (vfs.h carries the witness).
	text_override_table_.clear();
	if (!vfs_expansion_override_table(options_.resource_dir, index_.mounted_expansion(),
				ExpansionLoadPoint::ArchivesClosed, options_.loose_override, text_override_table_))
		text_override_table_.clear();
	text_override_ = rtxt::File{};
	std::string override_error;
	have_text_override_ = !text_override_table_.empty() &&
			rtxt::parse(text_override_table_.data(), text_override_table_.size(), text_override_,
					override_error);
	assets_ = std::make_unique<assets::AssetStore>(&index_);
	catalog_ = mission_catalog::build(index_, have_text_override_ ? &text_override_ : nullptr);
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
	config_.set_weapon_roster(inmatch::game_cfg_weapon_roster(index_));
	gametext_ = rtxt::File{};
	std::string gametext_error;
	std::vector<uint8_t> bytes;
	have_gametext_ = index_.read_file("gametext.bin", bytes) &&
			rtxt::parse(bytes.data(), bytes.size(), gametext_, gametext_error);
	config_.set_default_texts(inmatch::game_cfg_default_texts(have_gametext_ ? &gametext_ : nullptr,
			have_text_override_ ? &text_override_ : nullptr));
	if (!config_.load()) {
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
	std::string text;
	std::string read_error;
	if (!io::read_file_text(options_.host_file, text, read_error)) {
		error = "the host file '" + options_.host_file + "' does not open";
		return false;
	}
	report_ = inmatch::read_host_file(text.data(), text.size(), config_.cfg(), rotation_, catalog_,
			options_.max_player_arg);
	for (const std::string &key : report_.unknown_keys)
		io::logf(io::LogLevel::kWarn, "opennova-serve: the host file key '%s' is not a host setting",
				key.c_str());
	for (const std::string &name : report_.unknown_missions)
		io::logf(io::LogLevel::kWarn, "opennova-serve: Mission '%s' is not in the mission catalog",
				name.c_str());
	if (!gamecfg::write_active_server_marker(gamecfg::kActiveServerMarkerFileName))
		io::logf(io::LogLevel::kWarn, "opennova-serve: %s did not open for writing",
				gamecfg::kActiveServerMarkerFileName);
	config_.cfg().dedicated = 1;
	(void)save_config();
	host_ = inmatch::host_session_settings(config_.cfg());
	novaworld_ = !options_.master_host.empty() && config_.cfg().networkconnecttype == 1;
	if (options_.master_host.empty() && config_.cfg().networkconnecttype == 1)
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

	// The session config: the cfg block's (the session's cap, and the dedicated
	// word its slot limit adds the host's own slot for), the session game type from the rotation's row
	// [orig: Game_StartMission @0x5244DE..0x52452B sets g_GameType from the
	//  rotation's current catalog entry +0x1128 on the authority, over the
	//  file's GameType], the mission identity and the expansion check.
	// The block as it stands, not the startup's reading of it: every mission start applies it
	// again, a map change's too, so what an admin SET wrote into it (the mp-attribute word, the
	// rules) reaches the next mission; the create's game_settings passwords stay the session's
	// (continue_session) (D-NET-NEW-L1-42) [orig: Game_StartMission @0x524360 - the first-load
	//  flag @0x5243A4, Game_ApplySessionSettingsToGlobals @0x524662 under it @0x52465E;
	//  CAdminServer_HandleSetCommand @0x405A60 - the cfg word @0x406190].
	// The block's title, cap and server type first (host_screen_from_block, which applies its
	// session words too), then the mission start's staging (inmatch/host_config.h).
	inmatch::GameConfig config = inmatch::host_screen_from_block(config_.cfg()).config;
	inmatch::apply_mission_start_session_config(config, nullptr, list, doc.get_mission_name());
	config.expansion = index_.mounted_expansion();
	config.expansion_version_checksum =
			vfs_expansion_version_checksum(options_.resource_dir, config.expansion);
	config.session_channel = novaworld_ ? inmatch::GameSessionChannel::NovaWorld
										: inmatch::GameSessionChannel::Lan;
	// The profile the row runs under, the boot's load over the working directory's player.sav
	// and weapon.sav (the boot's seed where they are missing, record 0 named after the
	// process's account): its name is the local profile's (g_LocalProfileName), which the
	// mission start copies onto the row's body, and its weapon record's sides the session
	// create's profile init gives the row (D-NET-352) [orig: PlayerProfile_LoadAllFromDisk
	//  @0x54F4D0 - GetUserNameA @0x54F535, File_CheckExists @0x54F586 / @0x54F6C7;
	//  Game_ApplySessionSettingsToGlobals @0x551680..0x551695; Player_InitPlayer
	//  @0x4E184F..0x4E186A; CNapiGameSession_CreateSession @0x4C97C0 - @0x4C9CCE].
	const serve::ServeProfile row_profile = serve::read_serve_profile(config_.cfg().player_index,
			serve::process_account_name(), config.expansion, characters_);
	config.player_name = row_profile.name;

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
	// A listed server's mission start publishes its listing ahead of the load, from the header
	// this read validated (run_mission_start_update).
	if (listing_ != nullptr) {
		request.mission_start_update = [this, header_name = doc.get_mission_name(),
				climate = static_cast<int32_t>(doc.header.climate)] {
			run_mission_start_update(header_name, climate);
		};
	}
	request.mission = std::move(doc);
	request.mission_basename = basename;
	request.files = mission::boot_files_from_index(index_);
	request.text_override_table = text_override_table_;
	request.assets = assets_.get();
	request.session = session_.get();
	request.role = role_.get();
	request.host = role_.get();
	request.host_cfg.title = title_;
	request.host_cfg.config = config;
	request.host_cfg.socket_mode = inmatch::SocketMode::Lan;
	// A listed server's session runs on the NovaWorld network type, whose
	// session already hosts, as SetNetworkType(1) stands through the match
	// [orig: Game_HostMultiplayerSession @0x4A6614; UI_ProcessLANSessionStateMachine
	//  @0x558e85..0x558e8c].
	request.host_cfg.network_type =
			novaworld_ ? inmatch::NetworkType::NovaWorld : inmatch::NetworkType::Lan;
	request.host_cfg.serve_and_play = host_.serve_and_play;
	// The process's client words, whose NovaWorld teardown leg the listing installs.
	request.host_cfg.client_process = &client_process_;
	request.host_cfg.game_root = options_.resource_dir;
	// The mounted Avatars.def's combos the player add validates against (D-NET-137).
	request.host_cfg.characters = characters_;
	// The row's sides from the profile's weapon record (a fresh record's without weapon.sav,
	// host_boot's default).
	if (row_profile.character_vars) request.host_cfg.local_character_vars = row_profile.character_vars;
	// The log devices and the socket's address (bound on every interface) for
	// the session's logs (inmatch/server_log_devices.h; inmatch/server_console.h).
	request.host_cfg.logs = log_devices_.logs();
	request.host_cfg.nopunt = options_.nopunt; // /NOPUNT (NapiNPServerCtx::nopunt)
	request.host_cfg.local_address = PeerAddr{0, bound_port_};
	request.host_cfg.local_address_known = true;
	// banned.txt and banlist.txt by bare name in the working directory, as retail opens them;
	// the session's round init loads both (server_ban_lists.h).
	request.host_cfg.ban_directory = std::string();
	// The process clock the send-interval legs and the receive reap measure (GetTickCount's):
	// the mission load runs no tick, its send pumps still fire a due leg (D-NET-256).
	request.host_cfg.clock_ms = inmatch::host_monotonic_milliseconds;
	request.session_score_ini = true;
	request.next_mission = next_mission;
	mission::KernelBootOptions &options = request.boot_options;
	options.playable = false; // Serve Only: no player of the host's own
	options.mp_session = true;
	options.terrain = true;
	options.wac = true;
	options.game_type = config.game_type;
	options.player_limit = static_cast<int32_t>(config.player_slot_limit());
	options.team_count = config.num_teams;
	request.fresh_kernel = [this]() -> mission::MissionKernel & {
		auto fresh = std::make_unique<mission::MissionKernel>();
		if (kernel_) fresh->carry_across_load_from(*kernel_);
		kernel_ = std::move(fresh);
		return *kernel_;
	};
	if (!inmatch::boot_host_mission(std::move(request), boot_, error)) {
		// The session create's mpreset exit: retail's crt_exit(0) ends the
		// process inside the create, so Game_Run's exit tail never runs (no
		// game.cfg save, no banned.txt save, activesrvr.txt left behind); its
		// atexit table still tears the NovaWorld session down (stop()). The
		// process exits with code 0. Only the starting map creates a session
		// here: a map change continues it
		// [orig: CNapiGameSession_CreateSession @0x4C97E7..0x4C97F0 ->
		//  crt_exit(0), skipping Game_Run @0x4A7FFF..0x4A800E].
		if (boot_.session_create == inmatch::CreateSessionResult::ProcessExit) {
			reset_exit_ = true;
			exit_save_owed_ = false;
		}
		return false;
	}
	// No device stages run between the phases on a headless host.
	if (!inmatch::start_host_mission(boot_, inmatch::HostStartDevice{}, error)) return false;
	// The mission start freezes its model-def list, whose count every S2C 0x68
	// cursor wraps at: a dedicated host builds it from the mission's loads,
	// with no renderer and no foliage leg [orig: Game_StartMission @0x524360
	// (the dedicated jump @0x5259B4..0x5259BF, the call @0x525A6E) ->
	// Render_InitMissionTextures @0x587120 (the reduced arm's call @0x58728C)
	// -> CEffectWorld_ResolveMaterialsAndRebuildChallengeList @0x5B3A80].
	loaded_model_list_count_ = static_cast<uint32_t>(inmatch::mission_start_model_page(
			*kernel_, mission::boot_files_from_index(index_), &boot_.charattr,
			/*authority=*/true, /*dedicated=*/true).size());
	select_chat_font();
	++missions_played_;
	io::logf(io::LogLevel::kInfo,
			"opennova-serve: '%s' (%s) up: %d entities, terrain %s, WAC %s, %u player slot(s)",
			map_file.c_str(), config.mission_name.c_str(), kernel_->promo.spawned,
			kernel_->terrain_store.valid() ? "loaded" : "absent",
			kernel_->wac_loaded ? "loaded" : "absent", config.player_slot_limit());
	return true;
}

// Every mission start reloads the HUD fonts for the display's width: the display mode the
// block holds (the last game.cfg video_res that parsed), else the defaults' desktop width.
// The bold slot is the chat display's wrap font, and with no hud.def mounted the chat box's
// width is 4. A font that does not read leaves the slot empty, and no line wraps.
// [orig: Game_StartMission @0x525A9A -> HUD_InitAllFonts @0x51EE20 (the bold name by the
//  width @0x51EE7E..0x51EEE4, HUD_LoadFontIntoSlot @0x51EF3F); Config_ParseSettingsLine
//  @0x5512F4..0x551303 (video_res into outWidth); Chat_AddMessageChannel1 @0x498673..0x4986AC]
void Server::select_chat_font() {
	int32_t width = 0;
	int32_t height = 0;
	gamecfg::display_size(config_.cfg(), options_.desktop_width, options_.desktop_height, width, height);
	const std::string name = hud::hud_label_font_choice(width).bold_fnt;
	if (name != chat_font_name_) {
		if (chat_font_valid_) fnt::fnt_free(&chat_font_);
		chat_font_ = fnt::fnt_font_t{};
		chat_font_valid_ = false;
		chat_font_name_ = name;
		std::vector<uint8_t> bytes;
		if (index_.read_file(name, bytes))
			chat_font_valid_ = fnt::fnt_parse(bytes.data(), bytes.size(), &chat_font_) == fnt::FNT_OK;
	}
	inmatch::NapiNPServerCtx &ctx = role_->state.host_owner.ctx;
	ctx.console_chat_wrap.font = chat_font_valid_ ? &chat_font_ : nullptr;
	ctx.console_chat_wrap.width = inmatch::kChatDisplayWidthNoHudDef;
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
	if (inmatch::begin_host_map_change(*role_, catalog_, &config_.cfg(), &host_) ==
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
// stepping by its delta and wrapping; --lan-port replaces the first port (0,
// the OS's pick, which the first bind takes)
// [orig: CNapiNetwork_OpenTransportSocket @0x4C6A40 — the one open @0x4C6A7C,
// the authority arm @0x4C6AA2 over mplanserverportmin / max / delta
// (g_GameConfigState+0x244 / +0x248 / +0x24C), the NovaWorld arm
// @0x4C6AF6..0x4C6B08 over mpnovaworldportmin / max / delta (+0x228 / +0x22C /
// +0x230, its random word +0x234 @0x4C6B08); NapiUdpSocket_CreateAndBind
// @0x62D2A0; net_ports.h]. The
// embedder owns the socket layer (net::startup), which is process-wide.
bool Server::open_socket(std::string &error) {
	// The NovaWorld quad's random word starts the scan at a drawn port; the
	// authority's LAN arm passes random 0 (inmatch::transport_bind_ports).
	const inmatch::NetworkType type =
			novaworld_ ? inmatch::NetworkType::NovaWorld : inmatch::NetworkType::Lan;
	const std::optional<uint32_t> first =
			options_.port ? std::optional<uint32_t>(*options_.port) : std::nullopt;
	const std::vector<uint16_t> ports =
			inmatch::transport_bind_ports(config_.cfg(), type, /*authority=*/true, first);
	// Every port of the scan binds on the block's mpipaddressstring (inmatch::
	// transport_bind_address carries the witness).
	const uint32_t address = inmatch::transport_bind_address(config_.cfg());
	for (const uint16_t port : ports) {
		socket_ = net::udp_bind(port, &bound_port_, address);
		if (socket_.is_valid()) break;
	}
	// The connect log's socket lines, the failure's with its SetTransportMode(0) teardown line
	// (inmatch::write_transport_socket_open_lines carries the witness).
	const PeerAddr bound{address, static_cast<uint16_t>(bound_port_)};
	inmatch::write_transport_socket_open_lines(log_devices_.connect(),
			socket_.is_valid() ? &bound : nullptr, address,
			inmatch::transport_port_quad(config_.cfg(), type, /*authority=*/true, first));
	if (!socket_.is_valid()) {
		error = "no port of the bind scan from " + std::to_string(ports.front()) + " is free";
		return false;
	}
	datagrams_ = std::make_unique<net::NetDatagramSocket>(socket_);
	// A second listing's session rides the same socket on a lane of its own, so its service too
	// learns the game socket's address from its datagrams (D-NET-346).
	demux_ = std::make_unique<DatagramDemux>(*datagrams_, second_listing_wanted() ? 2 : 1);
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
	r.published_cap = static_cast<int>(config.player_slot_limit());
	r.password = !config.server_password.empty();
	r.listen_host = false;
	r.lan_only = config_.cfg().mp_novaworld_host_lan_only != 0 ? 1 : 0;
	r.category = config_.cfg().mp_dfx_category; // DFXCATEGORY1, where the title's lists carry it
	r.allow_ping = config_.cfg().ping != 0;
	r.send_player_list = config_.cfg().sendplayerlist != 0;
	r.country = config_.cfg().country;
	// GCC, and the first mission's METPROTOCOL ID report's: the install's CC.BIN (D-NET-NEW-L9-22).
	// [orig: Server_InitServerTicks @0x5018C0 - Game_ReadCCBinFile @0x5018E8 into byte_C87044]
	r.gcc = vfs_country_code(options_.resource_dir);
	r.expansion = index_.mounted_expansion();
	r.tracers = (config.mp_attributes & inmatch::GameConfig::kMpAttribNoTracers) == 0;
	// [orig: dword_24D218C read @0x4FECD7]
	r.skins = config.allow_custom_skins != 0;
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
	// The admin console's save seam (bind_admin_console), for the service's SetServerName /
	// SetServerMsg / SetMPReset.
	ServeListing::Seams seams;
	seams.config_block = &config_.cfg();
	seams.save_config = [this] { (void)save_config(); };
	seams.gametext = [this](const char *section, const char *key, const char *fallback) {
		return game_text(have_gametext_ ? &gametext_ : nullptr, section, key, fallback);
	};
	listing_ = std::make_unique<ServeListing>(listing_columns(), std::move(seams));
	ListerOptions lister_options = base_lister_options();
	lister_options.master_host = options_.master_host;
	lister_options.master_gate_port = options_.master_gate_port;
	lister_options.credentials = options_.credentials;
	lister_options.connect_log = &log_devices_.connect();
	server_logs_ = log_devices_.logs();
	lister_options.server_logs = &server_logs_;
	last_lister_exit_ = -1;
	lister_ = std::make_unique<Lister>(lister_options, *listing_, &demux_->session());
	demux_->set_session_claim([this](const PeerAddr &from, const uint8_t *data, std::size_t len) {
		return lane_claims(lister_.get(), second_lister_.get(), from, data, len);
	});
	// Each player's teardown sends its ClientHostPlayerRemoved on the hosting session itself
	// (inmatch::NapiNPServerCtx::nwu_player_removed, D-NET-NEW-L9-29), and on the second
	// listing's beside it (D-NET-1385, none of retail's) [orig: Server_HandlePlayerDisconnect @0x51B5C0 -
	//  CNapiGameSession_SendPlayerRemovedIfHosting called @0x51B5F3].
	client_process_.nwu_player_removed = [this](int player_number) {
		if (lister_ != nullptr) lister_->host_role().clear_player_slot(player_number);
		if (second_lister_ != nullptr) second_lister_->host_role().clear_player_slot(player_number);
	};
	set_lobby_text(*lister_);
	if (!lister_->start()) {
		error = listing_failure(lister_->exit_code(), config_.cfg().networkconnecttype,
				options_.master_host);
		return false;
	}
	while (!lister_->hosting()) {
		if (cancel != nullptr && cancel->load()) {
			error = "stopped before the NovaWorld session hosted";
			return false;
		}
		if (!lister_->tick(wall_ms())) {
			error = listing_failure(lister_->exit_code(), config_.cfg().networkconnecttype,
					options_.master_host);
			return false;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	io::logf(io::LogLevel::kInfo, "opennova-serve: listed on NovaWorld (%s), the game on UDP %u",
			lister_options.master_host.c_str(), bound_port_);
	return true;
}

ListerOptions Server::base_lister_options() const {
	ListerOptions options;
	options.title = title_;
	options.gate_tag = gate_tag_;
	// Loopback and the OpenNova service by default, NovaLogic's NovaWorld behind --allow-public.
	options.allow_public = options_.allow_public;
	options.gate_ports = inmatch::gate_bind_ports(config_.cfg());
	options.extended_metrics = config_.cfg().mp_extract_extended_metric_information != 0;
	// NWHWI at the connect: the boot's device facts and game.cfg's display mode, formatted
	// [orig: CNapiGameSession_OnNovaWorldConnected @0x4D1570 - sprintf called @0x4D178F].
	HardwareIdentity hw = device_identity_;
	gamecfg::display_size(config_.cfg(), hw.desktop_width, hw.desktop_height, hw.display_width,
			hw.display_height);
	options.hardware_identity = make_nwhwi(hw);
	return options;
}

// The Host list's tokens are the mounted gametext's NovaWorld and TimeOfDay strings.
void Server::set_lobby_text(Lister &lister) {
	lister.host_role().set_lobby_text(make_host_lobby_text(
			[this](const char *section, const char *key, std::string &out) {
				if (!have_gametext_) return false;
				out = gametext_.get_in_section(section, key);
				return !out.empty();
			}));
}

// The socket's second session lane. OpenNova's own composition, none of retail's: a stock host
// lists on the one service its gate names (D-NET-1385, ADR 0068). Everything below is the
// second listing's policy:
// - it waits for the starting map (the first listing's hosting holds the boot, retail's order;
//   the second never does), and nothing of it blocks the frame thread: its gate's DNS lookup
//   runs on a worker, and a torn-down try's web exchange is left to land on its own;
// - it publishes its first full lists the moment it hosts, as a mission start would;
// - its service's decision (ServerStopHosting, its punt, a ServerCommand's stop, a refused
//   login, host request or re-host) ends it there alone for the rest of the run, as a stock
//   host's listing that does not host ends: no retry against that service;
// - a fault on the way (a gate that does not answer or resolve, a request that never reached
//   the service, no hosting by kSecondListingHostDeadlineMs) is tried again after
//   kSecondListingRetryFirstMs, doubling up to kSecondListingRetryMaxMs, at most
//   kSecondListingTriesMax tries.
constexpr std::size_t kSecondListingLane = 1;

bool Server::second_listing_wanted() const {
	return novaworld_ && !options_.also_master_host.empty();
}

void Server::begin_second_try() {
	if (second_resolve_.valid() || second_lister_ != nullptr || second_stopped_) return;
	// The destination rule is the run's own, the same at every try: a gate it refuses is final.
	if (!destination_permitted(options_.also_master_host, options_.allow_public)) {
		io::logf(io::LogLevel::kWarn,
				"opennova-serve: the second listing's gate %s needs --allow-public; it is not tried "
				"this run",
				options_.also_master_host.c_str());
		second_stopped_ = true;
		second_reason_ = "its gate needs --allow-public";
		second_tag_.clear();
		return;
	}
	// The destination rule by name, as the listing's own lookups apply it (destination_policy.h);
	// the try then probes the dotted address, which the rule passes.
	second_resolve_ = std::async(std::launch::async,
			[host = options_.also_master_host,
					allow_public = options_.allow_public]() -> std::optional<std::string> {
				net::Endpoint gate{};
				if (!resolve_destination(host, gate, allow_public, "the second listing's gate"))
					return std::nullopt;
				return std::to_string(gate.ip[0]) + "." + std::to_string(gate.ip[1]) + "." +
						std::to_string(gate.ip[2]) + "." + std::to_string(gate.ip[3]);
			});
}

void Server::start_second_listing(const std::string &gate_address, uint32_t now_ms) {
	if (listing_ == nullptr || lister_ == nullptr || demux_ == nullptr || second_lister_ != nullptr ||
			second_stopped_)
		return;
	if (second_source_ == nullptr) second_source_ = std::make_unique<SecondListing>(*listing_);
	ListerOptions options = base_lister_options();
	options.master_host = gate_address;
	options.master_gate_port = options_.also_master_gate_port;
	options.credentials = options_.also_credentials;
	// Its own AppId, apart from the first registration's: each service's .joi hands its joiners
	// its own registration's, so the match's join check knows which listing sent each (code 9,
	// NapiNPConnection::novaworld_listing) and routes its ticket to that listing's service.
	options.taken_app_id = static_cast<uint32_t>(lister_->host_role().app_id());
	// The process's _connectlog.txt and _inout.txt are retail's lines of its one listing: the
	// second listing writes neither, only its console lines, which its label marks.
	options.label = "(second listing)";
	second_lister_ = std::make_unique<Lister>(options, *second_source_,
			&demux_->session(kSecondListingLane));
	second_source_->set_lister(second_lister_.get());
	second_published_ = false;
	second_started_ms_ = now_ms;
	// Each lane's claim reads the listers as they stand when a datagram lands, so neither holds a
	// lister that went.
	demux_->set_session_claim([this](const PeerAddr &from, const uint8_t *data, std::size_t len) {
		return lane_claims(second_lister_.get(), lister_.get(), from, data, len);
	}, kSecondListingLane);
	set_lobby_text(*second_lister_);
	listing_->set_second(second_lister_.get());
	if (!second_lister_->start()) {
		end_second_listing(/*stop_hosting=*/false);
		schedule_second_retry(now_ms, "its sockets did not open");
	}
}

void Server::schedule_second_retry(
		uint32_t now_ms, const std::string &why, const std::string &tag) {
	second_tag_ = tag;
	if (++second_faults_ >= kSecondListingTriesMax) {
		second_stopped_ = true;
		second_reason_ =
				"it did not host in " + std::to_string(second_faults_) + " tries; the last: " + why;
		io::logf(io::LogLevel::kWarn,
				"opennova-serve: the second listing (%s) did not host (%s); %d tries, not tried "
				"again this run",
				options_.also_master_host.c_str(), why.c_str(), second_faults_);
		return;
	}
	second_reason_ = why;
	second_backoff_ms_ = second_backoff_ms_ == 0
			? kSecondListingRetryFirstMs
			: std::min(second_backoff_ms_ * 2u, kSecondListingRetryMaxMs);
	second_retry_at_ms_ = now_ms + second_backoff_ms_;
	// The supervisor's copy of when it comes (second_listing_report), in its own clock.
	second_retry_at_unix_ = static_cast<int64_t>(std::time(nullptr)) + second_backoff_ms_ / 1000u;
	io::logf(io::LogLevel::kWarn,
			"opennova-serve: the second listing (%s) did not host (%s); trying again in %u s",
			options_.also_master_host.c_str(), why.c_str(), second_backoff_ms_ / 1000u);
}

SecondListingReport Server::second_listing_report() const {
	SecondListingReport report;
	if (!second_listing_wanted())
		return report;
	// The players its service sent, whatever its state: they stay when the listing ends.
	if (role_ != nullptr) {
		for (const inmatch::NapiNPConnection &conn :
				role_->state.host_owner.ctx.np_protocol.connection_list)
			if (conn.type == inmatch::NapiNPConnection::kTypeServerSide &&
					inmatch::player_slot_active(conn) && conn.novaworld_listing == 1)
				report.slots.push_back(static_cast<int>(conn.reply.player_slot));
		std::sort(report.slots.begin(), report.slots.end());
	}
	if (second_stopped_) {
		report.state = SecondListingState::NotListed;
		report.reason = second_reason_;
		report.tag = second_tag_;
		return report;
	}
	if (second_lister_ != nullptr && second_lister_->hosting()) {
		report.state = SecondListingState::Listed;
		return report;
	}
	report.attempt = second_faults_ + 1;
	report.attempts = kSecondListingTriesMax;
	// A try under way (its gate's lookup or its lister), or the first not yet begun.
	if (second_lister_ != nullptr || second_resolve_.valid() || second_faults_ == 0) {
		report.state = SecondListingState::Trying;
		return report;
	}
	report.state = SecondListingState::Retrying;
	report.next_try_at = second_retry_at_unix_;
	report.reason = second_reason_;
	report.tag = second_tag_;
	return report;
}

void Server::tick_second_listing() {
	if (!second_listing_wanted() || listing_ == nullptr) return;
	// The torn-down tries' web exchanges that landed go.
	second_http_left_.erase(std::remove_if(second_http_left_.begin(), second_http_left_.end(),
			[](const std::future<net::HttpReply> &left) {
				return left.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
			}), second_http_left_.end());
	if (second_stopped_) return;
	const uint32_t now = wall_ms();
	if (second_lister_ == nullptr) {
		if (second_resolve_.valid()) {
			if (second_resolve_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
			const std::optional<std::string> address = second_resolve_.get();
			if (address)
				start_second_listing(*address, now);
			else
				schedule_second_retry(now, "its gate did not resolve or was refused");
		} else if (static_cast<int32_t>(now - second_retry_at_ms_) >= 0) {
			begin_second_try();
		}
		return;
	}
	if (!second_lister_->finished()) (void)second_lister_->tick(now);
	if (second_lister_->hosting()) {
		if (second_published_) return;
		second_published_ = true;
		second_backoff_ms_ = 0;
		second_faults_ = 0;
		second_reason_.clear();
		second_tag_.clear();
		// Its first full Host list and PlayerList, its HostKey among them, as the mission start's
		// update sends a fresh host's (its host result published nothing, D-NET-465).
		second_lister_->publish_server_info(rotation_.round_count);
		io::logf(io::LogLevel::kInfo, "opennova-serve: also listed on NovaWorld (%s)%s",
				options_.also_master_host.c_str(),
				second_lister_->host_role().requires_join_ticket()
						? "; its joiners' tickets go to it"
						: "");
		return;
	}
	// Not hosting. Its service's decision, final for the run as a stock host's listing that does
	// not host ends: ServerStopHosting (the session left up, as a stock host's is until its
	// match's NovaWorld exit), its punt (ServerLeaveNovaWorld, the session ended), a
	// ServerCommand's stop (the hosting left with ClientStopHosting), a re-host refused after its
	// session's reconnect (the hosting left with none of those: NwuHostRole's
	// handle_host_result), or a refusal that ended the try (a login, a HOSTKEY page or a host
	// request refused: Lister::refused).
	const bool finished = second_lister_->finished();
	const bool punted = finished && second_lister_->exit_code() == kExitStoppedByService;
	const bool rehost_refused = second_published_ && !finished;
	const bool command_stopped = second_source_ != nullptr && second_source_->command_stopped();
	if (second_lister_->stopped_by_service() || punted || command_stopped || rehost_refused ||
			(finished && second_lister_->refused())) {
		// Why, for the supervisor: the stop the service sent, or the refusal the lister ended on
		// (a punt's among them).
		second_tag_.clear();
		if (second_lister_->stopped_by_service()) {
			second_reason_ = "the service stopped the hosting";
		} else if (command_stopped) {
			second_reason_ = "a command from the service stopped it";
		} else if (rehost_refused) {
			second_reason_ = "the service refused the hosting again after its session reconnected";
		} else {
			second_reason_ = second_lister_->failure().empty()
					? std::string("the service refused it")
					: second_lister_->failure();
			second_tag_ = second_lister_->failure_tag();
		}
		io::logf(io::LogLevel::kWarn,
				"opennova-serve: the second listing's service (%s) stopped or refused it (%s); it "
				"is "
				"not tried again this run",
				options_.also_master_host.c_str(), second_reason_.c_str());
		second_stopped_ = true;
		end_second_listing(/*stop_hosting=*/false);
		return;
	}
	if (!finished) {
		// Still on its way (the gate, the session, the login and the HOSTKEY, the host request),
		// within the deadline.
		if (now - second_started_ms_ <= kSecondListingHostDeadlineMs) return;
		end_second_listing(/*stop_hosting=*/false);
		schedule_second_retry(now,
				"it was not listed within " +
						std::to_string(kSecondListingHostDeadlineMs / 60000u) + " minutes");
		return;
	}
	// A fault on the way: a gate or a session that did not answer, a request that never reached
	// the service; the lister's own account of it when it has one.
	const std::string why = second_lister_->failure().empty()
			? std::string("its gate or its session did not answer")
			: second_lister_->failure();
	const std::string tag = second_lister_->failure_tag();
	end_second_listing(/*stop_hosting=*/false);
	schedule_second_retry(now, why, tag);
}

void Server::announce_joins() {
	if (role_ == nullptr)
		return;
	// The service a listing's gate names, as the console calls it: NovaLogic's NovaWorld under
	// novaworld.net, OpenNova's on any other (destination_policy.h), the gate beside it.
	const auto service = [](const std::string &gate, uint16_t port) {
		return std::string(is_novaworld_domain_host(gate) ? "NovaLogic's NovaWorld ("
														  : "OpenNova's NovaWorld (") +
				gate + ":" + std::to_string(port) + ")";
	};
	std::set<uint32_t> present;
	for (const inmatch::NapiNPConnection &conn :
			role_->state.host_owner.ctx.np_protocol.connection_list) {
		if (conn.type != inmatch::NapiNPConnection::kTypeServerSide ||
				!inmatch::player_slot_active(conn))
			continue;
		present.insert(conn.connection_id);
		if (!announced_joins_.insert(conn.connection_id).second)
			continue;
		const std::string via = !novaworld_ ? std::string("on the LAN")
				: conn.novaworld_listing == 1
				? "via " + service(options_.also_master_host, options_.also_master_gate_port)
				: "via " + service(options_.master_host, options_.master_gate_port);
		io::logf(io::LogLevel::kInfo, "opennova-serve: [join] \"%s\" %s", conn.player_name.c_str(),
				via.c_str());
	}
	for (auto it = announced_joins_.begin(); it != announced_joins_.end();)
		it = present.count(*it) != 0 ? std::next(it) : announced_joins_.erase(it);
}

void Server::end_second_listing(bool stop_hosting) {
	if (second_lister_ == nullptr) return;
	if (listing_ != nullptr) listing_->set_second(nullptr);
	if (second_source_ != nullptr) second_source_->set_lister(nullptr);
	if (stop_hosting)
		second_lister_->stop();
	else
		second_lister_->disconnect();
	if (demux_ != nullptr) demux_->set_session_claim(nullptr, kSecondListingLane);
	// A web exchange still out lands on its own: the lister goes without waiting on it.
	std::future<net::HttpReply> left = second_lister_->release_http();
	if (left.valid()) second_http_left_.push_back(std::move(left));
	second_lister_.reset();
	second_published_ = false;
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
	tick_second_listing();
	inmatch::FrameInput input;
	input.delta_seconds = delta_seconds;
	input.loaded_model_list_count = loaded_model_list_count_;
	const inmatch::FrameOutcome outcome = session_->advance(input);
	// No presenter drains the presentation half of the outbox, nor the entity presenter's
	// queues: the fire ring among them, which grew by every round fired until the next map.
	kernel_->world.out.discard_presentation();
	world::discard_entity_presentation(kernel_->world);
	announce_joins();
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
	begun_ = false;
	// Game_Run's exit: game.cfg saved, the subsystems (the socket among them)
	// shut down, then the lock deleted, unconditionally [orig: Game_Run
	// @0x4A7FFF Game_SaveConfig, @0x4A8004 Game_ShutdownSubsystems,
	// @0x4A8009..0x4A800E DeleteFileA("activesrvr.txt")].
	const bool exit_tail = exit_save_owed_;
	exit_save_owed_ = false;
	// An error arm's router resets the NovaWorld session first: SetNetworkType(0) runs
	// ResetToDisconnected, whose goodbye burst alone (no ClientStopHosting, whatever the hosting
	// state) and its NWU DISCONNECTING line come ahead of SetTransportMode(0)'s socket teardown
	// line below. A NovaWorld exit (12) after the service's ServerStopHosting is one (the lister
	// held its session up to it, D-NET-NEW-L9-20) (D-NET-NEW-L9-23).
	// [orig: PostMenu_RouteMissionExit @0x568460 - loc_5686DA: CNapiNetwork_SetNetworkType(0)
	//  called @0x5686E1, CNapiNetwork_SetTransportMode(0) called @0x5686F9;
	//  CNapiNetwork_SetNetworkType @0x4C4A50 - CNapiGameSession_ResetToDisconnected called
	//  @0x4C4A7D; CNapiGameSession_ResetToDisconnected @0x4D0890 - RequestDisconnect called
	//  @0x4D08A1; CNapiNPConnection_RequestDisconnect @0x61E0F0 - SetState(6) @0x61E0FA;
	//  CNapiNPConnection_SetState @0x626250 - TeardownActiveConnection called @0x62628F (the
	//  state-5 burst, then the OnDisconnect line); CNapiNetwork_SetTransportMode @0x4C8750 -
	//  CNapiNetwork_TearDownSocket called @0x4C877B]
	const inmatch::PostMissionRoute exit_route = inmatch::route_mission_exit(
			role_ ? role_->state.host_owner.ctx.mission_exit_reason : 0, /*novaworld=*/true);
	if (lister_ && role_ && exit_route.error != inmatch::PostMissionError::None) {
		// The first listing's service ended its hosting (the NovaWorld exit), so its burst goes
		// alone; the second's still hosts, so it deregisters as a stock host does, first.
		end_second_listing(/*stop_hosting=*/true);
		lister_->disconnect();
	}
	// The game socket's teardown line, once, while the _connectlog.txt log is still on (no
	// connection ever came): the session's FullDestroy on the rotation's end or the quit, else
	// the shutdown's (D-NET-NEW-L8-8) [orig: CNapiNetwork_TearDownSocket @0x4C4C90 (the line
	// @0x4C4CBE..0x4C4CE7) from CNapiGameSession_FullDestroy @0x4C9763 / @0x4C976F and
	// NapiLib_Shutdown @0x4C9663 / @0x4C9674, each behind a nonzero socket state, the
	// shutdown's ahead of the net log's close]
	if (role_) log_devices_.connect().line(inmatch::net_tearing_down_socket_text());
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
	// Its network's: a server an error-arm exit left unstopped writes its /INOUT lines (the
	// shutdown's session reset, behind +0x68), then the net log stops and flushes
	// [orig: @0x4A5398 NapiLib_Shutdown - ResetActiveSession called @0x4C958B].
	if (exit_tail && role_) role_->stop_unstopped_server_at_exit(log_devices_.logs());
	if (exit_tail) log_devices_.close(role_ ? role_->state.host_owner.ctx.np_protocol.host_run_duration_ms : 0u);
	// The admin server's sockets close with the process.
	if (admin_tcp_) admin_tcp_->close();
	if (role_) role_->set_socket(nullptr);
	if (listing_) listing_->unbind();
	// The NovaWorld deregistration: ClientStopHosting, then the goodbye burst,
	// on the same socket before it closes. The session create's exit sends the
	// burst alone: retail's crt_exit runs no ClientStopHosting, but its atexit
	// table destroys the NovaWorld session, whose connection teardown sends the
	// burst, and the Winsock stack is still up then, held by the remote-admin
	// server's static reference, whose release runs after both NovaWorld
	// handlers (the table runs last-registered first)
	// [orig: CNapiGameSession_Destroy, registered @0x7939DA (initializer slot
	//  0x7C0578) -> CNapiGameSession_ResetToDisconnected @0x4D0890 ->
	//  CNapiNPConnection_Destroy -> SendDisconnectPacket @0x61F2A0 -> sendto;
	//  CAdminServer_Construct's WSAStartup @0x402C22 (slot 0x7C0554), its
	//  WSACleanup @0x406DE2 from the destructor registered @0x79377A].
	// The second listing goes the same way as the first, ahead of it. A try's gate lookup and the
	// torn-down tries' web exchanges still out are waited out after the first listing's own
	// deregistration (each holds copies alone, no lister or lane), so a hung exchange never holds
	// it back past a container's stop grace.
	end_second_listing(/*stop_hosting=*/!reset_exit_);
	second_source_.reset();
	if (lister_) {
		if (reset_exit_)
			lister_->disconnect();
		else
			lister_->stop();
		last_lister_exit_ = lister_->exit_code();
		lister_.reset();
	}
	second_resolve_ = {};
	second_http_left_.clear();
	listing_.reset();
	demux_.reset();
	datagrams_.reset();
	if (socket_.is_valid()) net::close_socket(socket_);
	if (exit_tail) gamecfg::remove_active_server_marker(gamecfg::kActiveServerMarkerFileName);
}

bool Server::save_config() {
	// [orig: Game_SaveConfig @0x54C490: fopen("game.cfg", "w"); a file that
	//  does not open writes nothing, @0x54C4AF..0x54C4BB. The NovaWorld
	//  ServerCommand calls it @0x4D2DDF (SetServerName / SetServerMsg) and
	//  @0x4D2E2D (SetMPReset), through ServeListing's seam]
	std::string error;
	if (config_.save(error))
		return true;
	io::logf(io::LogLevel::kWarn, "opennova-serve: %s", error.c_str());
	return false;
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
	// The round clock with no session bound is the process's (RemoteAdminConsoles::use_process_state).
	admin_consoles_.use_process_state(&client_process_);
	admin_server_ = std::make_unique<AdminServer>(std::move(config), admin_consoles_, admin_rand_,
			[this](std::string_view line) {
				(void)files_.write(inmatch::kAdminLogFileName, inmatch::admin_log_file_text(line),
						/*append=*/true);
			});
	admin_tcp_ = std::make_unique<net::AdminTcpServer>(*admin_server_);
	const uint16_t port = inmatch::remote_admin_listen_port(config_.cfg());
	if (port == 0) return;
	if (admin_tcp_->listen(port))
		io::logf(io::LogLevel::kInfo, "opennova-serve: remote admin on TCP %u", admin_tcp_->port());
	else
		io::logf(io::LogLevel::kWarn, "opennova-serve: remote admin did not listen on TCP %u", port);
}

// The console over the session's context, for the whole run: the cfg block SET writes and
// Game_SaveConfig saves, the host's rotation, the mounted gametext and avatars; GOTO
// MENUSTATE's quit is the binding's (RemoteAdminConsoles: exit reason 1 on the context). The
// chat seams are a listen host's: this host's CHAT SEND and CHAT GET run over the context's
// CHAT ring and flood table (server_console.h).
void Server::bind_admin_console() {
	rotation_admin_ = std::make_unique<inmatch::HostRotationAdmin>(rotation_, catalog_);
	inmatch::AdminConsole::Seams seams;
	seams.config_block = &config_.cfg();
	seams.save_config = [this] { (void)save_config(); };
	seams.rotation = rotation_admin_.get();
	seams.game_text = [this](std::string_view section, std::string_view key) {
		return have_gametext_ ? gametext_.get_in_section(std::string(section), std::string(key))
							  : std::string();
	};
	seams.characters = &characters_;
	// CMD SAVENFO names the mission's items.def rows.
	seams.items_table = [this]() -> const def::DefItemsFile * {
		return kernel_ ? kernel_->items_table() : nullptr;
	};
	// CMD's WAC overlay toggle on this authority (D-NET-371).
	seams.toggle_overlay = [this](const std::string &line) {
		return kernel_ && kernel_->wac.toggle_cheat(line, true, kernel_->world);
	};
	// GOTO MENUSTATE's input action 3 on this host: exit reason 1 on the context (the binding's
	// quit); the active connection's disconnect drops nothing, as a Serve Only host has no client
	// connection. The next frame's mission exit takes the router's reason-1 arm, which destroys
	// the session; with no menu to land on, the session's end is the process's (frame, quit()).
	// [orig: Input_HandleActionBinding case 3 @0x49AF26 (g_MissionExitReason = 1),
	//  CNapiNetwork_DisconnectActiveConnection @0x4C918F (no connection, no record);
	//  PostMenu_RouteMissionExit @0x5684AB -> CNapiGameSession_FullDestroy @0x4C96A0 (the call
	//  @0x568683)]
	// CMD's binding records (a typed `tod 1230`) join the role's input event queue, which every
	// update's drain dispatches (engine inmatch/admin_console.h Seams::input_events).
	seams.input_events = &role_->input_events();
	inmatch::RemoteAdminBinding binding;
	binding.host = &role_->state.host_owner.ctx;
	admin_consoles_.bind(binding, std::move(seams));
}

// The scene is the Game Loop whenever this runs: a map change runs inside one frame, so the
// pre and post menus' frames, which retail spends between the missions, never reach the pump.
// The challenge's rand() is the process's one CRT stream, the one the game's draws share on the
// main thread; the port keeps that stream on the world (World::crt_rand, D-NET-115), so the pump
// draws from it and hands it back, and an accepted connection advances the session's draws as
// retail's does. [orig: CAdminServer_AcceptConnection @0x405783..0x4057AC (32 draws of
//  rand() % 255 + 1); srand @0x51C1AA, the session's one seed]
void Server::pump_admin() {
	inmatch::AdminConsole *console = admin_consoles_.console();
	if (!admin_tcp_ || !admin_tcp_->listening() || console == nullptr) return;
	console->set_scene(inmatch::AdminScene::GameLoop);
	console->set_main_frame(main_frame_);
	admin_rand_ = kernel_->world.crt_rand;
	admin_tcp_->pump();
	kernel_->world.crt_rand = admin_rand_;
}

} // namespace opennova::serve
