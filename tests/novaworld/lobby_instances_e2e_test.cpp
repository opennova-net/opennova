// Several lobby instances on one database, sharing one lobby address (apps/novaworld_lobby/
// nw_udp_listener.h, gate_listener.h; engine/net/novaworld/lobby_repository.h): real NovaWorld
// UDP listeners, each registered as an instance of its own at the one endpoint every lobby
// announces (a deployment's load balancer's), and a real gate, over loopback UDP in one process.
// Each listener binds a port of its own, as the balancer's targets do, and the test's clients
// reach each at its port, as the balancer's flows would.
//
//   - two listeners register at the one endpoint, each a row of its own; the first one's start
//     reaps an instance unseen past the stale window with its rows, leaves one unseen for two
//     minutes (a database failover's outage), and clears a row no instance holds;
//   - the gate hands every client the one address, every time;
//   - a host listed on each: their RIDs differ (one sequence), each listing and session row names
//     its instance, and a push queued as web queues it reaches the host on its own
//     instance alone;
//   - forget_owner goes to both instances, each taking its own;
//   - an instance's row reaped while its lobby runs (an outage past the stale window): the lobby's
//     next heartbeat registers it again and writes its session rows and listing anew, and a push
//     reaches its host;
//   - a lobby whose config names no endpoint does not start (none is derived from the port it
//     binds);
//   - a flood of malformed datagrams at a lobby: its receive batch is bounded, so a push still
//     reaches its host and its heartbeat still moves its row;
//   - a gate whose read of the instances failed hands its fallback lobby (the instances it read
//     may all have stopped since) and reads again only kLobbyRetryFactor refreshes later;
//   - one instance drained: the other takes clients, so the gate still hands out the address,
//     and both listings stay; both drained, the gate (no fallback) answers no one;
//   - a drained instance whose row is reaped while its lobby runs registers again drained, its
//     own lobby's drain or an admin's through the row (which its heartbeat reads);
//   - a lobby's sweep deletes its row of no session it holds (a drop whose delete failed);
//   - a clean stop deletes the instance's row;
//   - a lobby replaced behind the address: its clean stop leaves the goodbye it owed a host
//     listed on it, which the new lobby sends that host's next packet (and no packet carrying
//     another key or from another address), so the host reconnects to the address it first had
//     at once and is listed on the new instance within seconds; with no goodbye left (a lobby
//     gone without its stop), the host's receive window runs out first, then the same.

#include "gate_listener.h"
#include "nw_udp_listener.h"
#include "program/server_config.h"

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
#include <net/npwire/nw_session_framing.h>
#include <net/npwire/protocol_message.h>
#include <net/npwire/session_keys.h>
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
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

namespace net = opennova::net;
namespace nws = opennova::novaworld_server;
namespace lobbydb = opennova::lobbydb;
namespace hostdb = opennova::hostdb;
using Notice = opennova::ClientSession::Notice;
using Clock = std::chrono::steady_clock;

const opennova::TitleIdentity &kJo = *opennova::title_identity(opennova::gameprofile::GAME_JO);

// The one lobby address every instance announces (a deployment's load balancer's), and a gate's
// fallback, an address of its own.
constexpr const char *kShared = "198.51.100.40:64206";
constexpr const char *kFallback = "198.51.100.99:64206";

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
		cfg.connection_keys = [key = index ^ 0x5A5A5A5Au] {
			return opennova::ConnectionKeys{ key, opennova::make_dev_scrk() };
		};
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

	// At `endpoint` (the one lobby address every instance announces), bound at `port` (0: one the
	// OS picks), as a load balancer's target is: a lobby announces its config's endpoint, never
	// the port it bound.
	bool start(const std::string &endpoint = kShared, uint16_t port = 0) {
		nws::ServerConfig config;
		config.nw_udp_port = port;
		config.lobby_endpoint = endpoint;
		return listener.start(config);
	}

	void stop() {
		listener.stop();
		manager.shutdown();
	}
};

// Instance `id`'s row, if any.
std::optional<lobbydb::LobbyInstance> instance_of(opennova::db::Database &db, int64_t id) {
	for (const lobbydb::LobbyInstance &i : lobbydb::list_instances(db))
		if (i.id == id)
			return i;
	return std::nullopt;
}

