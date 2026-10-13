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
#include <net/napi/session.h>
#include <net/npwire/datagram_demux.h>
#include <net/npwire/nw_session_framing.h>
#include <net/npwire/session_hello.h>
#include <net/npwire/session_keys.h>

#include <algorithm>
#include <array>
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

// The gate's reply naming `body`'s VARs, as the gate socket receives it.
std::vector<uint8_t> gate_reply(const std::string &body) {
	std::vector<uint8_t> inner(body.begin(), body.end());
	nwu_decrypt(inner.data(), inner.size(), GATE_NWU_KEY);
	return enveloped(inner);
}

uint8_t opcode_of(const std::vector<uint8_t> &dg) {
	uint8_t opcode = 0;
	std::vector<uint8_t> body;
	return nw_decode_inbound(dg.data(), dg.size(), opcode, body) ? opcode : 0;
}

// The 0x41 `dg` carries, parsed (false when it is none).
bool hello_of(const std::vector<uint8_t> &dg, ClientHello &out) {
	uint8_t opcode = 0;
	std::vector<uint8_t> body;
	return nw_decode_inbound(dg.data(), dg.size(), opcode, body) &&
			opcode == SESSION_OPCODE_CLIENT_HELLO &&
			parse_client_hello(body.data(), body.size(), out);
}

// The server's 0x81 to the session's 0x41 `hello_dg`: its CI and identity echoed, the CI
// changed by `ci_xor`.
std::vector<uint8_t> server_hello_to(const std::vector<uint8_t> &hello_dg, uint32_t ci_xor = 0) {
	uint8_t opcode = 0;
	std::vector<uint8_t> body;
	ClientHello hello;
	if (!nw_decode_inbound(hello_dg.data(), hello_dg.size(), opcode, body) ||
	    opcode != SESSION_OPCODE_CLIENT_HELLO || !parse_client_hello(body.data(), body.size(), hello))
		return {};
	ServerHello reply = build_server_hello(hello, 0x7F000001u, 40000);
	reply.ci ^= ci_xor;
	return nw_encode_outbound(SESSION_OPCODE_SERVER_HELLO, server_hello_to_bytes(reply));
}

