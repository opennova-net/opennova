#pragma once

#include <napi/tlv.h>  // NapiMessage (lobby container shape)
#include <npwire/protocol_message.h>
#include <npwire/session_hello.h>

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
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
//   process_periodic_update() -> ClientConnected (0x43): begin lobby verification
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
		std::string na = "jop:cus2"; // gate/game tag echoed in ClientAuth NA

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

		// The 16-byte protocol GUID emitted as PG in the hello and the auth. By
		// default (use_default_pg) the builder picks the GUID for `pn`:
		// NOVAWORLDUDP -> the lobby GUID, JointOperations -> the game GUID. Set
		// `pg` + use_default_pg=false to override with explicit bytes.
		std::array<uint8_t, 16> pg{};
		bool use_default_pg = true;

		// CU chunks to emit in the ClientAuth (NW-S3). The retail client sends
		// a named var set (Application/BuildDateAndTime/Debug/CountryName/
		// Language/TimeZoneBias/GateTag/MetTag/UdpCode1/UdpCode2/MaxPacketSize)
		// from CNapiGameSession_ConnectToNovaWorld @ 0x4d4640; UdpCode1/UdpCode2
		// are the gate-issued session-auth codes (gate VARs UDPCODE1/UDPCODE2)
		// that live NW's join callbacks validate. Empty by default (the OpenNova
		// server doesn't require them); the binding populates it from the gate
		// response + client env when targeting live NW. Each is emitted as a
		// CU flat-TLV whose value is make_client_cu_chunk(type, name, value).
		struct CuVar {
			std::string name;
			std::string value;
			uint8_t type = 1;  // 1 or 2 (HandleClientJoin's CU loop accepts both)
		};
		std::vector<CuVar> cu_vars;

		// Verify-request "Cookie" var-list (NW-S5, witnessed byte-for-byte in the
		// genuine .204 capture, fixtures/novaworld/nw204_lobby.hexcap frame 10166).
		// CNapiGameSession_SendVerifyRequest @ 0x4d3620 serializes session+388 as a
		// "Cookie" var-list, filled by CNapiSession_ReadLocaleInfo @ 0x4ce390 from
		// the browser form fields CNapiGameSession_OnNovaWorldConnected @ 0x4d1570
		// set. Each (name, value) becomes a ClientVar{VarFNum="0",VarName,VarValue}
		// child of a ClientVarList(VarList="Cookie") inside the
		// ClientRequestVerifyResult. The retail set (in order) is CountryName,
		// Language, TimeZoneBias, MyInstalledExpBits, NWUID, NWCDKIID, NWCDKIIDEXP1,
		// NWPSSK, NWUSID, NWHWI — and on the wire NWCDKIID/NWCDKIIDEXP1 are EMPTY
		// yet the live server still returns Success=1, so the lobby verify is NOT
		// credential-gated. Empty here -> a bare ClientRequestVerifyResult (the
		// OpenNova server is permissive). The entry named "NWUID" with an empty
		// value is filled at runtime from the ServerSessionInit's NWUID (echo).
		std::vector<std::pair<std::string, std::string>> verify_cookie_vars;

		// Preset for the in-match game session: the ClientHello the client sends
		// to a host after NWJoin, which flips the connection protocol from the
		// lobby (NOVAWORLDUDP) to the game (JOINTOPERATIONS). The complete
		// CO/AP/BDAT/PN/PG/PV1/PV2 identity is the retail CNapiNetwork_Init
		// @ 0x4ca4a0 block, independently witnessed in the LAN ClientAuth capture.
		static Config jointoperations();
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

	// Run one session-periodic update after the caller has drained inbound
	// datagrams. Once ServerSessionInit has established the NP connection and
	// moved the session to Verifying, the first update emits ClientConnected;
	// later updates do not repeat it. Keeping this boundary explicit matches
	// retail's conn_state==5 && session_state==2 timing instead of replying
	// synchronously from handle_datagram().
	void process_periodic_update(std::vector<std::vector<uint8_t>> &out);

	// Build a keep-alive: a header-only 0x43 with no inner messages (advances
	// our seq, acks the peer). Valid once Verified.
	std::vector<uint8_t> build_heartbeat();

	// Build a ClientGoodBye (0x46) datagram and mark the session Closed.
	std::vector<uint8_t> build_goodbye();

	// Wrap one lobby container (e.g. a ClientHostRequest / ClientHostUpdate built
	// via napi/session.h) as a ready-for-wire 0x43 ProtocolMessage — the
	// host-registration send path. Valid only once Verified (returns an empty
	// vector otherwise); advances our seq + acks the peer like any other 0x43.
	std::vector<uint8_t> build_lobby_message(const NapiMessage &container);

	State state() const { return state_; }
	bool is_verified() const { return state_ == State::Verified; }

	uint32_t client_index() const { return cfg_.client_index; }
	uint32_t client_key() const { return cfg_.client_key; }
	uint32_t server_host_key() const { return server_hk_; }
	uint32_t server_key() const { return server_sk_; }
	const std::string &client_scrk() const { return client_scrk_; }
	const std::string &server_scrk() const { return server_scrk_; }
	// The NovaworldWebDomainNameAndPortNumber CU delivered in the ServerSessionInit
	// (0x82), e.g. "207.178.209.204:80". On live NW the gate's startupurl carries a
	// "[domainname]" placeholder; this is the real web host the client substitutes
	// for the HTTP login/GSB/join legs. Empty until the SessionInit is parsed.
	const std::string &server_web_domain() const { return server_web_domain_; }
	const std::string &server_nwuid() const { return server_nwuid_; }
	const std::string &sess_id_string() const { return sess_id_string_; }
	const std::string &last_error() const { return last_error_; }

