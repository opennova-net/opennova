#pragma once

#include "admin_feed.h"
#include "listing.h"
#include "listing_source.h"

#include "net_datagram_socket.h"
#include "net_http.h"
#include "net_sockets.h"

#include <net/novaworld/gate_probe.h>
#include <net/novaworld/http_flow.h>
#include <net/novaworld/lobby_vars.h>
#include <net/novaworld/nwu_host_role.h>
#include <net/novaworld/nwu_lobby_session.h>
#include <net/npwire/idatagram_socket.h>

#include <cstdint>
#include <future>
#include <memory>
#include <random>
#include <string>
#include <vector>

namespace opennova::nw_lister {

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

// One NovaWorld listing kept up: the engine's NWU driver and host role, the account login and the
// hosting page over LobbyHttpFlow, and the columns and roster from a ListingSource. The process
// owns the socket layer (net::startup / shutdown); the lister never starts or stops it.
class Lister {
public:
	// opennova-nw-lister: the listing file (and the admin port) as the source, and two UDP sockets
	// of the lister's own.
	explicit Lister(ListerOptions options);
	// An embedder's source; with `session_socket` the NWU session rides that socket (the game
	// host's, through its DatagramDemux view: retail's one socket, D-NET-346) instead of one of the
	// lister's own. Both outlive the lister. The gate probe keeps a socket of its own, as retail's
	// gate worker does.
	Lister(ListerOptions options, ListingSource &source, IDatagramSocket *session_socket);
	~Lister();
	Lister(const Lister &) = delete;
	Lister &operator=(const Lister &) = delete;

	// Start the source, bind the sockets and send the gate probe. False (exit_code() says why)
	// when the source cannot list or a socket does not bind.
	bool start();
	// One pass at the wall clock: the session, the HTTP leg, the host role, the listing. False
	// once the lister has finished; exit_code() then says how.
	bool tick(uint32_t now_ms);
	// Deregister and finish: ClientStopHosting on one pump, then the goodbye burst.
	void stop();

	int exit_code() const { return exit_code_; }
	bool hosting() const { return role_.is_hosting(); }
	bool finished() const { return phase_ == Phase::Done; }
	// The session and its host leg, for the source behind a live server (the GSID, the join
	// tickets, the match facts) and the shared socket's claim.
	NwuLobbySession &lobby() { return lobby_; }
	const NwuLobbySession &lobby() const { return lobby_; }
	NwuHostRole &host_role() { return role_; }

private:
	enum class Phase { Connecting, LoggingIn, FetchingHostKey, Requesting, Hosting, Done };

	Lister(ListerOptions options, std::unique_ptr<ListingSource> owned, ListingSource *source,
	       IDatagramSocket *session_socket);

	void on_verified();
	void on_http_reply(const net::HttpReply &reply);
	void ship(const HttpRequestSpec &spec);
	void request_host();
	void sync_flow_context();
	std::vector<std::pair<std::string, std::string>> cookie_vars();
	void refresh_listing(bool force);
	HostRegistration registration() const;
	void sync_roster();
	// Record the outcome; the teardown runs at the end of the tick, outside the driver's hooks.
	void end(int code);
	void teardown();

	ListerOptions options_;
	std::unique_ptr<ListingSource> owned_source_;
	ListingSource *source_ = nullptr;
	IDatagramSocket *shared_session_socket_ = nullptr;
	Phase phase_ = Phase::Connecting;
	bool ending_ = false;
	int exit_code_ = kExitStopped;
	uint32_t now_ms_ = 0;      // the session's clock: the embedder's, from the first pass
	uint32_t clock_origin_ = 0;
	bool clock_started_ = false;
	std::mt19937 rng_;

	net::ScopedSocket gate_udp_;
	net::ScopedSocket session_udp_;
	std::unique_ptr<net::NetDatagramSocket> gate_raw_;
	std::unique_ptr<net::NetDatagramSocket> session_raw_;
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

} // namespace opennova::nw_lister
