#pragma once

// A test's scripted HTTP peer on 127.0.0.1 at a port the OS picks: it accepts connections one at
// a time, reads each request (its head, then the Content-Length's bytes when it names one), keeps
// the first request's bytes, writes the scripted reply parts in order (each its own send, `gap_ms`
// apart) and then closes, or holds the connection open until it is destroyed. For the HTTP
// exchange's and the menu browser transport's ctests. Infrastructure only.

#include <net/socket/socket.h>

#include <base/io/strutil.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace test_http {

namespace net = opennova::net;

class LoopbackHttpPeer {
public:
	explicit LoopbackHttpPeer(std::vector<std::string> reply_parts, bool close_after = true,
			int gap_ms = 0)
		: parts_(std::move(reply_parts)), close_after_(close_after), gap_ms_(gap_ms) {
		listener_ = net::tcp_listen(0, 4, &port_, /*loopback_only=*/true);
		thread_ = std::thread([this] { run(); });
	}
	~LoopbackHttpPeer() {
		stop_.store(true);
		thread_.join();
		net::close_socket(listener_);
	}
	LoopbackHttpPeer(const LoopbackHttpPeer &) = delete;
	LoopbackHttpPeer &operator=(const LoopbackHttpPeer &) = delete;

	uint16_t port() const { return port_; }
	int connections() const { return connections_.load(); }
	// The first request's bytes, once read (empty before).
	std::string request() const {
		std::lock_guard<std::mutex> lock(mu_);
		return request_;
	}
	// Wait at most `ms` for the first request to have been read.
	bool wait_request(int ms) const {
		const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
		while (std::chrono::steady_clock::now() < until) {
			if (read_.load()) return true;
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
		return read_.load();
	}

private:
	static size_t wanted_length(const std::string &head) {
		const size_t at = opennova::strutil::ifind(head, "Content-Length:");
		return at == std::string::npos ? 0 : static_cast<size_t>(std::atol(head.c_str() + at + 15));
	}

	void send_text(net::Socket &peer, const std::string &text) {
		for (size_t sent = 0; sent < text.size() && !stop_.load();) {
			const int n = net::tcp_send_nonblocking(peer,
					reinterpret_cast<const uint8_t *>(text.data()) + sent, text.size() - sent);
			if (n == net::TCP_SEND_WOULD_BLOCK) {
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
				continue;
			}
			if (n <= 0) return;
			sent += static_cast<size_t>(n);
		}
	}

	void serve(net::Socket &peer) {
		std::string bytes;
		uint8_t buf[4096];
		for (;;) {
			const size_t head_end = bytes.find("\r\n\r\n");
			if (head_end != std::string::npos &&
					bytes.size() >= head_end + 4 + wanted_length(bytes.substr(0, head_end + 4)))
				break;
			if (stop_.load()) return;
			if (net::tcp_wait_readable(peer, 20) <= 0) continue;
			const int n = net::tcp_recv_nonblocking(peer, buf, sizeof(buf));
			if (n == net::TCP_RECV_WOULD_BLOCK) continue;
			if (n <= 0) break;
			bytes.append(reinterpret_cast<const char *>(buf), static_cast<size_t>(n));
		}
		{
			std::lock_guard<std::mutex> lock(mu_);
			if (!read_.load()) request_ = bytes;
		}
		read_.store(true);
		for (size_t i = 0; i < parts_.size() && !stop_.load(); ++i) {
			if (i > 0 && gap_ms_ > 0) std::this_thread::sleep_for(std::chrono::milliseconds(gap_ms_));
			send_text(peer, parts_[i]);
		}
		while (!close_after_ && !stop_.load()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}

	void run() {
		while (!stop_.load()) {
			if (net::tcp_wait_readable(listener_, 20) <= 0) continue;
			net::Endpoint from;
			net::Socket peer = net::tcp_accept(listener_, from);
			if (!peer.is_valid()) continue;
			++connections_;
			serve(peer);
			net::close_socket(peer);
		}
	}

	std::vector<std::string> parts_;
	bool close_after_;
	int gap_ms_;
	net::Socket listener_;
	uint16_t port_ = 0;
	std::atomic<bool> stop_{false};
	std::atomic<bool> read_{false};
	std::atomic<int> connections_{0};
	mutable std::mutex mu_;
	std::string request_;
	std::thread thread_;
};

} // namespace test_http
