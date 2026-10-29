// The first NovaWorld connect's give-ups (D-NET-571): the NWU lobby driver runs retail's
// NovaWorld menu poll over its session (the connect poll, the validation wait, the verify poll,
// the connection wait) instead of one 60 s deadline, and each give-up stops the enumerator's
// probe and reports the menu's NWEC. Socket-free: in-memory datagram sockets stand in, and the
// service's 0x81 / 0x82 are built with the engine's own writers.
// [orig: UI_ProcessLANSessionStateMachine @0x558DE0 - states 5..8 @0x55919E..0x55923C;
//  CNapiGameSession_ProcessPeriodicUpdate @0x4D4400 - @0x4D4483..0x4D44B1;
//  CNapiGameSession_PollConnectPhase1 @0x4D4C30; CGameSession_WaitForNWUValidation @0x4D0F20;
//  CNapiGameSession_PollConnectPhase2 @0x4D4D70; CGameSession_WaitForNWUConnection @0x4D1230;
//  UI_ShowNetworkErrorByCode @0x5584A0]

#include <net/napi/envelope.h>
#include <net/protocol/protocol.h>
#include <base/gameprofile/gameprofile.h>
#include <net/napi/session.h>
#include <net/novacrypto/nwu.h>
#include <net/novaworld/connect_or_host.h>
#include <net/novaworld/gate_probe.h>
#include <net/novaworld/http_flow.h> // http_getting_line / http_hosting_line
#include <net/novaworld/nwu_host_role.h>
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

// JO's title identity (net/protocol/protocol.h), the identity these tests speak.
static const opennova::Protocol &kJo = *opennova::title_protocol("jo");

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
// The enumerator's record of kServer's 0x81 (its SN, the service's "OpenNova"): its two lines.
const std::string kDiscovered = "NWU SESSION DISCOVERED AT 127.0.0.1:64206 [OpenNova]";
const std::string kRemoved = "NWU SESSION AT 127.0.0.1:64206 [OpenNova] HAS BEEN REMOVED.";
// The menu runs one connect state a frame: the accepting 0x82 the rigs land at 200 ends the
// connect poll in that frame, and the validation wait (the verify poll's re-stamp) runs in the
// next, at 210. [orig: UI_ProcessLANSessionStateMachine @0x558DE0 - state 6 set @0x5591B0, the
//  validation wait @0x5591D5]
constexpr uint32_t kVerifyStampAt = 210;