int run() {
	test_db::TestDatabase database({ /*migrate=*/true, /*seed_games=*/true });
	opennova::db::ConnectionPool pool(database.options());
	opennova::db::Database db = database.open();

	// Before any lobby starts: an instance whose process went (ten minutes unseen) with a session
	// row and a listing, one unseen for two minutes (a database failover's outage), and a session
	// row no instance holds.
	const int64_t stale = lobbydb::register_instance(db, kShared);
	lobbydb::SessionRow old_row;
	old_row.lobby_id = stale;
	old_row.connection_id = 1;
	old_row.peer_ip = "10.9.9.9";
	old_row.peer_port = 1;
	old_row.state = "active";
	old_row.connected_ms = 1;
	old_row.last_seen_ms = 1;
	TEST_EXPECT(lobbydb::insert_session(db, old_row) > 0);
	hostdb::HostRow stale_listing;
	stale_listing.rid = lobbydb::next_rid(db);
	stale_listing.game = "jop_2_consumer";
	stale_listing.server_name = "Stale Host";
	stale_listing.lobby_id = stale;
	hostdb::upsert_host(db, stale_listing);
	db.exec("UPDATE lobby_instances SET last_seen_at = now() - interval '10 minutes' WHERE id = "
			"$1;",
			{ opennova::db::BindValue(stale) });
	const int64_t outage = lobbydb::register_instance(db, kShared);
	db.exec("UPDATE lobby_instances SET last_seen_at = now() - interval '2 minutes' WHERE id = $1;",
			{ opennova::db::BindValue(outage) });
	lobbydb::SessionRow unheld = old_row;
	unheld.lobby_id = 0;
	unheld.peer_ip = "10.9.9.7";
	TEST_EXPECT(lobbydb::insert_session(db, unheld) > 0);

	// Two lobbies at the one address, each its own instance: A's start reaped the stale instance
	// with its rows and cleared the row no instance holds; the instance in its outage stays.
	Lobby a(pool);
	Lobby b(pool);
	TEST_EXPECT(a.start() && b.start());
	TEST_EXPECT(a.listener.lobby_id() > 0 && b.listener.lobby_id() > 0 &&
			a.listener.lobby_id() != b.listener.lobby_id());
	TEST_EXPECT(
			a.listener.instance_endpoint() == kShared && b.listener.instance_endpoint() == kShared);
	TEST_EXPECT(!instance_of(db, stale) && instance_of(db, outage));
	TEST_EXPECT(!hostdb::find_host_by_rid(db, stale_listing.rid));
	for (const auto &s : lobbydb::list_sessions(db))
		TEST_EXPECT(s.peer_ip != old_row.peer_ip && s.peer_ip != unheld.peer_ip);
	auto instances = lobbydb::list_instances(db);
	TEST_EXPECT(instances.size() == 3);
	for (const auto &i : instances)
		TEST_EXPECT(i.endpoint == kShared);
	lobbydb::remove_instance(db, outage);

	// The gate, no fallback, reading the instances every 100 ms: every client gets the one
	// address, every time.
	nws::GateListener gate;
	gate.set_db_pool(&pool);
	gate.set_lobby_refresh_ms(100);
	nws::ServerConfig gate_config;
	gate_config.gate_udp_port = 0;
	gate_config.web_host = "127.0.0.1";
	TEST_EXPECT(gate.start(gate_config));
	for (uint8_t host = 2; host < 12; ++host) {
		TEST_EXPECT(probe_from(host, gate.bound_port()) == kShared);
		TEST_EXPECT(probe_from(host, gate.bound_port()) == kShared);
	}

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
	const auto again = instance_of(db, a.listener.lobby_id());
	TEST_EXPECT(again && again->endpoint == kShared && again->state == "active");
	bool session_back = false;
	for (const auto &s : lobbydb::list_sessions(db))
		if (lobbydb::session_addr(s) == host_a.addr())
			session_back = s.lobby_id == a.listener.lobby_id() && s.rid == row_a->rid && s.hosting;
	TEST_EXPECT(session_back);
	TEST_EXPECT(lobbydb::queue_server_command(db, row_a->rid, "SetServerName \"A again\"") ==
			lobbydb::PushResult::Queued);
	TEST_EXPECT(host_a.command_lands("A again", 3000));

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
		const int64_t seen_before = instance_of(db, a.listener.lobby_id())->last_seen_ms;
		Flood flood(a.listener.bound_port(), 8);
		std::this_thread::sleep_for(std::chrono::milliseconds(300));
		TEST_EXPECT(lobbydb::queue_server_command(db, row_a->rid, "SetServerName \"Flooded\"") ==
				lobbydb::PushResult::Queued);
		TEST_EXPECT(host_a.command_lands("Flooded", 8000));
		TEST_EXPECT(wait_for(
				[&] {
					const auto now_a = instance_of(db, a.listener.lobby_id());
					return now_a && now_a->last_seen_ms != seen_before;
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

	// A second gate (reading every 500 ms, its fallback an address of its own) whose read fails
	// (the table renamed away for a moment): it drops what it read before and hands every client
	// its fallback, since the instances it read may all have stopped since; it reads again only
	// kLobbyRetryFactor refreshes later (3 s), not at its next refresh.
	nws::GateListener slow_gate;
	slow_gate.set_db_pool(&pool);
	slow_gate.set_lobby_refresh_ms(500);
	nws::ServerConfig slow_config = gate_config;
	slow_config.lobby_endpoint = kFallback;
	TEST_EXPECT(slow_gate.start(slow_config));
	TEST_EXPECT(probe_from(2, slow_gate.bound_port()) == kShared);
	std::this_thread::sleep_for(std::chrono::milliseconds(600));
	db.exec("ALTER TABLE lobby_instances RENAME TO lobby_instances_away;");
	TEST_EXPECT(probe_from(2, slow_gate.bound_port()) == kFallback);
	const auto failed_read = Clock::now();
	db.exec("ALTER TABLE lobby_instances_away RENAME TO lobby_instances;");
	// A second past its failed read (past its refresh, short of its retry): still its fallback,
	// though both instances take clients; past the retry, the one address again.
	std::this_thread::sleep_until(failed_read + std::chrono::milliseconds(1000));
	const bool within_retry = Clock::now() - failed_read < std::chrono::milliseconds(2500);
	const std::string held_over = probe_from(2, slow_gate.bound_port());
	if (within_retry)
		TEST_EXPECT(held_over == kFallback);
	TEST_EXPECT(wait_for([&] { return probe_from(2, slow_gate.bound_port()) == kShared; }, 6000));
	slow_gate.stop();

	// B drains: A still takes clients, so every probe still gets the one address, and both
	// listings stay. A drained by an admin through its row too: none takes clients, and the
	// gate, with no fallback, answers no one.
	TEST_EXPECT(b.listener.drain());
	for (uint8_t host = 2; host < 12; ++host)
		TEST_EXPECT(probe_from(host, gate.bound_port()) == kShared);
	TEST_EXPECT(lobbydb::live_lobbies(db).instances == 1);
	TEST_EXPECT(
			hostdb::find_host_by_rid(db, row_a->rid) && hostdb::find_host_by_rid(db, row_b->rid));
	const int64_t a_id = a.listener.lobby_id();
	TEST_EXPECT(lobbydb::set_instance_state(db, a_id, "draining"));
	TEST_EXPECT(wait_for([&] { return probe_from(2, gate.bound_port()).empty(); }, 3000));

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
	// drained by its own lobby, and A, drained by an admin through its row (which A's next
	// heartbeat reads). Under new ids, neither counts as taking clients; B's listing comes back
	// on its new row.
	const int64_t a_seen = instance_of(db, a_id)->last_seen_ms;
	TEST_EXPECT(wait_for(
			[&] {
				const auto now_a = instance_of(db, a_id);
				return now_a && now_a->last_seen_ms != a_seen;
			},
			8000));
	const int64_t b_id = b.listener.lobby_id();
	lobbydb::remove_instance(db, a_id);
	lobbydb::remove_instance(db, b_id);
	TEST_EXPECT(wait_for(
			[&] {
				return a.listener.lobby_id() != a_id && b.listener.lobby_id() != b_id &&
						instance_of(db, a.listener.lobby_id()) &&
						instance_of(db, b.listener.lobby_id());
			},
			8000));
	TEST_EXPECT(instance_of(db, a.listener.lobby_id())->state == "draining");
	TEST_EXPECT(instance_of(db, b.listener.lobby_id())->state == "draining");
	TEST_EXPECT(lobbydb::live_lobbies(db).instances == 0);
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
	b.stop();
	a.stop();
	TEST_EXPECT(lobbydb::list_instances(db).empty());
	TEST_EXPECT(lobbydb::list_sessions(db).empty() && hostdb::list_hosts(db).empty());
	return 0;
}

// A host's NovaWorld session to the one lobby address, which the test reaches at 127.0.0.1 and
// the port each lobby in turn binds (as the balancer's flows reach each task in turn), pumped by
// the test at the session's own clock: `step` moves it 20 ms a turn, about as the wall clock moves,
// and `drive` holds it.
struct RelistHost {
	opennova::ClientSession session;
	net::ScopedSocket socket;
	uint16_t port = 0;
	net::Endpoint address;
	uint32_t clock = 0;
	uint32_t heard = 0; // the clock at the latest datagram it took

	RelistHost(uint16_t lobby_port, opennova::ClientSession::Config cfg) : session(std::move(cfg)) {
		socket = net::ScopedSocket(net::udp_bind(0, &port));
		address.ip = { 127, 0, 0, 1 };
		address.port = lobby_port;
	}

	void send(const std::vector<uint8_t> &datagram) {
		if (!datagram.empty())
			net::udp_send_to(socket.get(), address, datagram.data(), datagram.size());
	}

	// Receive, dispatch, pump and answer, the clock `advance` ms on each turn, until `done` holds
	// or `ms` of wall time passed.
	bool turns(const std::function<bool()> &done, int ms, uint32_t advance) {
		const auto deadline = Clock::now() + std::chrono::milliseconds(ms);
		while (!done() && Clock::now() < deadline) {
			clock += advance;
			session.set_clock_ms(clock);
			uint8_t rx[4096];
			net::Endpoint from;
			const int n = net::udp_recv_from(socket.get(), rx, sizeof rx, from, 20);
			std::vector<std::vector<uint8_t>> out;
			if (n > 0) {
				heard = clock;
				session.handle_datagram(rx, static_cast<size_t>(n), out);
			}
			session.finish_receive_batch(out);
			session.pump(out);
			session.process_periodic_update();
			for (const auto &datagram : out)
				send(datagram);
		}
		return done();
	}
	bool drive(const std::function<bool()> &done, int ms) { return turns(done, ms, 0); }
	bool step(const std::function<bool()> &done, int ms) { return turns(done, ms, 20); }
};

// A header-only SESSION packet addressing the session whose key is `session_id`, at sequence 1.
std::vector<uint8_t> session_packet(uint32_t session_id) {
	std::vector<uint8_t> header(opennova::PROTOCOL_PACKET_HEADER_SIZE, 0);
	for (int i = 0; i < 4; ++i)
		header[i] = static_cast<uint8_t>(session_id >> (8 * i));
	header[4] = 1;
	return opennova::nw_encode_outbound(
			opennova::SESSION_OPCODE_PROTOCOL_MESSAGE, std::move(header));
}

// A lobby replaced behind the one lobby address, as a deployment replaces a lobby task behind its
// load balancer. A host listed on the first lobby: the first stops (its listing with it, nothing
// sent: the balancer ended its flows before ECS signalled the stop), leaving the goodbye it owed
// the host's session (lobby_goodbyes), and a new lobby serves the same address (bound at the
// first one's port, as the balancer's next flow from the host reaches the new task).
//   - A packet from the host's address carrying another key, and one from another address
//     carrying the session's, draw nothing. The host's next packets (its server-info update, then
//     its send-interval leg's), which still address the first lobby's session, draw the
//     ServerGoodBye the first one's stop sends, {1, 9, ..., "NP.C:SH:STOP"}, from the new lobby:
//     the host tears its session down and reconnects to the address it first had at once,
//     re-probing, re-joining, re-verifying and re-hosting with CurrentlyHosting=1, and is listed
//     on the new instance under a new RID within seconds of the stop by its clock, never near its
//     240 s receive window.
//   - A lobby gone without its clean stop leaves no goodbye (the second's deleted here), so what
//     the host sends a third lobby at the address draws nothing until its receive window runs out
//     (240 s by its clock); its reap arms the same reconnect, and it is listed on the third.
// [orig: NapiNPProtocol_StopServer @0x62A820 (the record @0x62a8c3..0x62a924);
//  Nwu_HandleDisconnect @0x623CE0; CNapiNPConnection_PumpStateMachine @0x6292e0, the reconnect
//  tail @0x629487..0x6296cb; CNapiNPConnection_PumpEnumeratorAndSend @0x6290C0, the target walk
//  @0x629183..0x6291B4; CNapiGameSession_HandleConnectVerifyResponse @0x4d5961..0x4d599e]
int test_relisted_behind_one_address() {
	test_db::TestDatabase database({ /*migrate=*/true, /*seed_games=*/true });
	opennova::db::ConnectionPool pool(database.options());
	opennova::db::Database db = database.open();
	Lobby first(pool);
	TEST_EXPECT(first.start());
	const uint16_t port = first.listener.bound_port();

	opennova::ClientSession::Config cfg = opennova::ClientSession::Config::lobby(kJo, kJo.gate_tag);
	cfg.client_index = 0x524c5354u;
	cfg.na = "relist:host";
	cfg.cookie_vars = [] {
		return std::vector<std::pair<std::string, std::string>>{ { "NWUID", "" } };
	};
	// The 0x81s' draws (D-NET-704): the connect's CK, then each re-join's the next.
	uint32_t next_key = 0x524c5331u;
	cfg.connection_keys = [&next_key] {
		return opennova::ConnectionKeys{ next_key++, opennova::make_dev_scrk() };
	};
	RelistHost host(port, std::move(cfg));
	TEST_EXPECT(host.socket.is_valid());
	const auto listing_on = [&db](int64_t lobby_id) -> std::optional<hostdb::HostRow> {
		for (const auto &h : hostdb::list_hosts(db))
			if (h.server_name == "Relisted Host" && h.lobby_id == lobby_id)
				return h;
		return std::nullopt;
	};
	using HostState = opennova::ClientSession::HostState;
	const auto listed_on = [&](int64_t lobby_id) {
		return [&host, &listing_on, lobby_id] {
			return host.session.host_state() == HostState::Established &&
					listing_on(lobby_id).has_value();
		};
	};

	host.send(host.session.start());
	TEST_EXPECT(host.drive([&] { return host.session.is_verified(); }, 8000));
	opennova::HostRegistration reg;
	reg.lobby_name = "jop_2_consumer";
	reg.server_name = "Relisted Host";
	reg.app_id = 2468;
	TEST_EXPECT(host.session.request_hosting(reg, 0));
	TEST_EXPECT(
			host.drive([&] { return host.session.host_state() == HostState::Established; }, 8000));
	const int64_t first_id = first.listener.lobby_id();
	TEST_EXPECT(wait_for([&] { return listing_on(first_id).has_value(); }, 3000));
	const uint32_t first_rid = listing_on(first_id)->rid;

	// The first lobby stops, its listing with it, and leaves the goodbye it owed the host's
	// session: its address, the SK its packets carry and the host's CK.
	first.stop();
	TEST_EXPECT(!hostdb::find_host_by_rid(db, first_rid));
	const std::vector<lobbydb::Goodbye> left = lobbydb::list_goodbyes(db, 240);
	TEST_EXPECT(left.size() == 1 && left[0].peer_ip == "127.0.0.1" &&
			left[0].peer_port == host.port && left[0].client_ck == 0x524c5331u &&
			left[0].server_sk != 0);
	const uint32_t first_sk = left.empty() ? 0 : left[0].server_sk;
	Lobby second(pool);
	TEST_EXPECT(second.start(kShared, port));
	const int64_t second_id = second.listener.lobby_id();
	TEST_EXPECT(second_id != first_id && second.listener.bound_port() == port);

	// Another key from the host's address, the session's key from another address: nothing back,
	// across more than one of the new lobby's reads of the goodbyes.
	net::ScopedSocket stranger(net::udp_bind(0));
	TEST_EXPECT(stranger.is_valid());
	const std::vector<uint8_t> other_key = session_packet(first_sk ^ 0x00010000u);
	const std::vector<uint8_t> session_key = session_packet(first_sk);
	for (int i = 0; i < 12; ++i) {
		host.send(other_key);
		net::udp_send_to(stranger.get(), host.address, session_key.data(), session_key.size());
		TEST_EXPECT(!host.drive([&] { return host.session.reconnecting(); }, 100));
	}
	uint8_t rx[512];
	net::Endpoint from;
	TEST_EXPECT(net::udp_recv_from(stranger.get(), rx, sizeof rx, from, 100) <= 0);

	// The host's next packets draw the first lobby's goodbye from the new one: torn down at once,
	// the STOP record latched.
	const uint32_t stopped_at = host.clock;
	TEST_EXPECT(host.session.send_host_update(true));
	TEST_EXPECT(host.step([&] { return host.session.reconnecting(); }, 5000));
	TEST_EXPECT(host.session.disconnect_event().dc == 9 &&
			host.session.disconnect_event().ddstr == "NP.C:SH:STOP");
	// Re-probed, re-joined, re-verified and re-hosted: listed on the new instance under a new RID,
	// seconds after the stop by its clock.
	TEST_EXPECT(host.step(listed_on(second_id), 15000));
	TEST_EXPECT(host.clock - stopped_at < 30000);
	TEST_EXPECT(host.session.host_reconnect_counter() == 1);
	const auto relisted = listing_on(second_id);
	TEST_EXPECT(relisted && relisted->rid != first_rid);
	std::printf("  the host listed on instance %lld relisted on instance %lld behind %s, %u ms "
				"after its lobby's stop by its clock\n",
			static_cast<long long>(first_id), static_cast<long long>(second_id), kShared,
			static_cast<unsigned>(host.clock - stopped_at));

	// The second lobby gone without its clean stop: no goodbye left, and a third serves the
	// address. What the host sends draws nothing.
	second.stop();
	db.exec("DELETE FROM lobby_goodbyes;");
	Lobby third(pool);
	TEST_EXPECT(third.start(kShared, port));
	const int64_t third_id = third.listener.lobby_id();
	TEST_EXPECT(host.session.send_host_update(true));
	TEST_EXPECT(!host.step([&] { return host.session.reconnecting(); }, 1500));
	// The receive window runs out, 240 s past what it last heard: the reap (its 0x46 burst reaches
	// the third lobby, which holds no session for it) arms its reconnect, and it is listed on the
	// third instance.
	host.clock = host.heard + 240001;
	host.session.set_clock_ms(host.clock);
	std::vector<std::vector<uint8_t>> burst;
	host.session.pump(burst);
	TEST_EXPECT(host.session.reconnecting() &&
			host.session.disconnect_event().ddstr == "NP.C:PT:CLNTTMOUT");
	for (const auto &datagram : burst)
		host.send(datagram);
	TEST_EXPECT(host.step(listed_on(third_id), 20000));
	TEST_EXPECT(host.session.host_reconnect_counter() == 2);
	const auto relisted_again = listing_on(third_id);
	TEST_EXPECT(relisted_again && relisted && relisted_again->rid != relisted->rid);
	std::printf("  with no goodbye left, it relisted on instance %lld past its receive window\n",
			static_cast<long long>(third_id));

	host.send(host.session.build_goodbye());
	TEST_EXPECT(wait_for([&] { return !listing_on(third_id).has_value(); }, 3000));
	third.stop();
	TEST_EXPECT(lobbydb::list_instances(db).empty());
	return 0;
}

} // namespace

int main(int argc, char **argv) {
	test_db::init(argc, argv);
	TEST_EXPECT(net::startup() == 0);
	std::printf("instances:\n");
	int rc = run();
	if (rc == 0) {
		std::printf("relisted:\n");
		rc = test_relisted_behind_one_address();
	}
	net::shutdown();
	if (rc == 0)
		std::printf("OK: lobby instances share one address, push their own, drain, reap the "
					"stale, and a host relists behind the address on its old lobby's goodbye\n");
	return rc;
}
