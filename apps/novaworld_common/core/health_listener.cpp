#include "health_listener.h"

#include <chrono>
#include <cstdio>
#include <string>

namespace opennova::novaworld_server {

namespace {

using Clock = std::chrono::steady_clock;

std::string reply(int code, const char *reason, const std::string &body, bool with_body,
		const char *extra_header = nullptr) {
	std::string out = "HTTP/1.1 " + std::to_string(code) + " " + reason +
			"\r\nContent-Type: text/plain\r\nCache-Control: no-store\r\nContent-Length: " +
			std::to_string(body.size()) + "\r\nConnection: close\r\n";
	if (extra_header != nullptr)
		out += std::string(extra_header) + "\r\n";
	out += "\r\n";
	if (with_body)
		out += body;
	return out;
}

} // namespace

std::string HealthListener::reply_to(std::string_view request, bool healthy) {
	// The request line: METHOD SP target SP HTTP/x.y, ended by CRLF.
	const size_t eol = request.find("\r\n");
	const std::string_view line = request.substr(0, eol);
	const size_t sp1 = line.find(' ');
	const size_t sp2 = sp1 == std::string_view::npos ? sp1 : line.find(' ', sp1 + 1);
	if (eol == std::string_view::npos || sp1 == std::string_view::npos ||
			sp2 == std::string_view::npos || line.substr(sp2 + 1).substr(0, 5) != "HTTP/")
		return reply(400, "Bad Request", "bad request", true);
	const std::string_view method = line.substr(0, sp1);
	std::string_view target = line.substr(sp1 + 1, sp2 - sp1 - 1);
	if (const size_t query = target.find('?'); query != std::string_view::npos)
		target = target.substr(0, query);
	if (target != "/livez")
		return reply(404, "Not Found", "not found", method != "HEAD");
	if (method != "GET" && method != "HEAD")
		return reply(405, "Method Not Allowed", "method not allowed", true, "Allow: GET, HEAD");
	return healthy ? reply(200, "OK", "ok", method == "GET")
				   : reply(503, "Service Unavailable", "stalled", method == "GET");
}

bool HealthListener::start(uint16_t port) {
	stopping_.store(false);
	listener_ = net::tcp_listen(port, 16, &bound_port_);
	if (!listener_.is_valid())
		return false;
	std::printf("[health] listening on TCP :%u (GET /livez)\n", static_cast<unsigned>(bound_port_));
	thread_ = std::thread([this] { run(); });
	return true;
}

void HealthListener::stop() {
	stopping_.store(true);
	if (thread_.joinable())
		thread_.join();
	net::close_socket(listener_);
}

void HealthListener::run() {
	while (!stopping_.load()) {
		// A pending connection reads as the listener readable; 250 ms at most,
		// so stop() is noticed within that.
		if (net::tcp_wait_readable(listener_, 250) <= 0)
			continue;
		net::Endpoint from;
		net::Socket client = net::tcp_accept(listener_, from);
		if (!client.is_valid())
			continue;
		answer(client);
		net::close_socket(client);
	}
}

void HealthListener::answer(net::Socket &client) {
	const auto deadline = Clock::now() + std::chrono::milliseconds(kRequestTimeoutMs);
	const auto left_ms = [&deadline] {
		const auto left =
				std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now())
						.count();
		return left > 0 ? static_cast<int>(left) : 0;
	};
	// The request's head, to its blank line (a probe sends no body), within the
	// time and size bounds; what came by then is what is answered.
	std::string request;
	uint8_t buffer[1024];
	while (request.find("\r\n\r\n") == std::string::npos && request.size() < kMaxRequestBytes &&
			!stopping_.load()) {
		const int wait = left_ms();
		if (wait == 0 || net::tcp_wait_readable(client, wait) <= 0)
			break;
		const int n = net::tcp_recv_nonblocking(client, buffer, sizeof(buffer));
		if (n == net::TCP_RECV_WOULD_BLOCK)
			continue;
		if (n <= 0)
			break;
		request.append(reinterpret_cast<const char *>(buffer), static_cast<size_t>(n));
	}
	const std::string out = reply_to(request, watchdog_.healthy());
	size_t sent = 0;
	while (sent < out.size() && left_ms() > 0) {
		const int n = net::tcp_send_nonblocking(
				client, reinterpret_cast<const uint8_t *>(out.data()) + sent, out.size() - sent);
		if (n == net::TCP_SEND_WOULD_BLOCK) {
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
			continue;
		}
		if (n < 0)
			return;
		sent += static_cast<size_t>(n);
	}
}

} // namespace opennova::novaworld_server
