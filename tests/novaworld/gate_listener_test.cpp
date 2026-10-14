// The real apps/novaworld_server GateListener over loopback UDP, on port 0.
// start() binds the port the OS picks and hands that socket to the receive
// thread, so bound_port() is the port a probe reaches and the port the reply
// advertises as POSTIPPORT (the host-status sink). UDPNOVAWORLD and STARTUPURL
// name the sibling ports the config passed to start() carries (main() passes
// the ports the NW UDP and HTTP listeners bound). The probe goes out as soon
// as start() returns, with no wait: the socket is bound by then. A second
// listener asking for that same port fails start(), the boot-fatal path.
//
// One lobby per title: the listener reads the games table (the repo's
// migrations and the games seed, on a shared in-memory database) at start(),
// and each probe's tag selects the row whose LobbyName, startupurl query and
// GLSVSS leg the reply carries. JO's tag, and the JO demo's alias of it, get
// the reply this gate always sent, byte for byte; DFX2's gets what NovaLogic's
// live gate answered it (dfx2-vs-jo-re.md section 6.5, DFX2-6) in this gate's
// shape (the POST pair, this service's address); OpenNova's gets its row's; a
// tag no row answers to gets nothing. A configured GLSVSS leg (ONNET_GLSVSS_*)
// stands in for a row with none, never for DFX2's own.

#include "gate_listener.h"
#include "net_sockets.h"
#include "server_config.h"

#include <net/napi/envelope.h>
#include <net/novacrypto/nwu.h>
#include <net/novaworld/db/sqlite.h>
#include <net/novaworld/gate_probe.h>
#include <net/novaworld/gate_response.h>

#include "common/file_io.h"
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
// empty request: none).
struct Lobby {
	std::string name;
	std::string query;
	std::string glsvss_request;
	std::string glsvss_rims;
	std::string glsvss_agrms;
};

// The reply this gate sent every tag before the games table chose its lobby,
// with the lobby's values in their places: for JO's lobby, "jop_2_consumer"
// and kJoQuery with no GLSVSS, it is that reply byte for byte.
std::string expected_reply(uint16_t gate_port, uint16_t client_port, const Lobby &lobby) {
	std::vector<std::pair<std::string, std::string>> vars = {
		{ "LobbyName", lobby.name },
		{ "POSTIPADDRESS", "127.0.0.1" },
		{ "POSTIPPORT", std::to_string(gate_port) },
		{ "udpnovaworld", "127.0.0.1:40123" },
		{ "udpcode1", "abc" },
		{ "udpcode2", "xyz" },
		{ "startupurl", "http://127.0.0.1:40456/nwprepare.dll?" + lobby.query },
		{ "usejunction", "0" },
		{ "clearjunction", "0" },
	};
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

// `tag`'s reply is the expected one for `lobby`.
int expect_lobby(uint16_t port, const std::string &tag, const Lobby &lobby) {
	const auto got = probe_text(port, tag);
	if (!got)
		std::fprintf(stderr, "  no gate reply to %s from :%u\n", tag.c_str(), port);
	TEST_EXPECT(got.has_value());
	const std::string want = expected_reply(port, got->client_port, lobby);
	if (got->text != want) {
		std::fprintf(stderr, "  %s's reply:\n%s\n  expected:\n%s\n", tag.c_str(), got->text.c_str(),
				want.c_str());
	}
	TEST_EXPECT(got->text == want);
	opennova::GateResponse parsed;
	TEST_EXPECT(opennova::gate_response_parse(got->text, parsed));
	TEST_EXPECT(parsed.lobby_name == lobby.name);
	return 0;
}

} // namespace

int main() {
	TEST_EXPECT(net::startup() == 0);

	// The service database as main() leaves it before the listeners start:
	// migrated, then the games seed.
	const std::filesystem::path source_dir{ OPENNOVA_SOURCE_DIR };
	opennova::db::ConnectionPool pool("file:gate_listener_test?mode=memory&cache=shared");
	{
		auto boot = pool.acquire();
		opennova::db::run_migrations(*boot, source_dir / "backend" / "migrations");
		boot->exec_script(test_io::read_file_text(
				(source_dir / "backend" / "seed" / "0001_games.sql").string()));
	}

	nws::ServerConfig config;
	config.public_host = "127.0.0.1";
	config.gate_udp_port = 0;
	// The siblings' ports as main() hands them over: what they bound.
	config.nw_udp_port = 40123;
	config.http_port = 40456;

	nws::GateListener gate;
	gate.set_db_pool(&pool);
	TEST_EXPECT(gate.start(config));
	const uint16_t port = gate.bound_port();
	TEST_EXPECT(port != 0);
	TEST_EXPECT(gate.games().games().size() == 3);

	opennova::GateResponse reply;
	{
		const auto jo = probe_text(port, opennova::GATE_PROBE_TAG_JOINTOPS);
		TEST_EXPECT(jo.has_value());
		TEST_EXPECT(opennova::gate_response_parse(jo->text, reply));
	}
	TEST_EXPECT((reply.post_ip == std::array<uint8_t, 4>{127, 0, 0, 1}));
	TEST_EXPECT(reply.post_port == port);
	TEST_EXPECT(reply.udp_novaworld == std::vector<std::string>{"127.0.0.1:40123"});
	const std::string startup = "http://127.0.0.1:40456/nwprepare.dll?";
	TEST_EXPECT(reply.startup_url.compare(0, startup.size(), startup) == 0);

	// Each tag's lobby: JO's reply unchanged, the JO demo's alias answering as
	// JO, DFX2's as the live DFX2 gate answers, and OpenNova's row.
	const Lobby jo{ "jop_2_consumer", kJoQuery, "", "", "" };
	const Lobby dfx2{ "dfx2_0_consumer", kDfx2Query, "[DFX2]KILLS", "3600000", "45000" };
	const Lobby on{ "opennova_0_consumer", kJoQuery, "", "", "" };
	if (expect_lobby(port, "jop:cus2", jo) != 0)
		return 1;
	if (expect_lobby(port, opennova::GATE_PROBE_TAG_JODEMO, jo) != 0)
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
