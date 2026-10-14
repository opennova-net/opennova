// The first NovaWorld connect's give-ups (D-NET-571): the NWU lobby driver runs retail's
// NovaWorld menu poll over its session (the connect poll, the validation wait, the verify poll,
// the connection wait) instead of one 60 s deadline, and each give-up stops the enumerator's
// probe and reports the menu's NWEC. Socket-free: in-memory datagram sockets stand in, and the
// service's 0x81 / 0x82 are built with the engine's own writers.
// [orig: UI_ProcessLANSessionStateMachine @0x558DE0 - states 5..8 @0x55919E..0x55923C;
//  CNapiGameSession_ProcessPeriodicUpdate @0x4D4400 - @0x4D4483..0x4D44B1;
//  CNapiGameSession_PollConnectPhase1 @0x4D4C30; CGameSession_WaitForNWUValidation @0x4D0F20;
//  CNapiGameSession_PollConnectPhase2 @0x4D4D70; CGameSession_WaitForNWUConnection @0x4D1230;
//  UI_ShowNovaWorldConnectError @0x5584A0]

#include <net/napi/envelope.h>
#include <net/title/title_identity.h>
#include <base/gameprofile/gameprofile.h>
#include <net/napi/session.h>
#include <net/novacrypto/nwu.h>
#include <net/novaworld/gate_probe.h>
#include <net/novaworld/nwu_lobby_session.h>
#include <net/napi/tlv.h>
#include <net/npwire/nw_session_framing.h>
#include <net/npwire/protocol_message.h>
#include <net/npwire/session_hello.h>
#include <net/npwire/session_keys.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>
#include <utility>
#include <vector>

// JO's title identity (net/title/title_identity.h), the identity these tests speak.
static const opennova::TitleIdentity &kJo = *opennova::title_identity(opennova::gameprofile::GAME_JO);

using namespace opennova;

namespace {

int failures = 0;
#define CHECK(c)                                                                                   \
	do {                                                                                           \
		if (!(c)) {                                                                                \
			std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);                               \
			++failures;                                                                            \
		}                                                                                          \
	} while (0)

class MemorySocket final : public IDatagramSocket {
public:
	std::deque<std::pair<PeerAddr, std::vector<uint8_t>>> inbound;
	std::vector<std::pair<PeerAddr, std::vector<uint8_t>>> sent;

	int recv_from(uint8_t *buf, std::size_t cap, PeerAddr &from) override {
		if (inbound.empty())
			return 0;
		auto dg = std::move(inbound.front());
		inbound.pop_front();
		from = dg.first;
		const std::size_t n = std::min(cap, dg.second.size());
		std::memcpy(buf, dg.second.data(), n);
		return static_cast<int>(n);
	}
	void send_to(const PeerAddr &to, const uint8_t *data, std::size_t len) override {
		sent.emplace_back(to, std::vector<uint8_t>(data, data + len));
	}
};

std::vector<uint8_t> enveloped(std::vector<uint8_t> payload) {
	std::vector<uint8_t> out(payload.size() + 16);
	std::size_t n = 0;
	napi_envelope_encode(payload.data(), payload.size(), out.data(), out.size(), &n);
	out.resize(n);
	return out;
}

// The gate's reply naming `body`'s VARs, as the gate socket receives it.
std::vector<uint8_t> gate_reply(const std::string &body) {
	std::vector<uint8_t> inner(body.begin(), body.end());
	nwu_decrypt(inner.data(), inner.size(), GATE_NWU_KEY);
	return enveloped(inner);
}

bool decode(const std::vector<uint8_t> &dg, uint8_t &opcode, std::vector<uint8_t> &body) {
	return nw_decode_inbound(dg.data(), dg.size(), opcode, body);
}

const PeerAddr kServer{ 0x0100007Fu, 64206 }; // 127.0.0.1:64206, the gate's one UDPNOVAWORLD