PeerAddr at(uint8_t a, uint8_t b, uint8_t c, uint8_t d, uint16_t port) {
	return peer_addr_from_octets({a, b, c, d}, port);
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

		// D-NET-469: a 0x81 finds the connection by its CI, whoever sends it; one with another CI
		// (or none) is the next protocol's, even from the server.
		// [orig: Nwu_HandleServerHello @0x626D20 - the CI lookup @0x627533, the misses return 0
		//  @0x626DCB]
		const std::vector<uint8_t> hello = server_hello_to(raw.sent.back().second);
		CHECK(!hello.empty());
		CHECK(lobby.claims(server, hello.data(), hello.size()));
		CHECK(lobby.claims(joiner, hello.data(), hello.size()));
		const std::vector<uint8_t> foreign = server_hello_to(raw.sent.back().second, 1);
		CHECK(!lobby.claims(server, foreign.data(), foreign.size()));
		const std::vector<uint8_t> bare = enveloped({SESSION_OPCODE_SERVER_HELLO, 0x00, 0x00, 0x00, 0x00});
		CHECK(!lobby.claims(server, bare.data(), bare.size()));
		// Until it takes a 0x81 the connection has no server, so no other server-direction opcode
		// is its; the taken 0x81's sender is its server from then on.
		// [orig: CNapiNPConnection_InitFromSession @0x626320 - @0x6263B4 / @0x6263B7]
		const std::vector<uint8_t> early = enveloped({SESSION_OPCODE_SERVER_AUTH, 0x00, 0x00, 0x00, 0x00});
		CHECK(!lobby.claims(server, early.data(), early.size()));
		demux.set_session_claim([&lobby](const PeerAddr &from, const uint8_t *data, std::size_t len) {
			return lobby.claims(from, data, len);
		});
		raw.inbound.push_back({server, hello});
		lobby.tick(20);
		CHECK(lobby.nw_peer() == server);
		CHECK(lobby.session()->state() == ClientSession::State::Auth);
		CHECK(!lobby.claims(server, hello.data(), hello.size())); // no longer probing

		for (const uint8_t opcode : {SESSION_OPCODE_SERVER_AUTH,
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
		raw.inbound.push_back({server, enveloped({SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE, 0, 0, 0, 0})});
		raw.inbound.push_back({joiner, enveloped({SESSION_OPCODE_CLIENT_HELLO, 0, 0, 0, 0})});
		raw.inbound.push_back({server, enveloped({SESSION_OPCODE_CLIENT_HELLO, 0, 0, 0, 0})});
		std::vector<PeerAddr> game_from;
		CHECK(drain(demux.game(), &game_from) == 2);
		CHECK(drain(demux.session()) == 1);
		lobby.close();
	}

	// --- D-NET-469: every gate UDPNOVAWORLD entry is a probe target. Each is split at its first
	// colon, its port the napi literal (0 without one, then 64206), its host resolved with DNS on;
	// a zero address makes none and a repeat is kept once. The 0x41 goes to every target, and the
	// 0x81 the connection takes, from whichever target, names its server for the 0x42 on.
	// [orig: CNapiGameSession_ConnectToNovaWorld @0x4D4640 - @0x4D4AB8 / @0x4D4AC0;
	//  CNapiGameSession_SendToAddress @0x4D0970 - @0x4D09A0, @0x4D09B0..0x4D09B2,
	//  @0x4D09C1..0x4D09C8, @0x4D09CF; NapiNPEnumerator_CreateTarget @0x624C30;
	//  CNapiNPConnection_PumpEnumeratorAndSend @0x6290C0 - the target walk @0x629183..0x6291B4;
	//  CNapiNPConnection_InitFromSession @0x626320 - @0x6263B4 / @0x6263B7]
	{
		MemorySocket gate;
		MemorySocket session;
		NwuLobbySession::Environment env;
		env.resolve_ipv4 = [](const std::string &host, PeerAddr &out) {
			if (host == "nw.example") {
				out = peer_addr_from_octets({10, 0, 0, 9}, out.port);
				return true;
			}
			std::array<uint8_t, 4> octets{};
			unsigned a = 0, b = 0, c = 0, d = 0;
			char tail = 0;
			if (std::sscanf(host.c_str(), "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) != 4) return false;
			octets = {static_cast<uint8_t>(a), static_cast<uint8_t>(b), static_cast<uint8_t>(c),
			          static_cast<uint8_t>(d)};
			out = peer_addr_from_octets(octets, out.port);
			return true;
		};
		std::vector<std::string> fatals;
		NwuLobbySession::Hooks hooks;
		hooks.on_fatal = [&fatals](const std::string &message) { fatals.push_back(message); };
		NwuLobbySession lobby(hooks, env);
		lobby.open(gate, session);
		lobby.probe("127.0.0.1", GATE_DEFAULT_PORT);
		gate.inbound.push_back({PeerAddr{0x0100007Fu, GATE_DEFAULT_PORT},
		                        gate_reply("VAR \"UDPNOVAWORLD\" \"127.0.0.1:$7D00\"\r\n"
		                                   "VAR \"UDPNOVAWORLD\" \"nw.example\"\r\n"
		                                   "VAR \"UDPNOVAWORLD\" \"0.0.0.0:5\"\r\n"
		                                   "VAR \"UDPNOVAWORLD\" \"unresolved.example:7\"\r\n"
		                                   "VAR \"UDPNOVAWORLD\" \"127.0.0.1:32000\"\r\n"
		                                   "VAR \"UDPNOVAWORLD\" \"127.0.0.2:0\"\r\n"
		                                   "VAR \"UDPNOVAWORLD\" \"255.255.255.255:9\"\r\n"
		                                   "VAR \"UDPNOVAWORLD\" \"127.0.0.3:65537\"\r\n")});
		lobby.tick(10);
		CHECK(fatals.empty());
		CHECK(lobby.session() != nullptr);
		const std::vector<std::pair<uint32_t, uint32_t>> want = {
				{at(127, 0, 0, 1, 0).ip, 32000}, {at(10, 0, 0, 9, 0).ip, 64206},
				{at(127, 0, 0, 2, 0).ip, 64206}, {0xFFFFFFFFu, 9}, {at(127, 0, 0, 3, 0).ip, 65537}};
		CHECK(lobby.targets().size() == want.size());
		for (std::size_t i = 0; i < want.size() && i < lobby.targets().size(); ++i) {
			CHECK(lobby.targets()[i].ip == want[i].first && lobby.targets()[i].port == want[i].second);
		}
		// One 0x41 per target, in order, the port narrowed to 16 bits at the send. D-NET-541: each
		// is that target's own announce, carrying the target's address and port (as kept, before
		// the narrowing) as EIP / EPN, and no ET; the rest of the hello is the connection's.
		// [orig: CNapiNPConnection_PumpEnumeratorAndSend @0x6290C0 - @0x629190..0x62919D;
		//  NapiNPSession_SendAnnouncePacket @0x61FA00 - EIP @0x61FCF5, EPN @0x61FD23, ET
		//  @0x61FD51; NapiNPEnumerator_CreateTarget @0x624C30 - @0x624C9F]
		const std::vector<PeerAddr> sent_to = {at(127, 0, 0, 1, 32000), at(10, 0, 0, 9, 64206),
		                                       at(127, 0, 0, 2, 64206), PeerAddr{0xFFFFFFFFu, 9},
		                                       at(127, 0, 0, 3, 1)};
		auto check_announces = [&](std::size_t first) {
			ClientHello lead;
			CHECK(hello_of(session.sent[first].second, lead));
			for (std::size_t i = 0; i < sent_to.size() && first + i < session.sent.size(); ++i) {
				const auto &dg = session.sent[first + i];
				CHECK(dg.first == sent_to[i]);
				ClientHello hello;
				CHECK(hello_of(dg.second, hello));
				CHECK(hello.eip == want[i].first && hello.epn == want[i].second);
				CHECK(hello.et == 0);
				CHECK(hello.ci == lead.ci && hello.pn == lead.pn && hello.pg == lead.pg);
			}
		};
		CHECK(session.sent.size() == sent_to.size());
		check_announces(0);
		// The re-announce goes to every target again, each its own announce once more.
		lobby.tick(10 + SESSION_CONNECT_RETRANSMIT_MS + 1);
		CHECK(session.sent.size() == 2 * sent_to.size());
		CHECK(session.sent.back().first == sent_to.back());
		if (session.sent.size() == 2 * sent_to.size())
			check_announces(sent_to.size());
		CHECK(lobby.nw_peer() == PeerAddr{});
		// Until then the connection has no server, so its own packets (all but the 0x41) go
		// nowhere. [orig: CNapiNPManager_SendTo @0x61EC20 - the address test @0x61EC59]
		const std::size_t sent_before = session.sent.size();
		lobby.send(enveloped({SESSION_OPCODE_CLIENT_PING, 0x00, 0x00, 0x00, 0x00}));
		CHECK(session.sent.size() == sent_before);

		// The second target answers: the connection takes its 0x81, and its 0x42 goes there.
		const PeerAddr second = sent_to[1];
		const std::vector<uint8_t> reply = server_hello_to(session.sent[0].second);
		CHECK(lobby.claims(second, reply.data(), reply.size()));
		session.inbound.push_back({second, reply});
		session.sent.clear();
		lobby.tick(10 + SESSION_CONNECT_RETRANSMIT_MS + 2);
		CHECK(lobby.session()->state() == ClientSession::State::Auth);
		CHECK(lobby.nw_peer() == second);
		CHECK(session.sent.size() == 1);
		CHECK(!session.sent.empty() && session.sent[0].first == second &&
		      opcode_of(session.sent[0].second) == SESSION_OPCODE_CLIENT_AUTH);
		// The connection's packets are its server's alone now; a late 0x81 is not taken.
		const std::vector<uint8_t> msg = enveloped({SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE, 0, 0, 0, 0});
		CHECK(lobby.claims(second, msg.data(), msg.size()));
		CHECK(!lobby.claims(sent_to[0], msg.data(), msg.size()));
		CHECK(!lobby.claims(sent_to[0], reply.data(), reply.size()));
		lobby.close();
	}
	// A gate naming only addresses that resolve to none still starts the connection, which probes
	// nothing, POST pair or not (the in-use word passes the POST test). D-NET-540: one naming no
	// UDPNOVAWORLD needs the POST address and port: without either the gate fails (state -9, the
	// menu's NWEC25), with both the connect goes on with no NWU session (on_sessionless, no
	// session, the in-use word clear). The command line's POST pair stands in for the gate's.
	// [orig: CNapiGateManager_ProcessResponse @0x4CED20 - the in-use word @0x4CF525..0x4CF541,
	//  the command line's POST pair @0x4CF54B..0x4CF562, the in-use test @0x4CF599..0x4CF5A0,
	//  the address @0x4CF5A6, the port @0x4CF627, -9 @0x4CF6AC;
	//  UI_ProcessLANSessionStateMachine @0x558DE0 - NWEC25 @0x5590FD]
	{
		struct Case {
			NwuLobbySession::CommandLine command_line;
			bool session;
			bool sessionless;
			const char *fatal;
		};
		const NwuLobbySession::CommandLine none;
		NwuLobbySession::CommandLine post_switches;
		post_switches.post_ip_address = "10.1.2.3";
		post_switches.post_port_number = 7000;
		NwuLobbySession::CommandLine unresolved_post = post_switches;
		unresolved_post.post_ip_address = "nowhere.example";
		const std::string post_ip = "VAR \"POSTIPADDRESS\" \"192.168.0.5\"\r\n";
		const std::string post_port = "VAR \"POSTIPPORT\" \"7598\"\r\n";
		const std::string lobby_name = "VAR \"LOBBYNAME\" \"jop_2_consumer\"\r\n";
		const std::string unresolved = "VAR \"UDPNOVAWORLD\" \"0.0.0.0:5\"\r\n";
		const std::vector<std::pair<std::string, Case>> cases = {
			{ unresolved, { none, true, false, nullptr } },
			{ unresolved + post_ip + post_port, { none, true, false, nullptr } },
			{ lobby_name, { none, false, false, "NWEC25" } },
			{ lobby_name + post_ip, { none, false, false, "NWEC25" } },
			{ lobby_name + post_port, { none, false, false, "NWEC25" } },
			{ lobby_name + post_ip + post_port, { none, false, true, nullptr } },
			{ lobby_name, { post_switches, false, true, nullptr } },
			{ lobby_name + post_port, { unresolved_post, false, false, "NWEC25" } },
		};
		for (const auto &[body, want] : cases) {
			MemorySocket gate;
			MemorySocket session;
			std::vector<std::string> fatals;
			int sessionless = 0;
			GateResponse seen;
			NwuLobbySession::Hooks hooks;
			hooks.on_fatal = [&fatals](const std::string &message) { fatals.push_back(message); };
			hooks.on_sessionless = [&sessionless]() { ++sessionless; };
			hooks.on_gate_response = [&seen](const GateResponse &gate) { seen = gate; };
			NwuLobbySession lobby(hooks, NwuLobbySession::Environment{});
			lobby.set_command_line(want.command_line);
			lobby.open(gate, session);
			lobby.probe("127.0.0.1", GATE_DEFAULT_PORT);
			gate.inbound.push_back({PeerAddr{0x0100007Fu, GATE_DEFAULT_PORT}, gate_reply(body)});
			lobby.tick(10);
			CHECK(lobby.targets().empty());
			CHECK(session.sent.empty());
			CHECK((lobby.session() != nullptr) == want.session);
			CHECK(lobby.match_facts().in_use == want.session);
			CHECK(sessionless == (want.sessionless ? 1 : 0));
			CHECK(want.fatal == nullptr ? fatals.empty()
										: fatals == std::vector<std::string>{ want.fatal });
			if (!want.command_line.post_ip_address.empty() && want.sessionless) {
				// The switches' pair is the response's, as the hook and the heartbeat read it.
				CHECK((lobby.gate_response().post_ip == std::array<uint8_t, 4>{ 10, 1, 2, 3 }));
				CHECK(lobby.gate_response().post_port == 7000u);
				CHECK((seen.post_ip == std::array<uint8_t, 4>{ 10, 1, 2, 3 }) &&
						seen.post_port == 7000u);
			}
			lobby.close();
		}
	}

	// D-NET-542: the connect's sources, first match only: the menu's JUNCTIONCODE (one target),
	// else every /UDPNOVAWORLD command-line entry, else the gate's entries. A command-line entry
	// marks the NWU session in use when the gate names none (so no POST pair is asked for). A set
	// USEJUNCTION (the gate's, or the command line's over it) empties JUNCTIONCODE at the
	// response, and the menu's entry (open()) empties it when the last response went through with
	// USEJUNCTION and CLEARJUNCTION both set.
	// [orig: CNapiGameSession_ConnectToNovaWorld @0x4D4640 - @0x4D4A4A, @0x4D4A71,
	//  @0x4D4A78..0x4D4A9E, @0x4D4AA2..0x4D4ACE; CNapiGateManager_ProcessResponse @0x4CED20 -
	//  @0x4CF52E..0x4CF541, @0x4CF6ED..0x4CF76E; UI_ProcessLANSessionStateMachine @0x558DE0 -
	//  @0x558E2D..0x558E64]
	{
		const std::string gate_entry = "VAR \"UDPNOVAWORLD\" \"127.0.0.1:64206\"\r\n";
		const std::string use_junction = "VAR \"USEJUNCTION\" \"1\"\r\n";
		const std::string clear_junction = "VAR \"CLEARJUNCTION\" \"1\"\r\n";
		const std::string lobby_only = "VAR \"LOBBYNAME\" \"jop_2_consumer\"\r\n";
		NwuLobbySession::CommandLine switches;
		switches.udp_novaworld = { "127.0.0.5:7001", "127.0.0.6", "0.0.0.0:9" };
		struct Run {
			std::vector<std::pair<uint32_t, uint32_t>> targets;
			std::vector<PeerAddr> hellos_to;
			std::vector<std::string> fatals;
			bool session = false;
			bool in_use = false;
			std::string junction_after;
		};
		auto run = [&](NwuLobbySession &lobby, const std::string &body) {
			MemorySocket gate;
			MemorySocket session;
			Run out;
			lobby.open(gate, session);
			lobby.probe("127.0.0.1", GATE_DEFAULT_PORT);
			gate.inbound.push_back(
					{ PeerAddr{ 0x0100007Fu, GATE_DEFAULT_PORT }, gate_reply(body) });
			lobby.tick(10);
			for (const auto &target : lobby.targets())
				out.targets.push_back({ target.ip, target.port });
			for (const auto &dg : session.sent) {
				if (opcode_of(dg.second) == SESSION_OPCODE_CLIENT_HELLO)
					out.hellos_to.push_back(dg.first);
			}
			out.session = lobby.session() != nullptr;
			out.in_use = lobby.match_facts().in_use;
			out.junction_after = lobby.junction_code();
			lobby.close();
			return out;
		};
		auto make = [](std::vector<std::string> &fatals) {
			NwuLobbySession::Hooks hooks;
			hooks.on_fatal = [&fatals](const std::string &message) { fatals.push_back(message); };
			return hooks;
		};
		const std::vector<std::pair<uint32_t, uint32_t>> switch_targets = {
			{ at(127, 0, 0, 5, 0).ip, 7001 }, { at(127, 0, 0, 6, 0).ip, 64206 }
		};

		// The command line's entries over the gate's.
		{
			std::vector<std::string> fatals;
			NwuLobbySession lobby(make(fatals), NwuLobbySession::Environment{});
			lobby.set_command_line(switches);
			const Run r = run(lobby, gate_entry);
			CHECK(fatals.empty() && r.session && r.in_use);
			CHECK(r.targets == switch_targets);
			CHECK((r.hellos_to ==
					std::vector<PeerAddr>{ at(127, 0, 0, 5, 7001), at(127, 0, 0, 6, 64206) }));
		}
		// The command line's entries alone mark the session in use: no POST pair is asked for.
		{
			std::vector<std::string> fatals;
			NwuLobbySession lobby(make(fatals), NwuLobbySession::Environment{});
			lobby.set_command_line(switches);
			const Run r = run(lobby, lobby_only);
			CHECK(fatals.empty() && r.session && r.in_use);
			CHECK(r.targets == switch_targets);
		}
		// JUNCTIONCODE first, its one address the only target.
		{
			std::vector<std::string> fatals;
			NwuLobbySession lobby(make(fatals), NwuLobbySession::Environment{});
			lobby.set_command_line(switches);
			lobby.set_junction_code("127.0.0.9:7009");
			const Run r = run(lobby, gate_entry);
			CHECK(fatals.empty() && r.session);
			CHECK((r.targets ==
					std::vector<std::pair<uint32_t, uint32_t>>{
							{ at(127, 0, 0, 9, 0).ip, 7009 } }));
			CHECK((r.hellos_to == std::vector<PeerAddr>{ at(127, 0, 0, 9, 7009) }));
			CHECK(r.junction_after == "127.0.0.9:7009");
		}
		// The gate's USEJUNCTION empties JUNCTIONCODE ahead of the connect, so the next source
		// stands; the command line's USEJUNCTION 0 overrides it and JUNCTIONCODE stays first.
		{
			std::vector<std::string> fatals;
			NwuLobbySession lobby(make(fatals), NwuLobbySession::Environment{});
			lobby.set_junction_code("127.0.0.9:7009");
			const Run r = run(lobby, gate_entry + use_junction);
			CHECK(fatals.empty() && r.junction_after.empty());
			CHECK((r.targets ==
					std::vector<std::pair<uint32_t, uint32_t>>{
							{ at(127, 0, 0, 1, 0).ip, 64206 } }));

			NwuLobbySession::CommandLine keep;
			keep.use_junction = 0;
			std::vector<std::string> kept_fatals;
			NwuLobbySession kept(make(kept_fatals), NwuLobbySession::Environment{});
			kept.set_command_line(keep);
			kept.set_junction_code("127.0.0.9:7009");
			const Run k = run(kept, gate_entry + use_junction);
			CHECK(k.junction_after == "127.0.0.9:7009");
			CHECK((k.targets ==
					std::vector<std::pair<uint32_t, uint32_t>>{
							{ at(127, 0, 0, 9, 0).ip, 7009 } }));
		}
		// The menu's entry: after a response with USEJUNCTION and CLEARJUNCTION both set, a
		// JUNCTIONCODE written since is emptied by the next open(); with CLEARJUNCTION clear (the
		// command line's 0 over the gate's 1) it stands.
		{
			for (const int32_t clear_override : { -1, 0 }) {
				std::vector<std::string> fatals;
				NwuLobbySession lobby(make(fatals), NwuLobbySession::Environment{});
				NwuLobbySession::CommandLine cl;
				cl.clear_junction = clear_override;
				lobby.set_command_line(cl);
				run(lobby, gate_entry + use_junction + clear_junction);
				lobby.set_junction_code("127.0.0.9:7009");
				MemorySocket gate;
				MemorySocket session;
				lobby.open(gate, session);
				CHECK(lobby.junction_code() == (clear_override == -1 ? "" : "127.0.0.9:7009"));
				lobby.close();
			}
		}
	}

	if (failures != 0) {
		std::printf("nwu_demux: %d failure(s)\n", failures);
		return 1;
	}
	std::printf("nwu_demux: ok\n");
	return 0;
}
