// GET /livez on ONNET_HEALTH_PORT for the programs with no HTTP of their own
// (apps/novaworld_common/core/health_listener.h): what each request is answered
// from a healthy watchdog and from a stalled one (a hand-stepped clock), then
// the listener itself over loopback TCP on a port the OS picks: 200 "ok", 503
// "stalled" once a watched loop stood still past the stall time, a probe that
// never finishes its request answered within the request timeout (and the next
// probe answered after it), the port let go at stop(). Database-free.

#include "core/health_listener.h"

#include <net_http.h>
#include <net_sockets.h>

#include "common/test_expect.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>

namespace nws = opennova::novaworld_server;
namespace net = opennova::net;

namespace {

bool starts_with(const std::string &text, const std::string &prefix) {
	return text.compare(0, prefix.size(), prefix) == 0;
}

bool ends_with(const std::string &text, const std::string &suffix) {
	return text.size() >= suffix.size() &&
			text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

int exercise_replies() {
	using L = nws::HealthListener;
	const std::string get = "GET /livez HTTP/1.1\r\nHost: x\r\n\r\n";
	std::string r = L::reply_to(get, true);
	TEST_EXPECT(starts_with(r, "HTTP/1.1 200 OK\r\n"));
	TEST_EXPECT(r.find("Content-Length: 2\r\n") != std::string::npos);
	TEST_EXPECT(r.find("Cache-Control: no-store\r\n") != std::string::npos);
	TEST_EXPECT(r.find("Connection: close\r\n") != std::string::npos);
	TEST_EXPECT(ends_with(r, "\r\n\r\nok"));
	r = L::reply_to(get, false);
	TEST_EXPECT(starts_with(r, "HTTP/1.1 503 Service Unavailable\r\n"));
	TEST_EXPECT(ends_with(r, "\r\n\r\nstalled"));
	// A query string is the same path; HTTP/1.0 is a request too.
	TEST_EXPECT(starts_with(L::reply_to("GET /livez?x=1 HTTP/1.0\r\n\r\n", true), "HTTP/1.1 200"));
	// HEAD: the status and the headers, no body (its length all the same).
	r = L::reply_to("HEAD /livez HTTP/1.1\r\n\r\n", true);
	TEST_EXPECT(starts_with(r, "HTTP/1.1 200 OK\r\n") &&
			ends_with(r, "Content-Length: 2\r\nConnection: close\r\n\r\n"));
	r = L::reply_to("HEAD /livez HTTP/1.1\r\n\r\n", false);
	TEST_EXPECT(starts_with(r, "HTTP/1.1 503") && ends_with(r, "\r\n\r\n"));
	// Anything else: no other detail.
	TEST_EXPECT(starts_with(L::reply_to("GET /healthz HTTP/1.1\r\n\r\n", true), "HTTP/1.1 404"));
	TEST_EXPECT(starts_with(L::reply_to("GET / HTTP/1.1\r\n\r\n", true), "HTTP/1.1 404"));
	r = L::reply_to("POST /livez HTTP/1.1\r\n\r\n", true);
	TEST_EXPECT(
			starts_with(r, "HTTP/1.1 405") && r.find("Allow: GET, HEAD\r\n") != std::string::npos);
	TEST_EXPECT(starts_with(L::reply_to("", true), "HTTP/1.1 400"));
	TEST_EXPECT(starts_with(L::reply_to("GET /livez\r\n\r\n", true), "HTTP/1.1 400"));
	TEST_EXPECT(starts_with(L::reply_to("GET /livez HTTP/1.1", true), "HTTP/1.1 400"));
	TEST_EXPECT(starts_with(L::reply_to("garbage\r\n\r\n", true), "HTTP/1.1 400"));
	std::printf("  replies: 200 ok, 503 stalled, HEAD, 404, 405, 400\n");
	return 0;
}

net::HttpReply get_livez(uint16_t port) {
	return net::http_exchange("GET", "http://127.0.0.1:" + std::to_string(port) + "/livez", {}, {},
			3000, [](const std::string &host, net::Endpoint &out) {
				return net::resolve_ipv4(host, out, false);
			});
}

std::string body_of(const net::HttpReply &reply) {
	return std::string(reply.body.begin(), reply.body.end());
}

int exercise_listener() {
	// A watchdog on a clock the test steps, over one loop the test turns.
	std::atomic<uint64_t> now_ms{ 1000 };
	std::atomic<uint64_t> turns{ 0 };
	nws::Watchdog watchdog(30000, [&now_ms] { return now_ms.load(); });
	watchdog.watch("test loop", [&turns] { return turns.load(); });

	nws::HealthListener listener(watchdog);
	TEST_EXPECT(listener.start(0));
	const uint16_t port = listener.bound_port();
	TEST_EXPECT(port != 0);

	net::HttpReply reply = get_livez(port);
	TEST_EXPECT(reply.transport_ok && reply.code == 200 && body_of(reply) == "ok");

	// The loop stands still past the stall time: 503 "stalled".
	now_ms += 31000;
	TEST_EXPECT(watchdog.check() == "test loop");
	reply = get_livez(port);
	TEST_EXPECT(reply.transport_ok && reply.code == 503 && body_of(reply) == "stalled");

	// It moves again: healthy.
	turns += 1;
	TEST_EXPECT(watchdog.check().empty());
	reply = get_livez(port);
	TEST_EXPECT(reply.code == 200);

	// A probe that never finishes its request is answered (400) once the
	// request timeout passes, and the next probe after it.
	{
		net::Endpoint to;
		TEST_EXPECT(net::resolve_ipv4("127.0.0.1", to, false));
		to.port = port;
		net::ScopedSocket slow(net::tcp_connect(to, 2000));
		TEST_EXPECT(slow.is_valid());
		const std::string partial = "GET /livez HTTP/1.1\r\n";
		TEST_EXPECT(net::tcp_send_all(
				slow.get(), reinterpret_cast<const uint8_t *>(partial.data()), partial.size()));
		const auto sent_at = std::chrono::steady_clock::now();
		reply = get_livez(port);
		const auto waited = std::chrono::steady_clock::now() - sent_at;
		TEST_EXPECT(reply.transport_ok && reply.code == 200);
		TEST_EXPECT(
				waited < std::chrono::milliseconds(nws::HealthListener::kRequestTimeoutMs + 2500));
	}

	// Another bind of the same port fails while it serves; after stop() it is free.
	{
		nws::HealthListener second(watchdog);
		TEST_EXPECT(!second.start(port));
	}
	listener.stop();
	reply = get_livez(port);
	TEST_EXPECT(!reply.transport_ok);
	nws::HealthListener again(watchdog);
	TEST_EXPECT(again.start(port));
	again.stop();
	std::printf("  listener: :%u answered ok, then stalled, then ok; a slow probe bounded; the "
				"port let go\n",
			static_cast<unsigned>(port));
	return 0;
}

} // namespace

int main() {
	TEST_EXPECT(net::startup() == 0);
	int rc = exercise_replies();
	if (rc == 0)
		rc = exercise_listener();
	net::shutdown();
	if (rc == 0)
		std::printf("OK: GET /livez on the health port\n");
	return rc;
}
