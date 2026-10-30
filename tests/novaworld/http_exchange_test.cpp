// net::http_exchange over the platform's TCP (net/http/http_exchange.h), against a loopback peer:
//  - the request's Host line (D-NET-1329): with no Host line of the caller's the transport writes
//    the URL's authority after the request line; a caller that names its own (the menu browser's
//    port-less host: browser_request_host) gets none from the transport, its line leading the rest;
//  - the menu browser's reply framing (HttpFraming::Browser): the head split once the bytes begin
//    "HTTP", the first "CONTENT-LENGTH" anywhere in the head bounding the body (its excess kept),
//    none reading to the close, the close's other separators, a reply with no head read as 100, and
//    the status the number past the head's first whitespace;
//  - a 301 / 302 handed over as it came, followed nowhere;
//  - the waits: no bound on the browser's wait for bytes (the cancel ends it), a bound wait that
//    runs out failing a browser reply and ending a plain one as the close would.
// [orig: CUIBrowser_SendHTTPRequest @0x658840 - "Host: %s" @0x658959 right past the request line;
//  GopherWebWidget_SendHttpPost @0x658B30 - "Host: %s" @0x658BAE; HTTP_RecvResponse @0x666EF0;
//  HTTP_ResponseHandler @0x659480 - the status read @0x6594B4..0x6594DA, no head 100 @0x6596F7]

#include <net/http/http_exchange.h>
#include <net/socket/socket.h>

#include <net/novaworld/http_flow.h> // browser_request_host

#include "common/loopback_http_peer.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

namespace net = opennova::net;
using test_http::LoopbackHttpPeer;

