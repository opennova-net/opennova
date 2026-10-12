#pragma once

#include "credentials.h"
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
#include <string>
#include <vector>

namespace opennova::serve {

struct ListerOptions {
	std::string master_host = "127.0.0.1";
	uint16_t master_gate_port = GATE_DEFAULT_PORT;
	// Until set, the destination policy holds: loopback and any host off NovaLogic's domain, a
	// name under novaworld.net refused (destination_policy.h).
	bool allow_public = false;
	// With an account, the host leg logs in and fetches the HOSTKEY first, as the game's menu does.
	Credentials credentials;
};

// opennova-serve's NovaWorld listing kept up: the engine's NWU driver and host role, the account
// login and the hosting page over LobbyHttpFlow, and the columns and roster from a ListingSource
// (the live match, ServeListing). The process owns the socket layer (net::startup / shutdown);
// the lister never starts or stops it.
class Lister {
public:
	// With `session_socket` the NWU session rides that socket (the game host's, through its
	// DatagramDemux view: retail's one socket, D-NET-346) instead of one of the lister's own. The
	// source and the socket outlive the lister. The gate probe keeps a socket of its own, as
	// retail's gate worker does.
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
	// Finish without deregistering: the goodbye burst alone, as a process exit's atexit teardown
	// of the NovaWorld session sends it, with no ClientStopHosting.
	// [orig: crt_exit -> doexit -> the atexit CNapiGameSession_Destroy (registered
	// @0x7939DA) -> CNapiGameSession_ResetToDisconnected @0x4D0890 ->
	// CNapiNPConnection_RequestDisconnect / CNapiNPConnection_Destroy ->
	// CNapiNPConnection_TeardownActiveConnection @0x6253C0 -> SendDisconnectPacket @0x61F2A0]
	void disconnect();
	// The mission start's server-info update: the source's columns and roster at once, then both
	// lists in full (NwuHostRole::publish_mission_start). An embedder whose source starts missions
	// runs it at each one; for a source without missions the lister runs it at its host result
	// and the update again at each mission change the source shows it
	// (ListingSource::starts_missions, D-NET-488). Nothing while not hosting.
	void publish_server_info();

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

	void on_verified();
	void on_listed();
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
	// `stop_hosting`: ClientStopHosting ahead of the goodbye burst (stop), or the burst alone
	// (disconnect).
	void teardown(bool stop_hosting);

	ListerOptions options_;
	ListingSource &source_;
	IDatagramSocket *shared_session_socket_ = nullptr;
	Phase phase_ = Phase::Connecting;
	bool ending_ = false;
	int exit_code_ = kExitStopped;
	uint32_t now_ms_ = 0;      // the session's clock: the embedder's, from the first pass
	uint32_t clock_origin_ = 0;
	bool clock_started_ = false;

	net::ScopedSocket gate_udp_;
	net::ScopedSocket session_udp_;
	std::unique_ptr<net::NetDatagramSocket> gate_socket_;
	// Unset when the session rides the embedder's socket.
	std::unique_ptr<net::NetDatagramSocket> session_socket_;
	NwuLobbySession lobby_;
	NwuHostRole role_;

	LobbyHttpFlow flow_;
	LobbyIdentityParams identity_;
	std::vector<std::pair<std::string, std::string>> identity_vars_;
	std::future<net::HttpReply> http_;
	std::string host_key_;
};

} // namespace opennova::serve
