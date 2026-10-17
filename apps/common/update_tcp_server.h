#pragma once

// The update listener's sockets: one listener per gateway port, the connections they accept,
// pumped into the engine's connection machine (net/update/update_server.h). A file reply may
// be megabytes, so each connection's replies are made a budget at a time and drain as its
// socket takes them: a connection is read, and its next replies made, only while less than
// kOutboxCap of what it was sent before still waits, so what it holds stays bounded however
// many requests its client sends without reading. Socket I/O stays here; the engine takes and
// returns bytes. opennova-update-tool's `serve` and the NovaWorld web program's update listener
// (ADR 0061 d3) run it alike, each over its own catalog, so it lives with the app layer's
// sockets.

#include "net_sockets.h"

#include <net/update/update_server.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace opennova::net {

class UpdateTcpServer {
public:
	// A line about a connection or a request, for the console.
	using LogSink = std::function<void(const std::string &line)>;

	// `max_connections`: the slots, kMaxConnections unless the embedder holds fewer.
	UpdateTcpServer(update::UpdateServer &server, LogSink log = {},
			size_t max_connections = kMaxConnections) :
			server_(server), log_(std::move(log)), max_connections_(max_connections) {}
	~UpdateTcpServer() { close(); }
	UpdateTcpServer(const UpdateTcpServer &) = delete;
	UpdateTcpServer &operator=(const UpdateTcpServer &) = delete;

	// Listen on each port, on every interface or on loopback alone. False when one fails.
	bool listen(const std::vector<uint16_t> &ports, bool loopback_only);
	const std::vector<uint16_t> &ports() const { return ports_; }

	// One pass: accept, read, answer, drain, close what ended or idled. True when it did
	// anything, so the caller may sleep when it did not.
	bool pump();

	void close();
	size_t connection_count() const { return clients_.size(); }
	// The reply bytes the connections hold, sent or not (each outbox's whole size).
	size_t queued_bytes() const;

	// What a connection may hold the server to: the slots, a peer's share, the idle time.
	static constexpr size_t kMaxConnections = 64;
	static constexpr size_t kMaxPerPeer = 8;
	static constexpr std::chrono::seconds kIdleTimeout{ 120 };
	// The unsent reply bytes past which a connection is neither read nor answered further
	// until its socket takes them. A read counts as activity (against kIdleTimeout) only when
	// nothing was owed, so a client that sends and never reads is closed as idle.
	static constexpr size_t kOutboxCap = 64 * 1024;

private:
	struct Client {
		net::Socket socket;
		net::Endpoint peer;
		uint32_t id = 0;
		std::vector<uint8_t> outbox;
		size_t sent = 0;
		bool more = false; // the server has replies waiting past its last budget
		bool closing = false;
		std::chrono::steady_clock::time_point last;
	};

	// The server's answer queued: the sent bytes dropped from the front, its replies after.
	static void take(Client &client, update::UpdateServer::Received received);

	void log(const std::string &line) const {
		if (log_)
			log_(line);
	}

	update::UpdateServer &server_;
	LogSink log_;
	size_t max_connections_ = kMaxConnections;
	std::vector<net::Socket> listeners_;
	std::vector<uint16_t> ports_;
	std::vector<Client> clients_;
};

} // namespace opennova::net
