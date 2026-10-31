#pragma once

// opennova-serve's server: the game's Serve Only host with no Godot (ADR 0051).
// It runs retail's dead /HOST path: the working directory's game.cfg
// (gamecfg::load), the game data mounted the way the game mounts it, the
// retail host file over the cfg block (inmatch/host_file.h), the directory
// lock (activesrvr.txt) and the cfg saved as Serve Only, the session settings
// from the block (inmatch/host_config.h), the one socket bound for the cfg's
// network type (the LAN server range, or on NovaWorld the mpnovaworld range),
// on NovaWorld the listing hosted on that socket before the mission boots (the
// lister's session, ADR 0051 d6), the starting mission booted as a
// DedicatedHost through the engine's one host boot (inmatch/host_boot.h) and
// the in-match session run round after round over the host file's rotation
// (the engine's map change, inmatch/map_change.h, game.cfg saved at each and
// at the NovaWorld ServerCommand's rename, message or mpreset change; the
// listing and its socket kept across it); at a clean exit it deregisters,
// saves game.cfg again and deletes the lock. Everything below the config, the
// mount, the socket and the wall clock is the engine's.
//
// The remote-admin server (ADR 0051 PR5b): admin_log.txt truncated at the
// launch, admin.cfg read once the subsystems are up and the listener opened on
// game.cfg's remote_admin_port when it is nonzero, then pumped once per game
// frame into the engine's console (inmatch/admin_console.h) over the host's
// own rotation (inmatch/rotation_admin.h). The ban files load at the session's
// round init and banned.txt is saved at the exit when an in-game ban dirtied it.
//
// The files are the process's working directory's, as retail's: game.cfg,
// activesrvr.txt, admin.cfg, admin_log.txt, banned.txt and banlist.txt open as
// bare relative names, never under --resource-dir (unless the server runs from
// there).

#include <base/io/crt_rand.h>
#include <base/resource_index/resource_index.h>
#include <formats/gamecfg/game_cfg.h>
#include <net/admin/admin_server.h>
#include <runtime/assets/asset_store.h>
#include <runtime/inmatch/admin_console.h>
#include <runtime/inmatch/remote_admin.h>
#include <runtime/inmatch/character_registry.h>
#include <runtime/inmatch/client_runtime.h> // ClientProcessState (the process's NovaWorld legs)
#include <runtime/inmatch/game_config_block.h>
#include <runtime/inmatch/host_boot.h>
#include <runtime/inmatch/host_file.h>
#include <runtime/inmatch/host_role.h>
#include <runtime/inmatch/main_loop_clock.h> // MainLoopClock (frame())
#include <runtime/inmatch/mission_rotation.h>
#include <runtime/inmatch/session.h>
#include <runtime/mission/mission_catalog.h>
#include <runtime/mission/mission_kernel.h>
#include <formats/rtxt/rtxt.h>
#include <net/novaworld/gate_probe.h>
#include <net/novaworld/lobby_vars.h>
#include <net/npwire/datagram_demux.h>
#include <runtime/inmatch/server_log_devices.h> // the log devices the switches arm

#include "admin_tcp_server.h"
#include "credentials.h"
#include "run_reports.h" // SecondListingReport
#include "net_datagram_socket.h"
#include "net_http.h"
#include "net_sockets.h"

#include <atomic>
#include <cstdint>
#include <future>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace opennova::serve {

class Lister;
struct ListerOptions;

// The display width a host takes when its platform reports none (opennova-serve off Windows,
// as on the hosted servers' Linux machines, or a failed GetSystemMetrics): a representative
// retail host's desktop. Retail always runs on a Windows desktop, whose width picks the HUD
// fonts (HUD_InitAllFonts @0x51EE20); 1024 picks Arial16b as every retail host wider than 800
// does, where a width of 0 would pick Arial12b, whose narrow glyphs split chat lines retail's
// do not. A platform equivalent, not a witnessed value (D-NET-720, platform class).
inline constexpr int32_t kHeadlessDesktopWidth = 1024;
// Its height, the 4:3 desktop that width names (the defaults' display height and NWHWI's desktop
// field); the same platform class (D-NET-720).
inline constexpr int32_t kHeadlessDesktopHeight = 768;

