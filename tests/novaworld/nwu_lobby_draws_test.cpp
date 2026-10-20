// D-NET-704: the NovaWorld lobby connection's identity. Each connect makes a fresh lobby protocol
// and one connection on it, so the CI its 0x41s and its 0x42 carry is that protocol's first
// connection index, 1 at every connect; its CK and SCRK are drawn when a 0x81 is taken, the CK
// first, from the process's one NAPI manager LCG, the stream the game protocol's keys draw from.
// The checks: CI 1 on two connects; no key before the 0x81; the 0x42's CK a manager key (its
// bytes chain as b' = 49b + 1) and its SCRK the 63 draws that follow it; the manager's next draw
// continuing the same stream; an injected source standing in. Socket-free: in-memory sockets,
// the service's 0x81 built with the engine's own writer.
// [orig: CNapiGameSession_InitNPConnection @0x4D3BE0 - NapiNPProtocol_Create @0x4D3C89 (the
//  counter +0xECC zeroed @0x625A81), CNapiNPConnection_Create @0x4D3F70 (@0x62B282..0x62B296);
//  Nwu_HandleServerHello @0x626D20 - InitFromSession @0x627EE5; CNapiNPConnection_InitFromSession
//  @0x626320 - NapiNP_GenerateSessionKey @0x6263F4 (the CK, stored @0x6263FB),
//  CNapiNPConnection_GenerateTxKey @0x626401;
//  NapiNPManager_Create @0x62C5E0, its one caller CNapiNetwork_Init @0x4CA638]

#include <net/napi/envelope.h>
#include <net/novacrypto/nwu.h>
#include <net/novaworld/gate_probe.h>
#include <net/novaworld/nwu_lobby_session.h>
#include <net/npwire/nw_session_framing.h>
#include <net/npwire/session_hello.h>
#include <net/npwire/session_keys.h>
#include <net/title/title_identity.h>
#include <base/gameprofile/gameprofile.h>

#include <algorithm>
#include <cstdint>
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

// The gate's reply naming the one UDPNOVAWORLD, as the gate socket receives it.
std::vector<uint8_t> gate_reply() {
	const std::string body = "VAR \"UDPNOVAWORLD\" \"127.0.0.1:64206\"\r\n";
	std::vector<uint8_t> inner(body.begin(), body.end());
	nwu_decrypt(inner.data(), inner.size(), GATE_NWU_KEY);
	std::vector<uint8_t> out(inner.size() + 16);
	std::size_t n = 0;
	napi_envelope_encode(inner.data(), inner.size(), out.data(), out.size(), &n);
	out.resize(n);
	return out;
}

const PeerAddr kServer{ 0x0100007Fu, 64206 };

// The last session datagram of `opcode`, decoded (false when none).
bool last_sent(const MemorySocket &socket, uint8_t opcode, std::vector<uint8_t> &body) {
	for (auto it = socket.sent.rbegin(); it != socket.sent.rend(); ++it) {
		uint8_t op = 0;
		if (nw_decode_inbound(it->second.data(), it->second.size(), op, body) && op == opcode)
			return true;
	}
	return false;
}

// One connect up to its 0x41: the gate names kServer and the session starts on the tick.
void connect(NwuLobbySession &lobby, MemorySocket &gate, MemorySocket &session) {
	lobby.open(gate, session);
	lobby.probe("127.0.0.1", GATE_DEFAULT_PORT);
	gate.inbound.push_back({ PeerAddr{ 0x0100007Fu, GATE_DEFAULT_PORT }, gate_reply() });
	lobby.tick(10);
}

// The service's 0x81 to the session's last 0x41, taken on the next tick; the 0x42 it answers.
bool answer_hello(NwuLobbySession &lobby, MemorySocket &session, ClientAuth &auth) {
	std::vector<uint8_t> body;
	ClientHello hello;
	if (!last_sent(session, SESSION_OPCODE_CLIENT_HELLO, body) ||
			!parse_client_hello(body.data(), body.size(), hello))
		return false;
	session.inbound.push_back({ kServer, nw_encode_outbound(SESSION_OPCODE_SERVER_HELLO,
			server_hello_to_bytes(build_server_hello(hello, 0x7F000001u, 40000))) });
	lobby.tick(20);
	return last_sent(session, SESSION_OPCODE_CLIENT_AUTH, body) &&
			parse_client_auth(body.data(), body.size(), auth);
}

