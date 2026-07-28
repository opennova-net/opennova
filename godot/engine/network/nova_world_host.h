#pragma once

#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/string.hpp>

#include <novaworld/client_session.h>
#include <novaworld/lobby_vars.h>   // opennova::HostRegistration

#include "network/nwu_lobby_session.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace godot {

// Godot-side NovaWorld HOST registration. The co-op/listen host (NovaSimulation
// with enable_host_listen) is reachable on a game UDP port but, on a raw LAN
// join, the gate never learns about it and a retail client can't browse to it.
// This node makes the host appear in the gate's /api/hosts + the retail server
// browser by running the witnessed NWU lobby session to the gate (the same
// ClientHello -> ... -> Verified handshake NovaWorldClient runs) and then
// sending a ClientHostRequest, refreshed by periodic ClientHostUpdate
// heartbeats. It is the host-direction sibling of NovaWorldClient (ADR 0010):
// sockets + signals here, protocol/crypto in libs/novaworld.
//
// [orig: CNapiGameSession_SendHostRequest @ 0x4d3700 / SendHostUpdate @ 0x4d3860]
//
// Usage from GDScript:
//   var h := NovaWorldHost.new()
//   add_child(h)
//   h.host = "127.0.0.1"; h.gate_port = 7597
//   h.server_name = "Taylor's Game"; h.mission_name = "ASH_G11A"
//   h.game_port = 32768; h.max_players = 32
//   h.registered.connect(_on_registered)
//   h.start()
//   # as joiners arrive / leave:
//   h.set_player_count(2)
class NovaWorldHost : public Node {
	GDCLASS(NovaWorldHost, Node)

public:
	enum State {
		STATE_IDLE = 0,
		STATE_GATE_PROBING,   // gate probe sent, awaiting UDPNOVAWORLD
		STATE_SESSION_HELLO,  // ClientHello sent, handshake in flight
		STATE_SESSION_JOIN,   // ClientAuth/verify in flight
		STATE_REGISTERING,    // session Verified, ClientHostRequest sent
		STATE_HOSTING,        // registered + heartbeating (browser-visible)
		STATE_DISCONNECTED,
		STATE_ERROR,
	};

	NovaWorldHost();
	~NovaWorldHost();

	// Gate endpoint (the NovaWorld gate the launcher redirects to).
	void set_host(const String &host);
	String get_host() const;
	void set_gate_port(int port);
	int get_gate_port() const;

	// Lobby-visible host identity (the GSB row).
	void set_server_name(const String &name);
	String get_server_name() const;
	void set_mission_name(const String &name);
	String get_mission_name() const;
	void set_max_players(int n);
	int get_max_players() const;
	void set_region(const String &region);
	String get_region() const;
	void set_player_name(const String &name);
	String get_player_name() const;

	// The reachable game UDP endpoint joiners connect to (the NovaSimulation
	// listen-host port). advertise_ip may be left empty to let the gate observe
	// the real source / apply its own ONNET_CLIENT_REFLECT_* override (the prod
	// path); set it when the host knows its own reachable address.
	void set_game_port(int port);
	int get_game_port() const;
	void set_advertise_ip(const String &ip);
	String get_advertise_ip() const;

	// AppId / LobbyName routed into HostSetup (gate app/lobby selection).
	void set_app_id(const String &app_id);
	String get_app_id() const;
	void set_lobby_name(const String &lobby_name);
	String get_lobby_name() const;

	// Lifecycle.
	void start();
	void stop();

	State get_state() const { return state_; }
	bool is_hosting() const { return state_ == STATE_HOSTING; }

	// Update the advertised current player count. Sent in the next
	// ClientHostUpdate (and immediately if already hosting).
	void set_player_count(int n);
	int get_player_count() const { return player_count_; }

	// Engine hooks.
	void _ready() override;
	void _process(double delta) override;

protected:
	static void _bind_methods();

private:
	// The gate/session pump is the shared NwuLobbySession driver (lobby_);
	// the hooks map its progress onto our State + signals.
	NwuLobbySession::Hooks make_lobby_hooks();
	void sync_session_state();
	void send_host_request();   // ClientHostRequest (registration)
	void send_host_update();    // ClientHostUpdate (heartbeat refresh)
	void enter_state(State next, const String &reason = String());

	// Gather the GDScript-set host config the libs builders consume.
	opennova::HostRegistration host_cfg() const;

	// Config.
	String host_ = "127.0.0.1";
	int gate_port_ = 7597;
	String server_name_ = "OpenNova Host";
	String mission_name_;
	int max_players_ = 32;
	String region_ = "us";
	String player_name_ = "Host";
	int game_port_ = 32768;
	String advertise_ip_;
	String app_id_ = "28";              // JO (pfid 28); HostSetup.AppId
	String lobby_name_ = "jop_2_consumer";

	// State.
	State state_ = STATE_IDLE;
	int player_count_ = 1;              // the host itself is the first player
	// The shared gate/session driver: sockets, ClientSession, ci/ck, the NW
	// endpoint, the gate auth-code stash, and the handshake timeout.
	NwuLobbySession lobby_;

	double keepalive_accum_ = 0.0;
	double keepalive_interval_s_ = 2.0;   // session 0x43 keepalive
	double update_accum_ = 0.0;
	double update_interval_s_ = 60.0;     // ClientHostUpdate refresh
	bool update_pending_ = false;         // player_count changed -> push now
};

} // namespace godot

VARIANT_ENUM_CAST(godot::NovaWorldHost::State);
