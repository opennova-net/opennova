// P7 Part 2 (B2) — the NovaWorld lobby var-builders (libs/novaworld/lobby_vars). Proves the moved
// builders reproduce the Godot bindings' hand-assembly byte-for-byte: the ClientHostRequest /
// ClientHostUpdate NapiMessage serialization, the NW-S5 identity set, and the host:port split.
// Server-free, Godot-free — the independent ctest-green bar for the B2 move.

#include "novaworld/lobby_vars.h"

#include <napi/session.h>
#include <napi/tlv.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace nw = opennova;

static int g_fail = 0;
static bool expect(bool cond, const char *msg) {
	if (!cond) {
		std::fprintf(stderr, "FAIL: %s\n", msg);
		++g_fail;
	}
	return cond;
}

static std::vector<uint8_t> serialize(const nw::NapiMessage &msg) {
	const std::size_t n = nw::napi_message_size(msg);
	std::vector<uint8_t> out(n);
	std::size_t written = 0;
	const int rc = nw::napi_message_encode(msg, out.data(), out.size(), &written);
	expect(rc == 0 && written == n, "napi_message_encode succeeds and fills the buffer");
	out.resize(written);
	return out;
}

// The OLD hand-assembly the binding did inline (NovaWorldHost::build_host_vars), reproduced here so
// the test pins the new builder against the exact bytes it replaced.
static std::vector<nw::ClientVar> old_build_host_vars(const nw::HostRegistration &c) {
	std::vector<nw::ClientVar> host;
	host.push_back({0, "ServerName", c.server_name});
	host.push_back({0, "ServerPortNumber", std::to_string(c.game_port)});
	host.push_back({0, "Players", std::to_string(c.player_count)});
	host.push_back({0, "MaxPlayers", std::to_string(c.max_players)});
	host.push_back({0, "Region", c.region});
	if (!c.advertise_ip.empty()) host.push_back({0, "ServerIP", c.advertise_ip});
	if (!c.mission_name.empty()) host.push_back({0, "MissionName", c.mission_name});
	return host;
}

static nw::NapiMessage old_make_host_request(const nw::HostRegistration &c, const std::string &nwuid) {
	std::vector<nw::ClientVar> cookie = {{0, "NWUID", nwuid}};
	std::vector<nw::ClientVar> host_setup = {
		{0, "AppId", c.app_id},
		{0, "LobbyName", c.lobby_name},
		{0, "MaxPlayers", std::to_string(c.max_players)},
		{0, "ServerPortNumber", std::to_string(c.game_port)},
	};
	std::vector<nw::ClientVar> player_list = {{0, "Slot0", c.player_name}};
	return nw::make_client_host_request(1, cookie, host_setup, old_build_host_vars(c), player_list);
}

static nw::NapiMessage old_make_host_update(const nw::HostRegistration &c) {
	std::vector<nw::ClientVar> player_list = {{0, "Slot0", c.player_name}};
	return nw::make_client_host_update(old_build_host_vars(c), player_list);
}

static void check_host_request_byte_equal(const nw::HostRegistration &cfg, const char *label) {
	const std::vector<uint8_t> a = serialize(old_make_host_request(cfg, "NWUID-XYZ"));
	const std::vector<uint8_t> b = serialize(nw::make_host_request(cfg, "NWUID-XYZ"));
	expect(a == b, label);
}

int main() {
	// 1. ClientHostRequest byte-equality — both conditional columns set.
	{
		nw::HostRegistration cfg;
		cfg.server_name = "Taylor's Game";
		cfg.mission_name = "ASH_G11A";
		cfg.max_players = 16;
		cfg.region = "eu";
		cfg.player_name = "Taylor";
		cfg.game_port = 40000;
		cfg.advertise_ip = "203.0.113.10";
		cfg.app_id = "28";
		cfg.lobby_name = "jop_2_consumer";
		cfg.player_count = 3;
		check_host_request_byte_equal(cfg, "make_host_request == hand-assembly (full)");

		const std::vector<uint8_t> u_old = serialize(old_make_host_update(cfg));
		const std::vector<uint8_t> u_new = serialize(nw::make_host_update(cfg));
		expect(u_old == u_new, "make_host_update == hand-assembly (full)");
	}

	// 2. ClientHostRequest/Update byte-equality — both optionals empty (the default ServerIP/MissionName).
	{
		nw::HostRegistration cfg; // defaults: no advertise_ip, no mission_name
		check_host_request_byte_equal(cfg, "make_host_request == hand-assembly (defaults, no optionals)");

		const std::vector<uint8_t> u_old = serialize(old_make_host_update(cfg));
		const std::vector<uint8_t> u_new = serialize(nw::make_host_update(cfg));
		expect(u_old == u_new, "make_host_update == hand-assembly (defaults)");
	}

	// 3. The NW-S5 identity set: names + order + the three empties + the fingerprint seed math.
	{
		nw::LobbyIdentityParams p;
		p.client_index = 0x11112222u;
		p.client_key = 0x33334444u;
		p.tz_bias = "-480";
		const auto vars = nw::make_lobby_identity_vars(p);
		expect(vars.size() == 10, "identity set has 10 vars");
		const char *names[] = {"CountryName", "Language",     "TimeZoneBias", "MyInstalledExpBits",
		                       "NWUID",       "NWCDKIID",     "NWCDKIIDEXP1", "NWPSSK",
		                       "NWUSID",      "NWHWI"};
		bool order_ok = vars.size() == 10;
		for (std::size_t i = 0; i < vars.size() && order_ok; ++i) {
			if (vars[i].first != names[i]) order_ok = false;
		}
		expect(order_ok, "identity set names + order match the NW-S5 list");
		expect(vars[0].second == "United States", "CountryName default");
		expect(vars[1].second == "English", "Language default");
		expect(vars[2].second == "-480", "TimeZoneBias passes through");
		expect(vars[3].second == "0", "MyInstalledExpBits default");
		expect(vars[4].second.empty() && vars[5].second.empty() && vars[6].second.empty(),
		       "NWUID / NWCDKIID / NWCDKIIDEXP1 are empty");
		expect(vars[7].second == nw::az_fingerprint(p.client_index ^ 0x5053534Bu, 23),
		       "NWPSSK = az_fingerprint(ci ^ 0x5053534B, 23)");
		expect(vars[8].second == nw::az_fingerprint(p.client_key ^ 0x55534944u, 16),
		       "NWUSID = az_fingerprint(ck ^ 0x55534944, 16)");
		expect(vars[9].second == p.nwhwi, "NWHWI passes through");
		// az_fingerprint is deterministic and length-exact.
		expect(nw::az_fingerprint(123, 23).size() == 23, "az_fingerprint length 23");
		expect(nw::az_fingerprint(123, 16) == nw::az_fingerprint(123, 16), "az_fingerprint deterministic");
	}

	// 4. parse_host_port — good / no-colon / empty-port / non-numeric-port.
	{
		std::string host;
		uint16_t port = 0;
		expect(nw::parse_host_port("1.2.3.4:7597", host, port) && host == "1.2.3.4" && port == 7597,
		       "parse_host_port good case");
		host.clear();
		port = 123;
		expect(!nw::parse_host_port("nohostport", host, port), "parse_host_port rejects no colon");
		expect(!nw::parse_host_port("1.2.3.4:", host, port), "parse_host_port rejects empty port");
		expect(!nw::parse_host_port("1.2.3.4:abc", host, port), "parse_host_port rejects non-numeric port");
	}

	if (g_fail == 0) {
		std::printf("lobby_vars_test: all checks passed\n");
	}
	return g_fail == 0 ? 0 : 1;
}