private:
	std::vector<uint8_t> build_client_hello();
	std::vector<uint8_t> build_client_auth();
	// Wrap a single lobby container (by name; fields/children optional) as a
	// 0x43 ProtocolMessage stream + header, ready for the wire.
	std::vector<uint8_t> build_lobby_packet(const NapiMessage &container);
	// Build the ClientRequestVerifyResult: a SessIdString field plus, when
	// cfg_.verify_cookie_vars is set, the "Cookie" var-list (NW-S5, NWUID echoed
	// from the ServerSessionInit). Bare when no cookie vars are configured.
	std::vector<uint8_t> build_verify_request();

	void on_server_hello(const std::vector<uint8_t> &body,
	                     std::vector<std::vector<uint8_t>> &out);
	void on_server_auth(const std::vector<uint8_t> &body);
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
	std::string server_nwuid_;     // ServerSessionInit NWUID — echoed in the verify
	std::string server_web_domain_;// ServerSessionInit NovaworldWebDomainNameAndPortNumber
	std::string sess_id_string_;   // ServerVerifyResult.SessIdString
	std::string last_error_;

	// The 0-default is deliberately preserved (start() resets it to 1 — see Risk #1 / capture frame 9739).
	SessionSequencing seq_{0, 0}; // outbound seq + last inbound ack [ADR 0013 shared framing]
	bool sent_client_connected_ = false;
	bool sent_verify_request_ = false;
	ProtocolReassemblyState reassembly_;
};

// Shared identity-message builders — the ClientHello / ClientAuth struct-fill that BOTH the lobby
// ClientSession (above) and the in-match np::JoinerConnection (libs/npruntime) use. The two builders
// differ ONLY in the CO source (lobby company vs the joiner's player name) and which envelope frames
// the bytes (both directions now share npwire's nw_encode_outbound / nw_decode_inbound), so the
// struct-fill is dedup'd here.
// [orig: one CNapiNPConnection identity block — NapiNPConnection_SendClientHello @0x61fe20 sources it
// from the same protocol config for both the 0x41 hello and the 0x42 join; the lobby/game paths split
// AFTER 0x82, not in the identity emit.]
ClientHello make_client_hello(const ClientSession::Config &cfg, std::string_view co);
ClientAuth make_client_auth(const ClientSession::Config &cfg, std::string_view co, uint32_t server_hk,
                            std::string_view client_scrk);

// The gate-sourced inputs to the 0x42-join CU var set. Both the join (client)
// and the host-registration (host) connection carry this same set — retail
// builds it once in CNapiGameSession_ConnectToNovaWorld, which serves both
// directions. `gate_tag` is the const protocol/gate tag (our ClientAuth `na`,
// e.g. "jop:cus2" — stru_B5FF50.protocol); the rest come straight from the
// gate response (ProcessResponse @ 0x4ced20) and are empty against the
// permissive OpenNova gate.
struct NovaWorldJoinCu {
	std::string application = "OpennovaGodotClient.exe";  // exe basename
	std::string gate_tag;        // GateTag  <- stru_B5FF50.protocol (== na)
	std::string met_tag;         // MetTag   <- gate VAR METLABEL  (byte_B5F4BC)
	std::string udp_code1;       // UdpCode1 <- gate VAR UDPCODE1  (byte_B5F8E8)
	std::string udp_code2;       // UdpCode2 <- gate VAR UDPCODE2  (byte_B5F908)
	std::string max_packet_size = "1300";
};

// Build the 0x42-join CU var set exactly as
// [orig: CNapiGameSession_ConnectToNovaWorld @ 0x4d4640] does: the session+316
// var list (CNapiVarList_SetOrCreate @ 0x6318c0), emitted as CU chunks by
// SendClientHello @ 0x61fe20. Retail order is fixed: Application,
// BuildDateAndTime, Debug, CountryName, Language, TimeZoneBias, GateTag, MetTag,
// UdpCode1, UdpCode2, MaxPacketSize. CountryName/Language/TimeZoneBias are EMPTY
// on the join (retail fills locale only in the verify "Cookie"). All chunks use
// type 2 (HandleClientJoin @ 0x62B750's CU loop accepts 1 or 2; the genuine .204
// capture frame 8977 shows type 2). The byte-global -> CU-var mapping was
// grilled (2026-06-24): the Kong setter names CMissionInfo_SetGateTag /
// _SetMetTag are misnomers — SetGateTag writes byte_B5F8E8 (the UdpCode1 CU),
// SetMetTag writes byte_B5F908 (the UdpCode2 CU); MetTag (byte_B5F4BC) is the
// METLABEL field (CMissionInfo_SetTargetName, +108). See docs/net §8 NW-S3.
std::vector<ClientSession::Config::CuVar> make_novaworld_join_cu(const NovaWorldJoinCu &in);

} // namespace opennova
