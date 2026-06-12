#pragma once

#include <godot_cpp/classes/http_request.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/packet_peer_udp.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>

#include <novacrypto/epask.h>
#include <novaworld/client_session.h>
#include <novaworld/http_login.h>

#include <cstdint>
#include <memory>
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
		// GDScript). STATE_JOINING: NWJoin HTTP in flight. STATE_IN_GAME_HELLO:
		// the JointOperations ClientHello has been sent to the host — the proto
		// switch boundary (terminal for this milestone; no in-match gameplay).
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

	// Join a hosted game (ADR 0010 Phase 5). Runs the NWJoin.dll HTTP handshake
	// for the GSB row's `rid`, resolves the host address, and opens a session to
	// the host with PN="JointOperations" — flipping the connection protocol from
	// the lobby to the in-match game. Stops at the JointOperations ClientHello
	// (the proto-switch boundary); emits joined_game(host, port).
	void join(int rid);

	// Engine hooks.
	void _ready() override;
	void _process(double delta) override;

protected:
	static void _bind_methods();

private:
	// Wire helpers. The gate leg stays here (it yields the session host:port);
	// the session protocol lives in libs/novaworld/client_session — this
	// binding is the socket pump (PacketPeerUDP + signals).
	void send_gate_probe();
	void begin_session();                                   // create ClientSession, send ClientHello
	void send_nw_datagram(const std::vector<uint8_t> &dg);  // put_packet on the NW socket
	void sync_session_state();                              // ClientSession::State -> our State + signals

	void poll_gate();
	void poll_session();

	// Server-browser HTTP leg. request_server_list() issues the GSB GET; the
	// completion callback parses it via libs/novaworld/gsb and caches rows.
	void request_server_list();
	void on_gsb_request_completed(int result, int response_code,
	                              const PackedStringArray &headers,
	                              const PackedByteArray &body);
	String gsb_url() const;       // OpenNova GSB URL from the gate's startup_url

	// Account login HTTP chain (ADR 0010 Phase 3).
	void on_login_request_completed(int result, int response_code,
	                                const PackedStringArray &headers,
	                                const PackedByteArray &body);
	// Join HTTP chain (ADR 0010 Phase 5).
	void on_join_request_completed(int result, int response_code,
	                               const PackedStringArray &headers,
	                               const PackedByteArray &body);
	// Open a UDP session to the host and send the JointOperations ClientHello —
	// the proto-switch boundary.
	void send_jointops_hello(const String &host, uint16_t port);

	String http_base() const;     // scheme://host:port from the gate's startup_url
	PackedStringArray request_headers(bool form_content_type) const;  // Cookie + optional form CT
	void merge_response_cookies(const PackedStringArray &headers);     // store Set-Cookie lines

	void enter_state(State next, const String &reason = String());

	// Config.
	String host_ = "127.0.0.1";
	int gate_port_ = 7597;
	String player_name_ = "GodotPlayer";

	// State.
	State state_ = STATE_IDLE;
	Dictionary server_info_;
	Ref<PacketPeerUDP> gate_socket_;
	Ref<PacketPeerUDP> nw_socket_;
	std::unique_ptr<opennova::ClientSession> session_;  // owns the session protocol

	// Server browser.
	HTTPRequest *browser_http_ = nullptr;   // child node, created in start()
	Array server_rows_;                     // cached GSB rows (Array of Dictionary)
	bool gsb_request_in_flight_ = false;

	// Account login + join (ADR 0010 Phase 3/5). Separate child HTTPRequests so
	// the multi-leg login/join sequences don't race the GSB fetch.
	HTTPRequest *login_http_ = nullptr;
	HTTPRequest *join_http_ = nullptr;
	opennova::CookieJar cookie_jar_;        // EPASK + NWHANDLE/PCID/LOGINSESSIONTAG...
	opennova::EpaskParams epask_;           // bundle from the prepare Set-Cookie
	enum LoginStep { LOGIN_IDLE = 0, LOGIN_PREPARE, LOGIN_POST, LOGIN_RELAY };
	LoginStep login_step_ = LOGIN_IDLE;
	String login_user_;
	String login_pass_;
	String nwhandle_;                       // captured on login success
	String pcid_;
	enum JoinStep { JOIN_IDLE = 0, JOIN_FIRST, JOIN_SECOND };
	JoinStep join_step_ = JOIN_IDLE;
	int join_rid_ = 0;

	// In-match game session (PN=JointOperations). The third UDP socket; we send
	// one ClientHello to the host and stop (the proto-switch milestone boundary).
	Ref<PacketPeerUDP> game_socket_;
	std::unique_ptr<opennova::ClientSession> game_session_;

	uint32_t client_index_ = 0;       // ci — generated at start()
	uint32_t client_key_ = 0;         // ck — generated at start()
	uint16_t nw_udp_port_ = 0;        // populated from gate response
	String nw_udp_host_;              // populated from gate response
	double tick_accum_ = 0.0;
	double heartbeat_interval_s_ = 2.0;
	double handshake_timeout_s_ = 5.0;
	double handshake_elapsed_ = 0.0;
};

} // namespace godot

VARIANT_ENUM_CAST(godot::NovaWorldClient::State);
