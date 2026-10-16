// Several lobby instances on one database (apps/novaworld_lobby/nw_udp_listener.h,
// gate_listener.h; engine/net/novaworld/lobby_repository.h): two real NovaWorld UDP listeners,
// two lobbies each registered as an instance of its own, and a real gate, over loopback
// UDP in one process.
//
//   - each listener registers its instance as it starts, at its endpoint;
//   - the gate spreads clients across the active instances by their address (pick_lobby: every
//     instance takes a share of a thousand addresses, an address always the same one) and a
//     client's retries land on the same lobby; real probes from several loopback addresses
//     reach both;
//   - a host listed on each: their RIDs differ (one sequence), each listing and session row names
//     its instance, and a push queued as web queues it reaches the host on its own
//     instance alone;
//   - forget_owner goes to both instances, each taking its own;
//   - an instance drained gets no new client from the gate (every probe the other's), and both
//     listings stay;
//   - an instance's row reaped while its lobby runs (an outage past the stale window): the lobby's
//     next heartbeat registers it again and writes its session rows and listing anew, and a push
//     reaches its host;
//   - a lobby configured with a running one's endpoint refuses to start, and one whose config
//     names no endpoint (none is derived from the port it binds);
//   - a flood of malformed datagrams at a lobby: its receive batch is bounded, so a push still
//     reaches its host and its heartbeat still moves its row;
//   - a gate whose read of the instances failed hands its fallback lobby (an instance it read
//     may have drained and stopped since) and reads again only kLobbyRetryFactor refreshes later;
//   - a drained instance whose row is reaped while its lobby runs registers again drained, its
//     own lobby's drain or an admin's through the row (which its heartbeat reads);
//   - a lobby's start takes over its endpoint's row once it stopped moving and clears that
//     previous run's rows, reaps an instance unseen past the stale window with its rows, and
//     leaves one unseen for two minutes (a database failover's outage) and the other
//     instances' rows alone;
//   - a lobby's sweep deletes its row of no session it holds (a drop whose delete failed);
//   - a clean stop deletes the instance's row.

#include "gate_listener.h"
#include "nw_udp_listener.h"
#include "server_config.h"

#include <base/gameprofile/gameprofile.h>
#include <net/napi/envelope.h>
#include <net/napi/session.h>
#include <net/novaworld/client_session.h>
#include <net/novaworld/connection/manager.h>
#include <net/novaworld/db/database.h>
#include <net/novaworld/gate_probe.h>
#include <net/novaworld/gate_response.h>
#include <net/novaworld/host_repository.h>
#include <net/novaworld/lobby_repository.h>
#include <net/title/title_identity.h>

#include "net_sockets.h"

#include "common/test_database.h"
#include "common/test_expect.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace net = opennova::net;
namespace nws = opennova::novaworld_server;
namespace lobbydb = opennova::lobbydb;
namespace hostdb = opennova::hostdb;
using Notice = opennova::ClientSession::Notice;
using Clock = std::chrono::steady_clock;

const opennova::TitleIdentity &kJo = *opennova::title_identity(opennova::gameprofile::GAME_JO);

// One probe from loopback address 127.0.0.`host` to the gate at `port`: the udpnovaworld it hands
// out, "" with no reply.
std::string probe_from(uint8_t host, uint16_t port) {
	const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0)
		return {};
	sockaddr_in local{};
	local.sin_family = AF_INET;
	local.sin_addr.s_addr = htonl(0x7F000000u | host);
	timeval timeout{ 2, 0 };
	::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
	std::string handed;
	if (::bind(fd, reinterpret_cast<sockaddr *>(&local), sizeof(local)) == 0) {
		const auto inner = opennova::gate_probe_build(kJo.gate_tag);
		std::vector<uint8_t> probe(inner.size() + 16);
		size_t size = 0;
		opennova::napi_envelope_encode(
				inner.data(), inner.size(), probe.data(), probe.size(), &size);
		sockaddr_in to{};
		to.sin_family = AF_INET;
		to.sin_addr.s_addr = htonl(0x7F000001u);
		to.sin_port = htons(port);
		::sendto(fd, probe.data(), size, 0, reinterpret_cast<sockaddr *>(&to), sizeof(to));
		uint8_t rx[2048];
		const ssize_t n = ::recv(fd, rx, sizeof(rx), 0);
		if (n > 0) {
			std::vector<uint8_t> reply(static_cast<size_t>(n));
			size_t len = 0;
			opennova::GateResponse parsed;
			if (opennova::napi_envelope_decode(
						rx, static_cast<size_t>(n), reply.data(), reply.size(), &len) == 0 &&
					opennova::gate_response_decrypt_and_parse(reply.data(), len, parsed) &&
					parsed.udp_novaworld.size() == 1)
				handed = parsed.udp_novaworld[0];
		}
	}
	::close(fd);
	return handed;
}

