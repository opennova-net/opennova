#pragma once

#include <novaworld/gate_probe.h>
#include <godot_cpp/classes/http_request.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>

#include <novaworld/client_session.h>
#include <novaworld/http_flow.h>
#include <novaworld/lobby_vars.h>

#include "network/nwu_lobby_session.h"

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace godot {

// Godot-side novaworld client. One instance per game client.
// Wraps the C++ libs (libs/novacrypto + libs/napi + libs/novaworld) via
// GDExtension so GDScript can drive the full handshake without seeing the
// raw NWU/CRC/TLV machinery.
//
// Usage from GDScript:
//   var nw := NovaWorldClient.new()
//   add_child(nw)
//   nw.host = "127.0.0.1"
//   nw.gate_port = 7597
//   nw.player_name = "Taylor"
//   nw.connected.connect(_on_connected)
//   nw.disconnected.connect(_on_disconnected)
//   nw.server_info_received.connect(_on_server_info)
//   nw.start()
//
// State machine:
//   Idle -> GateProbing -> SessionHello -> SessionJoin -> Connected
//                                                       \-> Disconnected
class NovaWorldClient : public Node {
	GDCLASS(NovaWorldClient, Node)

public:
	enum State {
		STATE_IDLE = 0,
		STATE_GATE_PROBING,
		STATE_SESSION_HELLO,
		STATE_SESSION_JOIN,
		STATE_CONNECTED,
		STATE_DISCONNECTED,
		STATE_ERROR,
		// Join legs (ADD only at the end — existing values must stay stable for
		// GDScript). STATE_JOINING: NWJoin HTTP in flight. STATE_IN_GAME_HELLO: the
		// in-match host:port has been resolved and handed to the game layer (joined_game);
		// NovaSimulation's joiner takes over the in-match handshake from here.
		STATE_JOINING,
		STATE_IN_GAME_HELLO,
	};

	NovaWorldClient();
	~NovaWorldClient();

	// Configurables (exposed as GDScript properties).
	void set_host(const String &host);
	String get_host() const;

	void set_gate_port(int port);
	int get_gate_port() const;

	void set_player_name(const String &name);
	String get_player_name() const;

	// Lifecycle.
	void start();
	void stop();

	State get_state() const { return state_; }
	bool is_session_active() const { return state_ == STATE_CONNECTED; }
	Dictionary get_server_info() const;

	// Structured session diagnostics: the state snapshot plus a bounded wire/
	// session trace ring (newest last). The trace lines keep the retail
	// _connectlog "SENDING N BYTES ... [0xNN]" shape so a capture diff still
	// lines up — this replaces the old always-on stdout traces.
	Dictionary get_session_debug() const;

	// Server browser (ADR 0010 Phase 2). The list is fetched over HTTP from
	// the GSB endpoint once the session is verified; rows arrive asynchronously
	// (watch the `server_list_updated` signal, then read get_server_rows()).
	Array get_server_rows() const;
	void refresh_server_list();   // re-fetch the GSB now (also auto-fired on connect)

	// Account login (ADR 0010 Phase 3). Runs the EPASK HTTP login chain
	// (prepare GET -> login POST -> relay GET) and fills the cookie jar the
	// client carries onto every later request. Emits login_succeeded /
	// login_failed. Session-only (nothing persisted).
	void login(const String &username, const String &password);

	// Join a hosted game (ADR 0010 Phase 5). Runs the NWJoin.dll HTTP handshake for
	// the GSB row's `rid`, resolves the in-match host address, and emits
	// joined_game(host, port). The game layer (NovaWorldPanel -> MainGame) then drives
	// the in-match join through NovaSimulation's joiner — this client does not send the
	// in-match ClientHello itself (one joiner seam for LAN / NW / env joins).
	void join(int rid);

	// Engine hooks.
	void _ready() override;
	void _process(double delta) override;

protected:
	static void _bind_methods();

private:
	// The gate/session pump is the shared NwuLobbySession driver (lobby_);
	// these hooks feed it the role-specific pieces and map its progress onto
	// our State + signals + trace ring.
	NwuLobbySession::Hooks make_lobby_hooks();
	void on_gate_response(const opennova::GateResponse &parsed);
	std::vector<std::pair<std::string, std::string>> make_verify_cookie_vars();
	void trace_sent_datagram(const std::vector<uint8_t> &dg);
	void on_session_datagram(const NwuLobbySession::RxInfo &rx);
	void sync_session_state(); // ClientSession::State -> our State + signals

	// Server-browser HTTP leg. trigger_gsb() ships the GSB GET the flow builds; the
	// completion callback feeds the response back and caches the parsed rows.
	void trigger_gsb();
	void on_gsb_request_completed(int result, int response_code,
	                              const PackedStringArray &headers,
	                              const PackedByteArray &body);

	// Account login HTTP chain (ADR 0010 Phase 3) — sequenced by LobbyHttpFlow.
	void on_login_request_completed(int result, int response_code,
	                                const PackedStringArray &headers,
	                                const PackedByteArray &body);
	// Join HTTP chain (ADR 0010 Phase 5) — sequenced by LobbyHttpFlow.
	void on_join_request_completed(int result, int response_code,
	                               const PackedStringArray &headers,
	                               const PackedByteArray &body);
	// The NWJoin handshake resolved the in-match host:port — hand it off to the game
	// layer via joined_game(host, port). Does NOT send an in-match hello; NovaSimulation's
	// joiner owns the single ClientHello (see the .cpp for why).
	void resolve_join_target(const String &host, uint16_t port);

	// Snapshot the gate/session outputs into the flow's LobbyHttpContext. Called at
	// each leg-initiation point (login / GSB / join) — never inside a leg callback,
	// so a multi-step login keeps a stable http_base(). set_context preserves the
	// flow's shared cookie jar (NWHANDLE/PCID ride login -> GSB -> join).
	void sync_flow_context();
	// Ship one HttpRequestSpec over the given HTTPRequest child (method/url/headers/body).
	Error ship_spec(HTTPRequest *http, const opennova::HttpRequestSpec &spec);

	void enter_state(State next, const String &reason = String());

	// Append one line to the bounded diagnostics ring (get_session_debug()).
	void trace(const String &line);

	// Config.
	String host_ = "127.0.0.1";
	int gate_port_ = opennova::GATE_DEFAULT_PORT;
	String player_name_ = "GodotPlayer";

	// State.
	State state_ = STATE_IDLE;
	Dictionary server_info_;
	// The shared gate/session driver: sockets, ClientSession, ci/ck, the NW
	// endpoint, and the handshake timeout all live in here.
	NwuLobbySession lobby_;

	// The CD-key/hardware identity set (CountryName..NWHWI), built once per
	// session and used for BOTH the UDP verify var-list and the HTTP login
	// cookies. NWUID is left empty here and filled from the SessionInit at use.
	std::vector<std::pair<std::string, std::string>> identity_vars_;

	// Server browser.
	HTTPRequest *browser_http_ = nullptr;   // child node, created in start()
	Array server_rows_;                     // cached GSB rows (Array of Dictionary)
	bool gsb_request_in_flight_ = false;    // transport bookkeeping (cancel before re-issue)

	// Account login + join (ADR 0010 Phase 3/5). Separate child HTTPRequests so
	// the multi-leg login/join sequences don't race the GSB fetch. The protocol/
	// sequencing/cookie-jar all live in flow_ (libs/novaworld); these are pure pumps.
	HTTPRequest *login_http_ = nullptr;
	HTTPRequest *join_http_ = nullptr;
	// The lobby HTTP orchestration: the EPASK login chain, the GSB fetch, and the
	// NWJoin handshake (URL builders + shared cookie jar + LoginStep/JoinStep). The
	// binding ships each HttpRequestSpec and feeds (transport_ok, code, headers, body)
	// back in. Context is set from the gate/session legs via sync_flow_context().
	opennova::LobbyHttpFlow flow_;

	String nw_web_domain_;            // NovaworldWebDomainNameAndPortNumber from the
	                                  // SessionInit (real NW); the HTTP login/GSB/join
	                                  // host that replaces the startupurl [domainname]
	double tick_accum_ = 0.0;
	double heartbeat_interval_s_ = 2.0;

	// The wire/session trace ring behind get_session_debug(), newest last.
	static constexpr int kTraceRingCap = 64;
	PackedStringArray trace_ring_;
};

} // namespace godot

VARIANT_ENUM_CAST(godot::NovaWorldClient::State);
