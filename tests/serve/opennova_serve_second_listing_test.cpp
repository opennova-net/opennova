// opennova-serve listed on two NovaWorld services at once (ServeOptions::also_master_host, none of
// retail's: a stock host lists on one), with no retail data and nothing off loopback: two real
// apps/novaworld_lobby NwUdpListeners, each behind an in-test gate, and the loose game directory
// of the NovaWorld test. The first listing hosts before the boot, as ever; the second hosts once
// the starting map is up, on the same game socket (each service lists the server at the address
// it observed, the game socket's, D-NET-346), and carries the same match: its first full Host
// list at once (its host result published nothing, D-NET-465), the same name and the same
// cookie-key ring's PCIDKey. A ServerCommand from the second service runs on the match. The
// second service's ServerStopHosting delists the server there alone: the goodbye burst drops its
// row, the first listing and the match go on, and the second is not tried again. A second
// listing whose gate names no session server does not host and leaves the server serving, its
// next try scheduled. Throughout, its state as a supervisor reads it (--second-listing-file:
// trying, listed, not listed or retrying, with why, the next try and the slots of its service's
// players), and the console's line naming the service each joiner came through.
#include "server.h"
#include <base/gameprofile/gameprofile.h>
#include <net/protocol/protocol.h>
#include "serve_test_support.h"
#include "../npruntime/joiner_mission_start.h" // the harness's end of a released mission start

#include "lister.h"
#include "nw_udp_listener.h"
#include "program/server_config.h"

#include "net_sockets.h"

#include <net/napi/envelope.h>
#include <net/napi/session.h>
#include <net/novacrypto/nwu.h>
#include <net/novacrypto/pubcrypto.h> // encode_pub_value (the joiner's CD identity cookie)
#include <net/novaworld/client_session.h>
#include <net/novaworld/connection/manager.h>
#include <net/novaworld/gate_probe.h>
#include <net/novaworld/join_identity.h> // build_pub_join_identity_plaintexts
#include <net/novaworld/lobby_identity.h> // read_exe_basename (the joiner's AP)
#include <net/novaworld/lobby_session.h>
#include <net/novaworld/nwu_host_role.h>
#include <runtime/inmatch/client_runtime.h>
#include <runtime/inmatch/napi_np_server_ctx.h>

#include <base/io/log.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// JO's title identity (net/protocol/protocol.h), the identity these tests speak.
static const opennova::Protocol &kJo = *opennova::title_protocol("jo");

using namespace opennova;
namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {

int failures = 0;
#define CHECK(c)                                                                     \
	do {                                                                             \
		if (!(c)) {                                                                  \
			std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);                 \
			++failures;                                                              \
		}                                                                            \
	} while (0)

std::string var(const VarList &list, const char *name) {
	return var_has(list, name) ? var_value(list, name) : std::string("<absent>");
}

// The log lines the run wrote, for its own console lines (the joins' sources); printed too.
std::mutex g_log_mu;
std::vector<std::string> g_log;
void log_sink(io::LogLevel level, const char *message) {
	{
		std::lock_guard<std::mutex> lock(g_log_mu);
		g_log.emplace_back(message);
	}
	if (level != io::LogLevel::kDebug)
		std::printf("%s\n", message);
}
bool logged(const std::string &text) {
	std::lock_guard<std::mutex> lock(g_log_mu);
	for (const std::string &line : g_log)
		if (line.find(text) != std::string::npos)
			return true;
	return false;
}

// One NovaWorld service on loopback: the lobby listener and the gate that names it.
struct Service {
	ConnectionManager manager;
	novaworld_server::NwUdpListener listener{manager};
	uint16_t gate_port = 0;
	net::ScopedSocket gate;
	std::atomic<bool> stop_gate{false};
	std::thread gate_thread;

