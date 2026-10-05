#pragma once

// opennova-serve's server: the game's Serve Only host with no Godot (ADR 0051).
// It reads the retail host file (inmatch/host_file.h), mounts the game data the
// way the game does, boots the starting mission through the engine's one
// mission kernel as a DedicatedHost over inmatch::HostRole, binds the retail
// LAN port range and runs the in-match session. Everything below the config,
// the socket and the wall clock is the engine's, the same calls the game's
// Serve Only path makes.

#include <base/resource_index/resource_index.h>
#include <runtime/assets/asset_store.h>
#include <runtime/inmatch/host_file.h>
#include <runtime/inmatch/host_role.h>
#include <runtime/inmatch/session.h>
#include <runtime/mission/mission_catalog.h>
#include <runtime/mission/mission_kernel.h>
#include <runtime/mission/mission_text.h>
#include <runtime/terrain_query/surface_type_map.h>

#include "net_datagram_socket.h"
#include "net_sockets.h"

#include <array>
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
	// The first port of the bind scan (--lan-port; 0 = kRetailLanPortMin, mplanserverportmin).
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

	// Mount, read the host file, bind the socket, boot the starting mission and
	// start the session. False with `error` when any leg fails; nothing is bound
	// then. The embedder starts the socket layer (net::startup) first and shuts
	// it down after stop(): it is process-wide.
	bool start(std::string &error);
	// One outer frame of `delta_seconds` wall clock. False once the session has
	// ended (end_message() says how).
	bool frame(double delta_seconds);
	// The host's exit: the round reset to every joiner, the STOP description,
	// the final flush, then the socket closes. Idempotent.
	void stop();

	uint16_t bound_port() const { return bound_port_; }
	const std::string &end_message() const { return end_message_; }
	const inmatch::HostScreenState &host_screen() const { return host_; }
	const inmatch::MissionRotation &rotation() const { return rotation_; }
	const inmatch::HostFileReport &host_file_report() const { return report_; }
	const std::vector<mission_catalog::Row> &catalog() const { return catalog_; }
	mission::MissionKernel &kernel() { return *kernel_; }
	inmatch::HostRole &role() { return *role_; }

private:
	bool mount(std::string &error);
	bool read_host_file(std::string &error);
	bool boot_mission(std::string &error);
	bool open_socket(std::string &error);

	ServeOptions options_;
	ResourceIndex index_;
	std::unique_ptr<assets::AssetStore> assets_;
	std::vector<mission_catalog::Row> catalog_;
	inmatch::HostScreenState host_;
	inmatch::MissionRotation rotation_;
	inmatch::HostFileReport report_;
	inmatch::ServerTextTable server_text_;
	mission::MissionText mission_text_;
	std::vector<terrain::SurfaceTileEntry> surface_tiles_;
	std::array<uint8_t, 256> tile_surface_table_{};
	std::unique_ptr<mission::MissionKernel> kernel_;
	std::unique_ptr<inmatch::HostRole> role_;
	std::unique_ptr<inmatch::Session> session_;
	net::Socket socket_;
	std::unique_ptr<net::NetDatagramSocket> datagrams_;
	uint16_t bound_port_ = 0;
	bool running_ = false;
	std::string end_message_;
};

} // namespace opennova::serve
