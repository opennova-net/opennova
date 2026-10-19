// opennova-serve's NovaWorld listing driver end to end: the Lister (apps/serve/lister.h) over real
// loopback UDP against the real apps/novaworld_lobby NwUdpListener and an in-test gate, its source
// an in-test stand-in for the live match. The host result publishes nothing (D-NET-465); the
// mission start's update publishes the whole Host list and the roster; a roster edit removes one
// player while the other keeps its slot, at once; a renamed column rides the next 1860-tick
// refresh; the stop removes the row.
// [orig: CNapiGameSession_HandleHostVerifyResponse @0x4d5c52..0x4d5c58;
//  Game_StartMission @0x5248c6..0x524903]

#include "lister.h"

#include <base/gameprofile/gameprofile.h>
#include <net/title/title_identity.h>
#include "nw_udp_listener.h"
#include "program/server_config.h"

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
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// JO's title identity (net/title/title_identity.h), the identity these tests speak.
static const opennova::TitleIdentity &kJo = *opennova::title_identity(opennova::gameprofile::GAME_JO);

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
							 "VAR \"UDPNOVAWORLD\" \"127.0.0.1:" +
			std::to_string(nw_port) + "\"\r\n";
	std::vector<uint8_t> inner(body.begin(), body.end());
	opennova::nwu_decrypt(inner.data(), inner.size(), opennova::GATE_NWU_KEY);
	std::vector<uint8_t> reply(inner.size() + 16);
	std::size_t reply_size = 0;
	opennova::napi_envelope_encode(
			inner.data(), inner.size(), reply.data(), reply.size(), &reply_size);
	reply.resize(reply_size);
	while (!stop) {
		uint8_t rx[1024];
		opennova::net::Endpoint from{};
		if (opennova::net::udp_recv_from(gate, rx, sizeof(rx), from, 50) <= 0)
			continue;
		opennova::net::udp_send_to(gate, from, reply.data(), reply.size());
	}
}

opennova::HostPlayerSlot player(int slot, const char *name) {
	opennova::HostPlayerSlot p;
	p.slot = slot;
	p.player_name = name;
	p.type = "0";
	return p;
}

// The live match's stand-in: the columns and the roster the test sets, reported changed at the
// next pass, as opennova-serve's ServeListing reports its match.
class MatchSource final : public opennova::serve::ListingSource {
public:
	MatchSource(opennova::HostRegistration columns, std::vector<opennova::HostPlayerSlot> players) :
			columns_(std::move(columns)), players_(std::move(players)) {}

	bool start(int &exit_code) override {
		(void)exit_code;
		return true;
	}
	bool refresh(uint32_t now_ms, bool force) override {
		(void)now_ms;
		const bool changed = force || changed_;
		changed_ = false;
		return changed;
	}
	opennova::HostRegistration registration() const override { return columns_; }
	std::vector<opennova::HostPlayerSlot> wanted_roster(
			const std::map<int, opennova::HostPlayerSlot> &current) const override {
		(void)current;
		return players_;
	}
	void on_command(const opennova::ServerCommand &command) override { (void)command; }

	void set(opennova::HostRegistration columns, std::vector<opennova::HostPlayerSlot> players) {
		columns_ = std::move(columns);
		players_ = std::move(players);
		changed_ = true;
	}

private:
	opennova::HostRegistration columns_;
	std::vector<opennova::HostPlayerSlot> players_;
	bool changed_ = false;
};

opennova::HostRegistration columns(const char *server_name) {
	opennova::HostRegistration r;
	r.server_name = server_name;
	r.mission_name = "ASH_G11A";
	return r;
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
	manager.on_lost(
			[&listener](const opennova::Connection &connection, opennova::DropReason reason) {
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
	std::atomic<bool> stop_gate{ false };
	std::thread gate_thread(
			[&] { serve_gate(gate_server.get(), listener.bound_port(), stop_gate); });

	MatchSource source(columns("Lister E2E"), { player(0, "Alpha"), player(1, "Bravo") });
	opennova::serve::ListerOptions options;
	options.title = &kJo;
	options.gate_tag = options.title->gate_tag;
	options.master_host = "127.0.0.1";
	options.master_gate_port = gate_port;
	opennova::serve::Lister lister(options, source, nullptr);
	expect(lister.start(), "the lister binds");

	// The owner's loop, on a synthetic clock.
	uint32_t now = 0;
	bool running = true;
	auto tick = [&](uint32_t step_ms = 10) {
		now += step_ms;
		running = lister.tick(now);
		std::this_thread::sleep_for(2ms);
	};
	auto tick_until = [&](const std::function<bool()> &done, int rounds) {
		for (int i = 0; i < rounds && running && !done(); ++i)
			tick();
		return done();
	};
	auto row = [&](const std::string &name) -> std::optional<opennova::LobbyState> {
		for (const auto &h : listener.snapshot_hosted())
			if (h.lobby.hosting && h.lobby.server_name == name)
				return h.lobby;
		return std::nullopt;
	};
	auto slot_of = [](const std::optional<opennova::LobbyState> &lobby, const std::string &name) {
		if (!lobby)
			return -1;
		for (const auto &slot : lobby->roster)
			if (slot.player_name == name)
				return slot.slot;
		return -1;
	};
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

	expect(tick_until([&] { return lister.hosting() && row("Lister E2E").has_value(); }, 1500),
			"the lister verifies, hosts and is listed");
	tick_until([] { return false; }, 50);
	expect(row("Lister E2E").has_value() && host_var("Lister E2E", "MissionName").empty(),
			"the host result publishes nothing");

	// The mission start: the whole Host list and the roster at once.
	lister.publish_server_info();
	expect(tick_until([&] { return host_var("Lister E2E", "MissionName") == "ASH_G11A"; }, 300),
			"the mission start's update publishes the whole Host list");
	expect(tick_until(
				   [&] {
					   const auto lobby = row("Lister E2E");
					   return lobby && lobby->roster.size() == 2;
				   },
				   300),
			"the row lists both players");
	expect(slot_of(row("Lister E2E"), "Alpha") == 0 && slot_of(row("Lister E2E"), "Bravo") == 1,
			"the players hold their slots");

	// Alpha leaves, Charlie arrives in the freed slot, the server is renamed.
	source.set(columns("Lister E2E Renamed"), { player(1, "Bravo"), player(0, "Charlie") });
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
	expect(lister.exit_code() == opennova::serve::kExitStopped,
			"a stop finishes with kExitStopped");
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
	if (g_failures == 0) {
		std::printf("OK: opennova-serve's lister lists, edits and deregisters through the real "
					"listener\n");
		return 0;
	}
	std::fprintf(stderr, "%d assertion(s) failed\n", g_failures);
	return 1;
}