struct ServeOptions {
	std::string resource_dir; // --resource-dir
	std::string expansion;    // /exp or /mod <name>: the last one, its first 32 bytes
	bool loose_override = false; // /d
	// OpenNova launch overrides for the existing engine controls. Omitted: the
	// session type's send period and the normal 600-byte snapshot budget.
	std::optional<uint32_t> send_holdoff_ticks; // --send-holdoff-ticks, 0..255
	std::optional<uint32_t> entity_send_budget; // --entity-send-budget, 100..1600
	std::string game;         // /game <code>
	bool loose_root = false;  // --loose-root: a directory with no archives mounts loose
	std::string host_file;    // /HOST <file>
	int32_t max_player_arg = 0; // /maxplayers N: the host file's MaxPlayers yields to it (0: none)
	bool lan_switch = false;  // /LAN: every game.cfg read forces mpnovaworldhostlanonly
	bool nopunt = false;      // /NOPUNT, retail's no-punt switch, as parsed (boot_policy.h nopunt)
	// The first port of the bind scan (--lan-port; 0 binds the OS's pick; unset, the cfg
	// range's head: game.cfg mplanserverportmin, or mpnovaworldportmin on the NovaWorld
	// network type).
	std::optional<uint16_t> port;
	bool log_debug = false;   // --log-debug
	// NovaWorld listing (ADR 0051 d6). With a gate host the
	// network type is game.cfg's `networkconnecttype` (1, NovaWorld, by default), as retail's
	// dead auto-host takes it; without one the server serves LAN (D-NET-358). Loopback and any
	// host off NovaLogic's domain are reachable by default; novaworld.net needs --allow-public.
	std::string master_host;                       // --master-host (empty: LAN)
	uint16_t master_gate_port = GATE_DEFAULT_PORT; // --master-gate-port
	bool allow_public = false;                     // --allow-public
	std::string credentials_path;                  // --credentials FILE
	Credentials credentials; // main() loads it and masks every value
	// A second NovaWorld listing of the same match beside the first (none of retail's, whose host
	// lists on one service: a hosted server's listing on NovaLogic's NovaWorld as well as
	// OpenNova's): its gate, the gate's port and its account, as the first listing's three. It
	// needs --master-host, and its gate is held to the same destination rule (--allow-public for
	// a host under novaworld.net). It hosts once the starting map is up, never holds up or ends
	// the server, and is tried again after a backoff when it does not host (Server's
	// tick_second_listing).
	std::string also_master_host;                       // --also-master-host (empty: none)
	uint16_t also_master_gate_port = GATE_DEFAULT_PORT; // --also-master-gate-port
	std::string also_credentials_path;                  // --also-credentials FILE
	Credentials also_credentials; // main() loads it and masks every value
	LogSwitches log_switches; // /PROFILE <path>, /PUNT.TXT, /PUNTLOG, /CHEATLOG, /NETLOG, /INOUT
	// What a supervisor reads of the run (run_reports.h), none of it retail's: whether the
	// status page prints and stdin is its chat input (--status-page on|off; on, as a retail
	// dedicated host draws its page), the tick health rewritten once a minute (--stats-file),
	// the error a non-zero exit ends on (--exit-file), the second listing's state, rewritten
	// whenever it changes (--second-listing-file; off for a run with none), and ONE's build,
	// written once as the server starts (--build-file); "" writes no file.
	bool status_page = true;
	bool rotation_first = false; // --rotation-first: hosted rotations start at entry zero
	std::string stats_file;
	std::string exit_file;
	std::string second_listing_file;
	std::string build_file;
	// --version: print the version line (serve_version_line) and exit, as opennova-hostd's does.
	// /HOST is not needed then, and --resource-dir and /game only name the game the line opens
	// with.
	bool print_version = false;
	// The primary desktop's width and height, the display size game.cfg's defaults take ahead of
	// its video_res line and the renderer's desktop NWHWI prints [orig: Config_SetDefaults
	// @0x54D030 - GetSystemMetrics(SM_CXSCREEN) called @0x54D0F3 into outWidth @0x54D0F6;
	// Renderer_Init @0x586350 - @0x58635C / @0x586363]. main() samples them on Windows
	// (read_primary_screen_size); a platform that reports none keeps the headless pair.
	int32_t desktop_width = kHeadlessDesktopWidth;
	int32_t desktop_height = kHeadlessDesktopHeight;
};