// A retail host's NovaWorld session to one lobby, pumped by the test's calls.
struct Host {
	opennova::ClientSession session;
	net::Socket socket;
	uint16_t port = 0;
	net::Endpoint lobby;
	std::vector<Notice> notices;

	Host(uint16_t lobby_port, uint32_t index) : session(config_of(index)) {
		socket = net::udp_bind(0, &port);
		lobby.ip = { 127, 0, 0, 1 };
		lobby.port = lobby_port;
	}
	~Host() { net::close_socket(socket); }

	static opennova::ClientSession::Config config_of(uint32_t index) {
		opennova::ClientSession::Config cfg =
				opennova::ClientSession::Config::lobby(kJo, kJo.gate_tag);
		cfg.client_index = index;
		cfg.client_key = index ^ 0x5A5A5A5Au;
		cfg.na = "instances:host";
		cfg.cookie_vars = [] {
			return std::vector<std::pair<std::string, std::string>>{ { "NWUID", "" } };
		};
		return cfg;
	}

	void send(const std::vector<uint8_t> &datagram) {
		if (!datagram.empty())
			net::udp_send_to(socket, lobby, datagram.data(), datagram.size());
	}

	bool service(int ms, const std::function<bool()> &done) {
		const auto deadline = Clock::now() + std::chrono::milliseconds(ms);
		uint8_t rx[4096];
		while (Clock::now() < deadline && !done()) {
			net::Endpoint from;
			const int n = net::udp_recv_from(socket, rx, sizeof rx, from, 20);
			std::vector<std::vector<uint8_t>> out;
			if (n > 0)
				session.handle_datagram(rx, static_cast<size_t>(n), out);
			session.finish_receive_batch(out);
			session.pump(out);
			session.process_periodic_update();
			for (const auto &dg : out)
				send(dg);
			for (Notice &notice : session.take_notices())
				notices.push_back(std::move(notice));
		}
		return done();
	}

	const Notice *notice(Notice::Kind kind, size_t from = 0) const {
		for (size_t i = from; i < notices.size(); ++i)
			if (notices[i].kind == kind)
				return &notices[i];
		return nullptr;
	}

	// Verified, then listed as `name`.
	bool host(const std::string &name) {
		send(session.start());
		if (!service(8000, [this] { return session.is_verified(); }))
			return false;
		session.queue_statement(
				opennova::make_client_host_request(0, { { 0, "NWUID", session.server_nwuid() } },
						{ { 0, "LobbyName", "jop_2_consumer" }, { 0, "MaxPlayers", "8" } },
						{ { 0, "ServerName", name }, { 0, "Players", "1" } },
						{ { 0, "PlayerName", "Host" } }));
		service(3000, [this] { return notice(Notice::Kind::HostResult) != nullptr; });
		const Notice *result = notice(Notice::Kind::HostResult);
		return result != nullptr && result->fields.success != 0;
	}

	// The ServerCommand notice whose one arg is `arg`, waited for while the session is pumped.
	bool command_lands(const std::string &arg, int ms) {
		return service(ms, [this, &arg] {
			for (const Notice &n : notices)
				if (n.kind == Notice::Kind::Command &&
						n.command.args == std::vector<std::string>{ arg })
					return true;
			return false;
		});
	}

	std::string addr() const { return "127.0.0.1:" + std::to_string(port); }
};

