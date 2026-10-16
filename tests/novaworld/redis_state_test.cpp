// The web role's shared state in Redis (apps/novaworld_common/redis_client.h,
// session_store.h, retail_pins.h) as two web replicas see it: two pools over
// one service's namespace on the Redis server -DOPENNOVA_TEST_REDIS names
// (common/test_redis.h). The client: a URL that does not read and a server that
// does not answer fail with RedisError, a wrong AUTH fails the connect, and a
// connection the server dropped is opened again for the next command; the
// command that found it dropped goes again only when its call allows
// (RedisResend): a server that reads a command and closes unanswered sees a
// single-shot one once and a resendable one twice, and a host key's or a pin's
// take on a dropped connection fails, its key kept. The retail sessions: what
// one replica writes the other reads, every field byte for byte, each key
// living SessionStore::kTtl; a value that does not read is no session; a host
// key is taken once however many threads on both replicas race for it. The
// retail login pins: one replica's pin resumes on the other, each read moves
// a pin's expiry and its account's index's out to RetailPins::kTtl again, a
// revocation on either drops every pin to the account and none to another,
// and a logout's take returns the pin once.

#include "redis_client.h"
#include "retail_pins.h"
#include "session_store.h"

#include "common/test_expect.h"
#include "common/test_redis.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

namespace nws = opennova::novaworld_server;

namespace {

// The two replicas of one service: `a` owns the namespace, `b` shares it.
struct Replicas {
	test_redis::TestRedis a;
	test_redis::TestRedis b{ a.prefix(), /*owns=*/false };
};

long long pttl(nws::RedisPool &redis, const std::string &name) {
	return redis.command({ "PTTL", redis.key(name) }).integer;
}

int test_client_errors() {
	{
		nws::RedisOptions o;
		o.url = "http://127.0.0.1:6379";
		nws::RedisPool pool(o);
		bool threw = false;
		try {
			pool.command({ "PING" });
		} catch (const nws::RedisError &e) {
			threw = std::string(e.what()).find("not a redis://") != std::string::npos;
		}
		TEST_EXPECT(threw);
	}
	{
		// A closed port, a password in the URL that no message may carry.
		nws::RedisOptions o;
		o.url = "redis://probe:pw-s3cret@127.0.0.1:1";
		o.connect_timeout_ms = 500;
		nws::RedisPool pool(o);
		bool lost = false;
		try {
			pool.command({ "PING" });
		} catch (const nws::RedisError &e) {
			const std::string what = e.what();
			lost = e.connection_lost() && what.find("pw-s3cret") == std::string::npos &&
					what.find("probe:***@127.0.0.1:1") != std::string::npos;
			if (!lost)
				std::fprintf(stderr, "  %s\n", what.c_str());
		}
		TEST_EXPECT(lost);
	}
	{
		// A password the server does not take fails the connect (the test
		// server runs with no AUTH, so any password is refused).
		nws::RedisOptions o = test_redis::options_for(test_redis::unique_prefix());
		o.password = "not-the-password";
		nws::RedisPool pool(o);
		bool refused = false;
		try {
			pool.command({ "PING" });
		} catch (const nws::RedisError &e) {
			refused = std::string(e.what()).find("AUTH refused") != std::string::npos &&
					std::string(e.what()).find("not-the-password") == std::string::npos;
			if (!refused)
				std::fprintf(stderr, "  %s\n", e.what());
		}
		TEST_EXPECT(refused);
	}
	{
		// An error reply throws and keeps the connection.
		test_redis::TestRedis redis;
		auto lease = redis.pool().acquire();
		bool threw = false;
		try {
			lease->command({ "NOSUCHCOMMAND" });
		} catch (const nws::RedisError &e) {
			threw = !e.connection_lost();
		}
		TEST_EXPECT(threw && lease->connected());
		TEST_EXPECT(lease->command({ "PING" }).string == "PONG");
	}
	return 0;
}

// The server ends a pooled connection while it sits idle (a failover, a
// restart, its idle timeout): a command whose call allows a second run opens a
// new one and runs once; a single-shot one fails, unrun, and the next command
// opens a new one.
int test_reconnect_after_drop() {
	test_redis::TestRedis redis;
	test_redis::TestRedis admin;
	auto lease = redis.pool().acquire();
	// Ends the lease's connection; its id.
	const auto kill = [&admin, &lease] {
		const int64_t id = lease->command({ "CLIENT", "ID" }).integer;
		const bool killed =
				admin.pool().command({ "CLIENT", "KILL", "ID", std::to_string(id) }).integer == 1;
		return killed ? id : int64_t{ 0 };
	};
	int64_t id = kill();
	TEST_EXPECT(id > 0);
	TEST_EXPECT(lease->command({ "INCR", redis.pool().key("n") }, nws::RedisResend::IfDropped)
						.integer == 1);
	TEST_EXPECT(lease->command({ "CLIENT", "ID" }).integer != id);
	TEST_EXPECT(redis.pool().command({ "GET", redis.pool().key("n") }).string == "1");

	id = kill();
	TEST_EXPECT(id > 0);
	bool lost = false;
	try {
		lease->command({ "INCR", redis.pool().key("n") }, nws::RedisResend::Never);
	} catch (const nws::RedisError &e) {
		lost = e.connection_lost();
	}
	TEST_EXPECT(lost && !lease->connected());
	TEST_EXPECT(lease->command({ "CLIENT", "ID" }).integer != id);
	TEST_EXPECT(redis.pool().command({ "GET", redis.pool().key("n") }).string == "1");
	return 0;
}

// A server that reads each connection's first command and closes it
// unanswered: the drop that meets a command's reply, after the server saw (a
// real one: ran) the command. Counts the commands it read.
class SwallowingServer {
public:
	SwallowingServer() {
		fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
		sockaddr_in addr{};
		addr.sin_family = AF_INET;
		addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		socklen_t len = sizeof(addr);
		if (fd_ < 0 || ::bind(fd_, reinterpret_cast<sockaddr *>(&addr), len) != 0 ||
				::listen(fd_, 8) != 0 ||
				::getsockname(fd_, reinterpret_cast<sockaddr *>(&addr), &len) != 0) {
			std::fprintf(stderr, "  the swallowing server did not listen\n");
			return;
		}
		port_ = ntohs(addr.sin_port);
		thread_ = std::thread([this] { run(); });
	}
	~SwallowingServer() {
		stop_ = true;
		if (thread_.joinable())
			thread_.join();
		if (fd_ >= 0)
			::close(fd_);
	}
	SwallowingServer(const SwallowingServer &) = delete;
	SwallowingServer &operator=(const SwallowingServer &) = delete;

