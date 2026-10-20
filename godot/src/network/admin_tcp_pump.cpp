#include "network/admin_tcp_pump.h"

#include "util/string_convert.h"

#include <net/novaworld/lobby_vars.h> // platform_inet_addr
#include <runtime/inmatch/remote_admin.h> // kAdminListenAddress

#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>

#include <algorithm>
#include <cstddef>

using namespace godot;

namespace {

bool send_all(const Ref<StreamPeerTCP> &p_peer, const std::vector<uint8_t> &p_bytes) {
	PackedByteArray data;
	data.resize(static_cast<int64_t>(p_bytes.size()));
	std::copy(p_bytes.begin(), p_bytes.end(), data.ptrw());
	return p_peer->put_data(data) == OK;
}

} // namespace

bool AdminTcpPump::listen(uint16_t port) {
	close();
	server_socket_.instantiate();
	// IPv4 alone, every interface (engine inmatch/remote_admin.h kAdminListenAddress carries the
	// witness): "*" would open Godot's dual-stack listener, which also accepts IPv6 admins.
	if (server_socket_->listen(port, opennova::inmatch::kAdminListenAddress) != OK) {
		server_socket_.unref();
		return false;
	}
	return true;
}

void AdminTcpPump::pump() {
	if (!listening()) return;
	if (server_socket_->is_connection_available()) {
		Ref<StreamPeerTCP> peer = server_socket_->take_connection();
		if (peer.is_valid()) {
			// The engine's address word: the source's dotted quad as the platform stores it.
			const uint32_t source = opennova::platform_inet_addr(opennova::to_std(peer->get_connected_host()));
			const opennova::AdminServer::Accepted result = server_.accept(source);
			if (!result.admitted) {
				peer->disconnect_from_host();
			} else if (!send_all(peer, result.send)) {
				server_.close(result.connection);
				peer->disconnect_from_host();
			} else {
				clients_.push_back(Client{peer, result.connection});
			}
		}
	}
	for (size_t i = 0; i < clients_.size();) {
		Client &client = clients_[i];
		bool keep = true;
		const size_t room = server_.receive_room(client.id);
		if (room != 0) {
			client.peer->poll();
			if (client.peer->get_status() != StreamPeerTCP::STATUS_CONNECTED) {
				keep = false; // the peer's close or an error
			} else {
				const int32_t available = client.peer->get_available_bytes();
				if (available > 0) {
					const Array got = client.peer->get_partial_data(
							std::min<int32_t>(available, static_cast<int32_t>(room)));
					const PackedByteArray bytes = got.size() == 2 ? PackedByteArray(got[1]) : PackedByteArray();
					if (got.size() != 2 || static_cast<int>(got[0]) != OK || bytes.is_empty()) {
						keep = false;
					} else {
						const opennova::AdminServer::Received received = server_.receive(
								client.id, bytes.ptr(), static_cast<size_t>(bytes.size()));
						for (const std::vector<uint8_t> &packet : received.send)
							if (!send_all(client.peer, packet)) break;
						keep = received.keep_open;
					}
				}
			}
		}
		if (keep) {
			++i;
			continue;
		}
		server_.close(client.id);
		client.peer->disconnect_from_host();
		clients_.erase(clients_.begin() + static_cast<std::ptrdiff_t>(i));
	}
}

void AdminTcpPump::close() {
	for (Client &client : clients_) {
		server_.close(client.id);
		client.peer->disconnect_from_host();
	}
	clients_.clear();
	if (server_socket_.is_valid()) server_socket_->stop();
	server_socket_.unref();
}
