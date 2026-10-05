// opennova-serve end to end (ADR 0051), with no retail data: a loose game
// directory holding one synthetic mission and a retail-format host file. The
// server mounts it, reads the host file, boots the mission headless as a
// DedicatedHost, binds a real UDP socket, answers a LAN browser's 0x41 probe
// with its 0x81 ServerHello, and admits a LAN joiner (a ClientRuntime on a
// second real socket) through the handshake into the match. Then the server's
// stop reaches the joiner.
#include "server.h"

#include <base/gameprofile/game_type.h>
#include <formats/mission/bms.h>
#include <formats/mission/bms_edit.h>
#include <net/npwire/lan_discovery.h>
#include <runtime/inmatch/client_runtime.h>
#include <runtime/inmatch/napi_np_connection.h>

#include "common/synthetic_mission.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace opennova;
namespace fs = std::filesystem;

namespace {

int failures = 0;
#define CHECK(c)                                                                     \
	do {                                                                             \
		if (!(c)) {                                                                  \
			std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);                 \
			++failures;                                                              \
		}                                                                            \
	} while (0)

bool write_bytes(const fs::path &path, const std::vector<uint8_t> &bytes) {
	std::ofstream out(path, std::ios::binary);
	out.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
	return static_cast<bool>(out);
}

// A deathmatch mission: the two placed entities every kernel test boots, and
// the solo start marker (6002) a deathmatch player spawns at.
std::vector<uint8_t> deathmatch_mission() {
	bms::File m = test_mission::two_entity_mission();
	m.header.magic[0] = 'B';
	m.header.magic[1] = 'M';
	m.header.magic[2] = 'S';
	m.header.magic[3] = static_cast<char>(bms::kMinVersion);
	std::snprintf(m.header.mission_name, sizeof(m.header.mission_name), "Serve Test Map");
	m.header.attrib_flags = bms::AttribFlags::Deathmatch;
	bms::Entity marker{};
	marker.type = bms::ItemType::Marker;
	marker.type_id = 6002;
	marker.x = 40 << 16;
	marker.y = 40 << 16;
	marker.id = 41;
	m.items.push_back(marker);
	mission::sync_counts(m);
	std::vector<uint8_t> bytes;
	std::string error;
	if (!bms::write(m, bytes, error)) std::printf("bms::write: %s\n", error.c_str());
	return bytes;
}

uint16_t free_udp_port() {
	uint16_t port = 0;
	net::ScopedSocket probe(net::udp_bind(0, &port));
	return probe.is_valid() ? port : 0;
}

} // namespace