// Parses the command line (argv[1..]): the server's own options from `args`, the retail
// switches walked over `command_line` (the process's own, proc::own_command_line), or over
// `args` spelled as one when it is empty (walk_retail_switches; a token the walk does not
// know is passed over, as the game passes it). Returns 0 when the options parsed (with
// --version, whatever else the run would need: `print_version` set), 1 on a usage error
// (`error` says why: an unknown `--` option among them), -1 when help was asked for.
int parse_serve_options(const std::vector<std::string> &args, ServeOptions &out, std::string &error,
		const std::string &command_line = "");
// The server's versions on one line (base/version/version_line.h), which --version prints, the
// server logs as it starts, its status page heads every print with and CMD VERSION posts: the
// game the launch runs (/game, else the folder's UPDATE.WIZ: boot_policy.h launch_product) with
// the release installed in --resource-dir (launch_product_version), ONE's build (one_build():
// its version, a staging build's run number among it, and its commit) and the protocol the
// title speaks. With neither --resource-dir nor /game the launch names no game, and the line is
// ONE's build alone ("ONE 0.0.22.123 (a1b2c3d, staging)").
std::string serve_version_line(const ServeOptions &options);
// The cap the host file's MaxPlayers line yields to under `title`: the walk's `/maxplayers N`
// where the title's command line takes the switch (Title::max_players_switch), else none (0).
int32_t host_file_max_player_arg(const Title &title, int32_t max_player_arg);
const char *serve_usage();

class ServeListing;
class SecondListing;

// The second listing's retry after a try that did not host for a fault on the way (a gate or
// session that did not answer, a request that never reached the service): the first wait,
// doubled at each further fault up to the longest, and no more than kSecondListingTriesMax tries
// in a run. A refusal of its service's is never retried, as a stock host's listing that does not
// host ends (ADR 0051 d6). OpenNova policy, none of retail's (a stock host lists once).
inline constexpr uint32_t kSecondListingRetryFirstMs = 60u * 1000u;
inline constexpr uint32_t kSecondListingRetryMaxMs = 15u * 60u * 1000u;
inline constexpr int kSecondListingTriesMax = 5;
// How long a second listing may take from its start to hosting (the gate, the session's verify,
// the login and the HOSTKEY, the host request) before it counts as one that did not host: a
// dedicated host waits on its host request with no timeout (NwuHostRole::poll_request's
// command-line host), which the first listing's pre-boot wait keeps and the second must not.
inline constexpr uint32_t kSecondListingHostDeadlineMs = 3u * 60u * 1000u;

class Server {
public:
	explicit Server(ServeOptions options);
	~Server();
	Server(const Server &) = delete;
	Server &operator=(const Server &) = delete;