// One connect on in-memory sockets: the gate names kServer, and the session starts on the
// first tick.
struct Rig {
	MemorySocket gate;
	MemorySocket session;
	std::vector<std::string> fatals;
	// The session as the last fatal found it (the driver drops a poll failure's connection once
	// the hook returns).
	bool had_session_at_fatal = false;
	ClientSession::State state_at_fatal = ClientSession::State::Idle;
	int lobby_state_at_fatal = -1;
	uint32_t flags_at_fatal = 0;
	uint32_t jfc_at_fatal = 0;
	uint32_t jfp_at_fatal = 0;
	NwuLobbySession lobby;

	Rig() : lobby(hooks(), NwuLobbySession::Environment{}) {
		lobby.set_title(&kJo, kJo.gate_tag);
	}

	NwuLobbySession::Hooks hooks() {
		NwuLobbySession::Hooks out;
		out.on_fatal = [this](const std::string &message) {
			fatals.push_back(message);
			const ClientSession *s = lobby.session();
			had_session_at_fatal = s != nullptr;
			if (s == nullptr)
				return;
			state_at_fatal = s->state();
			lobby_state_at_fatal = s->lobby_state();
			flags_at_fatal = s->session_flags();
			jfc_at_fatal = s->join_fail_code();
			jfp_at_fatal = s->join_fail_param();
		};
		return out;
	}

	void start(uint32_t at) {
		lobby.open(gate, session);
		lobby.probe("127.0.0.1", GATE_DEFAULT_PORT);
		gate.inbound.push_back({ PeerAddr{ 0x0100007Fu, GATE_DEFAULT_PORT },
				gate_reply("VAR \"UDPNOVAWORLD\" \"127.0.0.1:64206\"\r\n") });
		lobby.tick(at);
	}

	std::size_t count(uint8_t opcode) const {
		std::size_t n = 0;
		for (const auto &dg : session.sent) {
			uint8_t op = 0;
			std::vector<uint8_t> body;
			if (decode(dg.second, op, body) && op == opcode)
				++n;
		}
		return n;
	}

	// The last datagram of `opcode` the session sent (empty when none).
	std::vector<uint8_t> last(uint8_t opcode) const {
		for (auto it = session.sent.rbegin(); it != session.sent.rend(); ++it) {
			uint8_t op = 0;
			std::vector<uint8_t> body;
			if (decode(it->second, op, body) && op == opcode)
				return it->second;
		}
		return {};
	}
};

// The service's 0x81 to the session's 0x41 `hello_dg`, its CI and identity echoed.
std::vector<uint8_t> server_hello_to(const std::vector<uint8_t> &hello_dg) {
	uint8_t opcode = 0;
	std::vector<uint8_t> body;
	ClientHello hello;
	if (!decode(hello_dg, opcode, body) || opcode != SESSION_OPCODE_CLIENT_HELLO ||
			!parse_client_hello(body.data(), body.size(), hello))
		return {};
	return nw_encode_outbound(SESSION_OPCODE_SERVER_HELLO,
			server_hello_to_bytes(build_server_hello(hello, 0x7F000001u, 40000)));
}

// The service's accepting 0x82 to the session's 0x42 `auth_dg`, its CI and CK echoed.
std::vector<uint8_t> server_auth_to(const std::vector<uint8_t> &auth_dg) {
	uint8_t opcode = 0;
	std::vector<uint8_t> body;
	ClientAuth auth;
	if (!decode(auth_dg, opcode, body) || opcode != SESSION_OPCODE_CLIENT_AUTH ||
			!parse_client_auth(body.data(), body.size(), auth))
		return {};
	const ServerAuth accept =
			build_server_auth(auth, 0x7F000001u, 40000, 0x51E55105u, "servercrypt");
	return nw_encode_outbound(SESSION_OPCODE_SERVER_AUTH, server_auth_to_bytes(accept));
}

