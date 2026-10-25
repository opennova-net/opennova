// apps/common's http_exchange and the request's Host line (D-NET-NEW-L9-49): with no Host line of
// the caller's the transport writes the URL's authority after the request line; a caller that
// names its own (the serve's NovaWorld legs, the menu browser's port-less host:
// browser_request_host) gets none from the transport, its line leading the rest. The request
// bytes are read off a loopback listener.
// [orig: CUIBrowser_SendHTTPRequest @0x658840 - "Host: %s" @0x658959 right past the request line;
//  GopherWebWidget_SendHttpPost @0x658B30 - "Host: %s" @0x658BAE]

#include "net_http.h"
#include "net_sockets.h"

#include <net/novaworld/http_flow.h> // browser_request_host

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

namespace net = opennova::net;

namespace {

int g_failures = 0;

void expect(bool cond, const char *what) {
	if (!cond) {
		std::fprintf(stderr, "FAIL: %s\n", what);
		++g_failures;
	}
}

// One exchange against a loopback listener that records the request's head and answers 200.
std::string request_head(const std::string &path, const std::vector<std::string> &headers) {
	uint16_t port = 0;
	net::ScopedSocket listener(net::tcp_listen(0, 1, &port, true));
	if (!listener.is_valid()) return {};
	std::string head;
	std::thread server([&]() {
		net::Endpoint from{};
		net::Socket peer{};
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		while (!peer.is_valid() && std::chrono::steady_clock::now() < deadline) {
			peer = net::tcp_accept(listener.get(), from);
			if (!peer.is_valid()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
		if (!peer.is_valid()) return;
		uint8_t buf[2048];
		while (head.find("\r\n\r\n") == std::string::npos &&
				std::chrono::steady_clock::now() < deadline) {
			const int n = net::tcp_recv_nonblocking(peer, buf, sizeof buf);
			if (n > 0)
				head.append(reinterpret_cast<const char *>(buf), static_cast<size_t>(n));
			else if (n == net::TCP_RECV_WOULD_BLOCK)
				std::this_thread::sleep_for(std::chrono::milliseconds(5));
			else
				break;
		}
		const std::string reply = "HTTP/1.0 200 OK\r\nContent-Length: 0\r\n\r\n";
		size_t sent = 0;
		while (sent < reply.size() && std::chrono::steady_clock::now() < deadline) {
			const int n = net::tcp_send_nonblocking(peer,
					reinterpret_cast<const uint8_t *>(reply.data()) + sent, reply.size() - sent);
			if (n > 0)
				sent += static_cast<size_t>(n);
			else if (n == net::TCP_SEND_WOULD_BLOCK)
				std::this_thread::sleep_for(std::chrono::milliseconds(5));
			else
				break;
		}
		net::close_socket(peer);
	});
	const net::HttpReply reply = net::http_exchange("GET",
			"http://127.0.0.1:" + std::to_string(port) + path, headers, {}, 3000,
			[](const std::string &host, net::Endpoint &out) { return net::resolve_ipv4(host, out, false); });
	server.join();
	expect(reply.transport_ok && reply.code == 200, "the exchange completes");
	return head;
}

} // namespace

int main() {
	net::startup();
	{
		// No Host line given: the transport's, the URL's authority, past the request line.
		const std::string head = request_head("/jop_2_main.mnx", {"Accept: text/html"});
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
		const std::string url_host = opennova::browser_request_host("http://127.0.0.1:80/x");
		const std::string head =
				request_head("/NWLogin.dll", {"Host: " + url_host, "Accept: text/html"});
		expect(head == "GET /NWLogin.dll HTTP/1.0\r\nHost: 127.0.0.1\r\nAccept: text/html\r\n\r\n",
		       "the caller's Host line leads its lines and the transport writes none");
	}
	net::shutdown();
	std::fprintf(stderr, g_failures == 0 ? "OK\n" : "FAILED (%d)\n", g_failures);
	return g_failures == 0 ? 0 : 1;
}