	// Read game.cfg, mount, read it again over weapon.def, read the host file,
	// write the lock and save game.cfg, bind the socket, boot the starting
	// mission and start the session; on the NovaWorld network type, host on
	// the gate's NovaWorld session between the bind and the boot, pumping it
	// until it hosts or fails (or `cancel` is set). False with `error` when any
	// leg fails; nothing is bound then, and stop() has run (so the exit's save
	// and the lock's delete too, once the mount succeeded). The embedder starts
	// the socket layer (net::startup) first and shuts it down after stop(): it
	// is process-wide.
	bool start(std::string &error, const std::atomic<bool> *cancel = nullptr);
	// start()'s two halves, for an embedder that acts between them: begin() runs everything
	// ahead of the session create (the reads, the mount, the host file, the lock, the socket
	// and, listed, the NovaWorld hosting), and start_session() boots the starting map, whose
	// boot creates the session from the cfg block as it stands then. Each is false with `error`
	// when a leg fails, and stop() has run then; start_session() before begin() refuses.
	bool begin(std::string &error, const std::atomic<bool> *cancel = nullptr);
	bool start_session(std::string &error);
	// One frame of the main loop, timed by the server's main-loop clock as the game's frame
	// is (engine inmatch/main_loop_clock.h): the elapsed time since the last frame's stamp,
	// game.cfg's lock_framerate locking it (the session's bank sleeps a frame banked under
	// 16 ms, as retail's loop does on a Serve Only host too), and the re-base a mission start
	// takes (the time since the last render, rendered()), so a map change's load is never
	// banked. A round end's mission exit runs the map change and boots the next map inside
	// the session (a listed server's listing and socket kept). False once the session has
	// ended (end_message() says how; rotation_ended() when the rotation ran out, quit() when
	// the remote admin quit it), and stop() has run.
	bool frame();
	// A direct frame of `delta_seconds` (a test's): it banks that delta alone, with no clock
	// read and no frame lock (inmatch::FrameInput's direct frame).
	bool frame(double delta_seconds);
	// The frame's render callback returned (the status page's draw, main.cpp): the stamp a
	// mission-start frame re-bases to [orig: Game_MainLoop @0x52BAC8..0x52BAD2].
	void rendered() { clock_.mark_rendered(); }
	// The last frame's session phases (the bank, its frame lock's sleeps included).
	const inmatch::FramePerf &last_frame_perf() const { return last_frame_perf_; }
	// The host's exit: the round reset queued to every joiner (never sent), the
	// STOP burst; the NovaWorld deregistration (ClientStopHosting and the
	// goodbye); then game.cfg saved, the socket closed and the lock deleted, as
	// Game_Run's exit. Idempotent.
	void stop();
	// Game_SaveConfig: the cfg block to the working directory's game.cfg. The
	// round-end map change calls it, as the PreMenu's init saves at every map
	// change (ADR 0051 PR3), and so do the admin console's SET and the
	// NovaWorld ServerCommand's name / message / mpreset change. False
	// (logged) when the file does not open.
	bool save_config();
	// `mpreset` is set: retail exits the process with code 0, at the load
	// when game.cfg sets it (before anything else runs) or at the session
	// create when the block holds it then; start() stops there, and the exit
	// tail (the save, the lock's delete, the NovaWorld stop) does not run.
	// [orig: Game_LoadConfig @0x5514A1..0x5514AC; CNapiGameSession_CreateSession
	//  @0x4C97E7..0x4C97F0]
	bool reset_exit() const { return reset_exit_; }

