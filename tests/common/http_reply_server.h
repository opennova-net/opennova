#pragma once

// A test's HTTP server: one canned response (its status and JSON body) for every
// request, on 127.0.0.1 alone at a port the OS picks, counting the requests and
// keeping the last request line. For a client under test that reaches out over
// HTTP (an ECS task's metadata endpoint, the service a machine agent posts to).
// Infrastructure only.

#include "net_sockets.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace test_http {

namespace net = opennova::net;

// One HTTP response for every request, on 127.0.0.1 at a port the OS picks,
// counting the requests and keeping the last request line.
class ReplyServer {
public:
	ReplyServer(int code, std::string body) : code_(code), body_(std::move(body)) {
		listener_ = net::tcp_listen(0, 8, &port_, /*loopback_only=*/true);
		thread_ = std::thread([this] { run(); });
	}
	~ReplyServer() {
		stop_.store(true);
		thread_.join();
		net::close_socket(listener_);
	}
	ReplyServer(const ReplyServer &) = delete;
	ReplyServer &operator=(const ReplyServer &) = delete;

	uint16_t port() const { return port_; }
	// http://127.0.0.1:<port>, no path.
	std::string base_url() const { return "http://127.0.0.1:" + std::to_string(port_); }
	int requests() const { return requests_.load(); }
	std::string last_line() const {
		std::lock_guard<std::mutex> lock(mu_);
		return last_line_;
	}

private:
	void run() {
		while (!stop_.load()) {
			if (net::tcp_wait_readable(listener_, 50) <= 0)
				continue;
			net::Endpoint from;
			net::Socket client = net::tcp_accept(listener_, from);
			if (!client.is_valid())
				continue;
			std::string request;
			uint8_t buf[2048];
			while (request.find("\r\n\r\n") == std::string::npos &&
					net::tcp_wait_readable(client, 1000) > 0) {
				const int n = net::tcp_recv_nonblocking(client, buf, sizeof(buf));
				if (n <= 0)
					break;
				request.append(reinterpret_cast<const char *>(buf), static_cast<size_t>(n));
			}
			{
				std::lock_guard<std::mutex> lock(mu_);
				last_line_ = request.substr(0, request.find("\r\n"));
			}
			++requests_;
			const std::string reply = "HTTP/1.1 " + std::to_string(code_) +
					" X\r\nContent-Type: application/json\r\nContent-Length: " +
					std::to_string(body_.size()) + "\r\nConnection: close\r\n\r\n" + body_;
			for (size_t sent = 0; sent < reply.size();) {
				const int n = net::tcp_send_nonblocking(client,
						reinterpret_cast<const uint8_t *>(reply.data()) + sent,
						reply.size() - sent);
				if (n == net::TCP_SEND_WOULD_BLOCK)
					continue;
				if (n <= 0)
					break;
				sent += static_cast<size_t>(n);
			}
			net::close_socket(client);
		}
	}

	int code_;
	std::string body_;
	net::Socket listener_;
	uint16_t port_ = 0;
	std::atomic<bool> stop_{ false };
	std::atomic<int> requests_{ 0 };
	mutable std::mutex mu_;
	std::string last_line_;
	std::thread thread_;
};

} // namespace test_http