	bool start() {
		manager.on_lost([this](const Connection &connection, DropReason reason) {
			listener.erase_lobby_state(connection.addr, drop_reason_name(reason));
		});
		novaworld_server::ServerConfig config;
		config.nw_udp_port = 0;
		if (!listener.start(config)) return false;
		gate = net::ScopedSocket(net::udp_bind(0, &gate_port));
		gate_thread = std::thread(
				[this] { serve_test::serve_gate(gate.get(), listener.bound_port(), true, stop_gate); });
		return true;
	}
	void stop() {
		stop_gate = true;
		if (gate_thread.joinable()) gate_thread.join();
		listener.stop();
	}
	// The service's row for the server named `name`, while it lists it.
	std::optional<LobbyState> row(const std::string &name) const {
		for (const auto &h : listener.snapshot_hosted())
			if (h.lobby.hosting && h.lobby.server_name == name) return h.lobby;
		return std::nullopt;
	}
};

// A gate whose reply names no session server (no UDPNOVAWORLD): a listing through it has no
// NovaWorld session to host on.
void serve_sessionless_gate(net::Socket &gate, std::atomic<bool> &stop) {
	const std::string body = "VAR \"LOBBYNAME\" \"jop_2_consumer\"\r\n";
	std::vector<uint8_t> inner(body.begin(), body.end());
	nwu_decrypt(inner.data(), inner.size(), GATE_NWU_KEY);
	std::vector<uint8_t> reply(inner.size() + 16);
	std::size_t reply_size = 0;
	napi_envelope_encode(inner.data(), inner.size(), reply.data(), reply.size(), &reply_size);
	reply.resize(reply_size);
	while (!stop) {
		uint8_t rx[1024];
		net::Endpoint from{};
		if (net::udp_recv_from(gate, rx, sizeof(rx), from, 50) <= 0) continue;
		net::udp_send_to(gate, from, reply.data(), reply.size());
	}
}

// A ServerCommand line as the service's statement delivers it, onto `role`.
bool deliver_command(NwuHostRole &role, const std::string &cmd) {
	NapiMessage m;
	m.name = "ServerCommand";
	NapiField f;
	f.name = "Cmd";
	f.data.assign(cmd.begin(), cmd.end());
	m.fields.push_back(f);
	ClientSession::Notice notice;
	notice.kind = ClientSession::Notice::Kind::Command;
	if (!parse_server_command(m, notice.command)) return false;
	return role.handle_notice(notice);
}

constexpr double kFrame = 1.0 / 62.5;

} // namespace