// The service's rejecting 0x82 (CR 0) to the session's 0x42 `auth_dg`, carrying JFC / JFP.
std::vector<uint8_t> server_reject_to(
		const std::vector<uint8_t> &auth_dg, uint32_t jfc, uint32_t jfp) {
	uint8_t opcode = 0;
	std::vector<uint8_t> body;
	ClientAuth auth;
	if (!decode(auth_dg, opcode, body) || opcode != SESSION_OPCODE_CLIENT_AUTH ||
			!parse_client_auth(body.data(), body.size(), auth))
		return {};
	ServerAuth reject = build_server_auth(auth, 0x7F000001u, 40000, 0, "");
	reject.cr = 0;
	reject.jfc = jfc;
	reject.jfp = jfp;
	return nw_encode_outbound(SESSION_OPCODE_SERVER_AUTH, server_auth_to_bytes(reject));
}

// The menu's map over the connect legs' codes: -23..-1 show NWEC(25 - code), anything else
// NWEC17. [orig: UI_ShowNovaWorldConnectError @0x5584A0 - @0x5584A4, the table @0x5586F8, the
//  default @0x5586DF]
void check_connect_error_tags() {
	CHECK(novaworld_connect_error_tag(-1) == "NWEC26");
	CHECK(novaworld_connect_error_tag(-2) == "NWEC27");
	CHECK(novaworld_connect_error_tag(-15) == "NWEC40");
	CHECK(novaworld_connect_error_tag(-17) == "NWEC42");
	CHECK(novaworld_connect_error_tag(-18) == "NWEC43");
	CHECK(novaworld_connect_error_tag(-20) == "NWEC45");
	CHECK(novaworld_connect_error_tag(-21) == "NWEC46");
	CHECK(novaworld_connect_error_tag(-23) == "NWEC48");
	CHECK(novaworld_connect_error_tag(0) == "NWEC17");
	CHECK(novaworld_connect_error_tag(-24) == "NWEC17");
	CHECK(novaworld_connect_error_tag(5) == "NWEC17");
	CHECK(SESSION_PERIODIC_UPDATE_TIMEOUT_MS == 20000u);
	CHECK(SESSION_CONNECT_POLL_TIMEOUT_MS == 30000u);
	CHECK(SESSION_CONNECT_VERIFY_TIMEOUT_MS == 20000u);
}

// A first connect no 0x81 answers: the periodic update gives it up 20 s after the session's
// state-1 entry (not at 20 s, past it), the probe stops (no 0x41 from then on, a 0x81 for the
// connection neither claimed nor read), and the validation wait reports the give-up word's
// NWEC42, once. [orig: CNapiGameSession_ProcessPeriodicUpdate @0x4D4400 - the 0x4E20 compare
//  @0x4D44A3, +0x12C = 1 @0x4D44B1; CNapiGameSession_PumpConnection @0x4D0A40 - NapiTimer_Stop
//  @0x4D0A71; CGameSession_WaitForNWUValidation @0x4D0F20 - -17 @0x4D111C]
void check_state_one_give_up() {
	Rig rig;
	rig.start(10);
	CHECK(rig.lobby.session() != nullptr);
	if (rig.lobby.session() == nullptr)
		return;
	const ClientSession &session = *rig.lobby.session();
	CHECK(session.lobby_state() == 1 && session.lobby_state_entered_ms() == 10);
	CHECK(rig.count(SESSION_OPCODE_CLIENT_HELLO) == 1);
	const std::vector<uint8_t> first_hello = rig.last(SESSION_OPCODE_CLIENT_HELLO);

	rig.lobby.tick(10 + SESSION_PERIODIC_UPDATE_TIMEOUT_MS);
	CHECK(rig.fatals.empty());
	CHECK(session.lobby_state() == 1);
	const std::size_t hellos = rig.count(SESSION_OPCODE_CLIENT_HELLO);
	CHECK(hellos == 2); // the re-announce

	rig.lobby.tick(10 + SESSION_PERIODIC_UPDATE_TIMEOUT_MS + 1);
	CHECK(rig.fatals == std::vector<std::string>{ "NWEC42" });
	CHECK(rig.had_session_at_fatal && rig.lobby_state_at_fatal == 0 && rig.flags_at_fatal == 0);
	CHECK(rig.lobby.session() == nullptr);
	CHECK(rig.count(SESSION_OPCODE_CLIENT_HELLO) == hellos);

	const std::vector<uint8_t> reply = server_hello_to(first_hello);
	CHECK(!reply.empty());
	CHECK(!rig.lobby.claims(kServer, reply.data(), reply.size()));
	rig.session.inbound.push_back({ kServer, reply });
	rig.lobby.tick(10 + SESSION_PERIODIC_UPDATE_TIMEOUT_MS + 2 + SESSION_CONNECT_RETRANSMIT_MS);
	CHECK(rig.lobby.session() == nullptr);
	CHECK(rig.count(SESSION_OPCODE_CLIENT_AUTH) == 0);
	CHECK(rig.count(SESSION_OPCODE_CLIENT_HELLO) == hellos);
	CHECK(rig.fatals.size() == 1);
	rig.lobby.close();
}