	uint16_t bound_port() const { return bound_port_; }
	bool running() const { return running_; }
	const ResourceIndex &index() const { return index_; }
	// The title the launch runs as, chosen as the game's launch chooses it (/game, else the
	// folder's UPDATE.WIZ, a folder with none being OpenNova's own: boot_policy.h
	// launch_product), and whether the launch names no game: a folder whose UPDATE.WIZ names a
	// game OpenNova does not play, with no /game (start() refuses it).
	const std::string &game_code() const { return game_code_; }
	bool foreign_folder() const { return foreign_folder_; }
	// The server's versions on one line, serve_version_line's for its options.
	const std::string &version_line() const { return version_line_; }
	// The title the server speaks and its gate tag (mount): null / empty before the mount.
	const Protocol *protocol() const { return protocol_; }
	const std::string &gate_tag() const { return gate_tag_; }
	const std::string &end_message() const { return end_message_; }
	bool rotation_ended() const { return rotation_ended_; }
	// The remote admin's GOTO MENUSTATE quit the session (exit reason 1).
	bool quit() const { return quit_; }
	// The missions the session has booted (the starting map is 1).
	int missions_played() const { return missions_played_; }
	// The chat display's wrap font picked at the last mission start (the host boot's
	// inmatch::ConsoleChatFont).
	const std::string &chat_font_name() const { return boot_.chat_font.name(); }
	// The cfg block (game.cfg, the host file over it; remote_admin_port is
	// the admin server's) and the weapon table its avail_wpn rows address.
	const gamecfg::GameCfg &game_cfg() const { return config_.cfg(); }
	const gamecfg::WeaponRoster &weapon_roster() const { return config_.weapon_roster(); }
	const inmatch::HostScreenState &host_screen() const { return host_; }
	const inmatch::HostRotation &rotation() const { return rotation_; }
	const inmatch::HostFileReport &host_file_report() const { return report_; }
	const std::vector<mission_catalog::Row> &catalog() const { return catalog_; }
	mission::MissionKernel &kernel() { return *kernel_; }
	inmatch::HostRole &role() { return *role_; }
	// True when the session runs on the NovaWorld network type (it is listed): a gate host was
	// given and game.cfg's networkconnecttype is 1.
	bool novaworld() const { return novaworld_; }
	// The NovaWorld listing's lister (null when not listing).
	Lister *lister() { return lister_.get(); }
	// The second listing's lister (ServeOptions::also_master_host): null when there is none, before
	// the starting map is up, between a failed try and the next, and once its service stopped it.
	Lister *second_lister() { return second_lister_.get(); }
	// The second listing is over for this run: its service stopped or refused it (ServerStopHosting,
	// its punt, a ServerCommand's stop, a refused login, host request or re-host), or its tries ran
	// out (kSecondListingTriesMax).
	bool second_listing_stopped() const { return second_stopped_; }
	// When the next try of a second listing that did not host starts (wall_ms; 0: none waiting).
	uint32_t second_listing_retry_at_ms() const { return second_lister_ ? 0u : second_retry_at_ms_; }
	// The second listing's state as a supervisor shows it (run_reports.h, --second-listing-file):
	// off without one, trying, listed, retrying after a fault (the next try's number and time, the
	// fault's reason), or not listed for the rest of the run (why), with the PlayerList slots of
	// the players in the match its service sent (NapiNPConnection::novaworld_listing 1).
	SecondListingReport second_listing_report() const;
	// The exit code the lister finished with when stop() tore it down (stop()'s own finish
	// included: a ServerStopHosting keeps the lister up until the match's NovaWorld exit stops
	// the server), -1 when there was none: the lister is gone before the caller can read it.
	int last_lister_exit() const { return last_lister_exit_; }
	// The remote-admin listener's bound port: game.cfg's remote_admin_port once it listens,
	// 0 when the port is 0 (off) or the listen failed.
	uint16_t admin_port() const { return admin_tcp_ ? admin_tcp_->port() : 0; }
	// The main-frame counter (g_MainFrameCounter, ex dword_A8705C): the world's own, which
	// every update steps and every mission start zeroes, as the game's console reads it: the
	// chat flood table's clock, which the console's typed line and the admin's CHAT SEND share.
	// [orig: Game_TickHudFrameCounters @0x434C00 (++g_MainFrameCounter @0x434C05), from
	//  Game_ProcessMainFrame @0x5265D5, the update callback; Game_ResetSessionHudState @0x434BD0
	//  from Game_StartMission @0x525BAA]
	uint32_t main_frame() const { return kernel_ ? kernel_->world.hud_frames.main_frame : 0u; }

private:
	// One frame over `input`'s timing (frame() and frame(double)).
	bool run_frame(inmatch::FrameInput input);
	// The session's update legs: the NovaWorld listings' pass ahead of each update's tick,
	// the admin pump past it (inmatch::Session::set_update_head / set_update_tail).
	void install_update_legs();
	// admin.cfg and the listener, once the subsystems are up [orig: Game_InitSubsystems
	// @0x4A72B8..0x4A72D9].
	void open_admin();
	// The console over the session's context, its seams and its rotation (the first boot).
	void bind_admin_console();
	// One update's admin pump [orig: CAdminServer_ProcessFrame @0x406F50 from
	// Game_ProcessMainFrame @0x5268F1, the update callback].
	void pump_admin();
	// The title the server speaks: /game's, JO's without one (net/protocol/protocol.h).
	// False when its session identity is not witnessed in full, or when the listing logs in
	// (--credentials) and its web flow is not.
	bool resolve_title(std::string &error);
	bool read_boot_config(std::string &error);
	bool mount(std::string &error);
	bool read_config_over_weapons(std::string &error);
	bool read_host_file(std::string &error);
	// The rotation's current map through the host boot: the starting map, or
	// with `next_mission` the map change's next one inside the kept session.
	bool boot_mission(bool next_mission, std::string &error);
	// The round end's mission exit on the authority: the main frame's verdict
	// and the Post Menu's route (inmatch/mission_exit.h), the map change for
	// an in-session one. False when the session ended.
	bool route_mission_exit(int32_t reason);
	bool open_socket(std::string &error);
	bool host_on_novaworld(std::string &error, const std::atomic<bool> *cancel);
	// What both listings' listers share (the title, the destination rule, the gate scan, the
	// verify identity), and a new lister's lobby text.
	ListerOptions base_lister_options() const;
	void set_lobby_text(Lister &lister);
	// The second listing (ServeOptions::also_master_host) on the socket's second session lane:
	// whether the run has one; a try's gate lookup, off the frame thread; its start at the address
	// that lookup found; its pass each frame (the publish once it hosts, the retry once it did not,
	// the teardown once its service stopped it); the next try's schedule after a fault (`why` and
	// `tag` its reason, second_listing_report's); and its teardown, with ClientStopHosting or the
	// goodbye burst alone, as the first listing's.
	bool second_listing_wanted() const;
	void begin_second_try();
	void start_second_listing(const std::string &gate_address, uint32_t now_ms);
	void tick_second_listing();
	void schedule_second_retry(
			uint32_t now_ms, const std::string &why, const std::string &tag = {});
	void end_second_listing(bool stop_hosting);
	// One console line per player the match added, naming the way it came (the NovaWorld service
	// whose listing sent it, or the LAN), none of retail's: its own SERVER PLAYER ADDED line names
	// none (NapiNPConnection::novaworld_listing). Each connection once, by its id.
	void announce_joins();
	// The mission start's server-info update inside the host boot (HostBootRequest::
	// mission_start_update), ahead of the load: the starting map's or the next one's columns as
	// the update reads them there (`header_name` and `climate`, the mission's BMS header's), the
	// publish, then the NovaWorld session's periodic update, which sends it.
	void run_mission_start_update(const std::string &header_name, int32_t climate);
	// The listing over the mission that just started (the first, or a map change's next).
	void bind_listing();
	HostRegistration listing_columns() const;

