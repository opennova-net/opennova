#include "admin_tcp_server.h"

#include <array>
#include <cstddef>

namespace opennova::net {

namespace {

// The listen backlog. [orig: CAdminServer_Listen @0x406E00, `listen(s, 5)`]
constexpr int kListenBacklog = 5;

// The engine's address word: the raw in_addr dword, a | b<<8 | c<<16 | d<<24.
uint32_t in_addr_word(const Endpoint &ep) {
	return static_cast<uint32_t>(ep.ip[0]) | (static_cast<uint32_t>(ep.ip[1]) << 8) |
	       (static_cast<uint32_t>(ep.ip[2]) << 16) | (static_cast<uint32_t>(ep.ip[3]) << 24);
}

} // namespace

bool AdminTcpServer::listen(uint16_t port, bool loopback_only) {
	close();
	listener_ = tcp_listen(port, kListenBacklog, &port_, loopback_only);
	return listener_.is_valid();
}

void AdminTcpServer::pump() {
	if (!listener_.is_valid()) return;
	Endpoint from;
	Socket accepted = tcp_accept(listener_, from);
	if (accepted.is_valid()) {
		const AdminServer::Accepted result = server_.accept(in_addr_word(from));
		if (!result.admitted) {
			close_socket(accepted);
		} else if (!tcp_send_all(accepted, result.send.data(), result.send.size())) {
			server_.close(result.connection);
			close_socket(accepted);
		} else {
			clients_.push_back(Client{accepted, result.connection});
		}
	}
	std::array<uint8_t, ADMIN_SERVER_BUFFER_BYTES> buffer{};
	for (size_t i = 0; i < clients_.size();) {
		Client &client = clients_[i];
		const size_t room = server_.receive_room(client.id);
		bool keep = true;
		if (room != 0) {
			const int n = tcp_recv_nonblocking(client.socket, buffer.data(), room);
			if (n == TCP_RECV_WOULD_BLOCK) {
				++i;
				continue;
			}
			if (n <= 0) {
				keep = false;
			} else {
				const AdminServer::Received received =
						server_.receive(client.id, buffer.data(), static_cast<size_t>(n));
				for (const std::vector<uint8_t> &packet : received.send)
					if (!tcp_send_all(client.socket, packet.data(), packet.size())) break;
				keep = received.keep_open;
			}
		}
		if (keep) {
			++i;
			continue;
		}
		server_.close(client.id);
		close_socket(client.socket);
		clients_.erase(clients_.begin() + static_cast<std::ptrdiff_t>(i));
	}
}

void AdminTcpServer::close() {
	for (Client &client : clients_) {
		server_.close(client.id);
		close_socket(client.socket);
	}
	clients_.clear();
	close_socket(listener_);
	port_ = 0;
}

} // namespace opennova::net