	uint16_t port() const { return port_; }
	int commands() const { return commands_.load(); }

private:
	void run() {
		while (!stop_) {
			pollfd listening{ fd_, POLLIN, 0 };
			if (::poll(&listening, 1, 50) <= 0)
				continue;
			const int conn = ::accept(fd_, nullptr, nullptr);
			if (conn < 0)
				continue;
			pollfd readable{ conn, POLLIN, 0 };
			char buf[4096];
			if (::poll(&readable, 1, 2000) > 0 && ::recv(conn, buf, sizeof(buf), 0) > 0)
				++commands_;
			::close(conn);
		}
	}

	int fd_ = -1;
	uint16_t port_ = 0;
	std::atomic<bool> stop_{ false };
	std::atomic<int> commands_{ 0 };
	std::thread thread_;
};

// A drop that meets the reply leaves a command the server may have run: a
// single-shot one (GETDEL, SET NX) is sent once and fails; one whose call
// allows a second run goes again once on a new connection, then fails.
int test_resend_policy() {
	for (const auto resend : { nws::RedisResend::Never, nws::RedisResend::IfDropped }) {
		SwallowingServer server;
		TEST_EXPECT(server.port() != 0);
		nws::RedisOptions o;
		o.url = "redis://127.0.0.1:" + std::to_string(server.port());
		nws::RedisConnection conn(o);
		bool lost = false;
		try {
			conn.command({ "GETDEL", "nw:host:tag" }, resend);
		} catch (const nws::RedisError &e) {
			lost = e.connection_lost();
		}
		TEST_EXPECT(lost && !conn.connected());
		const int want = resend == nws::RedisResend::Never ? 1 : 2;
		if (server.commands() != want)
			std::fprintf(
					stderr, "  the server read %d commands, want %d\n", server.commands(), want);
		TEST_EXPECT(server.commands() == want);
	}
	return 0;
}

// The stores' takes are single-shot: on a pooled connection the server
// dropped while it sat idle, a host key's take and a pin's take fail (the
// request fails), and the key they did not take serves the next take. A look
// goes again on a new connection and answers.
int test_takes_never_resent() {
	test_redis::TestRedis redis;
	test_redis::TestRedis admin;
	nws::SessionStore sessions(redis.pool());
	nws::RetailPins pins(redis.pool());
	// Ends the pool's one connection while it sits idle.
	const auto drop_idle = [&redis, &admin] {
		const int64_t id = redis.pool().command({ "CLIENT", "ID" }).integer;
		return admin.pool().command({ "CLIENT", "KILL", "ID", std::to_string(id) }).integer == 1;
	};
	// Whether `take` failed on its lost connection.
	const auto lost = [](const auto &take) {
		try {
			take();
		} catch (const nws::RedisError &e) {
			return e.connection_lost();
		}
		return false;
	};

	nws::HostSession host;
	host.session_tag = nws::SessionStore::generate_tag("NWHost.dll");
	host.host_key = std::string(48, 'K');
	host.success = "jop_2_host2.htm";
	host.pfid = "28";
	sessions.put_host(host.session_tag, host);
	TEST_EXPECT(drop_idle());
	TEST_EXPECT(lost([&] { sessions.take_host(host.session_tag); }));
	const auto taken = sessions.take_host(host.session_tag);
	TEST_EXPECT(taken && taken->host_key == host.host_key);

	pins.put("cookie-take", { 9, 1 });
	TEST_EXPECT(drop_idle());
	TEST_EXPECT(lost([&] { pins.take("cookie-take"); }));
	const auto pin = pins.take("cookie-take");
	TEST_EXPECT(pin && pin->user_id == 9);

	pins.put("cookie-look", { 9, 1 });
	TEST_EXPECT(drop_idle());
	TEST_EXPECT(pins.get("cookie-look").has_value());
	return 0;
}

int test_sessions_across_replicas() {
	Replicas r;
	nws::SessionStore a(r.a.pool());
	nws::SessionStore b(r.b.pool());

	nws::LoginSession login;
	login.session_tag = nws::SessionStore::generate_tag("NWLogin.dll");
	login.user_id = 4242;
	login.username = "name:with:colons";
	login.pcid = "0e2e0001";
	login.nwh = "";
	login.nwhandle = std::string("Hand\0le\xff 12:", 12);
	login.exp_bits = "1";
	login.msgbase = "jop_2_msg.htm";
	login.success = "jop_2_main.htm";
	login.failure = "";
	a.put_login(login.session_tag, login);
	const auto read = b.get_login(login.session_tag);
	TEST_EXPECT(read.has_value());
	TEST_EXPECT(read->session_tag == login.session_tag && read->user_id == 4242);
	TEST_EXPECT(read->username == login.username && read->pcid == login.pcid && read->nwh.empty() &&
			read->nwhandle == login.nwhandle && read->exp_bits == "1");
	TEST_EXPECT(read->msgbase == login.msgbase && read->success == login.success &&
			read->failure.empty());
	const long long ttl = pttl(r.a.pool(), "login:" + login.session_tag);
	const long long kTtlMs =
			std::chrono::duration_cast<std::chrono::milliseconds>(nws::SessionStore::kTtl).count();
	TEST_EXPECT(ttl > kTtlMs - 10000 && ttl <= kTtlMs);
	b.erase_login(login.session_tag);
	TEST_EXPECT(!a.get_login(login.session_tag).has_value());
	TEST_EXPECT(!a.get_login("NWServer:NWLogin.dll:SESSIONTAG:1:00000000").has_value());

	nws::JoinSession join;
	join.session_tag = nws::SessionStore::generate_tag("NWJoin.dll");
	join.success = "jop_2_join.joi";
	join.failure = "jop_2_main.htm";
	join.msgbase = "jop_2_msg.htm";
	join.needexpkey = "";
	join.pfid = "28";
	join.rid = "4242";
	b.put_join(join.session_tag, join);
	const auto joined = a.get_join(join.session_tag);
	TEST_EXPECT(joined && joined->success == join.success && joined->failure == join.failure &&
			joined->msgbase == join.msgbase && joined->needexpkey.empty() && joined->pfid == "28" &&
			joined->rid == "4242");
	TEST_EXPECT(pttl(r.b.pool(), "join:" + join.session_tag) > kTtlMs - 10000);

	// A value that does not read (another version's, a stray write) is no
	// session.
	for (const char *bad : { "", "x", "3:abc", "1:a1:b", "99:short", "2:ab2:cd2:ef9" }) {
		r.a.pool().command({ "SET", r.a.pool().key("host:bad"), bad });
		TEST_EXPECT(!b.get_host("bad").has_value());
	}
	return 0;
}

// /NWHost.dll's second leg serves a tag's HOSTKEY once: sixteen threads on two
// replicas take the same tag at once and exactly one gets it.
int test_host_key_taken_once() {
	Replicas r;
	nws::SessionStore a(r.a.pool());
	nws::SessionStore b(r.b.pool());
	for (int round = 0; round < 10; ++round) {
		nws::HostSession host;
		host.session_tag = nws::SessionStore::generate_tag("NWHost.dll");
		host.host_key = std::string(48, 'A' + round);
		host.success = "jop_2_host2.htm";
		host.pfid = "28";
		a.put_host(host.session_tag, host);
		TEST_EXPECT(b.get_host(host.session_tag).has_value()); // a look takes nothing
		std::atomic<int> served{ 0 };
		std::atomic<int> wrong{ 0 };
		std::vector<std::thread> threads;
		for (int t = 0; t < 16; ++t) {
			nws::SessionStore &store = t % 2 == 0 ? a : b;
			threads.emplace_back([&store, &host, &served, &wrong] {
				if (const auto s = store.take_host(host.session_tag)) {
					++served;
					if (s->host_key != host.host_key || s->success != host.success ||
							s->pfid != "28")
						++wrong;
				}
			});
		}
		for (std::thread &t : threads)
			t.join();
		TEST_EXPECT(served == 1 && wrong == 0);
		TEST_EXPECT(!a.get_host(host.session_tag).has_value());
	}
	return 0;
}

int test_pins_across_replicas() {
	Replicas r;
	nws::RetailPins a(r.a.pool());
	nws::RetailPins b(r.b.pool());
	a.put("cookie-one", { 7, 3 });
	a.put("cookie-two", { 7, 3 });
	b.put("cookie-other", { 8, 1 });
	b.put("cookie-moved", { 7, 3 });
	// The same retail process logged in again as another account: the cookie's
	// pin is that account's now, though user 7's index still lists it.
	a.put("cookie-moved", { 70, 2 });

	const auto one = b.get("cookie-one");
	TEST_EXPECT(one && one->user_id == 7 && one->credentials_gen == 3);
	TEST_EXPECT(!b.get("").has_value() && !b.get("cookie-none").has_value());
	const long long kTtlMs =
			std::chrono::duration_cast<std::chrono::milliseconds>(nws::RetailPins::kTtl).count();
	TEST_EXPECT(pttl(r.a.pool(), "pin:cookie-one") > kTtlMs - 10000);
	TEST_EXPECT(pttl(r.a.pool(), "pins:7") > kTtlMs - 10000);

	// A pin in use outlives kTtl: each read moves its expiry, and its account's
	// index's, out to kTtl again (here a pin and an index a minute from
	// expiring), and a cookie with no pin moves nothing.
	for (const char *name : { "pin:cookie-one", "pins:7" })
		r.a.pool().command({ "PEXPIRE", r.a.pool().key(name), "60000" });
	TEST_EXPECT(b.get("cookie-one").has_value());
	TEST_EXPECT(pttl(r.a.pool(), "pin:cookie-one") > kTtlMs - 10000);
	TEST_EXPECT(pttl(r.a.pool(), "pins:7") > kTtlMs - 10000);
	r.a.pool().command({ "PEXPIRE", r.a.pool().key("pins:7"), "60000" });
	TEST_EXPECT(!b.get("cookie-none").has_value());
	TEST_EXPECT(pttl(r.a.pool(), "pins:7") <= 60000);
	TEST_EXPECT(a.get("cookie-two").has_value());
	TEST_EXPECT(pttl(r.a.pool(), "pins:7") > kTtlMs - 10000);

	// A stale pin is dropped only while it is still the one found stale.
	a.drop_if("cookie-two", { 7, 2 });
	TEST_EXPECT(b.get("cookie-two").has_value());

	// The revocation on one replica holds on the other: user 7's pins go, user
	// 8's and the moved cookie's stay.
	TEST_EXPECT(b.revoke(7) == 2);
	TEST_EXPECT(!a.get("cookie-one").has_value() && !a.get("cookie-two").has_value());
	const auto moved = a.get("cookie-moved");
	TEST_EXPECT(moved && moved->user_id == 70);
	TEST_EXPECT(a.get("cookie-other").has_value());
	TEST_EXPECT(a.revoke(7) == 0);

	// A logout's take returns the pin once.
	const auto taken = a.take("cookie-other");
	TEST_EXPECT(taken && taken->user_id == 8);
	TEST_EXPECT(!b.take("cookie-other").has_value());
	a.drop_if("cookie-moved", { 70, 2 });
	TEST_EXPECT(!b.get("cookie-moved").has_value());
	return 0;
}

} // namespace

int main(int argc, char **argv) {
	test_redis::init(argc, argv);
	struct Case {
		const char *name;
		int (*fn)();
	};
	const Case cases[] = {
		{ "client_errors", test_client_errors },
		{ "reconnect_after_drop", test_reconnect_after_drop },
		{ "resend_policy", test_resend_policy },
		{ "takes_never_resent", test_takes_never_resent },
		{ "sessions_across_replicas", test_sessions_across_replicas },
		{ "host_key_taken_once", test_host_key_taken_once },
		{ "pins_across_replicas", test_pins_across_replicas },
	};
	for (const Case &c : cases) {
		std::printf("-- %s\n", c.name);
		if (c.fn() != 0) {
			std::fprintf(stderr, "FAIL: %s\n", c.name);
			return 1;
		}
	}
	std::printf("OK: redis state\n");
	return 0;
}