// Malformed datagrams (no envelope a lobby reads) to 127.0.0.1:`port` from `senders` threads, as
// fast as each can send, until it is destroyed.
class Flood {
public:
	Flood(uint16_t port, int senders) {
		for (int i = 0; i < senders; ++i)
			threads_.emplace_back([this, port] {
				const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
				sockaddr_in to{};
				to.sin_family = AF_INET;
				to.sin_addr.s_addr = htonl(0x7F000001u);
				to.sin_port = htons(port);
				const uint8_t junk[48] = {};
				while (!stop_.load(std::memory_order_relaxed))
					::sendto(fd, junk, sizeof junk, 0, reinterpret_cast<const sockaddr *>(&to),
							sizeof to);
				::close(fd);
			});
	}
	~Flood() {
		stop_.store(true);
		for (std::thread &t : threads_)
			t.join();
	}

private:
	std::atomic<bool> stop_{ false };
	std::vector<std::thread> threads_;
};

template <typename Done> bool wait_for(Done done, int ms) {
	const auto deadline = Clock::now() + std::chrono::milliseconds(ms);
	while (!done() && Clock::now() < deadline)
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
	return done();
}

struct Lobby {
	opennova::ConnectionManager manager;
	nws::NwUdpListener listener;

	explicit Lobby(opennova::db::ConnectionPool &pool) : listener(manager) {
		listener.set_db_pool(&pool);
		manager.on_lost([this](const opennova::Connection &c, opennova::DropReason r) {
			listener.erase_lobby_state(c.addr, opennova::drop_reason_name(r));
		});
	}

	// At `endpoint`, or with none its own: a port free a moment ago (bound and let go) and the
	// endpoint that names it, since a lobby announces its config's endpoint, never the port it
	// bound.
	bool start(const std::string &endpoint = {}) {
		nws::ServerConfig config;
		config.nw_udp_port = 0;
		config.lobby_endpoint = endpoint;
		if (endpoint.empty()) {
			{
				net::ScopedSocket probe(net::udp_bind(0, &config.nw_udp_port));
			}
			config.lobby_endpoint = "127.0.0.1:" + std::to_string(config.nw_udp_port);
		}
		return listener.start(config);
	}
};

int test_pick_spreads() {
	const std::vector<std::string> two = { "198.51.100.1:64206", "198.51.100.2:64206" };
	std::map<std::string, int> counts;
	for (uint32_t ip = 0x0A000000u; ip < 0x0A000400u; ++ip) {
		const std::string first = nws::pick_lobby(ip, two, "fallback");
		TEST_EXPECT(nws::pick_lobby(ip, two, "fallback") == first); // stable
		++counts[first];
	}
	TEST_EXPECT(counts.size() == 2 && counts[two[0]] > 300 && counts[two[1]] > 300);
	TEST_EXPECT(nws::pick_lobby(0x0A000001u, {}, "fallback") == "fallback");
	// No instance and no fallback: no lobby to hand out (the gate answers nothing).
	TEST_EXPECT(nws::pick_lobby(0x0A000001u, {}, "").empty());
	TEST_EXPECT(nws::pick_lobby(0x0A000001u, { "only:1" }, "fallback") == "only:1");
	std::printf("  a thousand addresses: %d and %d\n", counts[two[0]], counts[two[1]]);
	return 0;
}