int main() {
	if (net::startup() != 0) return (std::printf("FAIL net::startup\n"), 1);
	const fs::path dir = fs::temp_directory_path() /
			("opennova_serve_test_" + std::to_string(
					std::chrono::steady_clock::now().time_since_epoch().count()));
	fs::create_directories(dir);
	CHECK(write_bytes(dir / "SERVETST.BMS", deathmatch_mission()));
	// A loose score.ini with a deathmatch row that is not the default table:
	// it must reach world.match, which the bring-up configures.
	{
		std::ofstream score(dir / "score.ini", std::ios::binary);
		score << "VERSION 40\r\n"
		      << "GAMETYPE \"DM\"\r\n"
		      << "FIELD \"NUMENEMYKILLS\" 1\r\n"
		      << "VAR \"FIRE\" 7\r\n"
		      << "VAR \"ENEMYKILL\" 9\r\n";
	}
	{
		std::ofstream host(dir / "test.host", std::ios::binary);
		host << "// opennova-serve test host file\r\n"
		     << "GameName \"Serve Test\"\r\n"
		     << "ServerMessage \"hello from the test\"\r\n"
		     << "MaxPlayers 8\r\n"
		     << "KillLimit 20\r\n"
		     << "TeamFF 1\r\n"
		     << "Mission NOSUCH.BMS\r\n"
		     << "Mission servetst.bms\r\n";
	}

	serve::ServeOptions options;
	{
		std::string error;
		const int parsed = serve::parse_serve_options(
				{"--resource-dir", dir.string(), "/host", (dir / "test.host").string(), "--loose-root",
						"--lan-port", std::to_string(free_udp_port())},
				options, error);
		CHECK(parsed == 0);
		serve::ServeOptions missing;
		CHECK(serve::parse_serve_options({"--resource-dir", dir.string()}, missing, error) == 1);
		CHECK(serve::parse_serve_options({"--help"}, missing, error) == -1);
	}

	serve::Server server(options);
	std::string error;
	const bool started = server.start(error);
	if (!started) {
		std::printf("start: %s (catalog rows: %zu)\n", error.c_str(), server.catalog().size());
		for (const mission_catalog::Row &row : server.catalog())
			std::printf("  row '%s'\n", row.file.c_str());
	}
	CHECK(started);
	if (!started) return 1;

	// The host file reached the session: the name, the message, the cap with the
	// dedicated slot (8 + 1), the 20-kill limit, the starting map; the unknown
	// mission was reported and skipped.
	const inmatch::GameConfig &config = server.role().state.host_owner.ctx.config;
	CHECK(config.server_name == "Serve Test");
	CHECK(config.custom_text == "hello from the test");
	CHECK(config.max_players == 9u);
	CHECK(config.score_limit == 20u);
	CHECK(config.mission_file == "servetst.bms");
	CHECK(config.mission_name == "Serve Test Map");
	CHECK(config.game_type == game_type::kDeathmatch);
	CHECK(server.host_file_report().unknown_missions.size() == 1);
	CHECK(server.rotation().entries.size() == 1);
	// The score.ini row reached the match the bring-up configured: its values
	// (FIRE status 0, ENEMYKILL status 3) and its one FIELD row.
	const world::Match &match = server.kernel().world.match;
	CHECK(match.rules().score_values.has_value());
	CHECK(match.rules().score_values && (*match.rules().score_values)[0] == 7);
	CHECK(match.rules().score_values && (*match.rules().score_values)[3] == 9);
	CHECK(match.rules().score_fields.size() == 1);
	// Serve Only: the dedicated role, no player of the host's own, no local client.
	CHECK(server.role().kind() == inmatch::RoleKind::DedicatedHost);
	CHECK(!server.role().state.host_owner.serve_and_play);
	CHECK(server.role().client_runtime() == nullptr);
	CHECK(!server.kernel().local.has_local_player());
	CHECK(server.kernel().world.rules.mp_session);
	CHECK(server.bound_port() == options.port);

	constexpr double kFrame = 1.0 / 62.5;
	const net::Endpoint server_ep{{127, 0, 0, 1}, server.bound_port()};

	// --- a LAN browser's probe: the server answers on its game socket.
	{
		uint16_t browse_port = 0;
		net::ScopedSocket browse(net::udp_bind(0, &browse_port));
		CHECK(browse.is_valid());
		const uint32_t ci = 0x1234;
		const std::vector<uint8_t> probe = build_lan_discovery_probe(ci);
		net::udp_send_to(browse.get(), server_ep, probe.data(), probe.size());
		LanDiscoveryServer found;
		bool answered = false;
		for (int f = 0; f < 60 && !answered; ++f) {
			CHECK(server.frame(kFrame));
			uint8_t rx[2048];
			net::Endpoint from{};
			const int n = net::udp_recv_from(browse.get(), rx, sizeof(rx), from, 10);
			if (n > 0) answered = parse_lan_discovery_reply(rx, static_cast<size_t>(n), ci, found);
		}
		CHECK(answered);
		CHECK(found.server_name == "Serve Test");
		CHECK(found.max_players == 9u);
		CHECK(found.current_players == 0u);
	}

	// --- a LAN joiner: the handshake, the spawn-gate burst, into the match.
	uint16_t joiner_port = 0;
	net::ScopedSocket joiner_sock(net::udp_bind(0, &joiner_port));
	CHECK(joiner_sock.is_valid());
	inmatch::ClientRuntime client("ServeJoiner");
	auto ship = [&](const std::vector<uint8_t> &d) {
		if (!d.empty()) net::udp_send_to(joiner_sock.get(), server_ep, d.data(), d.size());
	};
	auto drain = [&]() {
		uint8_t rx[4096];
		net::Endpoint from{};
		for (;;) {
			const int n = net::udp_recv_from(joiner_sock.get(), rx, sizeof(rx), from, 5);
			if (n <= 0) break;
			client.receive(rx, static_cast<size_t>(n));
		}
	};
	uint32_t tick = 1;
	ship(client.start());
	bool in_match = false;
	for (int f = 0; f < 900 && !in_match; ++f) {
		if (!server.frame(kFrame)) {
			std::printf("the session ended: %s\n", server.end_message().c_str());
			break;
		}
		drain();
		for (std::vector<uint8_t> &d : client.Client_ProcessNetworkFrame(tick)) ship(d);
		++tick;
		in_match = client.in_match();
	}
	CHECK(in_match);
	// The joiner plays: it deploys at the deathmatch start marker, and its body
	// is the host's pool-0 entity its connection owns.
	for (int f = 0; f < 600 && in_match && !client.is_deployed(); ++f) {
		if (!server.frame(kFrame)) break;
		drain();
		for (std::vector<uint8_t> &d : client.Client_ProcessNetworkFrame(tick)) ship(d);
		++tick;
	}
	CHECK(client.is_deployed());
	// The host holds the joiner as its one remote (type-1) connection.
	int remotes = 0;
	world::EntityHandle body{};
	for (const inmatch::NapiNPConnection &c : server.role().state.host_owner.ctx.np_protocol.connection_list) {
		if (c.type != inmatch::NapiNPConnection::kTypeServerSide) continue;
		++remotes;
		body = c.link.owned_entity;
	}
	CHECK(remotes == 1);
	CHECK(body.valid());
	CHECK(body.valid() && client.self_handle() == body.packed);
	std::printf("opennova_serve: joiner in match after %u client frames on UDP %u\n", tick,
			server.bound_port());

	server.stop();
	CHECK(!server.frame(kFrame));
	net::shutdown();
	std::error_code ec;
	fs::remove_all(dir, ec);
	if (failures != 0) {
		std::printf("opennova_serve: %d failure(s)\n", failures);
		return 1;
	}
	std::printf("opennova_serve: ok\n");
	return 0;
}
