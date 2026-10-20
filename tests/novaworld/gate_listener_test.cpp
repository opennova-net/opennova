// The real GateListener (apps/novaworld_gate) over loopback UDP, on port 0.
// start() binds the port the OS picks and hands that socket to the receive
// thread, so bound_port() is the port a probe reaches and the port the reply
// advertises as POSTIPPORT (the host-status sink). UDPNOVAWORLD and STARTUPURL
// name what the config passed to start() carries (the gate's fallback lobby
// endpoint, the web endpoint and its port), never a port another listener
// bound: the lobbies and web are other processes. The probe goes out as soon
// as start() returns, with no wait: the socket is bound by then. A second
// listener asking for that same port fails start(), the boot-fatal path.
//
// One lobby per title: the listener reads the games table (the repo's
// migrations and the games seed, on a database of the test's own) at start(),
// and each probe's tag selects the row whose LobbyName, startupurl query and
// GLSVSS leg the reply carries. JO's tag, and the JO demo's alias of it, get
// the reply this gate always sent, byte for byte; DFX2's gets what NovaLogic's
// live gate answered it (dfx2-vs-jo-re.md section 6.5, DFX2-6), in that reply's
// shape too (D-NET-721: no POST pair, the startupurl on http://[domainname]/,
// a TimeOnGateServer line in asctime's shape after the reflected pair);
// OpenNova's gets its row's; a tag no row answers to gets nothing. A
// configured GLSVSS leg (ONNET_GLSVSS_*) stands in for a row with none, never
// for DFX2's own.
//
// A gate with no lobby to hand out (no instance takes clients, no fallback)
// answers no probe, and its heartbeat says so: probes_unanswered counts them
// since the last heartbeat, lobbies_live the instances that take clients (the
// deployment's alarm on a gate that answers no one while its /livez says ok).
// Two instances sharing one lobby address (as every lobby of a deployment
// announces its load balancer's): the gate hands every client that address
// and counts both; one drained, it still hands it out and counts one; both
// drained, it answers nothing again.

#include "gate_listener.h"
#include <net/title/title_identity.h>
#include <base/gameprofile/gameprofile.h>
#include "net_sockets.h"
#include "program/server_config.h"

#include <net/napi/envelope.h>
#include <net/novacrypto/nwu.h>
#include <net/novaworld/db/database.h>
#include <net/novaworld/gate_probe.h>
#include <net/novaworld/gate_response.h>
#include <net/novaworld/lobby_repository.h>

#include "common/test_database.h"
#include "common/test_expect.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#ifndef OPENNOVA_SOURCE_DIR
#error "OPENNOVA_SOURCE_DIR must be set by CMake"
#endif

namespace net = opennova::net;
namespace nws = opennova::novaworld_server;

// The JO and JO demo titles' gate tags (net/title/title_identity.h), the probes' payloads.
static const opennova::TitleIdentity &kJo = *opennova::title_identity(opennova::gameprofile::GAME_JO);
static const opennova::TitleIdentity &kJoDemo =
		*opennova::title_identity(opennova::gameprofile::GAME_JO_DEMO);