namespace {

int g_failures = 0;

void expect(bool cond, const char *what) {
	if (!cond) {
		std::fprintf(stderr, "FAIL: %s\n", what);
		++g_failures;
	}
}

bool loopback(const std::string &host, net::Endpoint &out) {
	return net::resolve_ipv4(host, out, false);
}

std::string url_of(const LoopbackHttpPeer &peer, const std::string &path) {
	return "http://127.0.0.1:" + std::to_string(peer.port()) + path;
}

std::string text_of(const std::vector<uint8_t> &bytes) {
	return std::string(bytes.begin(), bytes.end());
}

net::HttpExchangeOptions browser_options(int io_ms = 0) {
	net::HttpExchangeOptions options;
	options.connect_ms = 3000;
	options.io_ms = io_ms;
	options.framing = net::HttpFraming::Browser;
	return options;
}

// One browser-framed GET against a peer that answers `parts` (closing after them unless
// `hold_open`).
net::HttpReply browser_get(const std::vector<std::string> &parts, bool hold_open = false,
		int io_ms = 3000) {
	LoopbackHttpPeer peer(parts, !hold_open);
	return net::http_exchange(net::tcp_stream_connector(), "GET", url_of(peer, "/x"), {}, {},
			loopback, browser_options(io_ms));
}

void test_host_line() {
	{
		// No Host line given: the transport's, the URL's authority, past the request line.
		LoopbackHttpPeer peer({"HTTP/1.0 200 OK\r\nContent-Length: 0\r\n\r\n"});
		const net::HttpReply reply = net::http_exchange("GET", url_of(peer, "/jop_2_main.mnx"),
				{"Accept: text/html"}, {}, 3000, loopback);
		expect(reply.transport_ok && reply.code == 200, "the exchange completes");
		const std::string head = peer.request();
		const size_t first = head.find("\r\n");
		expect(head.rfind("GET /jop_2_main.mnx HTTP/1.0\r\n", 0) == 0, "the request line");
		expect(first != std::string::npos &&
		               head.compare(first + 2, 16, std::string("Host: 127.0.0.1:")) == 0,
		       "the transport's Host line is the URL's authority, port included");
		expect(head.find("Accept: text/html\r\n") != std::string::npos, "then the caller's lines");
	}
	{
		// The caller's Host line (the menu browser's: no port) is the only one, right past the
		// request line.
		LoopbackHttpPeer peer({"HTTP/1.0 200 OK\r\nContent-Length: 0\r\n\r\n"});
		const std::string url_host = opennova::browser_request_host("http://127.0.0.1:80/x");
		const net::HttpReply reply = net::http_exchange("GET", url_of(peer, "/NWLogin.dll"),
				{"Host: " + url_host, "Accept: text/html"}, {}, 3000, loopback);
		expect(reply.transport_ok, "the exchange completes");
		expect(peer.request() ==
		               "GET /NWLogin.dll HTTP/1.0\r\nHost: 127.0.0.1\r\nAccept: text/html\r\n\r\n",
		       "the caller's Host line leads its lines and the transport writes none");
	}
}

void test_browser_framing() {
	{
		const net::HttpReply r = browser_get(
				{"HTTP/1.0 200 OK\r\nContent-Length: 5\r\nSet-Cookie: A=1\r\n\r\nhello"});
		expect(r.transport_ok && r.code == 200, "a 200 reads");
		expect(r.headers == std::vector<std::string>({"Content-Length: 5", "Set-Cookie: A=1"}),
		       "the header lines past the status line");
		expect(text_of(r.body) == "hello", "the body");
	}
	{
		// The length reached completes the reply though the peer holds the connection, and the
		// excess of the same receive stays.
		const net::HttpReply r =
				browser_get({"HTTP/1.0 200 OK\r\nContent-Length: 2\r\n\r\nabcd"}, /*hold_open=*/true, 0);
		expect(r.transport_ok && r.code == 200, "a reply whole at its length ends at once");
		expect(text_of(r.body) == "abcd", "the excess past the length is kept");
	}
	{
		// The first "CONTENT-LENGTH" anywhere in the head decides, atol at +15: here 0, so the
		// reply is whole at its head.
		LoopbackHttpPeer peer(
				{"HTTP/1.0 200 OK\r\nX-Content-Length-Note: 3\r\nContent-Length: 10\r\n\r\n", "late"},
				/*close_after=*/false, /*gap_ms=*/500);
		const net::HttpReply r = net::http_exchange(net::tcp_stream_connector(), "GET",
				url_of(peer, "/"), {}, {}, loopback, browser_options(0));
		expect(r.transport_ok && r.code == 200 && r.body.empty(),
		       "the first CONTENT-LENGTH substring bounds the body");
	}
	{
		// No length: the body to the close, its parts joined.
		LoopbackHttpPeer peer({"HTTP/1.0 200 OK\r\nContent-Type: text/html\r\n\r\nab", "cd"}, true, 30);
		const net::HttpReply r = net::http_exchange(net::tcp_stream_connector(), "GET",
				url_of(peer, "/"), {}, {}, loopback, browser_options(3000));
		expect(r.transport_ok && text_of(r.body) == "abcd", "no length reads to the close");
	}
	{
		const net::HttpReply r = browser_get({"HTTP/1.1  404 Not Found\r\n\r\n"});
		expect(r.transport_ok && r.code == 404, "the status past the first whitespace (atol)");
		const net::HttpReply t = browser_get({"HTTP/1.0\t302 Found\r\n\r\n"});
		expect(t.code == 302, "a tab separates the status as a space does");
		const net::HttpReply w = browser_get({"HTTP/1.0 2000 Wide\r\n\r\n"});
		expect(w.code == 2000, "every digit of the status is read");
	}
	{
		// A close before any CR LF CR LF: the next separator splits it.
		const net::HttpReply r = browser_get({"HTTP/1.0 200 OK\nX: y\n\nbody"});
		expect(r.transport_ok && r.code == 200, "an LF-only head reads at the close");
		expect(text_of(r.body) == "body", "the body past the \\n\\n separator");
		const net::HttpReply whole = browser_get({"HTTP/1.0 200 OK"});
		expect(whole.transport_ok && whole.code == 200 && text_of(whole.body) == "HTTP/1.0 200 OK",
		       "no separator at all: the whole text is the head and the body alike");
	}
	{
		const net::HttpReply r = browser_get({"hello"});
		expect(r.transport_ok && r.code == 100 && text_of(r.body) == "hello" && r.headers.empty(),
		       "an answer with no HTTP head reads as 100, its bytes the body");
		const net::HttpReply none = browser_get({});
		expect(none.transport_ok && none.code == 100 && none.body.empty(),
		       "a close with no bytes reads as 100 with no body");
	}
}

void test_plain_framing() {
	LoopbackHttpPeer peer({"HTTP/1.1 201 Created\r\nX-Content-Length-Note: 3\r\nContent-Length: 4\r\n"
	                       "\r\nabcdef"});
	const net::HttpReply r = net::http_exchange("GET", url_of(peer, "/api"), {}, {}, 3000, loopback);
	expect(r.transport_ok && r.code == 201, "the plain status");
	expect(text_of(r.body) == "abcd", "the plain framing cuts at its Content-Length line's value");
}

void test_redirect_followed_nowhere() {
	LoopbackHttpPeer target({"HTTP/1.0 200 OK\r\nContent-Length: 0\r\n\r\n"});
	const std::string location = url_of(target, "/moved");
	const net::HttpReply r = browser_get(
			{"HTTP/1.0 302 Found\r\nLocation: " + location + "\r\nContent-Length: 4\r\n\r\nmove"});
	expect(r.transport_ok && r.code == 302, "the 302 is the reply");
	expect(text_of(r.body) == "move", "with its own body");
	bool has_location = false;
	for (const std::string &line : r.headers) has_location = has_location || line == "Location: " + location;
	expect(has_location, "and its Location line, for the flow's cookie rule");
	std::this_thread::sleep_for(std::chrono::milliseconds(100));
	expect(target.connections() == 0, "the transport follows no redirect");
}

void test_waits() {
	{
		// The browser's wait for bytes has no bound: still waiting at 300 ms, a cancel ends it.
		LoopbackHttpPeer silent({}, /*close_after=*/false);
		std::atomic<bool> cancelled{false};
		net::HttpExchangeOptions options = browser_options(0);
		options.cancelled = &cancelled;
		std::atomic<bool> done{false};
		net::HttpReply reply;
		std::thread worker([&] {
			reply = net::http_exchange(net::tcp_stream_connector(), "GET", url_of(silent, "/"), {},
					{}, loopback, options);
			done.store(true);
		});
		std::this_thread::sleep_for(std::chrono::milliseconds(300));
		expect(!done.load(), "a silent server is waited on without a bound");
		const auto at = std::chrono::steady_clock::now();
		cancelled.store(true);
		worker.join();
		const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now() - at).count();
		expect(!reply.transport_ok && reply.error == "cancelled" && reply.connected,
		       "the cancel ends the exchange, unreported as a reply");
		expect(took < 1000, "the cancel is seen within a slice");
	}
	{
		LoopbackHttpPeer silent({}, /*close_after=*/false);
		const auto at = std::chrono::steady_clock::now();
		const net::HttpReply r = net::http_exchange(net::tcp_stream_connector(), "GET",
				url_of(silent, "/"), {}, {}, loopback, browser_options(200));
		const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now() - at).count();
		expect(!r.transport_ok && r.error == "reply timed out", "a bound wait fails a browser reply");
		expect(took >= 150 && took < 2000, "after its bound");
	}
	{
		LoopbackHttpPeer partial({"HTTP/1.0 200 OK\r\n\r\npart"}, /*close_after=*/false);
		const net::HttpReply r =
				net::http_exchange("GET", url_of(partial, "/"), {}, {}, 200, loopback);
		expect(r.transport_ok && text_of(r.body) == "part",
		       "a bound wait ends a plain reply as the close would");
	}
}

} // namespace

int main() {
	net::startup();
	test_host_line();
	test_browser_framing();
	test_plain_framing();
	test_redirect_followed_nowhere();
	test_waits();
	net::shutdown();
	std::fprintf(stderr, g_failures == 0 ? "OK\n" : "FAILED (%d)\n", g_failures);
	return g_failures == 0 ? 0 : 1;
}
