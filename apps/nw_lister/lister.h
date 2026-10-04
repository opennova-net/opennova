#pragma once

#include "admin_feed.h"
#include "listing.h"

#include "net_datagram_socket.h"
#include "net_http.h"
#include "net_sockets.h"

#include <net/novaworld/gate_probe.h>
#include <net/novaworld/http_flow.h>
#include <net/novaworld/lobby_vars.h>
#include <net/novaworld/nwu_host_role.h>
#include <net/novaworld/nwu_lobby_session.h>

#include <cstdint>
#include <filesystem>
#include <future>
#include <memory>
#include <random>
#include <string>
#include <vector>

namespace opennova::lister {

// The process exit codes.
enum ExitCode : int {
	kExitStopped = 0,          // stopped and deregistered
	kExitUsage = 1,            // bad command line
	kExitBadInput = 2,         // the listing or credentials file did not load
	kExitNetwork = 3,          // a socket could not be opened
	kExitFailed = 4,           // the session, the login, the HOSTKEY or the host request failed
	kExitStoppedByService = 5, // the service stopped the hosting (ServerStopHosting)
};

struct ListerOptions {
	std::string listing_path;
	std::string master_host = "127.0.0.1";
	uint16_t master_gate_port = GATE_DEFAULT_PORT;
	// Until set, every destination must be on 127.0.0.0/8 and names do not resolve.
	bool allow_public = false;
	// With an account, the host leg logs in and fetches the HOSTKEY first, as the game's menu does.
	Credentials credentials;
	std::string admin_host; // empty: the listing file alone
	uint16_t admin_port = kAdminDefaultPort;
};

// One NovaWorld listing kept up without the game: the engine's NWU driver and host role over two
// UDP sockets, the account login and the hosting page over LobbyHttpFlow, and the columns and
// roster from the listing file (or the game server's admin port).
class Lister {
public:
	explicit Lister(ListerOptions options);
	~Lister();
	Lister(const Lister &) = delete;
	Lister &operator=(const Lister &) = delete;

	// Load the listing, bind both sockets and send the gate probe. False (exit_code() says why)
	// when the listing does not load or a socket does not bind.
	bool start();
	// One pass at the wall clock: the session, the HTTP leg, the host role, the listing. False
	// once the lister has finished; exit_code() then says how.
	bool tick(uint32_t now_ms);
	// Deregister and finish: ClientStopHosting on one pump, then the goodbye burst.
	void stop();

	int exit_code() const { return exit_code_; }
	bool hosting() const { return role_.is_hosting(); }

private:
	enum class Phase { Connecting, LoggingIn, FetchingHostKey, Requesting, Hosting, Done };

	void on_verified();
	void on_http_reply(const net::HttpReply &reply);
	void ship(const HttpRequestSpec &spec);
	void request_host();
	void sync_flow_context();
	std::vector<std::pair<std::string, std::string>> cookie_vars();
	void refresh_listing(bool force);
	HostRegistration registration() const;
	std::vector<HostPlayerSlot> wanted_roster() const;
	void sync_roster();
	// Record the outcome; the teardown runs at the end of the tick, outside the driver's hooks.
	void end(int code);
	void teardown();

	ListerOptions options_;
	Phase phase_ = Phase::Connecting;
	bool ending_ = false;
	int exit_code_ = kExitStopped;
	uint32_t now_ms_ = 0;
	std::mt19937 rng_;

	Listing listing_;
	std::filesystem::file_time_type listing_mtime_{};
	uint32_t listing_checked_ms_ = 0;
	AdminFeed admin_;
	AdminSnapshot admin_snapshot_;

	net::ScopedSocket gate_udp_;
	net::ScopedSocket session_udp_;
	std::unique_ptr<IDatagramSocket> gate_socket_;
	std::unique_ptr<IDatagramSocket> session_socket_;
	NwuLobbySession lobby_;
	NwuHostRole role_;

	LobbyHttpFlow flow_;
	LobbyIdentityParams identity_;
	std::vector<std::pair<std::string, std::string>> identity_vars_;
	std::future<net::HttpReply> http_;
	std::string host_key_;
};

} // namespace opennova::lister