namespace {

// What one probe drew: the reply's plaintext and the probing socket's port
// (the reply's ReflectedPortNumber). No reply within `wait_ms`: nullopt.
struct Exchange {
	std::string text;
	uint16_t client_port = 0;
};

std::optional<Exchange> probe_text(uint16_t port, const std::string &tag, int wait_ms = 3000) {
	uint16_t client_port = 0;
	net::ScopedSocket client(net::udp_bind(0, &client_port));
	if (!client.is_valid())
		return std::nullopt;

	const auto inner = opennova::gate_probe_build(tag);
	std::vector<uint8_t> probe(inner.size() + 16);
	size_t probe_size = 0;
	if (opennova::napi_envelope_encode(
				inner.data(), inner.size(), probe.data(), probe.size(), &probe_size) != 0)
		return std::nullopt;
	probe.resize(probe_size);
	net::Endpoint to;
	to.ip = {127, 0, 0, 1};
	to.port = port;
	if (net::udp_send_to(client.get(), to, probe.data(), probe.size()) <= 0)
		return std::nullopt;

	uint8_t rx[2048];
	net::Endpoint from{};
	const int n = net::udp_recv_from(client.get(), rx, sizeof(rx), from, wait_ms);
	if (n <= 0 || from.port != port)
		return std::nullopt;
	std::vector<uint8_t> reply(static_cast<size_t>(n));
	size_t reply_len = 0;
	if (opennova::napi_envelope_decode(
				rx, static_cast<size_t>(n), reply.data(), reply.size(), &reply_len) != 0)
		return std::nullopt;
	reply.resize(reply_len);
	// The client's decrypt of the gate reply [orig: NapiNP_DecryptBuffer @0x633bcc]
	// (gate_response_decrypt_and_parse's first step).
	opennova::nwu_encrypt(reply.data(), reply.size(), opennova::GATE_NWU_KEY);
	return Exchange{ std::string(reply.begin(), reply.end()), client_port };
}

// The startupurl queries: JO's, the one this gate always answered (and a
// retail JO client fetched), and DFX2's, as NovaLogic's live gate answers it.
constexpr const char *kJoQuery = "ver1=[VER1]&ver2=[VER2]&cc=[CC]&gt=[GT]&url=jop_2_start.htm";
constexpr const char *kDfx2Query =
		"ver1=[VER1]&ver2=[VER2]&cc=[CC]&pfid=38&gt=[GT]&url=dfx2_0_start.htm";

// One lobby's answer: its LobbyName, startupurl query and GLSVSS leg (an
// empty request: none), and whether its title's live reply is witnessed
// (TitleIdentity::gate_reply_witnessed: DFX2's).
struct Lobby {
	std::string name;
	std::string query;
	std::string glsvss_request;
	std::string glsvss_rims;
	std::string glsvss_agrms;
	bool witnessed = false;
};

// asctime's shape with no newline: "Www Mmm dd hh:mm:ss yyyy", the day
// space-padded (the live DFX2 gate's "Wed Oct 14 07:50:11 2026").
bool asctime_shaped(const std::string &text) {
	static const std::string days = "SunMonTueWedThuFriSat";
	static const std::string months = "JanFebMarAprMayJunJulAugSepOctNovDec";
	if (text.size() != 24 || text[3] != ' ' || text[7] != ' ' || text[10] != ' ' ||
			text[13] != ':' || text[16] != ':' || text[19] != ' ')
		return false;
	const size_t day = days.find(text.substr(0, 3));
	const size_t month = months.find(text.substr(4, 3));
	if (day == std::string::npos || day % 3 != 0 || month == std::string::npos || month % 3 != 0)
		return false;
	const auto digit = [&](size_t i) { return text[i] >= '0' && text[i] <= '9'; };
	return (text[8] == ' ' || digit(8)) && digit(9) && digit(11) && digit(12) && digit(14) &&
			digit(15) && digit(17) && digit(18) && digit(20) && digit(21) && digit(22) &&
			digit(23);
}

// The reply this gate sent every tag before the games table chose its lobby,
// with the lobby's values in their places: for JO's lobby, "jop_2_consumer"
// and kJoQuery with no GLSVSS, it is that reply byte for byte.
std::string expected_reply(uint16_t gate_port, uint16_t client_port, const Lobby &lobby) {
	std::vector<std::pair<std::string, std::string>> vars = { { "LobbyName", lobby.name } };
	if (!lobby.witnessed) {
		vars.push_back({ "POSTIPADDRESS", "127.0.0.1" });
		vars.push_back({ "POSTIPPORT", std::to_string(gate_port) });
	}
	const std::string web = lobby.witnessed ? "[domainname]" : "nw.example.test:40456";
	vars.push_back({ "udpnovaworld", "198.51.100.7:40123" });
	vars.push_back({ "udpcode1", "abc" });
	vars.push_back({ "udpcode2", "xyz" });
	vars.push_back({ "startupurl", "http://" + web + "/nwprepare.dll?" + lobby.query });
	vars.push_back({ "usejunction", "0" });
	vars.push_back({ "clearjunction", "0" });
	if (!lobby.glsvss_request.empty()) {
		vars.push_back({ "GLSVSSREQUEST", lobby.glsvss_request });
		vars.push_back({ "GLSVSSRIMS", lobby.glsvss_rims });
		vars.push_back({ "GLSVSSAGRMS", lobby.glsvss_agrms });
	}
	vars.push_back({ "ReflectedIpAddress", "127.0.0.1" });
	vars.push_back({ "ReflectedPortNumber", std::to_string(client_port) });
	std::string text = "GATEPROTOCOL \"1.0\"\r\n";
	for (const auto &[key, value] : vars)
		text += "VAR \"" + key + "\" \"" + value + "\"\r\n";
	return text;
}

// `tag`'s reply is the expected one for `lobby`: byte for byte, but for a
// witnessed shape's TimeOnGateServer value (the gate's clock), which is
// asctime-shaped and ends the reply.
int expect_lobby(uint16_t port, const std::string &tag, const Lobby &lobby) {
	const auto got = probe_text(port, tag);
	if (!got)
		std::fprintf(stderr, "  no gate reply to %s from :%u\n", tag.c_str(), port);
	TEST_EXPECT(got.has_value());
	const std::string want = expected_reply(port, got->client_port, lobby);
	std::string text = got->text;
	if (lobby.witnessed) {
		const std::string head = "VAR " + std::string(1, '"') + "TimeOnGateServer" +
				std::string(1, '"') + " " + std::string(1, '"');
		const size_t at = text.rfind(head);
		TEST_EXPECT(at != std::string::npos);
		const std::string line = text.substr(at + head.size());
		TEST_EXPECT(line.size() == 24 + 3 && line[24] == '"' && line[25] == '\r' &&
				line[26] == '\n');
		TEST_EXPECT(asctime_shaped(line.substr(0, 24)));
		text.erase(at);
	}
	if (text != want) {
		std::fprintf(stderr, "  %s's reply:\n%s\n  expected:\n%s\n", tag.c_str(), got->text.c_str(),
				want.c_str());
	}
	TEST_EXPECT(text == want);
	opennova::GateResponse parsed;
	TEST_EXPECT(opennova::gate_response_parse(got->text, parsed));
	TEST_EXPECT(parsed.lobby_name == lobby.name);
	return 0;
}

} // namespace