uint32_t hello_ci(const MemorySocket &session) {
	std::vector<uint8_t> body;
	ClientHello hello;
	if (!last_sent(session, SESSION_OPCODE_CLIENT_HELLO, body) ||
			!parse_client_hello(body.data(), body.size(), hello))
		return 0;
	return hello.ci;
}

// A manager key's bytes each follow the one before as b' = 49b + 1 (mod 256).
bool bytes_chain(uint32_t key) {
	for (int shift = 24; shift > 0; shift -= 8) {
		const uint32_t b = (key >> shift) & 0xFFu;
		if (((49u * b + 1u) & 0xFFu) != ((key >> (shift - 8)) & 0xFFu))
			return false;
	}
	return true;
}

// The SCRK the mod-32 walk gives after a draw whose state was `last` mod 32: 63 steps of
// x' = 17x + 1 over "0123456789BCDFGHJKLMNPQRSTVWXYZ", slot 31 appending nothing.
std::string scrk_after(uint32_t last) {
	static const char charset[] = "0123456789BCDFGHJKLMNPQRSTVWXYZ";
	std::string out;
	uint32_t x = last & 31u;
	for (int i = 0; i < 63; ++i) {
		x = (17u * x + 1u) & 31u;
		if (x != 31u)
			out.push_back(charset[x]);
	}
	return out;
}

uint32_t low_byte_after(uint32_t b, int steps) {
	b &= 0xFFu;
	for (int i = 0; i < steps; ++i)
		b = (49u * b + 1u) & 0xFFu;
	return b;
}

// CI 1 at every connect, and no key until the 0x81.
void check_ci_is_the_lobby_protocols_first() {
	MemorySocket gate;
	MemorySocket session;
	NwuLobbySession lobby(NwuLobbySession::Hooks{}, NwuLobbySession::Environment{});
	lobby.set_title(&kJo, kJo.gate_tag);
	connect(lobby, gate, session);
	CHECK(lobby.session() != nullptr);
	CHECK(lobby.client_index() == 1u && hello_ci(session) == 1u);
	CHECK(lobby.client_key() == 0u);
	CHECK(lobby.session() != nullptr && lobby.session()->client_scrk().empty());
	lobby.close();

	MemorySocket gate2;
	MemorySocket session2;
	connect(lobby, gate2, session2);
	CHECK(lobby.client_index() == 1u && hello_ci(session2) == 1u);
	lobby.close();
}

// The 0x81 draws the CK, then the SCRK, from the manager, and the manager's next draw goes on
// from there: its first byte 64 steps after the CK's last (the SCRK's 63 draws and one).
void check_keys_draw_from_the_manager_at_the_0x81() {
	MemorySocket gate;
	MemorySocket session;
	NwuLobbySession lobby(NwuLobbySession::Hooks{}, NwuLobbySession::Environment{});
	lobby.set_title(&kJo, kJo.gate_tag);
	connect(lobby, gate, session);
	ClientAuth auth;
	CHECK(answer_hello(lobby, session, auth));
	CHECK(auth.ci == 1u);
	CHECK(auth.ck != 0u && bytes_chain(auth.ck));
	CHECK(auth.scrk == scrk_after(auth.ck));
	CHECK(lobby.client_key() == auth.ck);
	CHECK(lobby.session() != nullptr && lobby.session()->client_scrk() == auth.scrk);
	const ConnectionKeys next = manager_connection_keys();
	CHECK((next.key >> 24) == low_byte_after(auth.ck, 64));
	lobby.close();
}

// An injected source stands in for the manager.
void check_injected_keys() {
	NwuLobbySession::Environment env;
	env.connection_keys = [] { return ConnectionKeys{ 0x5A5B5C5Du, "INJECTEDSCRK" }; };
	MemorySocket gate;
	MemorySocket session;
	NwuLobbySession lobby(NwuLobbySession::Hooks{}, env);
	lobby.set_title(&kJo, kJo.gate_tag);
	connect(lobby, gate, session);
	ClientAuth auth;
	CHECK(answer_hello(lobby, session, auth));
	CHECK(auth.ck == 0x5A5B5C5Du && auth.scrk == "INJECTEDSCRK");
	lobby.close();
}

} // namespace

int main() {
	check_ci_is_the_lobby_protocols_first();
	check_keys_draw_from_the_manager_at_the_0x81();
	check_injected_keys();
	if (failures != 0) {
		std::printf("%d failure(s)\n", failures);
		return 1;
	}
	std::printf("OK: the lobby connection's CI is 1 and its CK / SCRK draw from the manager at the 0x81\n");
	return 0;
}