// One connect on in-memory sockets: the gate names kServer; its reply is taken three frames ahead
// of the connect's (the menu's states 3 and 4 between), and the first 0x41 goes at the frame
// after the connect's (the command-line host connects and sends in the reply's own frame).
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
	// The connect log: on when set, its lines in order (the gate socket bound on 2300).
	bool log_on = false;
	std::vector<std::string> log;
	// The NWU packet lines (SENDING / PROCESSING), kept apart so the other pins read the rest.
	std::vector<std::string> packet_log;
	// Every connect-log line in order, the packet lines included.
	std::vector<std::string> all;
	// The first connect's validated hook: how many times, and the log's size when it ran.
	int validated = 0;
	std::size_t validated_at_log = 0;
	// The gate's reply body start() hands the gate socket.
	std::string gate_body = "VAR \"UDPNOVAWORLD\" \"127.0.0.1:64206\"\r\n";
	NwuLobbySession lobby;

	explicit Rig(bool command_line_host = false)
		: lobby(hooks(), environment(command_line_host)) {
		lobby.set_title(&kJo, kJo.gate_tag);
	}

	static NwuLobbySession::Environment environment(bool command_line_host) {
		NwuLobbySession::Environment env;
		env.gate_local_port = []() { return uint16_t{2300}; };
		env.command_line_host = command_line_host;
		return env;
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
		out.on_connect_validated = [this]() {
			++validated;
			validated_at_log = log.size();
		};
		out.connect_log_on = [this]() { return log_on; };
		out.connect_log = [this](const std::string &text) {
			all.push_back(text);
			if (text.rfind("NWU SENDING ", 0) == 0 || text.rfind("NWU PROCESSING ", 0) == 0)
				packet_log.push_back(text);
			else
				log.push_back(text);
		};
		return out;
	}

	void start(uint32_t at) {
		lobby.open(gate, session);
		lobby.probe("127.0.0.1", GATE_DEFAULT_PORT);
		gate.inbound.push_back({ PeerAddr{ 0x0100007Fu, GATE_DEFAULT_PORT }, gate_reply(gate_body) });
		for (uint32_t t = at - 3; t <= at + 1; ++t)
			lobby.tick(t);
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
// NWEC17. [orig: UI_ShowNetworkErrorByCode @0x5584A0 - @0x5584A4, the table @0x5586F8, the
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
// connection neither claimed nor read), the connect poll ends in that frame, and the validation
// wait, the menu's next frame, reports the give-up word's NWEC42, once.
// [orig: CNapiGameSession_ProcessPeriodicUpdate @0x4D4400 - the 0x4E20 compare @0x4D44A3, +0x12C
//  = 1 @0x4D44B1; CNapiGameSession_PumpConnection @0x4D0A40 - NapiTimer_Stop @0x4D0A71;
//  CGameSession_WaitForNWUValidation @0x4D0F20 - -17 @0x4D111C]
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
	CHECK(rig.fatals.empty());
	CHECK(session.lobby_state() == 0);
	rig.lobby.tick(10 + SESSION_PERIODIC_UPDATE_TIMEOUT_MS + 2);
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
// in the frame after the one whose connect poll sees the session out of state 1 and re-stamps,
// and the verify poll gives up 20 s after that re-stamp (not at it, past it), the connection that
// was up tearing down (its 0x46 burst to the server); the connection wait reports NWEC46 the
// next frame. The first connect's state-1 give-up does not fire on a session past state 1.
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
	rig.lobby.tick(101); // the 0x42 goes at the first pump past the 0x81's frame
	CHECK(session.state() == ClientSession::State::Auth);
	const std::vector<uint8_t> auth = rig.last(SESSION_OPCODE_CLIENT_AUTH);
	CHECK(!auth.empty());
	rig.session.inbound.push_back({ kServer, server_auth_to(auth) });
	rig.lobby.tick(200);
	CHECK(session.state() == ClientSession::State::Verifying);
	CHECK(session.lobby_state() == 2 || session.lobby_state() == 3);
	CHECK(rig.fatals.empty());
	rig.lobby.tick(kVerifyStampAt); // the validation wait's frame: the re-stamp
	CHECK(rig.fatals.empty());

	// Past 20 s from the state-1 entry, but the session left state 1: no give-up there.
	rig.lobby.tick(10 + SESSION_PERIODIC_UPDATE_TIMEOUT_MS + 1);
	CHECK(rig.fatals.empty());
	rig.lobby.tick(kVerifyStampAt + SESSION_CONNECT_VERIFY_TIMEOUT_MS);
	CHECK(rig.fatals.empty());
	CHECK(rig.count(SESSION_OPCODE_CLIENT_GOODBYE) == 0);

	rig.lobby.tick(kVerifyStampAt + SESSION_CONNECT_VERIFY_TIMEOUT_MS + 1);
	CHECK(rig.fatals.empty());
	CHECK(rig.count(SESSION_OPCODE_CLIENT_GOODBYE) >= 1);
	rig.lobby.tick(kVerifyStampAt + SESSION_CONNECT_VERIFY_TIMEOUT_MS + 2);
	CHECK(rig.fatals == std::vector<std::string>{ "NWEC46" });
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
// probe stays stopped, the 0x42 leg sends nothing more, and the validation wait, the menu's next
// frame, reports NWEC43.
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
	rig.lobby.tick(101); // the 0x42 goes at the first pump past the 0x81's frame
	CHECK(session.state() == ClientSession::State::Auth);
	const std::vector<uint8_t> auth = rig.last(SESSION_OPCODE_CLIENT_AUTH);
	CHECK(!auth.empty());
	rig.session.inbound.push_back({ kServer, server_reject_to(auth, 14, 2) });
	rig.lobby.tick(200);
	CHECK(rig.fatals.empty());
	rig.lobby.tick(210);
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

// The service's 0x83 to a connection up since `auth_dg`'s accept (server_auth_to's SCRK): packet
// `seq` (the first, 1, by default) with one lobby container `name` carrying `fields`.
std::vector<uint8_t> server_statement_to(const std::vector<uint8_t> &auth_dg, const char *name,
		const std::vector<std::pair<std::string, std::string>> &fields, uint32_t seq = 1) {
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
	hdr.seq_num = seq;
	hdr.ack_count = 0;
	hdr.connection_flags = 0;
	std::vector<uint8_t> packet;
	if (!encode_protocol_packet_plaintext(hdr, { record }, "servercrypt", packet))
		return {};
	return nw_encode_outbound(SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE, std::move(packet));
}

// A first connect up to its accept: the 0x81 at 100, the accepting 0x82 at 200, the frame whose
// connect poll ends (the validation wait is the next). Returns the 0x42 the 0x82 answered (empty
// when the connect did not get that far).
std::vector<uint8_t> bring_to_accept(Rig &rig) {
	rig.start(10);
	if (rig.lobby.session() == nullptr)
		return {};
	rig.session.inbound.push_back(
			{ kServer, server_hello_to(rig.last(SESSION_OPCODE_CLIENT_HELLO)) });
	rig.lobby.tick(100);
	rig.lobby.tick(101); // the 0x42 goes at the first pump past the 0x81's frame
	const std::vector<uint8_t> auth = rig.last(SESSION_OPCODE_CLIENT_AUTH);
	if (auth.empty())
		return {};
	rig.session.inbound.push_back({ kServer, server_auth_to(auth) });
	rig.lobby.tick(200);
	return auth;
}

// A first connect up to its verify poll: the accept, then the validation wait's frame at
// kVerifyStampAt (the re-stamp). Returns the 0x42 the 0x82 answered.
std::vector<uint8_t> bring_to_verify(Rig &rig) {
	const std::vector<uint8_t> auth = bring_to_accept(rig);
	if (!auth.empty())
		rig.lobby.tick(kVerifyStampAt);
	return auth;
}

// The session login's end over a connect in its verify poll: the service's accepting
// ServerVerifyResult (packet 1) taken at `at` (the verify poll ends), the connection wait the
// frame after (YIPPEE), the web lobby's open the frame after that (the validated hook).
void log_in(Rig &rig, const std::vector<uint8_t> &auth, uint32_t at = 300) {
	rig.session.inbound.push_back({ kServer,
			server_statement_to(auth, "ServerVerifyResult", { { "Success", "1" } }, 1) });
	rig.lobby.tick(at);
	rig.lobby.tick(at + 2);
	rig.lobby.tick(at + 4);
}

// A verify the service rejects (ServerVerifyResult Success=0): the rejection runs the connect's
// abort, the connection tearing down with its 0x46 burst, and drops the session to state 0, so
// the verify poll ends and the connection wait, the menu's next frame, reports the session not
// up, NWEC48, once: the session's own error is not reported while the poll runs. The driver then
// drops the connection.
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
	CHECK(rig.fatals.empty());
	rig.lobby.tick(310);
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
// tearing the connection down to state 0) reports NWEC45 in the frame after, once.
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
	rig.lobby.tick(101); // the 0x42 goes at the first pump past the 0x81's frame
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
	CHECK(rig.fatals.empty());
	rig.lobby.tick(210);
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
	rig.lobby.tick(101); // the 0x42 goes at the first pump past the 0x81's frame
	const std::vector<uint8_t> auth = rig.last(SESSION_OPCODE_CLIENT_AUTH);
	CHECK(!auth.empty());
	rig.lobby.tick(10 + SESSION_PERIODIC_UPDATE_TIMEOUT_MS + 1);
	CHECK(rig.fatals.empty());
	rig.lobby.tick(10 + SESSION_PERIODIC_UPDATE_TIMEOUT_MS + 2);
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

// D-NET-705: the disconnect request the shell's stop and exit make (NovaWorldClient::stop,
// disconnect_at_exit) sends nothing from a connection that is not up, its first connect still in
// Hello or Auth, and the burst from one that is, from its accepted 0x82 on (Verifying here).
// [orig: CNapiNPConnection_RequestDisconnect @0x61E0F0 - state 5 -> 6 @0x61E0F3..0x61E0FA, any
//  other state but 1 -1 @0x61E110; the accepted 0x82's state 5 NapiNP_HandleServerJoinResponse
//  @0x629EAC]
void check_disconnect_request_by_state() {
	Rig rig;
	rig.start(10);
	CHECK(rig.lobby.session() != nullptr);
	if (rig.lobby.session() == nullptr)
		return;
	ClientSession &session = *rig.lobby.session();
	std::vector<std::vector<uint8_t>> out;
	CHECK(session.state() == ClientSession::State::Hello);
	session.request_disconnect(out);
	CHECK(out.empty());

	rig.session.inbound.push_back(
			{ kServer, server_hello_to(rig.last(SESSION_OPCODE_CLIENT_HELLO)) });
	rig.lobby.tick(100);
	rig.lobby.tick(101); // the 0x42 goes at the first pump past the 0x81's frame
	CHECK(session.state() == ClientSession::State::Auth);
	session.request_disconnect(out);
	CHECK(out.empty());

	rig.session.inbound.push_back({ kServer, server_auth_to(rig.last(SESSION_OPCODE_CLIENT_AUTH)) });
	rig.lobby.tick(200);
	CHECK(session.state() == ClientSession::State::Verifying);
	session.request_disconnect(out);
	CHECK(!out.empty() && out.size() == session.disconnect_burst_count());
	for (const auto &dg : out) {
		uint8_t op = 0;
		std::vector<uint8_t> body;
		CHECK(decode(dg, op, body) && op == SESSION_OPCODE_CLIENT_GOODBYE);
	}
	rig.lobby.close();
}

// The teardown's lines: the 0x46 burst's SENDING lines, then OnDisconnect's DISCONNECTING, as
// TeardownActiveConnection sends the burst before its callback: an escape's abort, a punt, and an
// owner's disconnect request sent through the driver; one whose burst never goes out through the
// driver writes its line at the close.
// [orig: CNapiNPConnection_TeardownActiveConnection @0x6253C0 - the state-5 burst
//  @0x6254B5..0x6254D3, the conn+0xC4 callback @0x6254E7; CNapiNPConnection_SendDisconnectPacket
//  @0x61F2A0 - the SENDING line's +0x270 load @0x61F514; CNapiGameSession_OnDisconnect @0x4CFAA0 -
//  @0x4CFB5C]
void check_teardown_line_order() {
	const auto order_ok = [](const std::vector<std::string> &all, std::size_t burst) {
		std::size_t down = all.size();
		for (std::size_t i = 0; i < all.size(); ++i)
			if (all[i].rfind("NWU DISCONNECTING FROM NOVAWORLD ", 0) == 0) {
				if (down != all.size()) return false; // one line
				down = i;
			}
		if (down == all.size()) return false;
		std::size_t before = 0;
		std::size_t after = 0;
		for (std::size_t i = 0; i < all.size(); ++i) {
			const bool goodbye = all[i].rfind("NWU SENDING ", 0) == 0 &&
					all[i].find("[0x46, p#0]") != std::string::npos;
			if (goodbye) (i < down ? before : after) += 1;
		}
		return before == burst && after == 0 && down > 0 &&
				all[down - 1].find("[0x46, p#0]") != std::string::npos;
	};
	{
		Rig rig;
		rig.log_on = true;
		bring_to_verify(rig);
		CHECK(rig.lobby.session() != nullptr);
		if (rig.lobby.session() == nullptr) return;
		const std::size_t burst = rig.lobby.session()->disconnect_burst_count();
		rig.all.clear();
		CHECK(rig.lobby.escape());
		rig.lobby.tick(300);
		CHECK(order_ok(rig.all, burst));
		rig.lobby.close();
	}
	{
		Rig rig;
		rig.log_on = true;
		const std::vector<uint8_t> auth = bring_to_verify(rig);
		log_in(rig, auth);
		CHECK(rig.lobby.session() != nullptr);
		if (rig.lobby.session() == nullptr) return;
		const std::size_t burst = rig.lobby.session()->disconnect_burst_count();
		rig.all.clear();
		rig.session.inbound.push_back({ kServer,
				server_statement_to(auth, "ServerLeaveNovaWorld", { { "MsgCode", "7" } }, 2) });
		rig.lobby.tick(400);
		CHECK(order_ok(rig.all, burst));
		rig.lobby.close();
	}
	{
		Rig rig;
		rig.log_on = true;
		bring_to_verify(rig);
		ClientSession *session = rig.lobby.session();
		CHECK(session != nullptr);
		if (session == nullptr) return;
		const std::size_t burst = session->disconnect_burst_count();
		rig.all.clear();
		std::vector<std::vector<uint8_t>> out;
		session->request_disconnect(out);
		CHECK(rig.all.empty()); // the line waits for the burst
		// A user leave's goodbye burst, as the live JO client sends it (the LN S1b record,
		// 2026-10-23): four datagrams, each 74 B of UDP payload (the 4 B envelope, the opcode, the
		// key and the unlatched record's seven TLVs).
		CHECK(burst == 4 && out.size() == burst);
		for (const auto &dg : out) CHECK(dg.size() == 74);
		for (const auto &dg : out) rig.lobby.send(dg);
		CHECK(order_ok(rig.all, burst));
		rig.lobby.close();
	}
	{
		Rig rig;
		rig.log_on = true;
		bring_to_verify(rig);
		ClientSession *session = rig.lobby.session();
		CHECK(session != nullptr);
		if (session == nullptr) return;
		rig.all.clear();
		std::vector<std::vector<uint8_t>> out;
		session->request_disconnect(out);
		rig.lobby.close();
		CHECK(!rig.all.empty() && rig.all.front().rfind("NWU DISCONNECTING FROM NOVAWORLD ", 0) == 0);
	}
}

// D-NET-719: a 0x83 too short for its header does not end the connection. Retail reads each
// missing header field as 0: a packet without the receiver key is tossed, one without a sequence
// consumed, an in-order one cut before its ACK parsed with no record (its sequence taken). So the
// verify still completes: the service's ServerVerifyResult as packet 1 is now stale and passed
// over, as packet 2 it is read.
// [orig: NapiNPProtocol_HandleSessionPacket @0x626A00 - the key @0x626B52..0x626B72, the sequence
//  @0x626B8A..0x626BCC, the in-order parse (the call @0x626BF4); CNapiNPConnection_ParseMessages
//  @0x625BC0 - the ACK @0x625CD3..0x625CE5, return 1 @0x625F21]
void check_short_protocol_packets_keep_the_connection() {
	Rig rig;
	const std::vector<uint8_t> auth = bring_to_verify(rig);
	CHECK(!auth.empty() && rig.lobby.session() != nullptr);
	if (auth.empty() || rig.lobby.session() == nullptr)
		return;
	uint8_t opcode = 0;
	std::vector<uint8_t> auth_body;
	ClientAuth parsed;
	CHECK(decode(auth, opcode, auth_body) && parse_client_auth(auth_body.data(), auth_body.size(), parsed));
	const auto le = [](std::vector<uint8_t> &out, uint32_t v) {
		for (int i = 0; i < 4; ++i)
			out.push_back(static_cast<uint8_t>(v >> (8 * i)));
	};
	std::vector<uint8_t> no_key{ 0x01, 0x02 };
	std::vector<uint8_t> no_seq;
	le(no_seq, parsed.ck);
	no_seq.push_back(0x09);
	std::vector<uint8_t> no_ack;
	le(no_ack, parsed.ck);
	le(no_ack, 1);
	no_ack.push_back(0x00);
	uint32_t at = 300;
	for (const auto &short_body : { no_key, no_seq, no_ack }) {
		rig.session.inbound.push_back(
				{ kServer, nw_encode_outbound(SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE, short_body) });
		rig.lobby.tick(at);
		at += 10;
		CHECK(rig.lobby.session() != nullptr &&
				rig.lobby.session()->state() == ClientSession::State::Verifying);
	}
	CHECK(rig.fatals.empty());
	// Packet 1 was the short one: a statement under 1 is passed over.
	rig.session.inbound.push_back(
			{ kServer, server_statement_to(auth, "ServerVerifyResult", { { "Success", "0" } }, 1) });
	rig.lobby.tick(at);
	at += 10;
	CHECK(rig.fatals.empty());
	rig.session.inbound.push_back(
			{ kServer, server_statement_to(auth, "ServerVerifyResult", { { "Success", "0" } }, 2) });
	rig.lobby.tick(at);
	CHECK(rig.fatals.empty()); // the verify poll ends; the connection wait is the next frame
	rig.lobby.tick(at + 10);
	CHECK(rig.fatals == std::vector<std::string>{ "NWEC48" });
	rig.lobby.close();
}

// The connect log's gate and connect lines (D-NET-956), each text as retail's sprintf makes
// it (the device adds the time and the line end): the gate's start, its wait each frame past
// 500 ms from the stamp (ahead of the reply's processing on the frame the reply lands), the
// reply's size and every line its reader returns (an empty one too), the reflected address, and
// ConnectToNovaWorld's lines around the session's start; then the validation wait's and the
// connection wait's, each in the frame after the poll ahead of it ends.
// [orig: CNapiGateManager_WaitForConnect @0x4CE6B0 - @0x4CE84D, @0x4CE88C, @0x4CEAE3;
//  CNapiGateManager_ProcessResponse @0x4CED20 - @0x4CEDF4, @0x4CEF02, @0x4CF037, @0x4CF083,
//  @0x4CF10C, @0x4CF884; CNapiGameSession_ConnectToNovaWorld @0x4D4640 - @0x4D46C3,
//  @0x4D4751, @0x4D4B0C, @0x4D4BC2; CGameSession_WaitForNWUValidation @0x4D0F20 - @0x4D0F79,
//  @0x4D11E3; CGameSession_WaitForNWUConnection @0x4D1230 - @0x4D13D3]
void check_connect_log_gate_and_connect() {
	Rig rig;
	rig.log_on = true;
	rig.lobby.open(rig.gate, rig.session);
	rig.lobby.probe("127.0.0.1", GATE_DEFAULT_PORT);
	CHECK((rig.log == std::vector<std::string>{ "GATE CONFIGURING GATE MANAGER.",
			"GATE STARTING GATE SERVICE.",
			"GATE GATE SERVICE STARTED \"jop:cus2\"... [0.0.0.0:2300]" }));
	rig.log.clear();
	rig.lobby.tick(400);
	CHECK(rig.log.empty());
	rig.lobby.tick(600);
	CHECK((rig.log == std::vector<std::string>{ "GATE WAITING FOR REPLY... [0.0.0.0:2300]" }));
	rig.log.clear();
	const std::string body = "GATEPROTOCOL \"1.0\"\r\n\r\nVAR \"UDPNOVAWORLD\" \"127.0.0.1:64206\"\r\n"
							 "VAR \"REFLECTEDIPADDRESS\" \"203.0.113.9\"\n"
							 "VAR \"REFLECTEDPORTNUMBER\" \"7001\"";
	rig.gate.inbound.push_back({ PeerAddr{ 0x0100007Fu, GATE_DEFAULT_PORT }, gate_reply(body) });
	rig.lobby.tick(1200);
	const std::vector<std::string> want = { "GATE WAITING FOR REPLY... [0.0.0.0:2300]",
		"GATE DONE WITH GATE LOOP [n/a]", "GATE GATE SERVICE SUCCEEDED.  EXTRACTING INFO.",
		"PROCESSING GATE DATA...(" + std::to_string(body.size()) + " bytes)",
		"GATE LINE #01 [GATEPROTOCOL \"1.0\"]", "GATE LINE #02 []",
		"GATE LINE #03 [VAR \"UDPNOVAWORLD\" \"127.0.0.1:64206\"]",
		"GATE LINE #04 [VAR \"REFLECTEDIPADDRESS\" \"203.0.113.9\"]",
		"GATE LINE #05 [VAR \"REFLECTEDPORTNUMBER\" \"7001\"]",
		"GATE GATE SERVICE DONE AND SUCCESSFUL [yourip=203.0.113.9:7001]." };
	CHECK(rig.log == want);
	for (std::size_t i = 0; i < rig.log.size() && i < want.size(); ++i) {
		if (rig.log[i] != want[i])
			std::printf("  line %zu: got [%s] want [%s]\n", i, rig.log[i].c_str(), want[i].c_str());
	}
	rig.log.clear();
	// The menu's states 3 and 4, then the connect's frame.
	rig.lobby.tick(1201);
	rig.lobby.tick(1202);
	CHECK(rig.log.empty() && rig.lobby.session() == nullptr);
	rig.lobby.tick(1203);
	CHECK((rig.log == std::vector<std::string>{ "NWU DO ALL KINDS OF STUFF.", "NWU INIT STUFF.",
			"NWU ATTEMPTING TO CONNECT TO NOVAWORLD...", "NWU GOING INTO JOIN LOOP..." }));
	CHECK(rig.lobby.session() != nullptr);
	if (rig.lobby.session() == nullptr)
		return;
	rig.lobby.tick(1204); // the first 0x41
	rig.log.clear();
	rig.session.inbound.push_back(
			{ kServer, server_hello_to(rig.last(SESSION_OPCODE_CLIENT_HELLO)) });
	rig.lobby.tick(1300);
	rig.lobby.tick(1301); // the 0x42 goes at the first pump past the 0x81's frame
	// The 0x81 the connection takes makes the enumerator's record.
	CHECK(rig.log == std::vector<std::string>{ kDiscovered });
	rig.log.clear();
	const std::vector<uint8_t> auth = rig.last(SESSION_OPCODE_CLIENT_AUTH);
	rig.session.inbound.push_back({ kServer, server_auth_to(auth) });
	rig.lobby.tick(1400);
	// The 0x82's state-5 entry writes OnNovaWorldConnected's line; the connect poll ends.
	CHECK((rig.log == std::vector<std::string>{ "NWU CONNECTED TO NOVAWORLD 127.0.0.1:64206 [NWServer]  [yourip=1.0.0.127:40000]" }));
	rig.log.clear();
	rig.lobby.tick(1410);
	CHECK((rig.log == std::vector<std::string>{ "NWU LEAVING JOIN LOOP...", "NWU WAITING FOR VALIDATION..." }));
	rig.log.clear();
	rig.session.inbound.push_back(
			{ kServer, server_statement_to(auth, "ServerVerifyResult", { { "Success", "1" } }) });
	rig.lobby.tick(1500);
	CHECK(rig.log.empty()); // the verify poll ends
	rig.lobby.tick(1510);
	CHECK((rig.log == std::vector<std::string>{ "NWU YIPPEE WE ARE CONNECTED AND VALIDATED." }));
	CHECK(rig.fatals.empty());
	// The connection's destroy drains the record.
	rig.log.clear();
	rig.lobby.close();
	CHECK(rig.log == std::vector<std::string>{ kRemoved });
}

// The NWU protocol's packet lines: each datagram the connection sends, ahead of its send, with
// its length past the envelope (the opcode included), its address, its opcode and a session
// packet's sequence (0 for the others); each session packet the parse takes with the same length
// and the server's address. The connection's first announce goes once the join loop runs.
// [orig: Network_LogNWUSendPacket @0x4CDED0 - @0x4CDF5C; CNapiNPConnection_SendSessionPacket
//  @0x61EDD0 - @0x61EFEA..0x61F00D; NapiNPSession_SendAnnouncePacket @0x61FA00 - @0x61FDAC;
//  CNapiNPConnection_SendClientJoin @0x61FE20 - @0x6203C3; Network_LogNWUProcessPacket @0x4CFC80
//  - @0x4CFCFD; CNapiNPConnection_ParseMessages @0x625BC0 - @0x625D1A..0x625D3E;
//  CNapiGameSession_InitPlayerConnection @0x4D4160 - NapiTimer_Start @0x4D43C2]
void check_connect_log_packet_lines() {
	Rig rig;
	rig.log_on = true;
	const std::vector<uint8_t> auth = bring_to_accept(rig);
	CHECK(!auth.empty());
	// The join loop's line comes ahead of the first announce's send.
	CHECK(!rig.log.empty());
	const auto sent_line = [&rig](std::size_t index, const char *op, uint32_t seq) {
		if (index >= rig.session.sent.size()) return std::string();
		uint8_t opcode = 0;
		std::vector<uint8_t> body;
		if (!decode(rig.session.sent[index].second, opcode, body)) return std::string();
		return "NWU SENDING " + std::to_string(body.size() + 1) + " BYTES TO 127.0.0.1:64206 [" + op +
		       ", p#" + std::to_string(seq) + "]";
	};
	CHECK(rig.packet_log.size() >= 2);
	if (rig.packet_log.size() >= 2) {
		CHECK(rig.packet_log[0] == sent_line(0, "0x41", 0));
		CHECK(rig.packet_log[1] == sent_line(1, "0x42", 0));
	}
	// The one-shot ClientConnected, session packet 1, at the next pump.
	const std::size_t before = rig.session.sent.size();
	rig.packet_log.clear();
	rig.lobby.tick(250);
	CHECK(!rig.packet_log.empty() && rig.packet_log.front() == sent_line(before, "0x43", 1));
	rig.packet_log.clear();
	const std::vector<uint8_t> verify =
			server_statement_to(auth, "ServerVerifyResult", { { "Success", "1" } });
	uint8_t opcode = 0;
	std::vector<uint8_t> body;
	CHECK(decode(verify, opcode, body));
	rig.session.inbound.push_back({ kServer, verify });
	rig.lobby.tick(300);
	CHECK(!rig.packet_log.empty() &&
			rig.packet_log.front() == "NWU PROCESSING " + std::to_string(body.size() + 1) +
					" BYTES FROM 127.0.0.1:64206");
	rig.lobby.close();
}

// The connect waits' failure lines, each ahead of its reset: the state-1 give-up's word, the
// verify poll's 20 s and the connection wait's timeout, a session out of states 2..4, a session
// not up at the connection wait. [orig: CGameSession_WaitForNWUValidation @0x4D0F20 - @0x4D0F79,
//  "%s : NWU FAILURE [%ld]!!!" @0x4D10BF, "%s : NWU FAILURE2 [%ld]!!!" @0x4D1185;
//  CNapiGameSession_PollConnectPhase2 @0x4D4D70 - @0x4D4DF9; CGameSession_WaitForNWUConnection
//  @0x4D1230 - "%s : NWU TIMEOUT2!!!" @0x4D1287, "%s : NWU NOT CONNECTED FOR SOME REASON..."
//  @0x4D1372]
// The service's 0x43 to the session's 0x42 `auth_dg` carrying one layer-4 record of `tag` with
// `payload`, at packet sequence `seq`.
std::vector<uint8_t> server_record_to(const std::vector<uint8_t> &auth_dg, uint8_t tag,
		const std::vector<uint8_t> &payload, uint32_t seq) {
	uint8_t opcode = 0;
	std::vector<uint8_t> body;
	ClientAuth auth;
	if (!decode(auth_dg, opcode, body) || opcode != SESSION_OPCODE_CLIENT_AUTH ||
			!parse_client_auth(body.data(), body.size(), auth))
		return {};
	ProtocolMessage record;
	record.flags.raw = 0x20u;
	record.flags.len8 = true;
	record.tag = tag;
	record.full_tag = tag;
	record.length = static_cast<uint32_t>(payload.size());
	record.payload = payload;
	ProtocolPacketHeader hdr;
	hdr.session_id = auth.ck;
	hdr.seq_num = seq;
	std::vector<uint8_t> packet;
	if (!encode_protocol_packet_plaintext(hdr, { record }, "servercrypt", packet))
		return {};
	return nw_encode_outbound(SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE, std::move(packet));
}

// The NWU protocol's other lines on a verified session: a record whose tag the client registers
// no handler for (tag 0 alone), with its tag and length; a missing-sequence list naming a
// sequence, once sent; the peer's resend list naming one, once its resends are out (a list naming
// none writes nothing); the punt's drain of the enumerator's record after the teardown.
// [orig: Network_LogUnknownNWUMessage @0x4CDF90 - @0x4CDFEB; CNapiNPConnection_DispatchMessage
//  @0x622570 - @0x622691, @0x6226FB..0x62272A; NWU_LogIncomingPacketError @0x4CE010 - @0x4CE05A;
//  CNapiNPConnection_SendMissingSeqList @0x623560 - @0x62368A..0x623690, @0x6237AA..0x6237BD;
//  NWU_LogOutgoingPacketError @0x4CE080 - @0x4CE0CA; NapiNP_HandleResendList @0x623800 -
//  @0x6239AA, @0x623A24..0x623A37; CNapiGameSession_HandlePuntNotification @0x4D20B0 -
//  @0x4D21A4, @0x4D21C8]
void check_connect_log_packet_errors() {
	Rig rig;
	rig.log_on = true;
	const std::vector<uint8_t> auth = bring_to_verify(rig);
	log_in(rig, auth);
	rig.log.clear();
	rig.session.inbound.push_back({ kServer, server_record_to(auth, 5, { 1, 2, 3 }, 2) });
	rig.lobby.tick(400);
	CHECK((rig.log == std::vector<std::string>{ "NWU UNKNOWN MESSAGE!! [5, 3 bytes]" }));
	rig.log.clear();
	// Sequence 4 ahead of 3: held, and the batch's missing-sequence list names 3.
	rig.session.inbound.push_back({ kServer, server_record_to(auth, 0, {}, 4) });
	rig.lobby.tick(500);
	CHECK((rig.log == std::vector<std::string>{ "NWU INCOMING PACKET ERROR!!" }));
	rig.log.clear();
	uint8_t opcode = 0;
	std::vector<uint8_t> body;
	ClientAuth parsed;
	CHECK(decode(auth, opcode, body) && parse_client_auth(body.data(), body.size(), parsed));
	std::vector<uint8_t> none;
	CHECK(encode_session_resend_list(parsed.ck, { 0 }, none));
	rig.session.inbound.push_back(
			{ kServer, nw_encode_outbound(SESSION_OPCODE_SERVER_RESEND_LIST, std::move(none)) });
	rig.lobby.tick(550);
	CHECK(rig.log.empty()); // a list naming no sequence: its next packet only, no line
	std::vector<uint8_t> named;
	CHECK(encode_session_resend_list(parsed.ck, { 1 }, named));
	const std::size_t sent = rig.session.sent.size();
	rig.session.inbound.push_back(
			{ kServer, nw_encode_outbound(SESSION_OPCODE_SERVER_RESEND_LIST, std::move(named)) });
	rig.lobby.tick(600);
	CHECK(rig.session.sent.size() > sent); // the resend went
	CHECK((rig.log == std::vector<std::string>{ "NWU OUTGOING PACKET ERROR!!" }));
	rig.log.clear();
	rig.session.inbound.push_back({ kServer,
			server_statement_to(auth, "ServerLeaveNovaWorld", { { "MsgCode", "7" } }, 3) });
	rig.lobby.tick(700);
	CHECK(rig.log.size() >= 2 && rig.log.back() == kRemoved &&
			rig.log[rig.log.size() - 2].rfind("NWU DISCONNECTING FROM NOVAWORLD ", 0) == 0);
	rig.log.clear();
	rig.lobby.close();
	CHECK(rig.log.empty()); // drained already
}

void check_connect_log_give_ups() {
	{
		Rig rig;
		rig.log_on = true;
		rig.start(10);
		rig.log.clear();
		rig.lobby.tick(10 + SESSION_PERIODIC_UPDATE_TIMEOUT_MS + 1);
		CHECK(rig.log.empty() && rig.fatals.empty()); // the give-up writes no line
		rig.lobby.tick(10 + SESSION_PERIODIC_UPDATE_TIMEOUT_MS + 2);
		CHECK((rig.log ==
				std::vector<std::string>{ "NWU LEAVING JOIN LOOP...", "NWU FAILURE [1]!!!" }));
		CHECK(rig.fatals == std::vector<std::string>{ "NWEC42" });
		rig.lobby.close();
	}
	{
		Rig rig;
		rig.log_on = true;
		bring_to_verify(rig);
		rig.log.clear();
		rig.lobby.tick(kVerifyStampAt + SESSION_CONNECT_VERIFY_TIMEOUT_MS + 1);
		// The abort tears the connection down: OnDisconnect's line, no record latched.
		CHECK((rig.log == std::vector<std::string>{ "NWU 20 secs are up! (b)", "NWU DISCONNECTING FROM NOVAWORLD 127.0.0.1:64206 [NWServer] [0,0,0,0,\"\",0,\"\"]",
				kRemoved }));
		CHECK(rig.fatals.empty());
		rig.log.clear();
		rig.lobby.tick(kVerifyStampAt + SESSION_CONNECT_VERIFY_TIMEOUT_MS + 2);
		CHECK((rig.log == std::vector<std::string>{ "NWU TIMEOUT2!!!" }));
		CHECK(rig.fatals == std::vector<std::string>{ "NWEC46" });
		rig.lobby.close();
	}
	{
		Rig rig;
		rig.log_on = true;
		const std::vector<uint8_t> auth = bring_to_verify(rig);
		rig.log.clear();
		rig.session.inbound.push_back(
				{ kServer, server_statement_to(auth, "ServerVerifyResult", { { "Success", "0" } }) });
		rig.lobby.tick(300);
		CHECK((rig.log == std::vector<std::string>{ "NWU DISCONNECTING FROM NOVAWORLD 127.0.0.1:64206 [NWServer] [0,0,0,0,\"\",0,\"\"]",
				kRemoved }));
		CHECK(rig.fatals.empty());
		rig.log.clear();
		rig.lobby.tick(310);
		CHECK((rig.log == std::vector<std::string>{ "NWU NOT CONNECTED FOR SOME REASON..." }));
		CHECK(rig.fatals == std::vector<std::string>{ "NWEC48" });
		rig.lobby.close();
	}
	{
		Rig rig;
		rig.log_on = true;
		rig.start(10);
		rig.session.inbound.push_back(
				{ kServer, server_hello_to(rig.last(SESSION_OPCODE_CLIENT_HELLO)) });
		rig.lobby.tick(100);
		rig.lobby.tick(101); // the 0x42 goes at the first pump past the 0x81's frame
		const std::vector<uint8_t> auth = rig.last(SESSION_OPCODE_CLIENT_AUTH);
		uint8_t opcode = 0;
		std::vector<uint8_t> body;
		ClientAuth parsed;
		CHECK(decode(auth, opcode, body) && parse_client_auth(body.data(), body.size(), parsed));
		const DisconnectEvent stop = make_disconnect_event(1, 2, 0, 0, "", 0, "NP.S:PT:STOP");
		rig.session.inbound.push_back({ kServer, server_auth_to(auth) });
		rig.session.inbound.push_back({ kServer,
				nw_encode_outbound(
						SESSION_OPCODE_SERVER_GOODBYE, server_goodbye_to_bytes(parsed.ck, stop)) });
		rig.log.clear();
		rig.lobby.tick(200);
		// The peer's goodbye latches its record (the receiver's role 1) ahead of the teardown.
		CHECK((rig.log == std::vector<std::string>{ "NWU CONNECTED TO NOVAWORLD 127.0.0.1:64206 [NWServer]  [yourip=1.0.0.127:40000]",
				"NWU DISCONNECTING FROM NOVAWORLD 127.0.0.1:64206 [NWServer] "
				"[1,2,0,0,\"\",0,\"NP.S:PT:STOP\"]" }));
		CHECK(rig.fatals.empty());
		rig.log.clear();
		rig.lobby.tick(210);
		CHECK((rig.log == std::vector<std::string>{ "NWU LEAVING JOIN LOOP...", "NWU FAILURE2 [0]!!!", kRemoved }));
		CHECK(rig.fatals == std::vector<std::string>{ "NWEC45" });
		rig.lobby.close();
	}
}

// The gate's failures without a POST pair, a response the parse counts nothing of (no line but
// the read ones), the log off (no line, whatever runs), and an entry over a kept response (no
// gate line: ConnectToNovaWorld's alone). [orig: CNapiGateManager_ProcessResponse @0x4CED20 -
//  "%s : GATE FAILING GATE SERVICE...NO NW POST IP" @0x4CF5EC, "...NO NW POST PORT" @0x4CF671,
//  the count's -9 with no line @0x4CF4F1..0x4CF51E; CNapiGateManager_WaitForConnect @0x4CE6B0 -
//  the kept test @0x4CE6CF]
void check_connect_log_gate_failures() {
	// No port of the gate quad bound: the manager's two lines, the start failure over the
	// context's state 2 (its SetState(-6) refused), the wait's -1, the menu's NWEC15; no probe.
	// [orig: CNapiGateManager_WaitForConnect @0x4CE6B0 - @0x4CE84D, @0x4CE88C, "%s : GATE GATE
	//  SERVICE START FAILED [%ld]." (the append @0x4CE8E7), -1 @0x4CEA41;
	//  CNapiGateManager_ProcessConnectStateMachine @0x633D10 - @0x633E3C, SetState(-6) @0x633E64;
	//  CNapiGateManager_SetState @0x633880 - @0x633886..0x63388B; UI_ProcessLANSessionStateMachine
	//  @0x558DE0 - NWEC15 @0x559016]
	{
		Rig rig;
		rig.log_on = true;
		CHECK(rig.lobby.gate_start_failed() == "NWEC15");
		CHECK((rig.log == std::vector<std::string>{ "GATE CONFIGURING GATE MANAGER.",
				"GATE STARTING GATE SERVICE.", "GATE GATE SERVICE START FAILED [2]." }));
		CHECK(rig.gate.sent.empty());
	}
	const auto gate_lines = [](const std::string &body) {
		Rig rig;
		rig.log_on = true;
		rig.lobby.open(rig.gate, rig.session);
		rig.lobby.probe("127.0.0.1", GATE_DEFAULT_PORT);
		rig.gate.inbound.push_back({ PeerAddr{ 0x0100007Fu, GATE_DEFAULT_PORT }, gate_reply(body) });
		rig.log.clear();
		rig.lobby.tick(10);
		rig.lobby.tick(11);
		CHECK(rig.fatals.empty()); // the menu's state 3 shows it, the frame after next
		rig.lobby.tick(12);
		rig.lobby.close();
		return std::make_pair(rig.log, rig.fatals);
	};
	const std::string port = "VAR \"POSTIPPORT\" \"7598\"";
	const auto no_ip = gate_lines(port);
	CHECK(!no_ip.first.empty() && no_ip.first.back() == "GATE FAILING GATE SERVICE...NO NW POST IP");
	CHECK(no_ip.second == std::vector<std::string>{ "NWEC25" });
	const auto no_port = gate_lines("VAR \"POSTIPADDRESS\" \"192.168.0.5\"");
	CHECK(!no_port.first.empty() &&
			no_port.first.back() == "GATE FAILING GATE SERVICE...NO NW POST PORT");
	const auto none = gate_lines("GATEPROTOCOL \"1.0\"");
	CHECK((none.first == std::vector<std::string>{ "GATE DONE WITH GATE LOOP [n/a]",
			"GATE GATE SERVICE SUCCEEDED.  EXTRACTING INFO.", "PROCESSING GATE DATA...(18 bytes)",
			"GATE LINE #01 [GATEPROTOCOL \"1.0\"]" }));
	CHECK(none.second == std::vector<std::string>{ "NWEC25" });

	{
		Rig rig;
		bring_to_verify(rig);
		rig.lobby.tick(kVerifyStampAt + SESSION_CONNECT_VERIFY_TIMEOUT_MS + 1);
		rig.lobby.tick(kVerifyStampAt + SESSION_CONNECT_VERIFY_TIMEOUT_MS + 2);
		CHECK(rig.log.empty() && rig.fatals == std::vector<std::string>{ "NWEC46" });
		rig.lobby.close();
	}
	{
		NwuLobbySession::GateMemory memory;
		Rig first;
		first.lobby.set_gate_memory(&memory);
		first.start(10);
		first.lobby.close();
		Rig again;
		again.log_on = true;
		again.lobby.set_gate_memory(&memory);
		again.lobby.open(again.gate, again.session);
		again.lobby.probe("127.0.0.1", GATE_DEFAULT_PORT);
		again.lobby.tick(1);
		again.lobby.tick(2);
		CHECK(again.log.empty() && again.lobby.session() == nullptr);
		again.lobby.tick(3);
		CHECK((again.log == std::vector<std::string>{ "NWU DO ALL KINDS OF STUFF.", "NWU INIT STUFF.",
				"NWU ATTEMPTING TO CONNECT TO NOVAWORLD...", "NWU GOING INTO JOIN LOOP..." }));
		again.lobby.close();
	}
}

// The gate worker's cadence with no reply: the probe at once, again whenever more than 2000 ms
// passed since the last, none past 10000 ms since the first, where the worker stops (ours
// reporting NWEC15 once): five probes. [orig: CNapiGateManager_WaitForConnect @0x4CE6B0 - the
// interval 0x7D0 @0x4CE7B4, the deadline 0x2710 @0x4CE7C6; CNapiGateManager_ProbeThreadProc
// @0x6339E0 - the deadline test @0x633A58, the interval test @0x633A6A, the stamp @0x633B11]
void check_gate_probe_cadence() {
	Rig rig;
	rig.lobby.open(rig.gate, rig.session);
	rig.lobby.probe("127.0.0.1", GATE_DEFAULT_PORT);
	CHECK(rig.gate.sent.size() == 1);
	const std::vector<uint8_t> probe = rig.gate.sent.empty() ? std::vector<uint8_t>{}
	                                                         : rig.gate.sent[0].second;
	const auto probes_at = [&](uint32_t at) {
		rig.lobby.tick(at);
		return rig.gate.sent.size();
	};
	CHECK(probes_at(2000) == 1);  // not more than the interval yet
	CHECK(probes_at(2001) == 2);
	CHECK(probes_at(4001) == 2);  // 2000 since the last
	CHECK(probes_at(4002) == 3);
	CHECK(probes_at(6003) == 4);
	CHECK(probes_at(8004) == 5);
	CHECK(probes_at(10000) == 5 && rig.fatals.empty());
	CHECK(probes_at(10001) == 5);
	CHECK((rig.fatals == std::vector<std::string>{ novaworld_gate_error_tag(0, false) }));
	CHECK(novaworld_gate_error_tag(0, false) == "NWEC15");
	CHECK(probes_at(12500) == 5 && rig.fatals.size() == 1);
	for (const auto &dg : rig.gate.sent)
		CHECK(dg.second == probe && dg.first.port == GATE_DEFAULT_PORT);
	rig.lobby.close();
}

// The dedicated host's gate wait: the worker's five probes and its silent end at 10 s, no reply
// read after it, then at 20 s past the probe the gate service's stop line naming the worker's
// socket and the hosting's end (NWEC15 to the hook), once. [orig:
// CNapiGateManager_WaitForGateResponse @0x4D1510 - the 0x4E20 compare @0x4D1541 (jbe @0x4D1546),
// CNapiGateManager_StopGateService (the call @0x4D154A); CNapiGateManager_StopGateService
// @0x4CEC10 - "%s : GATE STOPPING GATE SERVICE... [%s]" @0x4CECA4]
void check_command_line_host_gate_wait() {
	Rig rig(/*command_line_host=*/true);
	rig.log_on = true;
	rig.lobby.open(rig.gate, rig.session);
	rig.lobby.probe("127.0.0.1", GATE_DEFAULT_PORT);
	for (uint32_t at : { 2001u, 4002u, 6003u, 8004u, 10000u, 10001u })
		rig.lobby.tick(at);
	CHECK(rig.gate.sent.size() == 5 && rig.fatals.empty());
	rig.gate.inbound.push_back({ PeerAddr{ 0x0100007Fu, GATE_DEFAULT_PORT },
			gate_reply("VAR \"UDPNOVAWORLD\" \"127.0.0.1:64206\"\r\n") });
	rig.log.clear();
	rig.lobby.tick(15000);
	CHECK(rig.gate.inbound.size() == 1 && rig.lobby.session() == nullptr);
	CHECK((rig.log == std::vector<std::string>{ "GATE WAITING FOR REPLY... [0.0.0.0:2300]" }));
	rig.log.clear();
	rig.lobby.tick(20000);
	CHECK(rig.fatals.empty());
	rig.log.clear();
	rig.lobby.tick(20001);
	CHECK(!rig.log.empty() && rig.log.back() == "GATE STOPPING GATE SERVICE... [0.0.0.0:2300]");
	CHECK(rig.fatals == std::vector<std::string>{ "NWEC15" });
	rig.log.clear();
	rig.lobby.tick(21000);
	CHECK(rig.log.empty() && rig.fatals.size() == 1 && rig.gate.sent.size() == 5);
	rig.lobby.close();
}

// The NovaWorld menu's gate states past the reply, one a frame: the reply's frame processes it
// (its lines, the gate state 2), state 2 sets 3 the next frame, state 3 sets 4 (or shows a failed
// gate's NWEC25 and stops), state 4 connects (no datagram: InitPlayerConnection only starts the
// enumerator's timer), and the next frame's pump sends the first 0x41, the re-announce counted
// from that send. The command-line host's wait goes on at once: the connect and its 0x41 in the
// reply's own frame. A kept response's entry takes the same frames from its state-1 frame.
// [orig: CNapiGateManager_ProcessResponse @0x4CED20 - 2 @0x4CF8AE, -9 @0x4CF6AC;
//  UI_ProcessLANSessionStateMachine @0x558DE0 - state 2 @0x55906B..0x559094, state 3
//  @0x5590B4..0x5590BB (NWEC25 @0x5590FD, state 0 @0x559111), 4 set @0x559131, the connect
//  (the call @0x559156), 5 set @0x55917E, the pump @0x558DFA;
//  CNapiNPConnection_PumpEnumeratorAndSend @0x6290C0 - the stamp test @0x629108..0x629119, the
//  stamp @0x62911F;
//  CNapiGateManager_WaitForConnect @0x4CE6B0 - the kept gate state 2 @0x4CE6F3]
void check_gate_menu_frames() {
	const std::string reply = "VAR \"UDPNOVAWORLD\" \"127.0.0.1:64206\"\r\n";
	{
		Rig rig;
		rig.lobby.open(rig.gate, rig.session);
		rig.lobby.probe("127.0.0.1", GATE_DEFAULT_PORT);
		rig.gate.inbound.push_back({ PeerAddr{ 0x0100007Fu, GATE_DEFAULT_PORT }, gate_reply(reply) });
		rig.lobby.tick(100); // the reply's frame
		CHECK(rig.lobby.session() == nullptr && rig.session.sent.empty());
		rig.lobby.tick(101); // state 2 -> 3
		rig.lobby.tick(102); // state 3 -> 4
		CHECK(rig.lobby.session() == nullptr && rig.session.sent.empty());
		rig.lobby.tick(103); // state 4: the connect
		CHECK(rig.lobby.session() != nullptr && rig.session.sent.empty());
		CHECK(rig.lobby.session() != nullptr && rig.lobby.session()->lobby_state() == 1 &&
				rig.lobby.session()->lobby_state_entered_ms() == 103);
		rig.lobby.tick(104); // the pump's first announce
		CHECK(rig.count(SESSION_OPCODE_CLIENT_HELLO) == 1);
		rig.lobby.tick(104 + SESSION_CONNECT_RETRANSMIT_MS);
		CHECK(rig.count(SESSION_OPCODE_CLIENT_HELLO) == 1);
		rig.lobby.tick(104 + SESSION_CONNECT_RETRANSMIT_MS + 1);
		CHECK(rig.count(SESSION_OPCODE_CLIENT_HELLO) == 2);
		CHECK(rig.fatals.empty());
		rig.lobby.close();
	}
	{
		Rig rig;
		rig.lobby.open(rig.gate, rig.session);
		rig.lobby.probe("127.0.0.1", GATE_DEFAULT_PORT);
		rig.gate.inbound.push_back({ PeerAddr{ 0x0100007Fu, GATE_DEFAULT_PORT },
				gate_reply("GATEPROTOCOL \"1.0\"") });
		rig.lobby.tick(100);
		rig.lobby.tick(101);
		CHECK(rig.fatals.empty());
		rig.lobby.tick(102);
		CHECK(rig.fatals == std::vector<std::string>{ "NWEC25" });
		rig.lobby.tick(103);
		rig.lobby.tick(104);
		CHECK(rig.fatals.size() == 1 && rig.lobby.session() == nullptr && rig.session.sent.empty());
		rig.lobby.close();
	}
	{
		Rig rig(/*command_line_host=*/true);
		rig.lobby.open(rig.gate, rig.session);
		rig.lobby.probe("127.0.0.1", GATE_DEFAULT_PORT);
		rig.gate.inbound.push_back({ PeerAddr{ 0x0100007Fu, GATE_DEFAULT_PORT }, gate_reply(reply) });
		rig.lobby.tick(100);
		CHECK(rig.lobby.session() != nullptr && rig.count(SESSION_OPCODE_CLIENT_HELLO) == 1);
		rig.lobby.close();
	}
	{
		NwuLobbySession::GateMemory memory;
		Rig first;
		first.lobby.set_gate_memory(&memory);
		first.start(10);
		first.lobby.close();
		Rig again;
		again.lobby.set_gate_memory(&memory);
		again.lobby.open(again.gate, again.session);
		again.lobby.probe("127.0.0.1", GATE_DEFAULT_PORT); // the state-1 frame: the gate state 2
		again.lobby.tick(1);
		again.lobby.tick(2);
		CHECK(again.lobby.session() == nullptr);
		again.lobby.tick(3);
		CHECK(again.lobby.session() != nullptr && again.session.sent.empty());
		again.lobby.tick(4);
		CHECK(again.count(SESSION_OPCODE_CLIENT_HELLO) == 1);
		again.lobby.close();
	}
}

// The gate reply's lines as its reader returns them: a LF, a CR or a CR LF ends one, an empty
// line is one, the first NUL ends the text, and a line keeps 32767 characters, the next one
// passed over. [orig: Napi_ReadLineFromBuffer @0x616CC0 - @0x616D1E, @0x616D23..0x616D3B,
//  @0x616D49..0x616D55; CNapiFileReader_ReadAndTokenizeLine @0x6336E0 - @0x63371A]
void check_gate_response_lines() {
	using namespace std::string_literals;
	CHECK((gate_response_lines("a\r\nb\rc\n\nd\0e"s) ==
			std::vector<std::string>{ "a", "b", "c", "", "d" }));
	CHECK((gate_response_lines("x\n") == std::vector<std::string>{ "x" }));
	CHECK(gate_response_lines("").empty());
	CHECK((gate_response_lines("\r\n") == std::vector<std::string>{ "" }));
	const std::string long_line = std::string(32768, 'x') + "\nY";
	const std::vector<std::string> cut = gate_response_lines(long_line);
	CHECK(cut.size() == 3 && cut[0] == std::string(32767, 'x') && cut[1].empty() && cut[2] == "Y");
}

// The client 0x43s in `datagrams` decoded to their statements' names and containers, in order.
std::vector<NapiMessage> client_statements(
		const std::vector<std::vector<uint8_t>> &datagrams, const std::string &client_scrk) {
	std::vector<NapiMessage> out;
	ProtocolReassemblyState reassembly;
	for (const auto &raw : datagrams) {
		uint8_t opcode = 0;
		std::vector<uint8_t> body;
		if (!decode(raw, opcode, body) || opcode != SESSION_OPCODE_PROTOCOL_MESSAGE)
			continue;
		ProtocolPacketHeader header;
		std::vector<ProtocolMessage> records;
		if (!decode_protocol_packet_plaintext(body.data(), body.size(), client_scrk, header, records))
			continue;
		for (const ProtocolMessage &record : records) {
			std::vector<uint8_t> payload;
			if (record.flags.settings_update || record.full_tag != 0 ||
					!reassemble_protocol_payload(reassembly, record, payload) || payload.empty())
				continue;
			std::size_t consumed = 0;
			std::vector<NapiMessage> stream;
			if (napi_stream_decode(payload.data(), payload.size(), stream, &consumed) == 0)
				for (NapiMessage &m : stream) out.push_back(std::move(m));
		}
	}
	return out;
}

// Every ServerStopHosting writes its triple to _inout.txt (the embedder's /INOUT device stamps
// it), whatever the hosting. [orig: CNapiGameSession_HandleServerMessage @0x4D1C50 - the atols
// @0x4D1CB3 / @0x4D1CD2 / @0x4D1CF3, the format @0x7CC478, the append @0x4D1DBC]
void check_stop_hosting_inout_line() {
	Rig rig;
	const std::vector<uint8_t> auth = bring_to_verify(rig);
	log_in(rig, auth);
	std::vector<std::string> inout;
	std::vector<std::string> stopped;
	NwuHostRole::Hooks hooks;
	hooks.inout_line = [&inout](const std::string &text) { inout.push_back(text); };
	hooks.on_stopped = [&stopped](const std::string &key) { stopped.push_back(key); };
	NwuHostRole role(rig.lobby, hooks);
	HostRegistration reg;
	reg.server_name = "Inout";
	role.request(reg);
	rig.lobby.tick(310);
	rig.session.inbound.push_back({ kServer,
			server_statement_to(auth, "ServerHostResult", { { "Success", "1" } }, 2) });
	rig.session.inbound.push_back({ kServer, server_statement_to(auth, "ServerStopHosting",
			{ { "MsgCode", "1001" }, { "MsgParam1", "2" }, { "MsgParam2", "-3" } }, 3) });
	rig.lobby.tick(320);
	if (ClientSession *session = rig.lobby.session())
		for (const auto &notice : session->take_notices()) role.handle_notice(notice);
	CHECK((inout == std::vector<std::string>{ "NOVAWORLD STOP HOSTING.  nwuservermsgcode=1001, "
			"nwuservermsgparam1=2, nwuservermsgparam2=-3" }));
	CHECK(stopped.size() == 1);
	rig.lobby.close();
}

// D-NET-571: the player's ESC during the first connect's poll. The connect poll (session state 1)
// reads it past its timeout test: its line, the connect's give-up, then the validation wait's
// escape leg (NWEC41); the verify poll (states 2 and 3) the same with its own lines (NWEC47), the
// connection, up by then, torn down with its burst. No poll, no escape.
// [orig: CNapiGameSession_PollConnectPhase1 @0x4D4C30 - "%s : NWU USER ESCAPED1" @0x4D4D22, the
//  give-up @0x4D4D2C, dword_B5F44C @0x4D4D31; CGameSession_WaitForNWUValidation @0x4D0F20 - "%s :
//  NWU USER HIT ESCAPE!!!" @0x4D102B, -16 @0x4D1041; CNapiGameSession_PollConnectPhase2 @0x4D4D70 -
//  @0x4D4E39, "%s : NWU USER ESCAPED2" @0x4D4E73, @0x4D4E7D; CGameSession_WaitForNWUConnection
//  @0x4D1230 - "%s : NWU USER HIT ESCAPE2!!!" @0x4D12F1, -22 @0x4D1324; UI_ShowNetworkErrorByCode
//  @0x5584A0 - NWEC(25 - code)]
// The session login's end, one menu state a frame: the verify poll ends in the frame that takes
// the accepting ServerVerifyResult, the connection wait's YIPPEE line is the next frame's, and the
// validated hook (the web lobby's open) the frame after, once. The command-line host's blocking
// sequence runs each wait at once after its poll's loop and opens no web lobby. The menu's two web
// opens' lines. [orig: UI_ProcessLANSessionStateMachine @0x558DE0 - 8 set @0x559210, the
//  connection wait @0x559235, 9 set @0x559242, "%s : HTTP Getting \"%s\"" @0x5592D8;
//  CGameSession_WaitForNWUConnection @0x4D1230 - @0x4D13D3;
//  CNapiGameSession_ConnectAndWaitForValidation @0x4D4EC0 - the waits @0x4D4EDD / @0x4D4EF3;
//  UI_HandleHostSessionStart @0x556D00 - "%s : HTTP hosting with \"%s\"" @0x556F88]
void check_connect_validated() {
	const std::string yippee = "NWU YIPPEE WE ARE CONNECTED AND VALIDATED.";
	{
		Rig rig;
		rig.log_on = true;
		const std::vector<uint8_t> auth = bring_to_verify(rig);
		CHECK(rig.validated == 0);
		rig.session.inbound.push_back({ kServer,
				server_statement_to(auth, "ServerVerifyResult", { { "Success", "1" } }, 1) });
		rig.lobby.tick(300);
		CHECK(rig.validated == 0);
		CHECK(rig.log.empty() || rig.log.back() != yippee);
		rig.lobby.tick(310);
		CHECK(rig.validated == 0);
		CHECK(!rig.log.empty() && rig.log.back() == yippee);
		rig.lobby.tick(320);
		CHECK(rig.validated == 1);
		CHECK(rig.validated_at_log == rig.log.size() && rig.validated_at_log > 0 &&
				rig.log[rig.validated_at_log - 1] == yippee);
		rig.lobby.tick(400);
		CHECK(rig.validated == 1);
		CHECK(rig.fatals.empty());
		rig.lobby.close();
	}
	{
		Rig rig(/*command_line_host=*/true);
		rig.log_on = true;
		const std::vector<uint8_t> auth = bring_to_accept(rig);
		CHECK(!rig.log.empty() && rig.log.back() == "NWU WAITING FOR VALIDATION...");
		rig.log.clear();
		rig.session.inbound.push_back({ kServer,
				server_statement_to(auth, "ServerVerifyResult", { { "Success", "1" } }, 1) });
		rig.lobby.tick(300);
		CHECK((rig.log == std::vector<std::string>{ yippee }));
		rig.lobby.tick(310);
		rig.lobby.tick(320);
		CHECK(rig.validated == 0 && rig.fatals.empty());
		rig.lobby.close();
	}
	CHECK(http_getting_line("http://gs.opennova.test/NWStart.dll?prepare=1") ==
			"HTTP Getting \"http://gs.opennova.test/NWStart.dll?prepare=1\"");
	CHECK(http_hosting_line("/NWHost.dll") == "HTTP hosting with \"/NWHost.dll\"");
}

void check_escape_legs() {
	{
		Rig rig;
		CHECK(!rig.lobby.escape()); // nothing open
		rig.log_on = true;
		rig.start(10);
		CHECK(rig.lobby.session() != nullptr && rig.lobby.session()->lobby_state() == 1);
		rig.log.clear();
		CHECK(rig.lobby.escape());
		rig.lobby.tick(20);
		CHECK((rig.log == std::vector<std::string>{ "NWU USER ESCAPED1" }));
		CHECK(rig.fatals.empty());
		rig.lobby.tick(30);
		CHECK((rig.log == std::vector<std::string>{ "NWU USER ESCAPED1", "NWU LEAVING JOIN LOOP...",
				"NWU USER HIT ESCAPE!!!" }));
		CHECK(rig.fatals == std::vector<std::string>{ "NWEC41" });
		CHECK(rig.lobby.session() == nullptr);
		CHECK(!rig.lobby.escape()); // the poll is over
		rig.lobby.close();
	}
	{
		Rig rig;
		rig.log_on = true;
		bring_to_verify(rig);
		rig.log.clear();
		const std::size_t sent = rig.session.sent.size();
		CHECK(rig.lobby.escape());
		rig.lobby.tick(300);
		CHECK((rig.log == std::vector<std::string>{ "NWU USER ESCAPED2",
				"NWU DISCONNECTING FROM NOVAWORLD 127.0.0.1:64206 [NWServer] [0,0,0,0,\"\",0,\"\"]",
				kRemoved }));
		CHECK(rig.fatals.empty());
		CHECK(rig.session.sent.size() > sent); // the 0x46 burst
		rig.lobby.tick(310);
		CHECK(!rig.log.empty() && rig.log.back() == "NWU USER HIT ESCAPE2!!!");
		CHECK(rig.fatals == std::vector<std::string>{ "NWEC47" });
		rig.lobby.close();
	}
	// A second connect on the same driver: its escape's drain writes the record's line where the
	// first's did, ahead of HIT ESCAPE2.
	{
		Rig rig;
		rig.log_on = true;
		bring_to_verify(rig);
		CHECK(rig.lobby.escape());
		rig.lobby.tick(300);
		rig.lobby.tick(310);
		rig.lobby.close();
		rig.log.clear();
		rig.fatals.clear();
		bring_to_verify(rig);
		CHECK(rig.log.size() >= 1);
		rig.log.clear();
		CHECK(rig.lobby.escape());
		rig.lobby.tick(300);
		rig.lobby.tick(310);
		CHECK((rig.log == std::vector<std::string>{ "NWU USER ESCAPED2",
				"NWU DISCONNECTING FROM NOVAWORLD 127.0.0.1:64206 [NWServer] [0,0,0,0,\"\",0,\"\"]",
				kRemoved, "NWU USER HIT ESCAPE2!!!" }));
		CHECK(rig.fatals == std::vector<std::string>{ "NWEC47" });
		rig.lobby.close();
	}
	// An ESC taken in the tick the 0x82 lands: the connect poll reads the state first and ends,
	// and the validation wait, the next frame, empties the key queue on its pass to the verify
	// poll: the key is dropped and the verify goes on. [orig: CGameSession_WaitForNWUValidation
	//  @0x4D0F20 - Input_ResetKeyQueue (the call @0x4D11F7)]
	{
		Rig rig;
		rig.log_on = true;
		rig.start(10);
		rig.session.inbound.push_back(
				{ kServer, server_hello_to(rig.last(SESSION_OPCODE_CLIENT_HELLO)) });
		rig.lobby.tick(100);
		rig.lobby.tick(101); // the 0x42 goes at the first pump past the 0x81's frame
		const std::vector<uint8_t> auth = rig.last(SESSION_OPCODE_CLIENT_AUTH);
		CHECK(!auth.empty());
		CHECK(rig.lobby.escape());
		rig.session.inbound.push_back({ kServer, server_auth_to(auth) });
		rig.lobby.tick(200);
		rig.log.clear();
		rig.lobby.tick(kVerifyStampAt);
		CHECK((rig.log == std::vector<std::string>{ "NWU LEAVING JOIN LOOP...",
				"NWU WAITING FOR VALIDATION..." }));
		rig.log.clear();
		rig.lobby.tick(300);
		CHECK(rig.log.empty());
		CHECK(rig.fatals.empty());
		CHECK(rig.lobby.session() != nullptr &&
				rig.lobby.session()->state() == ClientSession::State::Verifying);
		rig.lobby.close();
	}
	// An ESC in the frame after the 0x82's (D-NET-571, Q7-17): the menu is in state 6, whose
	// validation wait reads no key and empties the queue on its pass, so the key is dropped:
	// no line, no 0x46 burst, the verify goes on. [orig: UI_ProcessLANSessionStateMachine
	//  @0x558DE0 - 6 set @0x5591B0, the validation wait @0x5591D5;
	//  CGameSession_WaitForNWUValidation @0x4D0F20 - Input_ResetKeyQueue (the call @0x4D11F7)]
	{
		Rig rig;
		rig.log_on = true;
		bring_to_accept(rig);
		rig.log.clear();
		CHECK(rig.lobby.escape());
		rig.lobby.tick(kVerifyStampAt);
		CHECK((rig.log == std::vector<std::string>{ "NWU LEAVING JOIN LOOP...",
				"NWU WAITING FOR VALIDATION..." }));
		rig.lobby.tick(300);
		rig.lobby.tick(310);
		CHECK(rig.log.size() == 2);
		CHECK(rig.fatals.empty());
		CHECK(rig.count(SESSION_OPCODE_CLIENT_GOODBYE) == 0);
		CHECK(rig.lobby.session() != nullptr &&
				rig.lobby.session()->state() == ClientSession::State::Verifying);
		rig.lobby.close();
	}
	// An ESC in the connection wait's frame: no poll reads it (the wait reads no key), the login
	// ends as it would. [orig: CGameSession_WaitForNWUConnection @0x4D1230 - the flags @0x4D1244,
	//  bit 2 @0x4D1332, no key read]
	{
		Rig rig;
		rig.log_on = true;
		const std::vector<uint8_t> auth = bring_to_verify(rig);
		rig.session.inbound.push_back({ kServer,
				server_statement_to(auth, "ServerVerifyResult", { { "Success", "1" } }, 1) });
		rig.lobby.tick(300);
		rig.log.clear();
		CHECK(rig.lobby.escape());
		rig.lobby.tick(310);
		rig.lobby.tick(320);
		CHECK((rig.log == std::vector<std::string>{ "NWU YIPPEE WE ARE CONNECTED AND VALIDATED." }));
		CHECK(rig.validated == 1 && rig.fatals.empty());
		CHECK(rig.count(SESSION_OPCODE_CLIENT_GOODBYE) == 0);
		CHECK(!rig.lobby.escape()); // the poll is over
		rig.lobby.close();
	}
}

// ConnectOrHost's lines: the legs' checks in order (the start line; not set up; the wrong state
// with the session's state; the attempt; the host leg's gate), and the host leg's result lines from
// the host role: started and moving on, not hosting, the 60 s timeout's two around its
// ClientStopHosting; the command-line host writes none and waits with no timeout.
// [orig: CNapiGameSession_ConnectOrHost @0x4D4F10 - @0x4D4F60, @0x4D4FD2, @0x4D56E3, @0x4D53AE,
//  @0x4D5038, @0x4D50C9, @0x4D51DF, @0x4D532D, @0x4D536D, @0x4D5245, @0x4D528E;
//  Game_HostMultiplayerSession @0x4A65A0 - @0x4A6725..0x4A6735]
void check_connect_or_host_lines() {
	using L = std::vector<std::string>;
	CHECK((connect_or_host_check_lines(true, NWEC_SESSION_NOT_SET_UP, 0) ==
			L{ "NWU START HOSTING OR PLAYING.", "NWU NOT SET UP!!!" }));
	CHECK((connect_or_host_check_lines(false, NWEC_WRONG_SESSION_STATE, 2) ==
			L{ "NWU START HOSTING OR PLAYING.", "NWU NOT CORRECT STATE[2]!!!" }));
	CHECK((connect_or_host_check_lines(true, NWEC_HOST_GATE_NOT_OK, 4) ==
			L{ "NWU START HOSTING OR PLAYING.", "NWU ATTEMPTING TO START HOSTING",
					"NWU HOSTING FAILED...GATEINFO NOT OK!!!" }));
	CHECK((connect_or_host_check_lines(true, nullptr, 4) ==
			L{ "NWU START HOSTING OR PLAYING.", "NWU ATTEMPTING TO START HOSTING" }));
	CHECK((connect_or_host_check_lines(false, nullptr, 4) ==
			L{ "NWU START HOSTING OR PLAYING.", "NWU ATTEMPTING TO START PLAYING" }));
	CHECK(play_leg_refusal(false, 0xFFu) == std::string(NWEC_SESSION_NOT_SET_UP));
	CHECK(play_leg_refusal(true, 0x2u) == std::string(NWEC_WRONG_SESSION_STATE));
	CHECK(play_leg_refusal(true, 0xAu) == nullptr);
	CHECK(play_not_playing_line(4) == "NWU STARTPLAYING failed.  Not in playing state[4]!!!");
	// The play poll's exit tag: the leg's own (the preset NWEC01, the timeout's NWEC02, the
	// escape's NWEC03) while the MsgCode word is 0, else the word's map, which overwrites it
	// [orig: CNapiGameSession_ConnectOrHost @0x4D4F10 - @0x4D55F7..0x4D569F].
	CHECK(play_leg_exit_tag(NWEC_SESSION_NOT_SET_UP, 0) == "NWEC01");
	CHECK(play_leg_exit_tag(NWEC_PLAY_TIMEOUT, 0) == "NWEC02");
	CHECK(play_leg_exit_tag(NWEC_SESSION_NOT_SET_UP, 3001) == "NWEC05");
	CHECK(play_leg_exit_tag(NWEC_PLAY_CANCELLED, 7) == "NWEC13");

	const auto hosting = [](bool command_line_host, const char *success, uint32_t result_at) {
		Rig rig(command_line_host);
		rig.log_on = true;
		const std::vector<uint8_t> auth = bring_to_verify(rig);
		log_in(rig, auth);
		std::vector<std::string> failed;
		NwuHostRole::Hooks hooks;
		hooks.on_failed = [&failed](const std::string &tag) { failed.push_back(tag); };
		NwuHostRole role(rig.lobby, hooks);
		HostRegistration reg;
		reg.server_name = "Lines";
		role.request(reg);
		rig.lobby.tick(310);
		role.tick(310);
		rig.log.clear();
		if (success != nullptr)
			rig.session.inbound.push_back({ kServer,
					server_statement_to(auth, "ServerHostResult", { { "Success", success } }, 2) });
		rig.lobby.tick(result_at);
		if (ClientSession *session = rig.lobby.session())
			for (const auto &notice : session->take_notices()) role.handle_notice(notice);
		role.tick(result_at);
		const std::vector<std::string> lines = rig.log;
		rig.lobby.close();
		return std::make_pair(lines, failed);
	};
	const auto started = hosting(false, "1", 320);
	CHECK((started.first == L{ "NWU HOSTING STARTED!  YIPPEE", "NWU MOVING ON TO HOSTING OR PLAYING..." }));
	const auto refused = hosting(false, "0", 320);
	CHECK((refused.first == L{ "NWU HOSTING FAILED...WE'RE NOT IN HOSTING STATE!!!" }));
	const auto timed_out = hosting(false, nullptr, 310 + SESSION_CONNECT_TIMEOUT_MS + 1);
	CHECK((timed_out.first == L{ "NWU HOSTING FAILED...TIMEOUT WAITING FOR REPLY FROM NOVAWORLD!!!",
			"NWU HOSTING TIMEOUT!!!" }));
	CHECK((timed_out.second == L{ NWEC_HOST_TIMEOUT }));
	const auto served = hosting(true, "1", 320);
	CHECK(served.first.empty());
	const auto waits = hosting(true, nullptr, 310 + SESSION_CONNECT_TIMEOUT_MS + 1);
	CHECK(waits.first.empty() && waits.second.empty());

	// The host poll ends the moment the session leaves state 5 without its success: a punt, a
	// ServerStopHosting or a lost session during the request writes the not-hosting line last and
	// fails with the session's MsgCode word through the host switch (a punt's 1001 -> NWEC53, a
	// stop's 1009 -> NWEC57, no session -> NWEC58); the role is idle again, so the wait no longer
	// holds the menus' admin pump nor the panel's Close; the command-line host writes no line.
	// [orig: CNapiGameSession_ConnectOrHost @0x4D4F10 - the state test @0x4D5195..0x4D519D,
	//  @0x4D51DF, the +0x130 switch @0x4D51F3..0x4D5207; HandlePuntNotification @0x4D20B0 - the
	//  word @0x4D2177, SetState(0) @0x4D21FA; HandleServerMessage @0x4D1C50 - @0x4D1D17,
	//  SetState(4) @0x4D1D29; Game_HostMultiplayerSession @0x4A671D..0x4A674F]
	struct Exit {
		std::vector<std::string> log;
		std::vector<std::string> failed;
		int stopped = 0;
		NwuHostRole::Phase phase = NwuHostRole::Phase::Requested;
	};
	const auto host_exit = [](bool command_line_host, const char *statement, const char *msg_code) {
		Rig rig(command_line_host);
		rig.log_on = true;
		const std::vector<uint8_t> auth = bring_to_verify(rig);
		log_in(rig, auth);
		Exit out;
		NwuHostRole::Hooks hooks;
		hooks.on_failed = [&out](const std::string &tag) { out.failed.push_back(tag); };
		hooks.on_stopped = [&out](const std::string &) { ++out.stopped; };
		NwuHostRole role(rig.lobby, hooks);
		HostRegistration reg;
		reg.server_name = "Exit";
		role.request(reg);
		rig.lobby.tick(310);
		role.tick(310);
		CHECK(role.phase() == NwuHostRole::Phase::Requested);
		rig.log.clear();
		if (statement != nullptr) {
			rig.session.inbound.push_back({ kServer,
					server_statement_to(auth, statement, { { "MsgCode", msg_code } }, 2) });
			rig.lobby.tick(320);
			if (ClientSession *session = rig.lobby.session())
				for (const auto &notice : session->take_notices()) role.handle_notice(notice);
		} else {
			rig.lobby.close();
		}
		role.tick(320);
		role.tick(330); // the wait ended once
		out.log = rig.log;
		out.phase = role.phase();
		rig.lobby.close();
		return out;
	};
	const auto punted = host_exit(false, "ServerLeaveNovaWorld", "1001");
	CHECK(!punted.log.empty() &&
			punted.log.back() == "NWU HOSTING FAILED...WE'RE NOT IN HOSTING STATE!!!");
	CHECK(std::count(punted.log.begin(), punted.log.end(),
				  std::string("NWU HOSTING FAILED...WE'RE NOT IN HOSTING STATE!!!")) == 1);
	CHECK((punted.failed == L{ "NWEC53" }));
	CHECK(punted.phase == NwuHostRole::Phase::Idle);
	const auto stopped = host_exit(false, "ServerStopHosting", "1009");
	CHECK((stopped.log == L{ "NWU HOSTING FAILED...WE'RE NOT IN HOSTING STATE!!!" }));
	CHECK((stopped.failed == L{ "NWEC57" }) && stopped.stopped == 0);
	CHECK(stopped.phase == NwuHostRole::Phase::Idle);
	const auto lost = host_exit(false, nullptr, nullptr);
	CHECK(!lost.log.empty() &&
			lost.log.back() == "NWU HOSTING FAILED...WE'RE NOT IN HOSTING STATE!!!");
	CHECK((lost.failed == L{ "NWEC58" }));
	const auto served_punt = host_exit(true, "ServerLeaveNovaWorld", "1001");
	CHECK(std::count(served_punt.log.begin(), served_punt.log.end(),
				  std::string("NWU HOSTING FAILED...WE'RE NOT IN HOSTING STATE!!!")) == 0);
	CHECK(served_punt.failed.size() == 1 && served_punt.phase == NwuHostRole::Phase::Idle);
}

// D-NET-490: a player's add on a hosting session sends ClientHostPlayerAdded and then, at once,
// the lists' update with 0: a ClientHostUpdate carrying the changed PlayerList (the new slot's
// vars); a removal sends ClientHostPlayerRemoved alone, its PlayerList change riding the next
// update. [orig: Server_PlayerAdd @0x51CBC0 - @0x51D4B5, the +5 test @0x51D4BA,
//  CPlayerManager_RebuildLists(0) @0x51D4C6; CPlayerManager_RebuildLists @0x4D4590 - the state-6
//  test @0x4D4593, CNapiGameSession_SendHostUpdate @0x4D45B5; Server_HandlePlayerDisconnect
//  @0x51B5C0 - the removal @0x51B5F3 / @0x51B601, no update]
void check_host_player_add_sends_update() {
	Rig rig;
	const std::vector<uint8_t> auth = bring_to_verify(rig);
	CHECK(!auth.empty());
	if (auth.empty())
		return;
	log_in(rig, auth);
	NwuHostRole role(rig.lobby, NwuHostRole::Hooks{});
	HostRegistration reg;
	reg.server_name = "Add Srv";
	CHECK(role.request(reg));
	rig.lobby.tick(310);
	rig.session.inbound.push_back(
			{ kServer, server_statement_to(auth, "ServerHostResult", { { "Success", "1" } }, 2) });
	rig.lobby.tick(320);
	ClientSession *session = rig.lobby.session();
	CHECK(session != nullptr);
	if (session == nullptr)
		return;
	for (const auto &notice : session->take_notices())
		role.handle_notice(notice);
	CHECK(session->host_state() == ClientSession::HostState::Established);
	// The request's marks stand: a first update (the add's) writes both lists; send it out of the
	// way so the add's own delta is what the next one carries.
	session->send_host_update(/*include_all=*/false);
	rig.lobby.tick(330);

	std::size_t before = rig.session.sent.size();
	HostPlayerSlot joiner;
	joiner.slot = 1;
	joiner.player_name = "Joiner";
	joiner.ip_and_port = "10.0.0.9:32768";
	joiner.team = "1";
	joiner.type = "0";
	role.set_player_slot(joiner);
	rig.lobby.tick(340);
	std::vector<std::vector<uint8_t>> sent;
	for (std::size_t i = before; i < rig.session.sent.size(); ++i)
		sent.push_back(rig.session.sent[i].second);
	std::vector<NapiMessage> statements = client_statements(sent, session->client_scrk());
	CHECK(statements.size() == 2);
	if (statements.size() == 2) {
		CHECK(statements[0].name == "ClientHostPlayerAdded");
		CHECK(statements[1].name == "ClientHostUpdate");
		std::size_t player_vars = 0;
		for (const NapiMessage &list : statements[1].children) {
			std::string name;
			for (const NapiField &f : list.fields)
				if (f.name == "VarList") name.assign(f.data.begin(), f.data.end());
			while (!name.empty() && name.back() == '\0')
				name.pop_back();
			if (name == "PlayerList")
				player_vars = list.children.size();
		}
		CHECK(player_vars == 5);
	}

	before = rig.session.sent.size();
	role.clear_player_slot(1);
	rig.lobby.tick(350);
	sent.clear();
	for (std::size_t i = before; i < rig.session.sent.size(); ++i)
		sent.push_back(rig.session.sent[i].second);
	statements = client_statements(sent, session->client_scrk());
	CHECK(statements.size() == 1 && !statements.empty() &&
			statements[0].name == "ClientHostPlayerRemoved");

	// D-NET-994: the teardown's legs read no roster, so a slot this hosting never announced
	// (a held server's player at the next host's session reset) still sends its statement, the
	// PlayerNumber the slot's index. [orig: Server_HandlePlayerDisconnect @0x51B5C0 - the call
	//  @0x51B5F3; CNapiGameSession_SendPlayerRemovedIfHosting @0x4D0E40 - the state test
	//  @0x4D0E48; CNapiGameSession_SendPlayerRemoved @0x4D01A0 - the slot's +0x14 @0x4D0205]
	before = rig.session.sent.size();
	role.clear_player_slot(7);
	rig.lobby.tick(355);
	sent.clear();
	for (std::size_t i = before; i < rig.session.sent.size(); ++i)
		sent.push_back(rig.session.sent[i].second);
	statements = client_statements(sent, session->client_scrk());
	CHECK(statements.size() == 1 && !statements.empty() &&
			statements[0].name == "ClientHostPlayerRemoved");
	if (statements.size() == 1) {
		std::string number;
		for (const NapiField &f : statements[0].fields)
			if (f.name == "PlayerNumber")
				number.assign(f.data.begin(), f.data.end());
		while (!number.empty() && number.back() == '\0')
			number.pop_back();
		CHECK(number == "7");
	}

	// D-NET-659: the local player's slot keeps the round init's +5, so a listen host's own add
	// sends its statement alone, the PlayerList change riding the next update. [orig:
	// Server_InitNewRoundState @0x51C8E0 - +5 = 1 @0x51CA35; Server_PlayerAdd @0x51CBC0 - the
	// local path @0x51CC33..0x51CC60, the test @0x51D4BA]
	before = rig.session.sent.size();
	HostPlayerSlot own;
	own.slot = 0;
	own.player_name = "Host";
	own.ip_and_port = "0.0.0.0:0";
	own.local = true;
	role.set_player_slot(own);
	rig.lobby.tick(360);
	sent.clear();
	for (std::size_t i = before; i < rig.session.sent.size(); ++i)
		sent.push_back(rig.session.sent[i].second);
	statements = client_statements(sent, session->client_scrk());
	CHECK(statements.size() == 1 && !statements.empty() &&
			statements[0].name == "ClientHostPlayerAdded");
	rig.lobby.close();
}

// D-NET-995: the session's first mission sends the gate its METPROTOCOL ID report, after
// the mission start's list update, to the reply's METIPADDRESS:METIPPORT over the session socket;
// a later mission sends none, and a reply missing METLABEL resolves no gate address, so nothing
// goes. [orig: CNapiGateManager_ProcessResponse @0x4CED20 - @0x4CF6B8..0x4CF6E2; Game_StartMission
//  @0x524908..0x524928; Server_SendMetricsToGate @0x4FB0A0]
void check_mission_start_id_report() {
	const PeerAddr metrics_gate{ 0x0100007Fu, 27999 };
	const std::string met = "VAR \"LOBBYNAME\" \"jop_2_consumer\"\r\n"
			"VAR \"METIPADDRESS\" \"127.0.0.1\"\r\nVAR \"METIPPORT\" \"27999\"\r\n";
	for (const bool labelled : { true, false }) {
		Rig rig;
		rig.gate_body = "VAR \"UDPNOVAWORLD\" \"127.0.0.1:64206\"\r\n" + met +
				(labelled ? "VAR \"METLABEL\" \"gate-label\"\r\nVAR \"METEXT\" \"1\"\r\n" : "");
		const std::vector<uint8_t> auth = bring_to_verify(rig);
		CHECK(!auth.empty());
		if (auth.empty())
			return;
		CHECK(rig.lobby.gate_address() == (labelled ? 0x0100007Fu : 0u));
		log_in(rig, auth);
		NwuHostRole role(rig.lobby, NwuHostRole::Hooks{});
		HostRegistration reg;
		reg.server_name = "Metrics Srv";
		reg.gcc = "us";
		reg.country_name = "United States";
		reg.language = "English";
		reg.tz_bias = 300;
		CHECK(role.request(reg));
		rig.lobby.tick(310);
		rig.session.inbound.push_back({ kServer,
				server_statement_to(auth, "ServerHostResult", { { "Success", "1" } }, 2) });
		rig.lobby.tick(320);
		if (ClientSession *session = rig.lobby.session())
			for (const auto &notice : session->take_notices())
				role.handle_notice(notice);
		auto gate_reports = [&rig, metrics_gate](std::size_t from) {
			std::vector<std::vector<uint8_t>> out;
			for (std::size_t i = from; i < rig.session.sent.size(); ++i)
				if (rig.session.sent[i].first == metrics_gate)
					out.push_back(rig.session.sent[i].second);
			return out;
		};
		std::size_t before = rig.session.sent.size();
		role.publish_mission_start(/*session_round_count=*/1);
		CHECK(gate_reports(before).empty());
		before = rig.session.sent.size();
		role.publish_mission_start(/*session_round_count=*/0);
		const std::vector<std::vector<uint8_t>> reports = gate_reports(before);
		if (!labelled) {
			CHECK(reports.empty());
			rig.lobby.close();
			continue;
		}
		CHECK(reports.size() == 1);
		if (reports.size() != 1)
			return;
		// The ID report is the call's last send.
		CHECK(rig.session.sent.back().first == metrics_gate);
		std::vector<uint8_t> inner(reports[0].size());
		std::size_t inner_size = 0;
		CHECK(napi_envelope_decode(reports[0].data(), reports[0].size(), inner.data(), inner.size(),
				&inner_size) == 0);
		inner.resize(inner_size);
		std::string plain(inner.begin(), inner.end());
		nwu_encrypt(reinterpret_cast<uint8_t *>(plain.data()), plain.size(), "1010101");
		// GV is the session title's own version text (JO's "V1.7.5.7"), never a literal of ours.
		// [orig: Game_ParseCommandLineAndInit - g_VersionText @0x4A7D90]
		const std::string version = kJo.version_text;
		CHECK(version == "V1.7.5.7");
		CHECK(plain == "METPROTOCOL 1\r\nBLOCK \"ID\"\r\n\tLABEL \"gate-label\"\r\n\tGCC \"us\"\r\n"
				"\tGV \"" + version + "\"\r\n\tCOUNTRYNAME \"United States\"\r\n"
				"\tLANG \"English\"\r\n\tTZB 300\r\nENDBLOCK\r\n");
		rig.lobby.close();
	}
}

// The first connect's sends ride the frame's head pump: after the receive and ahead of the
// periodic update's give-up tests and the menu state's work. So a 0x41 re-announce that falls due
// in an ESC's frame goes before the ESC stops the probe, and a 0x42 that falls due in the
// validation wait's frame after a state-1 give-up goes before the reset (the give-up leaves the
// connect state standing: RequestDisconnect returns -1 for it).
// [orig: UI_ProcessLANSessionStateMachine @0x558DE0 - the pump @0x558DFA ahead of the state's
//  case; CNapiGameSession_ProcessPeriodicUpdate @0x4D4400 - NapiNPManager_Pump @0x4D441F,
//  NapiNPProtocol_Pump @0x4D442E, the give-up tests @0x4D4436..0x4D44B1;
//  CNapiNPConnection_PumpFlags @0x629780 - PumpEnumeratorAndSend @0x6297B7, PumpStateMachine
//  @0x6297C3; CNapiGameSession_PollConnectPhase1 @0x4D4C30 - the ESC @0x4D4CDE;
//  CNapiNPConnection_RequestDisconnect @0x61E0F0 - -1 @0x61E110;
//  CGameSession_WaitForNWUValidation @0x4D0F20 - -17 @0x4D111C]
void check_head_pump_resends() {
	{
		Rig rig;
		rig.start(10); // the first 0x41 at 11
		CHECK(rig.count(SESSION_OPCODE_CLIENT_HELLO) == 1);
		CHECK(rig.lobby.escape());
		rig.lobby.tick(11 + SESSION_CONNECT_RETRANSMIT_MS + 1); // the re-announce is due here
		CHECK(rig.count(SESSION_OPCODE_CLIENT_HELLO) == 2);
		CHECK(rig.fatals.empty());
		rig.lobby.tick(11 + SESSION_CONNECT_RETRANSMIT_MS + 2);
		CHECK(rig.fatals == std::vector<std::string>{ "NWEC41" });
		CHECK(rig.count(SESSION_OPCODE_CLIENT_HELLO) == 2);
		rig.lobby.close();
	}
	{
		Rig rig;
		rig.start(10);
		rig.session.inbound.push_back(
				{ kServer, server_hello_to(rig.last(SESSION_OPCODE_CLIENT_HELLO)) });
		// The 0x81 late enough that the connect state's own 20 s join timeout stays clear of the
		// validation frame.
		rig.lobby.tick(5000);
		rig.lobby.tick(5001); // the first 0x42
		CHECK(rig.count(SESSION_OPCODE_CLIENT_AUTH) == 1);
		const uint32_t give_up_at = 10 + SESSION_PERIODIC_UPDATE_TIMEOUT_MS + 1;
		rig.lobby.tick(give_up_at); // a re-send due, then the state-1 give-up
		CHECK(rig.count(SESSION_OPCODE_CLIENT_AUTH) == 2);
		CHECK(rig.fatals.empty());
		const uint32_t validation_at = give_up_at + SESSION_CONNECT_RETRANSMIT_MS + 1;
		rig.lobby.tick(validation_at); // a re-send due, then the validation wait's NWEC42
		CHECK(rig.count(SESSION_OPCODE_CLIENT_AUTH) == 3);
		CHECK(rig.fatals == std::vector<std::string>{ "NWEC42" });
		CHECK(rig.state_at_fatal == ClientSession::State::Auth);
		rig.lobby.close();
	}
}

// D-NET-992: the connection's 0x42 is its state machine's alone. The 0x81 enters the connect
// state, stamping its entry and the last 0x42 one retransmit interval back, and sends nothing; the
// 0x42 goes at the first pump whose clock has passed the 0x81's, then each time more than the
// interval passed since the last 0x42 (not since the last 0x41).
// [orig: CNapiNPConnection_InitFromSession @0x626320 - +0x5E0 = now @0x62648A, +0x5E4 = now -
//  +0x5B0 @0x626490..0x626499; CNapiNPConnection_PumpStateMachine @0x6292E0 case 3 - the test
//  @0x629580..0x629588, the stamp @0x62958F, CNapiNPConnection_SendClientJoin @0x61FE20 (its one
//  caller @0x629595); CNapiGameSession_InitPlayerConnection @0x4D4160 - +0x5B0 = 3000 @0x4D4318]
void check_first_join_timing() {
	Rig rig;
	rig.start(10); // the first 0x41 at 11
	const std::vector<uint8_t> hello = rig.last(SESSION_OPCODE_CLIENT_HELLO);
	rig.session.inbound.push_back({ kServer, server_hello_to(hello) });
	rig.lobby.tick(2000);
	CHECK(rig.lobby.session() != nullptr &&
			rig.lobby.session()->state() == ClientSession::State::Auth);
	CHECK(rig.count(SESSION_OPCODE_CLIENT_AUTH) == 0);
	rig.lobby.tick(2000); // a frame whose clock has not passed the 0x81's
	CHECK(rig.count(SESSION_OPCODE_CLIENT_AUTH) == 0);
	rig.lobby.tick(2001);
	CHECK(rig.count(SESSION_OPCODE_CLIENT_AUTH) == 1);
	// The re-send counts from that 0x42: not at the 0x41's interval (11 + 3001), only past 2001 + 3000.
	rig.lobby.tick(11 + SESSION_CONNECT_RETRANSMIT_MS + 1);
	CHECK(rig.count(SESSION_OPCODE_CLIENT_AUTH) == 1);
	rig.lobby.tick(2001 + SESSION_CONNECT_RETRANSMIT_MS);
	CHECK(rig.count(SESSION_OPCODE_CLIENT_AUTH) == 1);
	rig.lobby.tick(2001 + SESSION_CONNECT_RETRANSMIT_MS + 1);
	CHECK(rig.count(SESSION_OPCODE_CLIENT_AUTH) == 2);
	CHECK(rig.count(SESSION_OPCODE_CLIENT_HELLO) == 1); // no 0x41 once the 0x81 is taken
	rig.lobby.close();
}

// The lobby 0x42 names the connection's peer, the taken 0x81's sender, as SIP (the address in
// network order) and SPN (the port as an LE dword), right after NA, in every 0x42 of the connect
// (LN S1d / S2: retail's lobby 0x42 carries SIP / SPN = its 0x81's sender; ours carried neither).
// [orig: Nwu_HandleServerHello @0x626D20 - the record's address / port @0x62769D / @0x6276A1;
//  CNapiNPConnection_InitFromSession @0x626320 - conn+0x30 / +0x34 @0x6263B4 / @0x6263B7;
//  CNapiNPConnection_SendClientJoin @0x61FE20 - SIP @0x6201AE..0x6201C5, SPN
//  @0x6201D2..0x6201E9]
void check_join_names_its_peer() {
	Rig rig;
	rig.start(10);
	const std::vector<uint8_t> hello = rig.last(SESSION_OPCODE_CLIENT_HELLO);
	rig.session.inbound.push_back({ kServer, server_hello_to(hello) });
	rig.lobby.tick(2000);
	rig.lobby.tick(2001);
	rig.lobby.tick(2001 + SESSION_CONNECT_RETRANSMIT_MS + 1);
	CHECK(rig.count(SESSION_OPCODE_CLIENT_AUTH) == 2);
	for (const auto &dg : rig.session.sent) {
		uint8_t op = 0;
		std::vector<uint8_t> body;
		if (!decode(dg.second, op, body) || op != SESSION_OPCODE_CLIENT_AUTH)
			continue;
		ClientAuth auth;
		CHECK(parse_client_auth(body.data(), body.size(), auth));
		CHECK(auth.sip == kServer.ip && auth.spn == kServer.port);
		// NA's value, then SIP 7F 00 00 01 and SPN CE FA 00 00 (64206), each a four-byte field.
		const std::vector<uint8_t> na_sip_spn = [&] {
			std::vector<uint8_t> v{ 'N', 'A', 0 };
			v.push_back(static_cast<uint8_t>(auth.na.size() + 1));
			v.push_back(0);
			v.insert(v.end(), auth.na.begin(), auth.na.end());
			static const uint8_t kSipSpn[] = { 0, 'S', 'I', 'P', 0, 4, 0, 0x7F, 0, 0, 1,
					'S', 'P', 'N', 0, 4, 0, 0xCE, 0xFA, 0, 0 };
			v.insert(v.end(), std::begin(kSipSpn), std::end(kSipSpn));
			return v;
		}();
		CHECK(!auth.na.empty() &&
				std::search(body.begin(), body.end(), na_sip_spn.begin(), na_sip_spn.end()) !=
						body.end());
	}
	rig.lobby.close();
}

int main() {
	check_join_names_its_peer();
	check_head_pump_resends();
	check_first_join_timing();
	check_host_player_add_sends_update();
	check_mission_start_id_report();
	check_connect_or_host_lines();
	check_escape_legs();
	check_connect_validated();
	check_connect_log_packet_errors();
	check_stop_hosting_inout_line();
	check_connect_error_tags();
	check_connect_log_gate_and_connect();
	check_connect_log_give_ups();
	check_connect_log_packet_lines();
	check_connect_log_gate_failures();
	check_gate_probe_cadence();
	check_gate_menu_frames();
	check_command_line_host_gate_wait();
	check_gate_response_lines();
	check_short_protocol_packets_keep_the_connection();
	check_disconnect_request_by_state();
	check_teardown_line_order();
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