	ServeOptions options_;
	// The device facts NWHWI prints, sampled once at the boot where retail's renderer reads them
	// (begin: after the subsystems' game.cfg read); the listing only formats them with game.cfg's
	// display mode.
	HardwareIdentity device_identity_;
	// The title the launch runs as (/game's, else the folder's UPDATE.WIZ's, the default title's
	// for none: launch_product), whether the folder is foreign with no /game, and the command file a
	// foreign folder's wizard names.
	std::string game_code_;
	bool foreign_folder_ = false;
	std::string product_command_;
	std::string version_line_; // version_line's
	// The title the server speaks (the launch's: mount) and its gate tag, the
	// title's or the mount's gt.ssc's (protocol_gate_tag).
	const Protocol *protocol_ = nullptr;
	std::string gate_tag_;
	ResourceIndex index_;
	// The mount's expansion text-override table, read once at the boot's
	// mount (vfs_expansion_override_table); empty = none.
	std::vector<uint8_t> text_override_table_;
	// The same table parsed, for the lookups that search it first (the
	// catalog's titles, the cfg defaults' texts; D-NET-675).
	rtxt::File text_override_;
	bool have_text_override_ = false;
	std::unique_ptr<assets::AssetStore> assets_;
	std::vector<mission_catalog::Row> catalog_;
	// The process's cfg block: game.cfg in the working directory and the
	// weapon table its avail_wpn rows address (inmatch/game_config_block.h).
	inmatch::GameConfigBlock config_;
	inmatch::HostScreenState host_;
	// The session config the last mission booted with, which a map change stages the next
	// mission from (boot_mission, inmatch::stage_next_mission).
	inmatch::GameConfig session_config_;
	inmatch::HostRotation rotation_;
	inmatch::HostFileReport report_;
	std::unique_ptr<mission::MissionKernel> kernel_;
	// The process's client words the role steps, whose NovaWorld teardown leg a listed server's
	// lister takes (host_on_novaworld); it outlives the role.
	inmatch::ClientProcessState client_process_;
	std::unique_ptr<inmatch::HostRole> role_;
	std::unique_ptr<inmatch::Session> session_;
	inmatch::HostBoot boot_;
	// The mission start's frozen model-def list's count: the S2C 0x68 cursor's
	// wrap (inmatch/loaded_model_list.h; FrameInput::loaded_model_list_count).
	uint32_t loaded_model_list_count_ = 0;
	rtxt::File gametext_;
	bool have_gametext_ = false;
	bool novaworld_ = false;
	net::Socket socket_;
	std::unique_ptr<net::NetDatagramSocket> datagrams_;
	// The one socket under the game's protocol and the NWU session (D-NET-346).
	std::unique_ptr<DatagramDemux> demux_;
	std::unique_ptr<ServeListing> listing_;
	std::unique_ptr<Lister> lister_;
	int last_lister_exit_ = -1;
	// The second listing: its source over the first's match, its lister while one runs, whether
	// its first full lists went out, its service's stop, and the retry clock.
	std::unique_ptr<SecondListing> second_source_;
	std::unique_ptr<Lister> second_lister_;
	bool second_published_ = false;
	bool second_stopped_ = false;
	uint32_t second_retry_at_ms_ = 0;
	uint32_t second_backoff_ms_ = 0;
	uint32_t second_started_ms_ = 0; // the running try's start (its hosting deadline's)
	int second_faults_ = 0;          // the tries that ended on a fault since it last hosted
	// Why the last try did not host, or why the listing is over (second_listing_report), with the
	// NWEC tag behind it ("" for none), and the next try's start in Unix seconds.
	std::string second_reason_;
	std::string second_tag_;
	int64_t second_retry_at_unix_ = 0;
	// The connections announce_joins has named, while they stay.
	std::set<uint32_t> announced_joins_;
	// A try's gate lookup (the gate's dotted address, or none when it did not resolve), which
	// runs off the frame thread: the name's DNS lookup could hold the match up for seconds.
	std::future<std::optional<std::string>> second_resolve_;
	// The web exchanges of torn-down tries still out, kept until each lands so no teardown on the
	// frame thread waited on one (Lister::release_http).
	std::vector<std::future<net::HttpReply>> second_http_left_;
	uint16_t bound_port_ = 0;
	// The socket's bind address (the block's mpipaddressstring, 0 for every interface), which
	// the /INOUT HOST STARTED and STOPPED lines print beside the port, as the game's host prints
	// its pump's (inmatch::inout_host_line).
	uint32_t bound_address_ = 0;
	bool running_ = false;
	bool begun_ = false;
	bool rotation_ended_ = false;
	bool quit_ = false;
	int missions_played_ = 0;
	// Game_Run's exit tail is owed: the save and the lock's delete, on every
	// return once the subsystems are up [orig: Game_Run @0x4A7FFF..0x4A800E].
	bool exit_save_owed_ = false;
	bool reset_exit_ = false;
	std::string end_message_;
	inmatch::ServerLogDevices log_devices_; // the logs the switches armed (server_log_devices.h)
	inmatch::ServerLogs server_logs_;       // their view the lister's _inout.txt line reads
	// The working directory as the admin log's device (server_log_devices.h).
	inmatch::WorkingDirectoryFiles files_;
	// The main loop's clock (frame()) and the last frame's session phases.
	inmatch::MainLoopClock clock_;
	inmatch::FramePerf last_frame_perf_;
	// The avatar registry PETERRABBIT SEXCHANGE picks from (the mounted Avatars.def).
	inmatch::CharacterRegistry characters_;
	// The remote-admin server: the rotation seam, the console, the wire server, its sockets
	// and the CRT stream its challenge draws from (the world's, synced around each pump).
	std::unique_ptr<inmatch::HostRotationAdmin> rotation_admin_;
	// The admin server's command half (inmatch/remote_admin.h): the console over the session's
	// context once the first boot binds it; the pump runs only in game frames, so nothing
	// reaches the session-less one first.
	inmatch::RemoteAdminConsoles admin_consoles_;
	io::CrtRand admin_rand_;
	std::unique_ptr<AdminServer> admin_server_;
	std::unique_ptr<net::AdminTcpServer> admin_tcp_;
};

} // namespace opennova::serve
