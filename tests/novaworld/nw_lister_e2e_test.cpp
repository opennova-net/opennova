// opennova-nw-lister end to end: the app's Lister (apps/nw_lister) over real loopback UDP against
// the real apps/novaworld_server NwUdpListener and an in-test gate. The row appears with the
// listing's players and, the listing having no mission start, the whole Host list its host result
// publishes in that start's stead (D-NET-488); a listing edit removes one player while the other
// keeps its slot; a renamed column rides the next 1860-tick refresh; a reported mission change
// publishes the whole list at once (D-NET-488); the stop removes the row. Then
// an embedder's source whose server starts missions (opennova-serve's shape): its host result
// publishes nothing (D-NET-465) and the mission start's update publishes the whole Host list.

#include "lister.h"
#include "nw_udp_listener.h"
#include "server_config.h"

#include "net_sockets.h"

#include <net/napi/envelope.h>
#include <net/novacrypto/nwu.h>
#include <net/novaworld/connection/manager.h>
#include <net/novaworld/gate_probe.h>
#include <net/novaworld/lobby_session.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
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

// An embedder's source whose server starts missions, as opennova-serve's live match does: the
// embedder runs Lister::publish_server_info at each mission start.
class MissionSource final : public opennova::nw_lister::ListingSource {
public:
	bool start(int &exit_code) override {
		(void)exit_code;
		return true;
	}
	bool refresh(uint32_t now_ms, bool force) override {
		(void)now_ms;
		return force;
	}
	opennova::HostRegistration registration() const override {
		opennova::HostRegistration r;
		r.server_name = "Lister Missions";
		r.mission_name = "SERVETST";
		return r;
	}
	std::vector<opennova::HostPlayerSlot> wanted_roster(
			const std::map<int, opennova::HostPlayerSlot> &current) const override {
		(void)current;
		return {};
	}
	void on_command(const opennova::ServerCommand &command) override { (void)command; }
	bool starts_missions() const override { return true; }
};

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
	opennova::novaworld_server::NwUdpListener listener(manager);
	manager.on_lost([&listener](const opennova::Connection &connection, opennova::DropReason reason) {
		listener.erase_lobby_state(connection.addr, opennova::drop_reason_name(reason));
	});
	opennova::novaworld_server::ServerConfig config;
	config.nw_udp_port = 0;
	if (!listener.start(config)) {
		std::fprintf(stderr, "FAIL: listener.start\n");
		return 1;
	}

	uint16_t gate_port = 0;
	opennova::net::ScopedSocket gate_server(opennova::net::udp_bind(0, &gate_port));
	std::atomic<bool> stop_gate{false};
	std::thread gate_thread([&] { serve_gate(gate_server.get(), listener.bound_port(), stop_gate); });

	const auto listing = std::filesystem::temp_directory_path() / "nw_lister_e2e_listing.json";
	write_listing(listing, R"({"server_name": "Lister E2E", "mission": "ASH_G11A", "players": ["Alpha", "Bravo"]})",
	              0);

	opennova::nw_lister::ListerOptions options;
	options.listing_path = listing.string();
	options.master_host = "127.0.0.1";
	options.master_gate_port = gate_port;
	opennova::nw_lister::Lister lister(options);
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
	// The value of `name` in the row's kept Host list ("" when absent). The fresh request carries
	// BuildHostVarLists' first run alone, so a column only the server-info update writes
	// (MissionName) reaches the service with a ClientHostUpdate.
	auto host_var = [&](const std::string &server, const char *name) {
		const auto lobby = row(server);
		if (!lobby)
			return std::string();
		const auto list = lobby->last_host_update.find("Host");
		return list == lobby->last_host_update.end() ? std::string()
													 : opennova::var_value(list->second, name);
	};
	// A listing has no mission start, whose update carries a fresh host's first full Host list:
	// the lister runs that update at its host result instead (D-NET-488, PERMANENT).
	expect(host_var("Lister E2E", "MissionName") == "ASH_G11A",
			"the listing's host result publishes the whole Host list");

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

	// A mission change the listing reports stands in for the mission start the lister cannot
	// see: the whole Host list goes out at once, the renamed column with it, well before the
	// next refresh (D-NET-488). [orig: Game_StartMission @0x5248c6..0x524903]
	write_listing(listing,
			R"({"server_name": "Lister E2E Moved", "mission": "ASH_G12A", "players": ["Bravo", "Charlie"]})",
			10);
	tick(2100); // past the listing check
	const bool moved = tick_until(
			[&] { return host_var("Lister E2E Moved", "MissionName") == "ASH_G12A"; }, 300);
	expect(moved, "a reported mission change publishes the mission start's update at once");

	lister.stop();
	expect(lister.exit_code() == opennova::nw_lister::kExitStopped, "a stop exits 0");
	bool gone = false;
	for (int i = 0; i < 100 && !gone; ++i) {
		std::this_thread::sleep_for(20ms);
		gone = !row("Lister E2E Moved");
	}
	expect(gone, "the stop removes the row");

	// An embedder's source that starts missions: the host result publishes nothing, so the row
	// keeps the request's Host list until the mission start's update publishes both lists.
	// [orig: CNapiGameSession_HandleHostVerifyResponse @0x4d5c52..0x4d5c58;
	//  Game_StartMission @0x5248c6..0x524903]
	{
		MissionSource source;
		opennova::nw_lister::ListerOptions embedded;
		embedded.master_host = "127.0.0.1";
		embedded.master_gate_port = gate_port;
		opennova::nw_lister::Lister host(embedded, source, nullptr);
		expect(host.start(), "the embedder's lister binds");
		uint32_t host_now = 0;
		bool host_running = true;
		auto host_until = [&](const std::function<bool()> &done, int rounds) {
			for (int i = 0; i < rounds && host_running && !done(); ++i) {
				host_now += 10;
				host_running = host.tick(host_now);
				std::this_thread::sleep_for(2ms);
			}
			return done();
		};
		expect(host_until(
					   [&] { return host.hosting() && row("Lister Missions").has_value(); }, 1500),
				"the embedder's lister hosts and is listed");
		host_until([] { return false; }, 50);
		expect(row("Lister Missions").has_value() &&
						host_var("Lister Missions", "MissionName").empty(),
				"the host result of a source that starts missions publishes nothing");
		host.publish_server_info();
		expect(host_until([&] { return host_var("Lister Missions", "MissionName") == "SERVETST"; },
					   300),
				"the mission start's update publishes the whole Host list");
		host.stop();
		bool unlisted = false;
		for (int i = 0; i < 100 && !unlisted; ++i) {
			std::this_thread::sleep_for(20ms);
			unlisted = !row("Lister Missions");
		}
		expect(unlisted, "the embedder's stop removes the row");
	}

	stop_gate = true;
	gate_thread.join();
	listener.stop();
	opennova::net::shutdown();
	std::filesystem::remove(listing);
	if (g_failures == 0) {
		std::printf("OK: opennova-nw-lister lists, edits and deregisters through the real listener\n");
		return 0;
	}
	std::fprintf(stderr, "%d assertion(s) failed\n", g_failures);
	return 1;
}