int main() {
	io::set_log_sink(&log_sink);
	if (net::startup() != 0) return (std::printf("FAIL net::startup\n"), 1);

	Service first;
	Service second;
	if (!first.start() || !second.start()) return (std::printf("FAIL the services did not start\n"), 1);

	const fs::path dir = serve_test::fresh_dir("second_listing");
	const fs::path work = serve_test::fresh_dir("second_listing_cwd");
	serve_test::ScopedCwd cwd(work);
	CHECK(serve_test::write_bytes(dir / "SERVETST.BMS", serve_test::deathmatch_mission()));
	CHECK(serve_test::write_bytes(dir / "gametext.bin",
			serve_test::gametext({
					{"NovaWorld",
							{{"STRNOVA07", "Jungle"}, {"STRNOVA08", "Desert"}, {"STRNOVA09", "Snow"},
									{"STRNOVA10", "None"}, {"STRNOVA11", "Yes"}, {"STRNOVA12", "No"}}},
					{"GateTypeAbbrev", {{"DM", "DM"}}},
			})));
	{
		std::ofstream host(dir / "test.host", std::ios::binary);
		host << "GameName \"Serve Twice\"\r\n"
		     << "MaxPlayers 8\r\n"
		     << "Mission servetst.bms\r\n";
	}
	const std::string name = "Serve Twice";

	// A second listing needs the first.
	{
		serve::ServeOptions options;
		std::string error;
		CHECK(serve::parse_serve_options({"--resource-dir", dir.string(), "/HOST",
				(dir / "test.host").string(), "--also-master-host", "127.0.0.1"},
				options, error) == 1);
		CHECK(error.find("--master-host") != std::string::npos);
	}
	// ...and another service: the first's own gate twice is refused.
	{
		serve::ServeOptions options;
		std::string error;
		CHECK(serve::parse_serve_options({"--resource-dir", dir.string(), "/HOST",
				(dir / "test.host").string(), "--master-host", "NW.Example", "--also-master-host",
				"nw.example"},
				options, error) == 1);
		CHECK(error.find("another service") != std::string::npos);
	}

	// A run with no second listing reports it off.
	{
		serve::ServeOptions lan;
		const serve::Server idle(lan);
		CHECK(idle.second_listing_report() == serve::SecondListingReport{});
		CHECK(serve::second_listing_json(idle.second_listing_report()) == "{\"state\":\"off\"}\n");
	}

	std::string error;
	serve::ServeOptions options;
	CHECK(serve::parse_serve_options(
			{"--resource-dir", dir.string(), "/HOST", (dir / "test.host").string(), "--loose-root",
					"--lan-port", "0", "--master-host", "127.0.0.1", "--master-gate-port",
					std::to_string(first.gate_port), "--also-master-host", "127.0.0.1",
					"--also-master-gate-port", std::to_string(second.gate_port)},
			options, error) == 0);
	auto holder = std::make_unique<serve::Server>(options);
	const bool started = holder->start(error);
	if (!started) std::printf("start: %s\n", error.c_str());
	CHECK(started);
	if (!started) return 1;
	serve::Server &server = *holder;
	// The first listing hosted inside the start; the second's first try started with the session
	// (its gate lookup on a worker: its lister comes in a frame once that lands).
	CHECK(server.lister() != nullptr && server.lister()->hosting());
	CHECK(!server.second_listing_stopped());
	// Its state for the supervisor (--second-listing-file): its first try under way.
	{
		const serve::SecondListingReport r = server.second_listing_report();
		CHECK(r.state == serve::SecondListingState::Trying && r.attempt == 1 &&
				r.attempts == serve::kSecondListingTriesMax && r.reason.empty() && r.slots.empty());
	}

	// --- The second hosts on the game socket and publishes its first full lists at once.
	std::optional<LobbyState> a;
	std::optional<LobbyState> b;
	for (int f = 0; f < 1500; ++f) {
		if (!server.frame(kFrame)) break;
		std::this_thread::sleep_for(2ms);
		a = first.row(name);
		b = second.row(name);
		if (a && b && var(b->last_host_update["Host"], "GameType") != "<absent>") break;
	}
	CHECK(server.second_lister() != nullptr && server.second_lister()->hosting());
	CHECK(a.has_value() && b.has_value());
	if (!a || !b || server.second_lister() == nullptr) return 1;
	CHECK(a->host_ip == "127.0.0.1" && b->host_ip == "127.0.0.1");
	CHECK(a->host_port == server.bound_port());
	CHECK(b->host_port == server.bound_port());
	CHECK(var(b->last_host_update["Host"], "MissionName") == "Serve Test Map");
	CHECK(var(b->last_host_update["Host"], "Dedicated") == "Yes");
	CHECK(var(b->last_host_update["Host"], "MaxPlayers") == "8");
	CHECK(var(b->last_host_update["Host"], "GameType") == "DM");
	{
		serve::SecondListingReport listed;
		listed.state = serve::SecondListingState::Listed;
		CHECK(server.second_listing_report() == listed);
	}

	// --- The same match: once the round's refresh advanced the cookie-key ring, both services
	// carry its current key, the one every joiner's CD cookie decrypts under, whichever service
	// sent the joiner.
	const inmatch::NapiNPServerCtx &ctx = server.role().state.host_owner.ctx;
	for (int f = 0; f < 120 && ctx.cookie_key_table.count < 2; ++f) CHECK(server.frame(kFrame));
	CHECK(ctx.cookie_key_table.count >= 2);
	{
		const std::string key = std::to_string(ctx.cookie_key_table.current());
		bool both = false;
		for (int f = 0; f < 500 && !both; ++f) {
			CHECK(server.frame(kFrame));
			std::this_thread::sleep_for(2ms);
			a = first.row(name);
			b = second.row(name);
			both = a && b && var(a->last_host_update["Host"], "PCIDKey") == key &&
					var(b->last_host_update["Host"], "PCIDKey") == key;
		}
		CHECK(both);
	}
	// The match's own NovaWorld facts stay the first listing's. Each registration carries an AppId
	// of its own, as each retail host request mints one, and the match holds both: a joiner's
	// APPID says which service sent it.
	CHECK(ctx.novaworld_gsid == server.lister()->host_role().gsid());
	CHECK(ctx.nwu_session_role == ClientSession::kSessionRoleHosting);
	CHECK(server.second_lister()->host_role().app_id() != server.lister()->host_role().app_id());
	CHECK(a->app_id == std::to_string(ctx.novaworld_app_id));
	CHECK(b->app_id == std::to_string(ctx.novaworld_second_app_id));
	CHECK(ctx.novaworld_second_app_id != 0 && !ctx.novaworld_second_join_tickets_armed);

	// --- A joiner the second service sent: the .joi's APPID and the CD identity cookie under the
	// PCIDKey that service lists (each as the existing NovaWorld test builds them, /NWJoin.dll's
	// encoding), dialing the endpoint that service advertises. The match admits it, and both
	// services' PlayerLists carry it.
	const net::Endpoint game_ep{{127, 0, 0, 1}, static_cast<uint16_t>(b->host_port)};
	uint16_t joiner_port = 0;
	net::ScopedSocket joiner_sock(net::udp_bind(0, &joiner_port));
	inmatch::ClientRuntime client("SecondJoiner");
	client.set_protocol(&kJo);
	auto ship = [&](const std::vector<uint8_t> &d) {
		if (!d.empty()) net::udp_send_to(joiner_sock.get(), game_ep, d.data(), d.size());
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
	client.set_app_id(b->app_id);
	{
		const std::optional<LobbyState> keyed = second.row(name);
		CHECK(keyed && !keyed->pcid_key.empty());
		const PubJoinIdentityPlaintexts identity =
				build_pub_join_identity_plaintexts("A-SECOND-JOINER1", "SecondJoiner");
		std::vector<uint8_t> cd;
		for (const auto &[cookie, plain] : {std::pair{"PUBPCID", &identity.pcid},
				     std::pair{"PUBNAMEINFO", &identity.name_info},
				     std::pair{"PUBSQUADINFO", &identity.squad_info}}) {
			std::string value;
			CHECK(keyed && encode_pub_value(*plain, keyed->pcid_key, value));
			cd.insert(cd.end(), cookie, cookie + std::char_traits<char>::length(cookie));
			cd.push_back(0);
			cd.insert(cd.end(), value.begin(), value.end());
			cd.push_back(0);
		}
		client.set_join_cd_cookie(std::move(cd));
	}
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
		push_released_mission_start(client, ship);
		++tick;
		in_match = client.in_match();
	}
	CHECK(in_match);
	{
		bool listed_twice = false;
		for (int f = 0; f < 300 && !listed_twice; ++f) {
			if (!server.frame(kFrame)) break;
			drain();
			for (std::vector<uint8_t> &d : client.Client_ProcessNetworkFrame(tick)) ship(d);
			push_released_mission_start(client, ship);
			++tick;
			a = first.row(name);
			b = second.row(name);
			listed_twice = a && b && a->roster.size() == 1 && b->roster.size() == 1;
		}
		CHECK(listed_twice);
		if (listed_twice) {
			CHECK(a->roster[0].player_name == "SecondJoiner");
			CHECK(b->roster[0].player_name == "SecondJoiner");
			CHECK(b->roster[0].ip_and_port == "127.0.0.1:" + std::to_string(joiner_port));
		}
		// The match tagged the joiner as the second listing's: its APPID was that listing's.
		// Its node keeps the executable its 0x42's AP named: this binary's, as the joiner reads it.
		const std::string joiner_exe = read_exe_basename().substr(0, kLobbyApChars);
		bool tagged = false;
		int joiner_slot = -1;
		std::string joiner_application;
		for (const inmatch::NapiNPConnection &conn : ctx.np_protocol.connection_list)
			if (conn.type == inmatch::NapiNPConnection::kTypeServerSide &&
					conn.peer.port == joiner_port) {
				tagged = conn.novaworld_listing == 1;
				joiner_slot = static_cast<int>(conn.reply.player_slot);
				joiner_application = conn.application;
			}
		CHECK(tagged);
		CHECK(!joiner_exe.empty() && joiner_application == joiner_exe);
		// The supervisor's state names its slot among the second listing's players, and the
		// console named the service it came through (none of the retail lines does).
		CHECK(server.second_listing_report().state == serve::SecondListingState::Listed);
		CHECK(server.second_listing_report().slots == std::vector<int>{ joiner_slot });
		CHECK(logged("[join] \"SecondJoiner\" via OpenNova's NovaWorld (127.0.0.1:" +
				std::to_string(second.gate_port) + ") running " + joiner_exe));
	}

	// --- A ServerCommand from the second service runs on the match, saved as the first's are.
	CHECK(deliver_command(server.second_lister()->host_role(),
			"SetServerMsg \"set by the second service\""));
	CHECK(ctx.config.custom_text == "set by the second service");
	CHECK(server.game_cfg().servermsg == "set by the second service");
	CHECK(server.frame(kFrame));

	// --- The second service stops its listing: delisted there alone, for the rest of the run.
	{
		ClientSession::Notice notice;
		notice.kind = ClientSession::Notice::Kind::StopHosting;
		notice.fields.msg_code = 7;
		CHECK(server.second_lister()->host_role().handle_notice(notice));
		CHECK(server.second_lister()->stopped_by_service());
		CHECK(server.frame(kFrame));
		CHECK(server.second_lister() == nullptr);
		CHECK(server.second_listing_stopped());
		// Not listed for the run, saying why; its service's joiner is still in the match.
		{
			const serve::SecondListingReport r = server.second_listing_report();
			CHECK(r.state == serve::SecondListingState::NotListed &&
					r.reason == "the service stopped the hosting" && r.tag.empty());
			CHECK(r.slots.size() == 1);
		}
		// Its goodbye burst reached the second service, which drops the row.
		bool dropped = false;
		for (int i = 0; i < 100 && !dropped; ++i) {
			CHECK(server.frame(kFrame));
			std::this_thread::sleep_for(20ms);
			dropped = !second.row(name).has_value();
		}
		CHECK(dropped);
		// The first listing and the match go on.
		CHECK(server.running());
		CHECK(server.lister() != nullptr && server.lister()->hosting());
		CHECK(first.row(name).has_value());
		CHECK(ctx.nwu_session_role == ClientSession::kSessionRoleHosting);
		for (int f = 0; f < 60; ++f) CHECK(server.frame(kFrame));
		CHECK(server.second_lister() == nullptr);
	}

	// --- The stop deregisters the first listing.
	server.stop();
	{
		bool gone = false;
		for (int i = 0; i < 100 && !gone; ++i) {
			std::this_thread::sleep_for(20ms);
			gone = !first.row(name).has_value();
		}
		CHECK(gone);
	}
	holder.reset();

	// --- A re-host the second service refuses (its ServerHostResult refusing the hosting that
	// stood, as after its session's reconnect) is that service's decision: the second listing
	// ends for the run with no retry, as a stock host's refused listing ends, and the first
	// listing and the match go on.
	{
		const fs::path work_rehost = serve_test::fresh_dir("second_listing_rehost_cwd");
		serve_test::ScopedCwd rehost_cwd(work_rehost);
		serve::ServeOptions rehost_options;
		std::string rehost_error;
		CHECK(serve::parse_serve_options(
				{"--resource-dir", dir.string(), "/HOST", (dir / "test.host").string(),
						"--loose-root", "--lan-port", "0", "--master-host", "127.0.0.1",
						"--master-gate-port", std::to_string(first.gate_port),
						"--also-master-host", "127.0.0.1", "--also-master-gate-port",
						std::to_string(second.gate_port)},
				rehost_options, rehost_error) == 0);
		auto rehost = std::make_unique<serve::Server>(rehost_options);
		const bool rehost_started = rehost->start(rehost_error);
		if (!rehost_started) std::printf("start: %s\n", rehost_error.c_str());
		CHECK(rehost_started);
		if (rehost_started) {
			bool hosting = false;
			for (int f = 0; f < 1500 && !hosting; ++f) {
				CHECK(rehost->frame(kFrame));
				std::this_thread::sleep_for(2ms);
				hosting = rehost->second_lister() != nullptr && rehost->second_lister()->hosting();
			}
			CHECK(hosting);
			if (hosting) {
				ClientSession::Notice refused;
				refused.kind = ClientSession::Notice::Kind::HostResult;
				refused.fields.success = 0;
				CHECK(rehost->second_lister()->host_role().handle_notice(refused));
				CHECK(!rehost->second_lister()->hosting());
				CHECK(rehost->frame(kFrame));
				CHECK(rehost->second_lister() == nullptr);
				CHECK(rehost->second_listing_stopped());
				CHECK(rehost->second_listing_report().state ==
								serve::SecondListingState::NotListed &&
						rehost->second_listing_report().reason ==
								"the service refused the hosting again after its session "
								"reconnected");
				CHECK(rehost->running());
				CHECK(rehost->lister() != nullptr && rehost->lister()->hosting());
			}
			rehost->stop();
		}
		rehost.reset();
		rehost_cwd.restore();
		std::error_code ec;
		fs::remove_all(work_rehost, ec);
	}

	// --- A second listing whose gate names no session server: it does not host, the server
	// serves on its first listing, and the next try waits out the backoff.
	{
		uint16_t bare_port = 0;
		net::ScopedSocket bare_gate(net::udp_bind(0, &bare_port));
		std::atomic<bool> stop_bare{false};
		std::thread bare_thread([&] { serve_sessionless_gate(bare_gate.get(), stop_bare); });
		const fs::path work_bare = serve_test::fresh_dir("second_listing_bare_cwd");
		serve_test::ScopedCwd bare_cwd(work_bare);
		serve::ServeOptions bare_options;
		std::string bare_error;
		CHECK(serve::parse_serve_options(
				{"--resource-dir", dir.string(), "/HOST", (dir / "test.host").string(),
						"--loose-root", "--lan-port", "0", "--master-host", "127.0.0.1",
						"--master-gate-port", std::to_string(first.gate_port),
						"--also-master-host", "127.0.0.1", "--also-master-gate-port",
						std::to_string(bare_port)},
				bare_options, bare_error) == 0);
		auto bare = std::make_unique<serve::Server>(bare_options);
		const bool bare_started = bare->start(bare_error);
		if (!bare_started) std::printf("start: %s\n", bare_error.c_str());
		CHECK(bare_started);
		if (bare_started) {
			// Its gate lookup, its lister, the gate's reply, then the failed try's schedule.
			for (int f = 0; f < 500 && bare->second_listing_retry_at_ms() == 0; ++f) {
				CHECK(bare->frame(kFrame));
				std::this_thread::sleep_for(2ms);
			}
			CHECK(bare->second_lister() == nullptr);
			CHECK(!bare->second_listing_stopped());
			CHECK(bare->second_listing_retry_at_ms() != 0);
			// Retrying: the next try's number and time (a minute on), and the fault's reason.
			{
				const serve::SecondListingReport r = bare->second_listing_report();
				const int64_t now = static_cast<int64_t>(std::time(nullptr));
				CHECK(r.state == serve::SecondListingState::Retrying && r.attempt == 2 &&
						r.attempts == serve::kSecondListingTriesMax);
				CHECK(r.next_try_at >= now + 50 && r.next_try_at <= now + 61);
				// A gate reply with no session server and no POST pair: the gate's state -9.
				CHECK(r.reason ==
								"the gate named no session server and lacks its POST address or "
								"port" &&
						r.tag == "NWEC25");
			}
			CHECK(bare->running());
			CHECK(bare->lister() != nullptr && bare->lister()->hosting());
			// The backoff holds the next try.
			for (int f = 0; f < 60; ++f) CHECK(bare->frame(kFrame));
			CHECK(bare->second_lister() == nullptr);
			bare->stop();
		}
		bare.reset();
		stop_bare = true;
		bare_thread.join();
		bare_cwd.restore();
		std::error_code ec;
		fs::remove_all(work_bare, ec);
	}

	first.stop();
	second.stop();
	net::shutdown();
	cwd.restore();
	std::error_code ec;
	fs::remove_all(dir, ec);
	fs::remove_all(work, ec);
	io::set_log_sink(nullptr);
	if (failures != 0) {
		std::printf("opennova_serve_second_listing: %d failure(s)\n", failures);
		return 1;
	}
	std::printf("opennova_serve_second_listing: ok\n");
	return 0;
}
