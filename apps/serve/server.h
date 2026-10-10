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
#include <runtime/inmatch/character_registry.h>
#include <runtime/inmatch/host_boot.h>
#include <runtime/inmatch/host_file.h>
#include <runtime/inmatch/host_role.h>
#include <runtime/inmatch/mission_rotation.h>
#include <runtime/inmatch/session.h>
#include <runtime/mission/mission_catalog.h>
#include <runtime/mission/mission_kernel.h>
#include <formats/rtxt/rtxt.h>
#include <net/novaworld/gate_probe.h>
#include <net/novaworld/lobby_vars.h>
#include <net/npwire/datagram_demux.h>

#include "admin_tcp_server.h"
#include "listing.h" // nw_lister::Credentials
#include "net_datagram_socket.h"
#include "net_sockets.h"
#include "server_logs.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace opennova::nw_lister {
class Lister;
}

namespace opennova::serve {

struct ServeOptions {
	std::string resource_dir; // --resource-dir
	std::string expansion;    // /exp or /mod <name>: the last one, its first 32 bytes
	bool loose_override = false; // /d
	std::string game;         // /game <code>
	bool loose_root = false;  // --loose-root: a directory with no archives mounts loose
	std::string host_file;    // /HOST <file>
	// The first port of the bind scan (--lan-port; 0 binds the OS's pick; unset, the cfg
	// range's head: game.cfg mplanserverportmin, or mpnovaworldportmin on the NovaWorld
	// network type).
	std::optional<uint16_t> port;
	bool log_debug = false;   // --log-debug
	// NovaWorld listing (ADR 0051 d6), in opennova-nw-lister's spellings. With a gate host the
	// network type is game.cfg's `networkconnecttype` (1, NovaWorld, by default), as retail's
	// dead auto-host takes it; without one the server serves LAN (D-NET-358). Loopback and any
	// host off NovaLogic's domain are reachable by default; novaworld.net needs --allow-public.
	std::string master_host;                       // --master-host (empty: LAN)
	uint16_t master_gate_port = GATE_DEFAULT_PORT; // --master-gate-port
	bool allow_public = false;                     // --allow-public
	std::string credentials_path;                  // --credentials FILE
	nw_lister::Credentials credentials;            // main() loads it and masks every value
	LogSwitches log_switches; // /PROFILE <path>, /PUNT.TXT, /PUNTLOG, /CHEATLOG (server_logs.h)
};

// Parses the command line (argv[1..]). Returns 0 when the options parsed, 1 on
// a usage error (`error` says why), -1 when help was asked for.
int parse_serve_options(const std::vector<std::string> &args, ServeOptions &out, std::string &error);
const char *serve_usage();

class ServeListing;

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
	// One outer frame of `delta_seconds` wall clock. A round end's mission exit
	// runs the map change and boots the next map inside the session (a listed
	// server's listing and socket kept). False once the session has ended
	// (end_message() says how; rotation_ended() when the rotation ran out,
	// quit() when the remote admin quit it), and
	// stop() has run.
	bool frame(double delta_seconds);
	// The host's exit: the round reset to every joiner, the STOP description,
	// the final flush; the NovaWorld deregistration (ClientStopHosting and the
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
	const std::string &end_message() const { return end_message_; }
	bool rotation_ended() const { return rotation_ended_; }
	// The remote admin's GOTO MENUSTATE quit the session (exit reason 1).
	bool quit() const { return quit_; }
	// The missions the session has booted (the starting map is 1).
	int missions_played() const { return missions_played_; }
	// The cfg block (game.cfg, the host file over it; remote_admin_port is
	// the admin server's) and the weapon table its avail_wpn rows address.
	const gamecfg::GameCfg &game_cfg() const { return cfg_; }
	const gamecfg::WeaponRoster &weapon_roster() const { return roster_; }
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
	nw_lister::Lister *lister() { return lister_.get(); }
	// The remote-admin listener's bound port: game.cfg's remote_admin_port once it listens,
	// 0 when the port is 0 (off) or the listen failed.
	uint16_t admin_port() const { return admin_tcp_ ? admin_tcp_->port() : 0; }
	// The per-main-frame counter (g_MainFrameCounter, ex dword_A8705C), one per frame(): the
	// chat flood table's clock, which the console's typed line and the admin's CHAT SEND share.
	// [orig: Game_TickHudFrameCounters @0x434C00, from Game_ProcessMainFrame @0x5265D5]
	uint32_t main_frame() const { return main_frame_; }

private:
	// The admin server's command half until the session's console stands: retail's console
	// reads the live globals from the first frame, the port's needs the host context, which
	// the first boot creates. The pump runs only in game frames, so nothing reaches it before.
	struct AdminForward final : AdminCommandHandler {
		inmatch::AdminConsole *console = nullptr;
		bool dispatch(const AdminSession &session, std::string_view line,
				std::vector<std::string> &replies) override;
		std::string status_report() override;
	};
	// admin.cfg and the listener, once the subsystems are up [orig: Game_InitSubsystems
	// @0x4A72B8..0x4A72D9].
	void open_admin();
	// The console over the session's context, its seams and its rotation (the first boot).
	void bind_admin_console();
	// One game frame's admin pump [orig: CAdminServer_ProcessFrame @0x406F50 from
	// Game_ProcessMainFrame @0x5268F1].
	void pump_admin();
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
	// The listing over the mission that just started (the first, or a map change's next).
	void bind_listing();
	HostRegistration listing_columns() const;