int main(int argc, char **argv) {
	test_db::init(argc, argv);
	TEST_EXPECT(net::startup() == 0);

	// The service database as main() leaves it before the listeners start:
	// migrated, then the games seed.
	test_db::TestDatabase scratch({ /*migrate=*/true, /*seed_games=*/true });
	opennova::db::ConnectionPool pool(scratch.options());

	nws::ServerConfig config;
	config.public_host = "127.0.0.1";
	config.gate_udp_port = 0;
	// What the gate's config names (ONNET_LOBBY_ENDPOINT, ONNET_WEB_HOST with
	// ONNET_HTTP_PORT), never a port another listener bound: the lobbies and web
	// are other processes, so the NW UDP port this process would bind is not it.
	config.lobby_endpoint = "198.51.100.7:40123";
	config.web_host = "nw.example.test";
	config.http_port = 40456;
	config.nw_udp_port = 64206;

	nws::GateListener gate;
	gate.set_db_pool(&pool);
	TEST_EXPECT(gate.start(config));
	const uint16_t port = gate.bound_port();
	TEST_EXPECT(port != 0);
	TEST_EXPECT(gate.games().games().size() == 3);

	opennova::GateResponse reply;
	{
		const auto jo = probe_text(port, kJo.gate_tag);
		TEST_EXPECT(jo.has_value());
		TEST_EXPECT(opennova::gate_response_parse(jo->text, reply));
	}
	TEST_EXPECT((reply.post_ip == std::array<uint8_t, 4>{127, 0, 0, 1}));
	TEST_EXPECT(reply.post_port == port);
	TEST_EXPECT(reply.udp_novaworld == std::vector<std::string>{ "198.51.100.7:40123" });
	const std::string startup = "http://nw.example.test:40456/nwprepare.dll?";
	TEST_EXPECT(reply.startup_url.compare(0, startup.size(), startup) == 0);

	// Each tag's lobby: JO's reply unchanged, the JO demo's alias answering as
	// JO, DFX2's as the live DFX2 gate answers, and OpenNova's row.
	const Lobby jo{ "jop_2_consumer", kJoQuery, "", "", "" };
	const Lobby dfx2{ "dfx2_0_consumer", kDfx2Query, "[DFX2]KILLS", "3600000", "45000", true };
	const Lobby on{ "opennova_0_consumer", kJoQuery, "", "", "" };
	if (expect_lobby(port, "jop:cus2", jo) != 0)
		return 1;
	if (expect_lobby(port, kJoDemo.gate_tag, jo) != 0)
		return 1;
	if (expect_lobby(port, "dfx2:0:cus:buffy", dfx2) != 0)
		return 1;
	if (expect_lobby(port, "opennova:0", on) != 0)
		return 1;
	{
		const auto got = probe_text(port, "dfx2:0:cus:buffy");
		TEST_EXPECT(got.has_value());
		opennova::GateResponse parsed;
		TEST_EXPECT(opennova::gate_response_parse(got->text, parsed));
		TEST_EXPECT(parsed.glsvss_request == "[DFX2]KILLS");
		TEST_EXPECT(parsed.glsvss_rims == 3600000 && parsed.glsvss_agrms == 45000);
		// The witnessed shape parses as the client parses it: no POST pair (the
		// udpnovaworld line leaves it unread) and the [domainname] startupurl.
		TEST_EXPECT(parsed.post_port == 0 && parsed.udp_novaworld.size() == 1);
		TEST_EXPECT(parsed.startup_url.rfind("http://[domainname]/nwprepare.dll?", 0) == 0);
	}

	// A configured GLSVSS leg stands in for the rows with none (JO's and
	// OpenNova's), never for DFX2's own.
	{
		nws::ServerConfig glsvss = config;
		glsvss.glsvss_request = "[JOP]CONFIGURED";
		glsvss.glsvss_rims = 111;
		glsvss.glsvss_agrms = 222;
		nws::GateListener configured;
		configured.set_db_pool(&pool);
		TEST_EXPECT(configured.start(glsvss));
		const uint16_t cport = configured.bound_port();
		const Lobby jo_configured{ "jop_2_consumer", kJoQuery, "[JOP]CONFIGURED", "111", "222" };
		if (expect_lobby(cport, "jop:cus2", jo_configured) != 0)
			return 1;
		if (expect_lobby(cport, "dfx2:0:cus:buffy", dfx2) != 0)
			return 1;
		configured.stop();
	}

	// A tag no row answers to is refused: no reply at all.
	TEST_EXPECT(!probe_text(port, "jop:cus3", 400).has_value());
	TEST_EXPECT(!probe_text(port, "opennova_0_consumer", 400).has_value());

	// No lobby instance takes clients and no fallback: no reply, each probe
	// counted in the heartbeat's probes_unanswered (taken once), lobbies_live 0;
	// then two instances at one lobby address, drained one by one. The gate
	// reads the instances at every probe here (a refresh of 0).
	{
		nws::ServerConfig lobbyless = config;
		lobbyless.lobby_endpoint.clear();
		nws::GateListener none;
		none.set_db_pool(&pool);
		none.set_lobby_refresh_ms(0);
		TEST_EXPECT(none.start(lobbyless));
		TEST_EXPECT(!probe_text(none.bound_port(), kJo.gate_tag, 400).has_value());
		TEST_EXPECT(!probe_text(none.bound_port(), kJo.gate_tag, 400).has_value());
		auto db = pool.acquire();
		nws::Heartbeat beat;
		beat.program = "gate";
		none.heartbeat(*db, beat);
		TEST_EXPECT(beat.probes_unanswered == 2);
		TEST_EXPECT(beat.lobbies_live == 0);
		TEST_EXPECT(nws::heartbeat_line(beat) ==
				"{\"heartbeat\":{\"program\":\"gate\",\"probes_unanswered\":2,"
				"\"lobbies_live\":0,\"db_lock_timeouts\":0,\"db_deadlocks\":0}}");
		const std::string shared = "198.51.100.9:64206";
		const int64_t first = opennova::lobbydb::register_instance(*db, shared);
		const int64_t second = opennova::lobbydb::register_instance(*db, shared);
		TEST_EXPECT(first != second);
		const auto handed = [&none]() -> std::vector<std::string> {
			const auto got = probe_text(none.bound_port(), kJo.gate_tag, 1000);
			opennova::GateResponse parsed;
			if (!got || !opennova::gate_response_parse(got->text, parsed))
				return {};
			return parsed.udp_novaworld;
		};
		TEST_EXPECT(handed() == std::vector<std::string>{ shared });
		nws::Heartbeat next;
		none.heartbeat(*db, next);
		TEST_EXPECT(next.probes_unanswered == 0);
		TEST_EXPECT(next.lobbies_live == 2);
		TEST_EXPECT(opennova::lobbydb::set_instance_state(*db, first, "draining"));
		TEST_EXPECT(handed() == std::vector<std::string>{ shared });
		none.heartbeat(*db, next);
		TEST_EXPECT(next.lobbies_live == 1);
		TEST_EXPECT(opennova::lobbydb::set_instance_state(*db, second, "draining"));
		TEST_EXPECT(handed().empty());
		none.heartbeat(*db, next);
		TEST_EXPECT(next.probes_unanswered == 1 && next.lobbies_live == 0);
		none.stop();
	}

	// The port is held: a second listener cannot bind it, and says so.
	nws::ServerConfig taken = config;
	taken.gate_udp_port = port;
	nws::GateListener second;
	TEST_EXPECT(!second.start(taken));

	gate.stop();
	net::shutdown();
	std::printf("OK: gate listener serves and advertises its bound port :%u, one lobby per tag\n",
			static_cast<unsigned>(port));
	return 0;
}
