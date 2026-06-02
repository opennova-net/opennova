#pragma once

#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/packet_peer_udp.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>

#include <cstdint>

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

	// Engine hooks.
	void _ready() override;
	void _process(double delta) override;

protected:
	static void _bind_methods();

private:
	// Wire helpers.
	void send_gate_probe();
	void send_session_hello();
	void send_session_join(uint32_t server_hk);
	void send_session_heartbeat();
	void send_session_goodbye();

	void poll_gate();
	void poll_session();

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

	uint32_t client_index_ = 0;       // ci — generated at start()
	uint32_t client_key_ = 0;         // ck — generated at start()
	uint32_t server_host_key_ = 0;    // hk — received in ServerHello
	uint16_t nw_udp_port_ = 0;        // populated from gate response
	String nw_udp_host_;              // populated from gate response
	double tick_accum_ = 0.0;
	double heartbeat_interval_s_ = 2.0;
	double handshake_timeout_s_ = 5.0;
	double handshake_elapsed_ = 0.0;
};

} // namespace godot

VARIANT_ENUM_CAST(godot::NovaWorldClient::State);