	ServeOptions options_;
	ResourceIndex index_;
	std::unique_ptr<assets::AssetStore> assets_;
	std::vector<mission_catalog::Row> catalog_;
	gamecfg::GameCfg cfg_;
	gamecfg::WeaponRoster roster_;
	inmatch::HostScreenState host_;
	inmatch::HostRotation rotation_;
	inmatch::HostFileReport report_;
	std::unique_ptr<mission::MissionKernel> kernel_;
	std::unique_ptr<inmatch::HostRole> role_;
	std::unique_ptr<inmatch::Session> session_;
	inmatch::HostBoot boot_;
	rtxt::File gametext_;
	bool have_gametext_ = false;
	bool novaworld_ = false;
	net::Socket socket_;
	std::unique_ptr<net::NetDatagramSocket> datagrams_;
	// The one socket under the game's protocol and the NWU session (D-NET-346).
	std::unique_ptr<DatagramDemux> demux_;
	std::unique_ptr<ServeListing> listing_;
	std::unique_ptr<nw_lister::Lister> lister_;
	uint16_t bound_port_ = 0;
	bool running_ = false;
	bool rotation_ended_ = false;
	bool quit_ = false;
	int missions_played_ = 0;
	// Game_Run's exit tail is owed: the save and the lock's delete, on every
	// return once the subsystems are up [orig: Game_Run @0x4A7FFF..0x4A800E].
	bool exit_save_owed_ = false;
	bool reset_exit_ = false;
	std::string end_message_;
	ServerLogDevices log_devices_; // the logs the switches armed (server_logs.h)
	// The working directory as the admin log's device (server_logs.h).
	WorkingDirectoryFiles files_;
	uint32_t main_frame_ = 0;
	// The avatar registry PETERRABBIT SEXCHANGE picks from (the mounted Avatars.def).
	inmatch::CharacterRegistry characters_;
	// The remote-admin server: the rotation seam, the console, the wire server, its sockets
	// and the CRT stream its challenge draws from (the world's, synced around each pump).
	std::unique_ptr<inmatch::HostRotationAdmin> rotation_admin_;
	std::unique_ptr<inmatch::AdminConsole> admin_console_;
	AdminForward admin_forward_;
	io::CrtRand admin_rand_;
	std::unique_ptr<AdminServer> admin_server_;
	std::unique_ptr<net::AdminTcpServer> admin_tcp_;
};

} // namespace opennova::serve