int run() {
	test_db::TestDatabase database({ /*migrate=*/true, /*seed_games=*/true });
	opennova::db::ConnectionPool pool(database.options());
	opennova::db::Database db = database.open();

	// A previous run's rows at an endpoint the third lobby will take, and a stale instance's.
	const std::string reused = "127.0.0.1:1";
	const int64_t previous = lobbydb::register_instance(db, reused);
	lobbydb::SessionRow old_row;
	old_row.lobby_id = previous;
	old_row.connection_id = 1;
	old_row.peer_ip = "10.9.9.9";
	old_row.peer_port = 1;
	old_row.state = "active";
	old_row.connected_ms = 1;
	old_row.last_seen_ms = 1;
	TEST_EXPECT(lobbydb::insert_session(db, old_row) > 0);

	Lobby a(pool);
	Lobby b(pool);
	TEST_EXPECT(a.start() && b.start());
	TEST_EXPECT(a.listener.lobby_id() > 0 && b.listener.lobby_id() > 0 &&
			a.listener.lobby_id() != b.listener.lobby_id());
	TEST_EXPECT(a.listener.instance_endpoint() ==
			"127.0.0.1:" + std::to_string(a.listener.bound_port()));
	auto instances = lobbydb::list_instances(db);
	TEST_EXPECT(instances.size() == 3); // the previous run's at `reused` waits for its lobby
	const std::set<std::string> both = { a.listener.instance_endpoint(),
		b.listener.instance_endpoint() };

	// The gate: its fallback A's, the instances read every 100 ms.
	nws::GateListener gate;
	gate.set_db_pool(&pool);
	gate.set_lobby_refresh_ms(100);
	nws::ServerConfig gate_config;
	gate_config.gate_udp_port = 0;
	gate_config.lobby_endpoint = a.listener.instance_endpoint();
	gate_config.web_host = "127.0.0.1";
	TEST_EXPECT(gate.start(gate_config));
	// `reused` counts among the active ones until its lobby's start; it is no client's here.
	db.exec("UPDATE lobby_instances SET state = 'draining' WHERE id = $1;",
			{ opennova::db::BindValue(previous) });
	std::this_thread::sleep_for(std::chrono::milliseconds(150));

	// Probes from twenty addresses: both lobbies handed out, each address its own every time.
	std::map<std::string, int> handed;
	for (uint8_t host = 2; host < 22; ++host) {
		const std::string first = probe_from(host, gate.bound_port());
		TEST_EXPECT(both.count(first) == 1);
		TEST_EXPECT(probe_from(host, gate.bound_port()) == first);
		++handed[first];
	}
	TEST_EXPECT(handed.size() == 2);
	std::printf("  twenty client addresses: %d and %d to the two lobbies\n", handed.begin()->second,
			handed.rbegin()->second);

	// A host on each: distinct RIDs, each listing and session row its instance's.
	Host host_a(a.listener.bound_port(), 0x41414141u);
	Host host_b(b.listener.bound_port(), 0x42424242u);
	TEST_EXPECT(host_a.host("Instance A Host"));
	TEST_EXPECT(host_b.host("Instance B Host"));
	std::optional<hostdb::HostRow> row_a;
	std::optional<hostdb::HostRow> row_b;
	for (const auto &h : hostdb::list_hosts(db)) {
		if (h.server_name == "Instance A Host")
			row_a = h;
		if (h.server_name == "Instance B Host")
			row_b = h;
	}
	TEST_EXPECT(row_a && row_b && row_a->rid != row_b->rid);
	TEST_EXPECT(
			row_a->lobby_id == a.listener.lobby_id() && row_b->lobby_id == b.listener.lobby_id());
	for (const auto &s : lobbydb::list_sessions(db)) {
		if (lobbydb::session_addr(s) == host_a.addr())
			TEST_EXPECT(s.lobby_id == a.listener.lobby_id() && s.rid == row_a->rid);
		if (lobbydb::session_addr(s) == host_b.addr())
			TEST_EXPECT(s.lobby_id == b.listener.lobby_id() && s.rid == row_b->rid);
	}

	// A push, queued as web queues it, reaches the host on its own instance alone.
	TEST_EXPECT(lobbydb::queue_server_command(db, row_b->rid, "SetServerName \"To B\"") ==
			lobbydb::PushResult::Queued);
	TEST_EXPECT(lobbydb::queue_server_command(db, row_a->rid, "SetServerName \"To A\"") ==
			lobbydb::PushResult::Queued);
	TEST_EXPECT(host_b.command_lands("To B", 3000));
	TEST_EXPECT(host_a.command_lands("To A", 3000));
	TEST_EXPECT(!host_a.command_lands("To B", 300) && !host_b.command_lands("To A", 300));

	// A row of B's that B holds no session for (a drop whose delete failed), hosting: B's sweep
	// takes it (checked at the end, its kSessionSeenWriteMs long since passed).
	lobbydb::SessionRow ghost;
	ghost.lobby_id = b.listener.lobby_id();
	ghost.connection_id = 7;
	ghost.peer_ip = "10.9.9.8";
	ghost.peer_port = 7;
	ghost.state = "active";
	ghost.rid = lobbydb::next_rid(db);
	ghost.hosting = true;
	ghost.connected_ms = 1;
	ghost.last_seen_ms = 1;
	ghost.id = lobbydb::insert_session(db, ghost);
	TEST_EXPECT(ghost.id > 0);

	// A's row is reaped while A runs (an outage past the stale window, as hosts.prune or another
	// lobby's start would delete it), its session rows and listing with it. A's next heartbeat
	// registers it again under a new id and writes its rows anew; it serves on, and a push
	// reaches its host.
	const int64_t old_a = a.listener.lobby_id();
	lobbydb::remove_instance(db, old_a);
	TEST_EXPECT(!hostdb::find_host_by_rid(db, row_a->rid));
	TEST_EXPECT(wait_for(
			[&] {
				const auto listing = hostdb::find_host_by_rid(db, row_a->rid);
				return listing && listing->lobby_id != old_a &&
						listing->lobby_id == a.listener.lobby_id();
			},
			8000));
	const auto again = lobbydb::find_instance(db, a.listener.instance_endpoint());
	TEST_EXPECT(again && again->id == a.listener.lobby_id() && again->state == "active");
	bool session_back = false;
	for (const auto &s : lobbydb::list_sessions(db))
		if (lobbydb::session_addr(s) == host_a.addr())
			session_back = s.lobby_id == a.listener.lobby_id() && s.rid == row_a->rid && s.hosting;
	TEST_EXPECT(session_back);
	TEST_EXPECT(lobbydb::queue_server_command(db, row_a->rid, "SetServerName \"A again\"") ==
			lobbydb::PushResult::Queued);
	TEST_EXPECT(host_a.command_lands("A again", 3000));

	// A lobby configured with A's endpoint (two lobbies named alike): A still moves its row, so the
	// newcomer refuses to start, and the row stays A's.
	{
		Lobby twin(pool);
		TEST_EXPECT(!twin.start(a.listener.instance_endpoint()));
		const auto still = lobbydb::find_instance(db, a.listener.instance_endpoint());
		TEST_EXPECT(still && still->id == a.listener.lobby_id());
	}
	// A lobby whose config names no endpoint does not start: its instance would announce an
	// address nobody configured (none is derived from the port it binds).
	{
		Lobby nameless(pool);
		nws::ServerConfig config;
		config.nw_udp_port = 0;
		const size_t before = lobbydb::list_instances(db).size();
		TEST_EXPECT(!nameless.listener.start(config));
		TEST_EXPECT(
				nameless.listener.lobby_id() == 0 && lobbydb::list_instances(db).size() == before);
	}

	// A flood of malformed datagrams at A's lobby, from eight senders as fast as they send: its
	// receive batch ends every kReceiveBatchMaxDatagrams all the same, so a push still reaches
	// A's host and A's heartbeat still moves its row.
	{
		const int64_t seen_before =
				lobbydb::find_instance(db, a.listener.instance_endpoint())->last_seen_ms;
		Flood flood(a.listener.bound_port(), 8);
		std::this_thread::sleep_for(std::chrono::milliseconds(300));
		TEST_EXPECT(lobbydb::queue_server_command(db, row_a->rid, "SetServerName \"Flooded\"") ==
				lobbydb::PushResult::Queued);
		TEST_EXPECT(host_a.command_lands("Flooded", 8000));
		TEST_EXPECT(wait_for(
				[&] {
					return lobbydb::find_instance(db, a.listener.instance_endpoint())
								   ->last_seen_ms != seen_before;
				},
				8000));
	}

	// forget_owner: one command per instance, each taken by its own lobby.
	a.listener.pause_command_drain(true);
	b.listener.pause_command_drain(true);
	lobbydb::queue_forget_owner(db, 4242);
	const auto forgets = [&db](int64_t lobby) {
		return db
				.query("SELECT 1 FROM lobby_commands WHERE kind = 'forget_owner' AND lobby_id = "
					   "$1;",
						{ opennova::db::BindValue(lobby) })
				.size();
	};
	TEST_EXPECT(forgets(a.listener.lobby_id()) == 1 && forgets(b.listener.lobby_id()) == 1);
	a.listener.pause_command_drain(false);
	b.listener.pause_command_drain(false);
	TEST_EXPECT(wait_for(
			[&] {
				return forgets(a.listener.lobby_id()) == 0 && forgets(b.listener.lobby_id()) == 0;
			},
			3000));

	// A second gate (reading every 500 ms) whose read fails (the table renamed away for a moment):
	// it drops what it read before and hands every client its fallback lobby (A's here), since
	// an instance it read may have drained and stopped since; it reads again only
	// kLobbyRetryFactor refreshes later (3 s), not at its next refresh. Its probes come from an
	// address the gates hand B (A's new id ordered the instances anew).
	uint8_t to_b = 0;
	for (uint8_t host = 2; host < 22 && to_b == 0; ++host)
		if (probe_from(host, gate.bound_port()) == b.listener.instance_endpoint())
			to_b = host;
	TEST_EXPECT(to_b != 0);
	nws::GateListener slow_gate;
	slow_gate.set_db_pool(&pool);
	slow_gate.set_lobby_refresh_ms(500);
	TEST_EXPECT(slow_gate.start(gate_config));
	TEST_EXPECT(probe_from(to_b, slow_gate.bound_port()) == b.listener.instance_endpoint());
	std::this_thread::sleep_for(std::chrono::milliseconds(600));
	db.exec("ALTER TABLE lobby_instances RENAME TO lobby_instances_away;");
	TEST_EXPECT(probe_from(to_b, slow_gate.bound_port()) == a.listener.instance_endpoint());
	const auto failed_read = Clock::now();
	db.exec("ALTER TABLE lobby_instances_away RENAME TO lobby_instances;");
	// A second past its failed read (past its refresh, short of its retry): still its own
	// lobby, though B takes clients; past the retry, B again.
	std::this_thread::sleep_until(failed_read + std::chrono::milliseconds(1000));
	const bool within_retry = Clock::now() - failed_read < std::chrono::milliseconds(2500);
	const std::string held_over = probe_from(to_b, slow_gate.bound_port());
	if (within_retry)
		TEST_EXPECT(held_over == a.listener.instance_endpoint());
	TEST_EXPECT(wait_for(
			[&] {
				return probe_from(to_b, slow_gate.bound_port()) == b.listener.instance_endpoint();
			},
			6000));
	slow_gate.stop();

	// B drains: every probe A's from the gate's next read, and both listings stay.
	TEST_EXPECT(b.listener.drain());
	std::this_thread::sleep_for(std::chrono::milliseconds(150));
	for (uint8_t host = 2; host < 22; ++host)
		TEST_EXPECT(probe_from(host, gate.bound_port()) == a.listener.instance_endpoint());
	TEST_EXPECT(
			hostdb::find_host_by_rid(db, row_a->rid) && hostdb::find_host_by_rid(db, row_b->rid));

	// A third lobby starts at `reused`, whose previous run's row stopped moving 20 s ago (past the
	// live window, inside the stale one): it takes that row over (its id kept), clears its
	// previous run, and reaps a stale instance (ten minutes unseen) with its rows; an instance
	// unseen for two minutes (past the old one-minute window: a database failover's outage) stays,
	// and A's and B's rows stay.
	db.exec("UPDATE lobby_instances SET last_seen_at = now() - interval '20 seconds' WHERE id = "
			"$1;",
			{ opennova::db::BindValue(previous) });
	const int64_t stale = lobbydb::register_instance(db, "127.0.0.1:2");
	hostdb::HostRow stale_listing;
	stale_listing.rid = lobbydb::next_rid(db);
	stale_listing.game = "jop_2_consumer";
	stale_listing.server_name = "Stale Host";
	stale_listing.lobby_id = stale;
	hostdb::upsert_host(db, stale_listing);
	db.exec("UPDATE lobby_instances SET last_seen_at = now() - interval '10 minutes' WHERE id = "
			"$1;",
			{ opennova::db::BindValue(stale) });
	const int64_t outage = lobbydb::register_instance(db, "127.0.0.1:3");
	db.exec("UPDATE lobby_instances SET last_seen_at = now() - interval '2 minutes' WHERE id = $1;",
			{ opennova::db::BindValue(outage) });
	Lobby c(pool);
	TEST_EXPECT(c.start(reused));
	TEST_EXPECT(c.listener.lobby_id() == previous);
	bool old_row_left = false;
	for (const auto &s : lobbydb::list_sessions(db))
		old_row_left = old_row_left || s.peer_ip == "10.9.9.9";
	TEST_EXPECT(!old_row_left);
	TEST_EXPECT(!hostdb::find_host_by_rid(db, stale_listing.rid));
	instances = lobbydb::list_instances(db);
	TEST_EXPECT(instances.size() == 4);
	bool outage_left = false;
	for (const auto &i : instances) {
		TEST_EXPECT(i.id != stale);
		outage_left = outage_left || i.id == outage;
	}
	TEST_EXPECT(outage_left);
	lobbydb::remove_instance(db, outage);
	TEST_EXPECT(
			hostdb::find_host_by_rid(db, row_a->rid) && hostdb::find_host_by_rid(db, row_b->rid));

	// B's sweep took the row it holds no session for; a push to its RID finds none.
	TEST_EXPECT(wait_for(
			[&] {
				for (const auto &s : lobbydb::list_sessions(db))
					if (s.id == ghost.id)
						return false;
				return true;
			},
			12000));
	TEST_EXPECT(lobbydb::queue_server_command(db, ghost.rid, "Cycle") ==
			lobbydb::PushResult::UnknownRid);

	// Drained instances whose rows are reaped while their lobbies run register again drained: B,
	// drained by its own lobby above, and C, drained by an admin through its row (which C's next
	// heartbeat reads). Under new ids, neither is active, so no gate hands either a new client;
	// B's listing comes back on its new row.
	const int64_t c_id = c.listener.lobby_id();
	TEST_EXPECT(lobbydb::set_instance_state(db, c_id, "draining"));
	const int64_t c_seen = lobbydb::find_instance(db, reused)->last_seen_ms;
	TEST_EXPECT(wait_for(
			[&] {
				const auto now_c = lobbydb::find_instance(db, reused);
				return now_c && now_c->last_seen_ms != c_seen;
			},
			8000));
	const int64_t b_id = b.listener.lobby_id();
	lobbydb::remove_instance(db, b_id);
	lobbydb::remove_instance(db, c_id);
	TEST_EXPECT(wait_for(
			[&] {
				const auto again_b = lobbydb::find_instance(db, b.listener.instance_endpoint());
				const auto again_c = lobbydb::find_instance(db, reused);
				return again_b && again_c && again_b->id != b_id && again_c->id != c_id;
			},
			8000));
	TEST_EXPECT(lobbydb::find_instance(db, b.listener.instance_endpoint())->state == "draining");
	TEST_EXPECT(lobbydb::find_instance(db, reused)->state == "draining");
	TEST_EXPECT(lobbydb::active_endpoints(db) ==
			std::vector<std::string>{ a.listener.instance_endpoint() });
	TEST_EXPECT(wait_for(
			[&] {
				const auto listing = hostdb::find_host_by_rid(db, row_b->rid);
				return listing && listing->lobby_id == b.listener.lobby_id();
			},
			3000));

	// Clean stops: each instance's row goes, its rows with it.
	host_a.send(host_a.session.build_goodbye());
	host_b.send(host_b.session.build_goodbye());
	gate.stop();
	c.listener.stop();
	b.listener.stop();
	a.listener.stop();
	TEST_EXPECT(lobbydb::list_instances(db).empty());
	TEST_EXPECT(lobbydb::list_sessions(db).empty() && hostdb::list_hosts(db).empty());
	return 0;
}

} // namespace

int main(int argc, char **argv) {
	test_db::init(argc, argv);
	TEST_EXPECT(net::startup() == 0);
	std::printf("pick_spreads:\n");
	int rc = test_pick_spreads();
	if (rc == 0) {
		std::printf("instances:\n");
		rc = run();
	}
	net::shutdown();
	if (rc == 0)
		std::printf("OK: lobby instances spread clients, push their own, drain, clear their own, "
					"reap the stale\n");
	return rc;
}
