// The one socket under the game's protocol and the NWU session (D-NET-346):
// DatagramDemux routes each datagram the way retail's NP manager offers it to
// its protocols, and NwuLobbySession::claims is the lobby protocol's
// acceptance (a server-direction opcode from its server's endpoint).
// Socket-free: an in-memory datagram socket stands in.
// [orig: NapiNPManager_HandlePacket @0x622f10; g_NPOpcodeHandlers @0x849d90;
//  NapiNPProtocol_HandleSessionPacket @0x626a00]

#include <net/napi/envelope.h>
#include <net/novacrypto/nwu.h>
#include <net/novaworld/gate_probe.h>
#include <net/novaworld/nwu_lobby_session.h>
#include <net/npwire/datagram_demux.h>
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
#define CHECK(c)                                                                     \
	do {                                                                             \
		if (!(c)) {                                                                  \
			std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);                 \
			++failures;                                                              \
		}                                                                            \
	} while (0)

class MemorySocket final : public IDatagramSocket {
public:
	std::deque<std::pair<PeerAddr, std::vector<uint8_t>>> inbound;
	std::vector<std::pair<PeerAddr, std::vector<uint8_t>>> sent;

	int recv_from(uint8_t *buf, std::size_t cap, PeerAddr &from) override {
		if (inbound.empty()) return 0;
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

int drain(IDatagramSocket &socket, std::vector<PeerAddr> *from = nullptr) {
	uint8_t buf[2048];
	int count = 0;
	for (;;) {
		PeerAddr peer{};
		if (socket.recv_from(buf, sizeof(buf), peer) <= 0) return count;
		if (from != nullptr) from->push_back(peer);
		++count;
	}
}

} // namespace

int main() {
	const PeerAddr server{0x0100007Fu, 64206};  // 127.0.0.1:64206
	const PeerAddr joiner{0x0200007Fu, 40000};  // 127.0.0.2:40000

	// --- The demux alone: the claim first, then the game when attached, else dropped.
	{
		MemorySocket socket;
		DatagramDemux demux(socket);
		demux.set_session_claim([&](const PeerAddr &from, const uint8_t *, std::size_t) { return from == server; });
		socket.inbound.push_back({server, {0x01}});
		socket.inbound.push_back({joiner, {0x02}});
		socket.inbound.push_back({server, {0x03}});
		// A recv on the game's view drains the socket for both.
		std::vector<PeerAddr> game_from;
		CHECK(drain(demux.game(), &game_from) == 1);
		CHECK(game_from.size() == 1 && game_from[0] == joiner);
		CHECK(drain(demux.session()) == 2);
		// Detached, what the session does not take reaches no protocol.
		demux.set_game_attached(false);
		socket.inbound.push_back({joiner, {0x04}});
		socket.inbound.push_back({server, {0x05}});
		CHECK(drain(demux.session()) == 1);
		CHECK(drain(demux.game()) == 0);
		CHECK(demux.dropped() == 1);
		// Attached again, the game's queue starts empty and takes new traffic.
		demux.set_game_attached(true);
		socket.inbound.push_back({joiner, {0x06}});
		CHECK(drain(demux.game()) == 1);
		// Both views send from the one socket.
		const uint8_t byte = 0x7F;
		demux.session().send_to(server, &byte, 1);
		demux.game().send_to(joiner, &byte, 1);
		CHECK(socket.sent.size() == 2);
	}

	// --- The lobby protocol's claim: a server-direction opcode from its server.
	{
		MemorySocket gate;
		MemorySocket raw;
		DatagramDemux demux(raw);
		NwuLobbySession lobby(NwuLobbySession::Hooks{}, NwuLobbySession::Environment{});
		lobby.open(gate, demux.session());
		CHECK(!lobby.claims(server, nullptr, 0));
		lobby.probe("127.0.0.1", GATE_DEFAULT_PORT);
		// The gate names the session's server; the session starts and sends its hello there.
		const std::string body = "VAR \"LOBBYNAME\" \"jop_2_consumer\"\r\n"
		                         "VAR \"UDPNOVAWORLD\" \"127.0.0.1:64206\"\r\n";
		std::vector<uint8_t> inner(body.begin(), body.end());
		nwu_decrypt(inner.data(), inner.size(), GATE_NWU_KEY);
		gate.inbound.push_back({PeerAddr{0x0100007Fu, GATE_DEFAULT_PORT}, enveloped(inner)});
		lobby.tick(10);
		CHECK(lobby.session() != nullptr);
		CHECK(!raw.sent.empty() && raw.sent.back().first == server);

		for (const uint8_t opcode : {SESSION_OPCODE_SERVER_HELLO, SESSION_OPCODE_SERVER_AUTH,
					 SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE, SESSION_OPCODE_SERVER_RESEND_LIST,
					 SESSION_OPCODE_SERVER_PING, SESSION_OPCODE_SERVER_GOODBYE, kNwuServerOpcodeLast}) {
			const std::vector<uint8_t> dg = enveloped({opcode, 0x00, 0x00, 0x00, 0x00});
			CHECK(lobby.claims(server, dg.data(), dg.size()));
			// The same packet from anyone else is not the session's connection.
			CHECK(!lobby.claims(joiner, dg.data(), dg.size()));
		}
		// A client-direction opcode (a joiner's hello, join or session packet, a LAN probe) is the
		// game protocol's, whoever sends it.
		for (const uint8_t opcode : {SESSION_OPCODE_CLIENT_HELLO, SESSION_OPCODE_CLIENT_AUTH,
					 SESSION_OPCODE_PROTOCOL_MESSAGE, SESSION_OPCODE_CLIENT_RESEND_LIST,
					 SESSION_OPCODE_CLIENT_PING, SESSION_OPCODE_CLIENT_GOODBYE, uint8_t{0x88}}) {
			const std::vector<uint8_t> dg = enveloped({opcode, 0x00, 0x00, 0x00, 0x00});
			CHECK(!lobby.claims(server, dg.data(), dg.size()));
		}
		// A datagram the envelope rejects is no protocol's.
		const std::vector<uint8_t> garbage = {0x83, 0x01, 0x02};
		CHECK(!lobby.claims(server, garbage.data(), garbage.size()));

		// Through the demux: the server's 0x83 reaches the session, a joiner's 0x41 the game.
		demux.set_session_claim([&lobby](const PeerAddr &from, const uint8_t *data, std::size_t len) {
			return lobby.claims(from, data, len);
		});
		raw.inbound.push_back({server, enveloped({SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE, 0, 0, 0, 0})});
		raw.inbound.push_back({joiner, enveloped({SESSION_OPCODE_CLIENT_HELLO, 0, 0, 0, 0})});
		raw.inbound.push_back({server, enveloped({SESSION_OPCODE_CLIENT_HELLO, 0, 0, 0, 0})});
		std::vector<PeerAddr> game_from;
		CHECK(drain(demux.game(), &game_from) == 2);
		CHECK(drain(demux.session()) == 1);
		lobby.close();
	}

	if (failures != 0) {
		std::printf("nwu_demux: %d failure(s)\n", failures);
		return 1;
	}
	std::printf("nwu_demux: ok\n");
	return 0;
}
