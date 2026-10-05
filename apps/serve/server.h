#pragma once

// opennova-serve's server: the game's Serve Only host with no Godot (ADR 0051).
// It runs retail's dead /HOST path: the working directory's game.cfg
// (gamecfg::load), the game data mounted the way the game mounts it, the
// retail host file over the cfg block (inmatch/host_file.h), the directory
// lock (activesrvr.txt) and the cfg saved as Serve Only, the session settings
// from the block (inmatch/host_config.h), the cfg's LAN port range bound, the
// starting mission booted as a DedicatedHost through the engine's one host
// boot (inmatch/host_boot.h) and the in-match session run round after round
// over the host file's rotation (the engine's map change,
// inmatch/map_change.h, game.cfg saved at each); at a clean exit it saves
// game.cfg again and deletes the lock. Everything below the config, the
// mount, the socket and the wall clock is the engine's.
//
// The files are the process's working directory's, as retail's: game.cfg and
// activesrvr.txt open as bare relative names, never under --resource-dir
// (unless the server runs from there).

#include <base/resource_index/resource_index.h>
#include <formats/gamecfg/game_cfg.h>
#include <runtime/assets/asset_store.h>
#include <runtime/inmatch/host_boot.h>
#include <runtime/inmatch/host_file.h>
#include <runtime/inmatch/host_role.h>
#include <runtime/inmatch/mission_rotation.h>
#include <runtime/inmatch/session.h>
#include <runtime/mission/mission_catalog.h>
#include <runtime/mission/mission_kernel.h>

#include "net_datagram_socket.h"
#include "net_sockets.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace opennova::serve {

struct ServeOptions {
	std::string resource_dir; // --resource-dir
	std::string expansion;    // /exp <name>
	bool loose_override = false; // /d
	std::string game;         // /game <code>
	bool loose_root = false;  // --loose-root: a directory with no archives mounts loose
	std::string host_file;    // /HOST <file>
	// The first port of the bind scan (--lan-port; 0 = game.cfg mplanserverportmin).
	uint16_t port = 0;
	bool log_debug = false;   // --log-debug
};

// Parses the command line (argv[1..]). Returns 0 when the options parsed, 1 on
// a usage error (`error` says why), -1 when help was asked for.
int parse_serve_options(const std::vector<std::string> &args, ServeOptions &out, std::string &error);
const char *serve_usage();

class Server {
public:
	explicit Server(ServeOptions options);
	~Server();
	Server(const Server &) = delete;
	Server &operator=(const Server &) = delete;

	// Read game.cfg, mount, read it again over weapon.def, read the host file,
	// write the lock and save game.cfg, bind the socket, boot the starting
	// mission and start the session. False with `error` when any leg fails;
	// nothing is bound then, and stop() has run (so the exit's save and the
	// lock's delete too, once the mount succeeded). The embedder starts the
	// socket layer (net::startup) first and shuts it down after stop(): it is
	// process-wide.
	bool start(std::string &error);
	// One outer frame of `delta_seconds` wall clock. A round end's mission exit
	// runs the map change and boots the next map inside the session. False
	// once the session has ended (end_message() says how; rotation_ended()
	// when the rotation ran out), and stop() has run.
	bool frame(double delta_seconds);
	// The host's exit: the round reset to every joiner, the STOP description,
	// the final flush; then game.cfg saved, the socket closed and the lock
	// deleted, as Game_Run's exit. Idempotent.
	void stop();
	// Game_SaveConfig: the cfg block to the working directory's game.cfg. The
	// round-end map change calls it, as the PreMenu's init saves at every map
	// change (ADR 0051 PR3). False (logged) when the file does not open.
	bool save_config();
	// game.cfg set `mpreset`: retail's load exits the process with code 0
	// before anything else runs, and start() stops there.
	bool reset_exit() const { return reset_exit_; }

	uint16_t bound_port() const { return bound_port_; }
	const std::string &end_message() const { return end_message_; }
	bool rotation_ended() const { return rotation_ended_; }
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

private:
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
	net::Socket socket_;
	std::unique_ptr<net::NetDatagramSocket> datagrams_;
	uint16_t bound_port_ = 0;
	bool running_ = false;
	bool rotation_ended_ = false;
	int missions_played_ = 0;
	// Game_Run's exit tail is owed: the save and the lock's delete, on every
	// return once the subsystems are up [orig: Game_Run @0x4A7FFF..0x4A800E].
	bool exit_save_owed_ = false;
	bool reset_exit_ = false;
	std::string end_message_;
};

} // namespace opennova::serve
