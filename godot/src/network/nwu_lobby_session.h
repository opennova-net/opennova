#pragma once

// The shared NWU gate/session driver (ADR 0010): gate probe -> gate response ->
// ClientSession handshake -> verified lobby session, pumped over two
// PacketPeerUDP sockets. NovaWorldClient (lobby browse/login/join) and
// NovaWorldHost (host registration) both delegate their formerly duplicated
// socket/pump legs here and keep only their role behavior + GDScript surface.
// Sockets live here; the protocol/crypto stay in engine/net/novaworld (ClientSession).
// Not a registered Godot class — a plain member of the two nodes.

#include <godot_cpp/classes/packet_peer_udp.hpp>
#include <godot_cpp/variant/string.hpp>

#include <net/novaworld/client_session.h>
#include <net/novaworld/gate_response.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace godot {

class NwuLobbySession {
public:
	// Everything the owner's per-datagram diagnostics need beyond session().
	struct RxInfo {
		std::vector<uint8_t> bytes;
		String src_ip;
		int src_port = 0;
		int state_before = 0; // opennova::ClientSession::State, as int
		int state_after = 0;
		bool ok = true;
		int replies = 0;
	};

	struct Hooks {
		// The parsed gate response, before the session handshake starts (the
		// client stashes its server_info dictionary here; the auth codes the
		// 0x42 join carries are stashed by the driver itself).
		std::function<void(const opennova::GateResponse &)> on_gate_response;
		// The current "Cookie" var-list for this role, read for each statement
		// (client: browser cookies + locale; host: the lobby identity set).
		std::function<std::vector<std::pair<std::string, std::string>>()> cookie_vars;
		// The session was created; its ClientHello ships right after.
		std::function<void(std::size_t cu_vars, std::size_t cookie_vars)> on_session_created;
		// An outbound datagram just shipped on the NW socket (trace hook).
		std::function<void(const std::vector<uint8_t> &)> on_sent;
		// One inbound session datagram was handled (trace hook; the session
		// state may have advanced — read session() for details).
		std::function<void(const RxInfo &)> on_received;
		// The session advanced: the owner maps ClientSession::State onto its
		// own GDScript-facing State enum + signals.
		std::function<void()> on_session_state;
		// Fatal: bind failure, malformed gate endpoint, session error, or a
		// connect deadline. The owner enters its ERROR state. The message is the
		// retail NWEC tag when one is witnessed for the failure.
		std::function<void(const String &)> on_fatal;
		// Recoverable decode noise on the gate socket (bad envelope/response).
		std::function<void(const String &)> on_soft_error;
	};

	explicit NwuLobbySession(Hooks hooks) : hooks_(std::move(hooks)) {}

	// Bind the gate + NW sockets and mint the ci/ck pair. False (after
	// on_fatal) when a bind fails. Does not probe yet — the owner enters its
	// probing state first, then calls probe(), preserving its signal order.
	bool open();
	// Send the enveloped gate probe toward the gate endpoint and start the gate
	// worker's retry/deadline clocks (SESSION_GATE_PROBE_RETRY_MS / _TIMEOUT_MS).
	void probe(const String &gate_host, int gate_port);
	// Close both sockets and drop the session. Owners send their goodbye
	// datagrams BEFORE closing (send()/session() are still live).
	void close();
	// Pump both sockets, the stage retransmits, the connect deadlines, and the
	// session's send-interval pump (keepalive + reap). Call every frame while open.
	void process(double delta);
	// Send one datagram to the NW session endpoint (fires on_sent).
	void send(const std::vector<uint8_t> &dg);
	// Send one raw datagram to an arbitrary endpoint over the gate socket: the
	// host's POST status heartbeat to the gate's POSTIPADDRESS:POSTIPPORT
	// (retail's CNapiNetwork_SendUDPPacket leg, a plain sendto outside the NP
	// session). No trace hook; the caller applies its own envelope.
	void send_to(const String &host, int port, const std::vector<uint8_t> &dg);

	bool sockets_open() const {
		return gate_socket_.is_valid() && nw_socket_.is_valid();
	}
	opennova::ClientSession *session() { return session_.get(); }
	const opennova::ClientSession *session() const { return session_.get(); }
	bool session_verified() const { return session_ && session_->is_verified(); }
	// The parsed gate response (the GLSVSS trio and the auth codes ride it).
	const opennova::GateResponse &gate_response() const { return gate_response_; }

	uint32_t client_index() const { return client_index_; }
	uint32_t client_key() const { return client_key_; }
	String nw_udp_host() const { return nw_udp_host_; }
	uint16_t nw_udp_port() const { return nw_udp_port_; }
	int nw_local_port() const {
		return nw_socket_.is_valid() ? nw_socket_->get_local_port() : 0;
	}
	// The driver's wall clock in ms (the session's GetTickCount).
	uint32_t clock_ms() const { return clock_ms_; }

private:
	void begin_session();
	void poll_gate();
	void poll_session();
	void send_gate_probe();
	// The handshake phase for the timeout diagnostic, matching the owner
	// states' names (gate_probing / session_hello / session_join).
	const char *phase_name() const;

	Hooks hooks_;
	String gate_host_;
	int gate_port_ = 0;
	bool probe_sent_ = false;
	std::vector<uint8_t> gate_probe_datagram_;
	Ref<PacketPeerUDP> gate_socket_;
	Ref<PacketPeerUDP> nw_socket_;
	std::unique_ptr<opennova::ClientSession> session_;
	uint32_t client_index_ = 0;
	uint32_t client_key_ = 0;
	String nw_udp_host_; // from the gate's UDPNOVAWORLD
	uint16_t nw_udp_port_ = 0;
	opennova::GateResponse gate_response_;
	// The wall clock (ms) the deadlines and the session's intervals run on.
	uint32_t clock_ms_ = 0;
	double clock_accum_s_ = 0.0;
	// The gate worker: probe re-sent every retry interval until the deadline.
	uint32_t gate_started_ms_ = 0;
	uint32_t gate_last_send_ms_ = 0;
	// The connect legs: the current stage datagram re-sent every retransmit
	// interval until the session's connect deadline.
	uint32_t connect_started_ms_ = 0;
	uint32_t connect_last_send_ms_ = 0;
	bool connect_reported_ = false;
};

} // namespace godot