// A first connect whose 0x82 lands but whose verify never completes: the validation wait passes
// on the tick that sees the session out of state 1 and re-stamps, and the verify poll gives up
// 20 s after that re-stamp (not at it, past it), the connection that was up tearing down (its
// 0x46 burst to the server) before the connection wait reports NWEC46. The first connect's
// state-1 give-up does not fire on a session past state 1.
// [orig: CGameSession_WaitForNWUValidation @0x4D0F20 - the re-stamp @0x4D120C;
//  CNapiGameSession_PollConnectPhase2 @0x4D4D70 - the 0x4E20 compare @0x4D4DB1, the give-up
//  @0x4D4E03 -> CNapiNPConnection_RequestDisconnect @0x61E0F0 (state 5 -> SetState(6));
//  CGameSession_WaitForNWUConnection @0x4D1230 - -21 @0x4D1296]
void check_verify_give_up() {
	Rig rig;
	rig.start(10);
	CHECK(rig.lobby.session() != nullptr);
	if (rig.lobby.session() == nullptr)
		return;
	const ClientSession &session = *rig.lobby.session();

	rig.session.inbound.push_back(
			{ kServer, server_hello_to(rig.last(SESSION_OPCODE_CLIENT_HELLO)) });
	rig.lobby.tick(100);
	CHECK(session.state() == ClientSession::State::Auth);
	const std::vector<uint8_t> auth = rig.last(SESSION_OPCODE_CLIENT_AUTH);
	CHECK(!auth.empty());
	rig.session.inbound.push_back({ kServer, server_auth_to(auth) });
	const uint32_t up_at = 200;
	rig.lobby.tick(up_at);
	CHECK(session.state() == ClientSession::State::Verifying);
	CHECK(session.lobby_state() == 2 || session.lobby_state() == 3);
	CHECK(rig.fatals.empty());

	// Past 20 s from the state-1 entry, but the session left state 1: no give-up there.
	rig.lobby.tick(10 + SESSION_PERIODIC_UPDATE_TIMEOUT_MS + 1);
	CHECK(rig.fatals.empty());
	rig.lobby.tick(up_at + SESSION_CONNECT_VERIFY_TIMEOUT_MS);
	CHECK(rig.fatals.empty());
	CHECK(rig.count(SESSION_OPCODE_CLIENT_GOODBYE) == 0);

	rig.lobby.tick(up_at + SESSION_CONNECT_VERIFY_TIMEOUT_MS + 1);
	CHECK(rig.fatals == std::vector<std::string>{ "NWEC46" });
	CHECK(rig.count(SESSION_OPCODE_CLIENT_GOODBYE) >= 1);
	CHECK(rig.state_at_fatal == ClientSession::State::Closed && rig.lobby_state_at_fatal == 0);
	CHECK(rig.lobby.session() == nullptr);
	for (const auto &dg : rig.session.sent) {
		uint8_t op = 0;
		std::vector<uint8_t> body;
		if (decode(dg.second, op, body) && op == SESSION_OPCODE_CLIENT_GOODBYE)
			CHECK(dg.first == kServer);
	}
	rig.lobby.close();
}

