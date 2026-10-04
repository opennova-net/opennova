// nw-lister end to end: the app's Lister (apps/nw_lister) over real loopback UDP against the real
// apps/novaworld_server NwUdpListener and an in-test gate. The row appears with the listing's
// players; a listing edit removes one player while the other keeps its slot; a renamed column
// rides the next 1860-tick refresh; the stop removes the row.

#include "lister.h"
#include "nw_udp_listener.h"
#include "server_config.h"

#include "net_sockets.h"

#include <net/napi/envelope.h>
#include <net/novacrypto/nwu.h>
#include <net/novaworld/connection/manager.h>
#include <net/novaworld/gate_probe.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

int g_failures = 0;

void expect(bool cond, const char *what) {
	if (!cond) {
		std::fprintf(stderr, "FAIL: %s\n", what);
		++g_failures;
	}
}

// A gate that answers each probe once, naming the listener as UDPNOVAWORLD.
void serve_gate(opennova::net::Socket &gate, uint16_t nw_port, std::atomic<bool> &stop) {
	const std::string body = "VAR \"LOBBYNAME\" \"jop_2_consumer\"\r\n"
	                         "VAR \"UDPNOVAWORLD\" \"127.0.0.1:" + std::to_string(nw_port) + "\"\r\n";
	std::vector<uint8_t> inner(body.begin(), body.end());
	opennova::nwu_decrypt(inner.data(), inner.size(), opennova::GATE_NWU_KEY);
	std::vector<uint8_t> reply(inner.size() + 16);
	std::size_t reply_size = 0;
	opennova::napi_envelope_encode(inner.data(), inner.size(), reply.data(), reply.size(), &reply_size);
	reply.resize(reply_size);
	while (!stop) {
		uint8_t rx[1024];
		opennova::net::Endpoint from{};
		if (opennova::net::udp_recv_from(gate, rx, sizeof(rx), from, 50) <= 0) continue;
		opennova::net::udp_send_to(gate, from, reply.data(), reply.size());
	}
}

void write_listing(const std::filesystem::path &path, const std::string &json, int age_s) {
	std::ofstream(path, std::ios::binary | std::ios::trunc) << json;
	// A fresh mtime per edit, whatever the file system's timestamp granularity.
	std::filesystem::last_write_time(path, std::filesystem::file_time_type::clock::now() +
	                                           std::chrono::seconds(age_s));
}

} // namespace

int main() {
	using namespace std::chrono_literals;
	if (opennova::net::startup() != 0) {
		std::fprintf(stderr, "FAIL: net::startup\n");
		return 1;
	}

	opennova::ConnectionManager manager;
	opennova::server::NwUdpListener listener(manager);
	manager.on_lost([&listener](const opennova::Connection &connection, opennova::DropReason reason) {
		listener.erase_lobby_state(connection.addr, opennova::drop_reason_name(reason));
	});
	opennova::server::ServerConfig config;
	config.nw_udp_port = 0;
	if (!listener.start(config)) {
		std::fprintf(stderr, "FAIL: listener.start\n");
		return 1;
	}
	std::this_thread::sleep_for(50ms);

	uint16_t gate_port = 0;
	opennova::net::ScopedSocket gate_server(opennova::net::udp_bind(0, &gate_port));
	std::atomic<bool> stop_gate{false};
	std::thread gate_thread([&] { serve_gate(gate_server.get(), listener.bound_port(), stop_gate); });

	const auto listing = std::filesystem::temp_directory_path() / "nw_lister_e2e_listing.json";
	write_listing(listing, R"({"server_name": "Lister E2E", "mission": "ASH_G11A", "players": ["Alpha", "Bravo"]})",
	              0);

	opennova::lister::ListerOptions options;
	options.listing_path = listing.string();
	options.master_host = "127.0.0.1";
	options.master_gate_port = gate_port;
	opennova::lister::Lister lister(options);
	expect(lister.start(), "the lister loads the listing and binds");

	// The owner's loop, on a synthetic clock.
	uint32_t now = 0;
	bool running = true;
	auto tick = [&](uint32_t step_ms = 10) {
		now += step_ms;
		running = lister.tick(now);
		std::this_thread::sleep_for(2ms);
	};
	auto tick_until = [&](const std::function<bool()> &done, int rounds) {
		for (int i = 0; i < rounds && running && !done(); ++i) tick();
		return done();
	};
	auto row = [&](const std::string &name) -> std::optional<opennova::LobbyState> {
		for (const auto &h : listener.snapshot_hosted())
			if (h.lobby.hosting && h.lobby.server_name == name) return h.lobby;
		return std::nullopt;
	};
	auto slot_of = [](const std::optional<opennova::LobbyState> &lobby, const std::string &name) {
		if (!lobby) return -1;
		for (const auto &slot : lobby->roster)
			if (slot.player_name == name) return slot.slot;
		return -1;
	};

	expect(tick_until([&] { return lister.hosting(); }, 1500), "the lister verifies and hosts");
	expect(tick_until([&] {
		       const auto lobby = row("Lister E2E");
		       return lobby && lobby->roster.size() == 2;
	       }, 300),
	       "the row lists both players");
	expect(slot_of(row("Lister E2E"), "Alpha") == 0 && slot_of(row("Lister E2E"), "Bravo") == 1,
	       "the players take the lowest free slots");

	// Alpha leaves, Charlie arrives, the server is renamed.
	write_listing(listing,
	              R"({"server_name": "Lister E2E Renamed", "mission": "ASH_G11A", "players": ["Bravo", "Charlie"]})",
	              5);
	tick(2100); // past the listing check
	expect(tick_until([&] { return slot_of(row("Lister E2E"), "Charlie") >= 0; }, 300),
	       "the roster change goes out at once");
	expect(slot_of(row("Lister E2E"), "Alpha") < 0, "the leaving player's slot is removed");
	expect(slot_of(row("Lister E2E"), "Bravo") == 1, "the staying player keeps its slot");
	expect(slot_of(row("Lister E2E"), "Charlie") == 0, "the arriving player takes the freed slot");
	expect(!row("Lister E2E Renamed"), "the renamed column waits for the refresh");
	tick(30000); // past SESSION_HOST_INFO_REFRESH_TICKS (1860 ticks, ~29.8 s)
	expect(tick_until([&] { return row("Lister E2E Renamed").has_value(); }, 300),
	       "the 1860-tick refresh carries the renamed column");

	lister.stop();
	expect(lister.exit_code() == opennova::lister::kExitStopped, "a stop exits 0");
	bool gone = false;
	for (int i = 0; i < 100 && !gone; ++i) {
		std::this_thread::sleep_for(20ms);
		gone = !row("Lister E2E Renamed");
	}
	expect(gone, "the stop removes the row");

	stop_gate = true;
	gate_thread.join();
	listener.stop();
	opennova::net::shutdown();
	std::filesystem::remove(listing);
	if (g_failures == 0) {
		std::printf("OK: nw-lister lists, edits and deregisters through the real listener\n");
		return 0;
	}
	std::fprintf(stderr, "%d assertion(s) failed\n", g_failures);
	return 1;
}
