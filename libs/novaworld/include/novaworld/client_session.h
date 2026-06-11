#pragma once

#include <napi/tlv.h>  // NapiMessage (lobby container shape)
#include <novaworld/protocol_message.h>
#include <novaworld/session_hello.h>

#include <cstdint>
#include <string>
#include <vector>

namespace opennova {

// Godot-free client mirror of LobbySession: the bytes-in / bytes-out state
// machine that drives the NovaWorld *session* UDP channel from the client
// side. The gate-probe leg (which yields the session host:port) lives in the
// caller; ClientSession starts at ClientHello and runs the handshake through
// the lobby verify exchange:
//
//   start()                       -> ClientHello (0x41)
//   <- ServerHello (0x81)            : learn host key `hk`
//   ClientAuth (0x42)                : hk echoed, ck = client key, client scrk
//   <- ServerAuth (0x82)             : learn server key `sk` + server scrk (cr==1)
//   ClientConnected (0x43)           : begin the lobby verify handshake
//   <- ServerStartVerify (0x83)
//   ClientRequestVerifyResult (0x43)
//   <- ServerVerifyResult (0x83)     : Success=1 -> Verified (lobby-ready)
//
// No socket I/O lives here (engine convention: portable C++ in libs/, Godot
// wrapper in engine/). The caller owns the UDP socket and pumps bytes: send
// what start()/handle_datagram() return, and feed every received datagram
// back into handle_datagram(). Each leg is unit-testable via in-process
// loopback against apps/novaworld_server's own parsers/builders — see
// docs/adr/0010-novaworld-client-completion.md.
//
// [mirror: apps/novaworld_server/nw_udp_listener.cpp — the server direction
//  of the same wire protocol; this class inverts request <-> response]
class ClientSession {
public:
	enum class State {
		Idle,       // nothing sent yet
		Hello,      // ClientHello sent, awaiting ServerHello
		Auth,       // ClientAuth sent, awaiting ServerAuth
		Verifying,  // ServerAuth accepted; lobby verify handshake in flight
		Verified,   // ServerVerifyResult(Success=1) — lobby-ready
		Closed,     // GoodBye sent
		Error,      // protocol/envelope error or server rejection
	};

	struct Config {
		uint32_t client_index = 1;   // ci — caller-chosen (binding: random)
		uint32_t client_key = 1;     // ck — caller-chosen (binding: random)
		std::string na = "jop:cus2"; // gate tag echo (memory reference_gate_tags)

		// ClientHello identity. The real NovaWorld server VALIDATES four fields
		// in HandleClientHello @ 0x6213B0 and silently drops the ClientHello
		// (no ServerHello -> client times out) unless they match exactly:
		//   NVS  == the Milota NAPI-protocol version string (it is the protocol
		//           version, NOT client identity),
		//   PN   == the server game id "NOVAWORLDUDP",
		//   PV1  == "0.0.0 2/10/2004 EM",
		//   PG   == the 16-byte protocol GUID (added in build_client_hello).
		// CO/AP/BDAT are read but never validated, so they stay our identity.
		// All four constants are from CNapiGameSession_InitNPConnection @ 0x4d3be0.
		std::string nvs  = "NAPI NP Version 0.0.1 1/12/2004 - 2/20/2004 Milota Copyright 2004 NovaLogic";
		std::string co   = "OpenNova";
		std::string ap   = "OpennovaGodotClient.exe";
		std::string bdat = "Jul 21 2009 18:54:41";  // retail BDAT (free field)
		std::string pn   = "NOVAWORLDUDP";           // validated == server game id
		std::string pv1  = "0.0.0 2/10/2004 EM";     // validated == server PV1
		std::string pv2  = "1";
	};

	// Two constructors rather than a `Config config = {}` default argument:
	// GCC/clang reject `= {}` for this aggregate-with-NSDMIs (MSVC accepts it),
	// which broke the Linux/macOS CI builds.
	ClientSession();
	explicit ClientSession(Config config);

	// Begin the handshake. Returns the ClientHello datagram to send (envelope
	// + NWU already applied — ready for the wire). Transitions Idle -> Hello.
	std::vector<uint8_t> start();

	// Feed one inbound datagram (received off the UDP socket, CRC envelope
	// intact). Appends zero or more datagrams to send in reply to `out`.
	// Returns false on a protocol/envelope error or a server rejection (state
	// becomes Error; see last_error()). A datagram that isn't expected in the
	// current state is ignored (returns true, leaves `out` untouched).
	bool handle_datagram(const uint8_t *data, size_t len,
	                     std::vector<std::vector<uint8_t>> &out);

	// Build a keep-alive: a header-only 0x43 with no inner messages (advances
	// our seq, acks the peer). Valid once Verified.
	std::vector<uint8_t> build_heartbeat();

	// Build a ClientGoodBye (0x46) datagram and mark the session Closed.
	std::vector<uint8_t> build_goodbye();

	State state() const { return state_; }
	bool is_verified() const { return state_ == State::Verified; }

	uint32_t client_index() const { return cfg_.client_index; }
	uint32_t client_key() const { return cfg_.client_key; }
	uint32_t server_host_key() const { return server_hk_; }
	uint32_t server_key() const { return server_sk_; }
	const std::string &client_scrk() const { return client_scrk_; }
	const std::string &server_scrk() const { return server_scrk_; }
	const std::string &sess_id_string() const { return sess_id_string_; }
	const std::string &last_error() const { return last_error_; }

private:
	std::vector<uint8_t> build_client_hello();
	std::vector<uint8_t> build_client_auth();
	// Wrap a single lobby container (by name; fields/children optional) as a
	// 0x43 ProtocolMessage stream + header, ready for the wire.
	std::vector<uint8_t> build_lobby_packet(const NapiMessage &container);

	void on_server_hello(const std::vector<uint8_t> &body,
	                     std::vector<std::vector<uint8_t>> &out);
	void on_server_auth(const std::vector<uint8_t> &body,
	                    std::vector<std::vector<uint8_t>> &out);
	void on_server_protocol_message(const std::vector<uint8_t> &body,
	                                std::vector<std::vector<uint8_t>> &out);
	void dispatch_server_container(const NapiMessage &container,
	                               std::vector<std::vector<uint8_t>> &out);
	void fail(std::string reason);

	Config cfg_;
	State state_ = State::Idle;

	uint32_t server_hk_ = 0;       // ServerHello.hk — echoed in ClientAuth
	uint32_t server_sk_ = 0;       // ServerAuth.sk — session_id on our 0x43s
	std::string client_scrk_;      // we generate; encrypts our 0x43 inner stream
	std::string server_scrk_;      // ServerAuth.scrk; decrypts inbound 0x83 inner
	std::string sess_id_string_;   // ServerVerifyResult.SessIdString
	std::string last_error_;

	uint32_t next_outbound_seq_ = 0;
	uint32_t last_inbound_seq_ = 0;
	bool sent_verify_request_ = false;
	ProtocolReassemblyState reassembly_;
};

} // namespace opennova