// D-NET-453: a first connect the service rejects (CR 0) is not a session error. The connection
// keeps the 0x82's JFC / JFP and drops to its down state with no teardown (no 0x46), and the
// periodic update gives the first connect (the word 0) up on it with the give-up word 2: the
// probe stays stopped, the 0x42 leg sends nothing more, and the validation wait reports NWEC43.
// [orig: NapiNP_HandleServerJoinResponse @0x629840 - @0x629CD8..0x629CF6, the store @0x629F95;
//  CNapiGameSession_ProcessPeriodicUpdate @0x4D4400 - the state-4 test @0x4D4436..0x4D4441,
//  +0x12C = 2 @0x4D446D, CNapiGameSession_ProcessNetwork @0x4D447E;
//  CGameSession_WaitForNWUValidation @0x4D0F20 - -18 @0x4D10FF]
void check_rejected_connect() {
	Rig rig;
	rig.start(10);
	CHECK(rig.lobby.session() != nullptr);
	if (rig.lobby.session() == nullptr)
		return;
	const ClientSession &session = *rig.lobby.session();
	rig.session.inbound.push_back(
			{ kServer, server_hello_to(rig.last(SESSION_OPCODE_CLIENT_HELLO)) });
	rig.lobby.tick(100);
	CHECK(session.state() == ClientSession::State::Auth);
	const std::vector<uint8_t> auth = rig.last(SESSION_OPCODE_CLIENT_AUTH);
	CHECK(!auth.empty());
	rig.session.inbound.push_back({ kServer, server_reject_to(auth, 14, 2) });
	rig.lobby.tick(200);
	CHECK(rig.fatals == std::vector<std::string>{ "NWEC43" });
	CHECK(rig.jfc_at_fatal == 14 && rig.jfp_at_fatal == 2);
	CHECK(rig.state_at_fatal == ClientSession::State::Closed && rig.lobby_state_at_fatal == 0);
	CHECK(rig.lobby.session() == nullptr);
	CHECK(rig.count(SESSION_OPCODE_CLIENT_GOODBYE) == 0);
	const std::size_t hellos = rig.count(SESSION_OPCODE_CLIENT_HELLO);
	const std::size_t auths = rig.count(SESSION_OPCODE_CLIENT_AUTH);
	rig.lobby.tick(200 + 2 * SESSION_CONNECT_RETRANSMIT_MS + 2);
	rig.lobby.tick(200 + 4 * SESSION_CONNECT_RETRANSMIT_MS + 4);
	CHECK(rig.count(SESSION_OPCODE_CLIENT_HELLO) == hellos);
	CHECK(rig.count(SESSION_OPCODE_CLIENT_AUTH) == auths);
	CHECK(rig.fatals.size() == 1);
	rig.lobby.close();
}

// The service's 0x83 to a connection up since `auth_dg`'s accept (server_auth_to's SCRK): the
// next packet (seq 1) with one lobby container `name` carrying `fields`.
std::vector<uint8_t> server_statement_to(const std::vector<uint8_t> &auth_dg, const char *name,
		const std::vector<std::pair<std::string, std::string>> &fields) {
	uint8_t opcode = 0;
	std::vector<uint8_t> body;
	ClientAuth auth;
	if (!decode(auth_dg, opcode, body) || opcode != SESSION_OPCODE_CLIENT_AUTH ||
			!parse_client_auth(body.data(), body.size(), auth))
		return {};
	NapiMessage container;
	container.name = name;
	for (const auto &field : fields) {
		NapiField f;
		f.name = field.first;
		f.data.assign(field.second.begin(), field.second.end());
		container.fields.push_back(f);
	}
	std::vector<NapiMessage> stream{ container };
	std::vector<uint8_t> stream_bytes(napi_stream_size(stream));
	std::size_t stream_size = 0;
	if (napi_stream_encode(stream, stream_bytes.data(), stream_bytes.size(), &stream_size) != 0)
		return {};
	stream_bytes.resize(stream_size);
	ProtocolMessage record;
	record.flags.raw = stream_size > 0xFFu ? 0x40u : 0x20u;
	record.flags.len16 = stream_size > 0xFFu;
	record.flags.len8 = stream_size <= 0xFFu;
	record.tag = 0;
	record.full_tag = 0;
	record.length = static_cast<uint32_t>(stream_size);
	record.payload = std::move(stream_bytes);
	ProtocolPacketHeader hdr;
	hdr.session_id = auth.ck;
	hdr.seq_num = 1;
	hdr.ack_count = 0;
	hdr.connection_flags = 0;
	std::vector<uint8_t> packet;
	if (!encode_protocol_packet_plaintext(hdr, { record }, "servercrypt", packet))
		return {};
	return nw_encode_outbound(SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE, std::move(packet));
}

