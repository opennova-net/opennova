#include "update_tcp_server.h"

#include <array>

namespace opennova::net {

namespace {

constexpr int kListenBacklog = 16;

bool same_peer(const Endpoint &a, const Endpoint &b) {
	return a.ip == b.ip;
}

} // namespace

bool UpdateTcpServer::listen(const std::vector<uint16_t> &ports, bool loopback_only) {
	close();
	for (uint16_t port : ports) {
		uint16_t bound = 0;
		Socket listener = tcp_listen(port, kListenBacklog, &bound, loopback_only);
		if (!listener.is_valid()) {
			close();
			return false;
		}
		listeners_.push_back(listener);
		ports_.push_back(bound);
	}
	return !listeners_.empty();
}

bool UpdateTcpServer::pump() {
	bool busy = false;
	const auto now = std::chrono::steady_clock::now();
	for (Socket &listener : listeners_) {
		Endpoint from;
		Socket accepted = tcp_accept(listener, from);
		if (!accepted.is_valid())
			continue;
		busy = true;
		size_t from_peer = 0;
		for (const Client &client : clients_)
			from_peer += same_peer(client.peer, from) ? 1 : 0;
		if (clients_.size() >= max_connections_ || from_peer >= kMaxPerPeer) {
			log("refused " + net::endpoint_to_string(from) + " (connection limit)");
			close_socket(accepted);
			continue;
		}
		Client client;
		client.socket = accepted;
		client.peer = from;
		client.id = server_.open();
		client.last = now;
		log("connect " + net::endpoint_to_string(from));
		clients_.push_back(std::move(client));
	}

	std::array<uint8_t, 4096> buffer{};
	for (size_t i = 0; i < clients_.size();) {
		Client &client = clients_[i];
		bool drop = false;
		const size_t unsent = client.outbox.size() - client.sent;
		if (!client.closing && unsent < kOutboxCap) {
			if (client.more) {
				busy = true;
				take(client, server_.receive(client.id, nullptr, 0));
			} else {
				const int n = tcp_recv_nonblocking(client.socket, buffer.data(), buffer.size());
				if (n > 0) {
					busy = true;
					if (unsent == 0)
						client.last = now;
					const std::string text(
							reinterpret_cast<const char *>(buffer.data()), size_t(n));
					const size_t eol = text.find("\r\n");
					log(net::endpoint_to_string(client.peer) + " < " + text.substr(0, eol));
					take(client, server_.receive(client.id, buffer.data(), static_cast<size_t>(n)));
				} else if (n == 0 || n == -1) {
					drop = true;
				}
			}
		}
		if (!drop && client.sent < client.outbox.size()) {
			const int n = tcp_send_nonblocking(client.socket,
					client.outbox.data() + client.sent, client.outbox.size() - client.sent);
			if (n > 0) {
				busy = true;
				client.last = now;
				client.sent += static_cast<size_t>(n);
				if (client.sent == client.outbox.size()) {
					client.outbox.clear();
					client.sent = 0;
				}
			} else if (n == -1) {
				drop = true;
			}
		}
		if (client.closing && client.outbox.empty())
			drop = true;
		if (now - client.last > kIdleTimeout) {
			log(net::endpoint_to_string(client.peer) + " idle");
			drop = true;
		}
		if (drop) {
			log("close " + net::endpoint_to_string(client.peer));
			server_.close(client.id);
			close_socket(client.socket);
			clients_.erase(clients_.begin() + static_cast<std::ptrdiff_t>(i));
			busy = true;
			continue;
		}
		++i;
	}
	return busy;
}

void UpdateTcpServer::take(Client &client, update::UpdateServer::Received received) {
	client.outbox.erase(
			client.outbox.begin(), client.outbox.begin() + static_cast<std::ptrdiff_t>(client.sent));
	client.sent = 0;
	client.outbox.insert(client.outbox.end(), received.send.begin(), received.send.end());
	client.more = received.more;
	if (!received.keep_open)
		client.closing = true;
}

size_t UpdateTcpServer::queued_bytes() const {
	size_t total = 0;
	for (const Client &client : clients_)
		total += client.outbox.size();
	return total;
}

void UpdateTcpServer::close() {
	for (Client &client : clients_) {
		server_.close(client.id);
		close_socket(client.socket);
	}
	clients_.clear();
	for (Socket &listener : listeners_)
		close_socket(listener);
	listeners_.clear();
	ports_.clear();
}

} // namespace opennova::net
