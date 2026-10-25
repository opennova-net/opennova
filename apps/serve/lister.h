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
#include <runtime/inmatch/server_files.h> // ServerLogs, server_logs_inout_line
#include <formats/gamecfg/game_cfg.h> // gamecfg::defaults (the shipped gate quad)
#include <net/npwire/idatagram_socket.h>
#include <runtime/inmatch/host_config.h> // inmatch::gate_bind_ports (the gate socket's scan)
#include <runtime/inmatch/punt_log.h> // inmatch::ConnectLog (the gate and connect lines)

#include <cstdint>
#include <future>
#include <memory>
#include <string>
#include <vector>

namespace opennova::serve {

struct ListerOptions {
	// The title the listing speaks (complete: resolve_title_identity; it outlives the lister)
	// and its gate manager's tag (title_gate_tag over the mount): the gate probe, the session's
	// NA, the web flow and the Host list's version pair.
	const TitleIdentity *title = nullptr;
	std::string gate_tag;
	std::string master_host = "127.0.0.1";
	uint16_t master_gate_port = GATE_DEFAULT_PORT;
	// Until set, the destination policy holds: loopback and any host off NovaLogic's domain, a
	// name under novaworld.net refused (destination_policy.h).
	bool allow_public = false;
	// With an account, the host leg logs in and fetches the HOSTKEY first, as the game's menu does.
	Credentials credentials;
	// The process's _connectlog.txt the gate and connect lines go to (the server's device; null:
	// none). It outlives the lister.
	inmatch::ConnectLog *connect_log = nullptr;
	// The process's log devices, whose `/INOUT` switch takes the session's stop-hosting line
	// (inmatch::server_logs_inout_line; null: none). It outlives the lister.
	const inmatch::ServerLogs *server_logs = nullptr;
	// The gate socket's bind scan, in order, on every interface: the process block's
	// mpgateserverlocalport quad (inmatch::gate_bind_ports; the server passes its game.cfg's),
	// the shipped block's when the embedder has none.
	std::vector<uint16_t> gate_ports = inmatch::gate_bind_ports(gamecfg::defaults());
	// The process block's mpextractextendedmetricinformation (+0x33C, the shipped default 1): clear,
	// the jar and the Cookie var-lists carry no locale three (LobbyHttpContext::extended_metrics).
	bool extended_metrics = gamecfg::defaults().mp_extract_extended_metric_information != 0;
	// The verify Cookie's NWHWI, the server's make_nwhwi over this machine and its game.cfg.
	std::string hardware_identity;
	// What every console line of this listing starts with, so a server's second listing
	// (ServeOptions::also_master_host) is told apart from its first ("": nothing, the first's).
	std::string label;
	// The AppId the host request registers under (NwuHostRole::request): a second listing's is
	// the match's, the first listing's (0: the request mints its own, as retail's does).
	uint32_t match_app_id = 0;
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
	// Deregister and finish: ClientStopHosting on one pump, then the goodbye burst (the burst
	// alone once the service's ServerStopHosting left the hosting).
	void stop();
	// Finish without deregistering: the goodbye burst alone, as a process exit's atexit teardown
	// of the NovaWorld session sends it, with no ClientStopHosting.
	// [orig: crt_exit -> doexit -> the atexit CNapiGameSession_Destroy (registered
	// @0x7939DA) -> CNapiGameSession_ResetToDisconnected @0x4D0890 ->
	// CNapiNPConnection_RequestDisconnect / CNapiNPConnection_Destroy ->
	// CNapiNPConnection_TeardownActiveConnection @0x6253C0 -> SendDisconnectPacket @0x61F2A0]
	void disconnect();
	// The mission start's server-info update: the source's columns and roster at once, then both
	// lists in full (NwuHostRole::publish_mission_start). The embedder runs it at each mission
	// start; the host result publishes nothing (D-NET-465). Nothing while not hosting. The
	// session's first mission (`session_round_count` 0) then sends the gate its METPROTOCOL ID
	// report (D-NET-NEW-L9-22).
	// [orig: Game_StartMission @0x5248c6..0x524928]
	void publish_server_info(int32_t session_round_count);

	int exit_code() const { return exit_code_; }
	// The gate socket's bound port (0 before start): the probe's source port.
	uint16_t gate_port() const { return gate_port_; }
	bool hosting() const { return role_.is_hosting(); }
	bool finished() const { return phase_ == Phase::Done; }
	// The service's ServerStopHosting stopped the hosting (the session stays up until the embedder
	// ends it, as a stock host's stays up until its match's NovaWorld exit).
	bool stopped_by_service() const { return stopped_by_service_; }
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
	// The label ahead of every console line (ListerOptions::label, with its separator).
	const char *who() const { return log_prefix_.c_str(); }

	ListerOptions options_;
	std::string log_prefix_; // options_.label and a space, or ""
	ListingSource &source_;
	IDatagramSocket *shared_session_socket_ = nullptr;
	Phase phase_ = Phase::Connecting;
	bool ending_ = false;
	// The service's ServerStopHosting stopped the hosting: whatever ends the lister after it
	// (the embedder's stop at the match's NovaWorld exit) finishes with kExitStoppedByService.
	bool stopped_by_service_ = false;
	int exit_code_ = kExitStopped;
	uint32_t now_ms_ = 0;      // the session's clock: the embedder's, from the first pass
	uint32_t clock_origin_ = 0;
	bool clock_started_ = false;

	net::ScopedSocket gate_udp_;
	uint16_t gate_port_ = 0; // gate_udp_'s bound port (the connect log's gate lines)
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
	// The HOSTKEY page's refresh GET, held until the host request is listed, and its answer due.
	HttpRequestSpec host_page_;
	bool host_page_in_flight_ = false;
	std::string host_key_;
};

} // namespace opennova::serve