// A first connect up to its verify: the 0x81 at 100, the accepting 0x82 at 200. Returns the 0x42
// the 0x82 answered (empty when the connect did not get that far).
std::vector<uint8_t> bring_to_verify(Rig &rig) {
	rig.start(10);
	if (rig.lobby.session() == nullptr)
		return {};
	rig.session.inbound.push_back(
			{ kServer, server_hello_to(rig.last(SESSION_OPCODE_CLIENT_HELLO)) });
	rig.lobby.tick(100);
	const std::vector<uint8_t> auth = rig.last(SESSION_OPCODE_CLIENT_AUTH);
	if (auth.empty())
		return {};
	rig.session.inbound.push_back({ kServer, server_auth_to(auth) });
	rig.lobby.tick(200);
	return auth;
}

// A verify the service rejects (ServerVerifyResult Success=0): the rejection runs the connect's
// abort, the connection tearing down with its 0x46 burst, and drops the session to state 0, so
// the verify poll ends and the connection wait reports the session not up, NWEC48, once: the
// session's own error is not reported while the poll runs. The driver then drops the connection.
// [orig: CNapiGameSession_HandleConnectVerifyResponse @0x4d5800 - CNapiGameSession_PumpConnection
//  @0x4D0A40 called @0x4d58ac, SetState(0) @0x4d58e1; CNapiGameSession_PollConnectPhase2
//  @0x4D4D70 - the states @0x4D4D90..0x4D4D9E; CGameSession_WaitForNWUConnection @0x4D1230 - bit 2
//  @0x4D1332, -23 @0x4D1381; CNapiGameSession_ResetToDisconnected @0x4D0890]
void check_verify_rejected() {
	Rig rig;
	const std::vector<uint8_t> auth = bring_to_verify(rig);
	CHECK(!auth.empty() && rig.lobby.session() != nullptr && rig.fatals.empty());
	if (auth.empty() || rig.lobby.session() == nullptr)
		return;
	CHECK(rig.lobby.session()->state() == ClientSession::State::Verifying);
	CHECK(rig.count(SESSION_OPCODE_CLIENT_GOODBYE) == 0);
	const std::vector<uint8_t> reject =
			server_statement_to(auth, "ServerVerifyResult", { { "Success", "0" } });
	CHECK(!reject.empty());
	rig.session.inbound.push_back({ kServer, reject });
	rig.lobby.tick(300);
	CHECK(rig.fatals == std::vector<std::string>{ "NWEC48" });
	CHECK(rig.lobby_state_at_fatal == 0 && rig.flags_at_fatal == 0);
	CHECK(rig.count(SESSION_OPCODE_CLIENT_GOODBYE) >= 1);
	for (const auto &dg : rig.session.sent) {
		uint8_t op = 0;
		std::vector<uint8_t> body;
		if (decode(dg.second, op, body) && op == SESSION_OPCODE_CLIENT_GOODBYE)
			CHECK(dg.first == kServer);
	}
	CHECK(rig.lobby.session() == nullptr);
	const std::size_t sent = rig.session.sent.size();
	rig.lobby.tick(300 + SESSION_CONNECT_VERIFY_TIMEOUT_MS + 1);
	CHECK(rig.fatals.size() == 1);
	CHECK(rig.session.sent.size() == sent);
	rig.lobby.close();
}

