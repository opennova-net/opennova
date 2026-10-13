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
#include <net/napi/session.h>
#include <net/novacrypto/nwu.h>
#include <net/novaworld/gate_probe.h>
#include <net/novaworld/nwu_lobby_session.h>
#include <net/npwire/nw_session_framing.h>
#include <net/npwire/session_hello.h>
#include <net/npwire/session_keys.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>
#include <utility>
#include <vector>

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
	NwuLobbySession lobby;

	Rig() : lobby(hooks(), NwuLobbySession::Environment{}) {}

	NwuLobbySession::Hooks hooks() {
		NwuLobbySession::Hooks out;
		out.on_fatal = [this](const std::string &message) { fatals.push_back(message); };
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
	CHECK(session.lobby_state() == 0 && session.session_flags() == 0);
	CHECK(rig.count(SESSION_OPCODE_CLIENT_HELLO) == hellos);

	const std::vector<uint8_t> reply = server_hello_to(first_hello);
	CHECK(!reply.empty());
	CHECK(!session.takes_server_hello(reply.data(), reply.size()));
	CHECK(!rig.lobby.claims(kServer, reply.data(), reply.size()));
	rig.session.inbound.push_back({ kServer, reply });
	rig.lobby.tick(10 + SESSION_PERIODIC_UPDATE_TIMEOUT_MS + 2 + SESSION_CONNECT_RETRANSMIT_MS);
	CHECK(session.state() == ClientSession::State::Hello);
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
	CHECK(session.state() == ClientSession::State::Closed);
	CHECK(session.lobby_state() == 0);
	for (const auto &dg : rig.session.sent) {
		uint8_t op = 0;
		std::vector<uint8_t> body;
		if (decode(dg.second, op, body) && op == SESSION_OPCODE_CLIENT_GOODBYE)
			CHECK(dg.first == kServer);
	}
	rig.lobby.close();
}

} // namespace

int main() {
	check_connect_error_tags();
	check_state_one_give_up();
	check_verify_give_up();
	if (failures != 0) {
		std::printf("nwu_lobby_connect: %d failure(s)\n", failures);
		return 1;
	}
	std::printf("nwu_lobby_connect: ok\n");
	return 0;
}