// The validation wait's -20: a session that left state 1 for a state outside 2..4 with no give-up
// (the service accepts the 0x82 and closes the connection in the same receive batch, its goodbye
// tearing the connection down to state 0) reports NWEC45, once.
// [orig: CGameSession_WaitForNWUValidation @0x4D0F20 - the states @0x4D1137..0x4D114A, -20
//  @0x4D1196]
void check_not_verifying() {
	Rig rig;
	rig.start(10);
	CHECK(rig.lobby.session() != nullptr);
	if (rig.lobby.session() == nullptr)
		return;
	rig.session.inbound.push_back(
			{ kServer, server_hello_to(rig.last(SESSION_OPCODE_CLIENT_HELLO)) });
	rig.lobby.tick(100);
	const std::vector<uint8_t> auth = rig.last(SESSION_OPCODE_CLIENT_AUTH);
	CHECK(!auth.empty());
	uint8_t opcode = 0;
	std::vector<uint8_t> body;
	ClientAuth parsed;
	CHECK(decode(auth, opcode, body) && parse_client_auth(body.data(), body.size(), parsed));
	const DisconnectEvent stop = make_disconnect_event(1, 2, 0, 0, "", 0, "NP.S:PT:STOP");
	rig.session.inbound.push_back({ kServer, server_auth_to(auth) });
	rig.session.inbound.push_back({ kServer,
			nw_encode_outbound(
					SESSION_OPCODE_SERVER_GOODBYE, server_goodbye_to_bytes(parsed.ck, stop)) });
	rig.lobby.tick(200);
	CHECK(rig.fatals == std::vector<std::string>{ "NWEC45" });
	CHECK(rig.lobby_state_at_fatal == 0);
	CHECK(rig.lobby.session() == nullptr);
	rig.lobby.tick(200 + SESSION_CONNECT_POLL_TIMEOUT_MS + 1);
	CHECK(rig.fatals.size() == 1);
	rig.lobby.close();
}

// The reset drops the connection: a first connect given up in state 1 while its 0x42 is
// outstanding takes no late accepting 0x82, so nothing advances after the reported NWEC42 (no
// 0x43, no further report, no session), whatever the owner goes on ticking.
// [orig: CNapiGameSession_ResetToDisconnected @0x4D0890 - the connection's destroy 0x62A4B0
//  @0x4D08A7, the session inactive @0x4D092E]
void check_late_accept_after_give_up() {
	Rig rig;
	rig.start(10);
	CHECK(rig.lobby.session() != nullptr);
	if (rig.lobby.session() == nullptr)
		return;
	rig.session.inbound.push_back(
			{ kServer, server_hello_to(rig.last(SESSION_OPCODE_CLIENT_HELLO)) });
	rig.lobby.tick(100);
	const std::vector<uint8_t> auth = rig.last(SESSION_OPCODE_CLIENT_AUTH);
	CHECK(!auth.empty());
	rig.lobby.tick(10 + SESSION_PERIODIC_UPDATE_TIMEOUT_MS + 1);
	CHECK(rig.fatals == std::vector<std::string>{ "NWEC42" });
	CHECK(rig.state_at_fatal == ClientSession::State::Auth);
	CHECK(rig.lobby.session() == nullptr);
	const std::vector<uint8_t> late = server_auth_to(auth);
	CHECK(!rig.lobby.claims(kServer, late.data(), late.size()));
	rig.session.inbound.push_back({ kServer, late });
	const std::size_t sent = rig.session.sent.size();
	for (uint32_t t = 20100; t <= 20100 + 2 * SESSION_CONNECT_VERIFY_TIMEOUT_MS; t += 1000)
		rig.lobby.tick(t);
	CHECK(rig.lobby.session() == nullptr);
	CHECK(rig.count(SESSION_OPCODE_PROTOCOL_MESSAGE) == 0);
	CHECK(rig.session.sent.size() == sent);
	CHECK(rig.fatals.size() == 1);
	rig.lobby.close();
}

} // namespace

int main() {
	check_connect_error_tags();
	check_state_one_give_up();
	check_verify_give_up();
	check_rejected_connect();
	check_verify_rejected();
	check_not_verifying();
	check_late_accept_after_give_up();
	if (failures != 0) {
		std::printf("nwu_lobby_connect: %d failure(s)\n", failures);
		return 1;
	}
	std::printf("nwu_lobby_connect: ok\n");
	return 0;
}
