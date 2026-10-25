#pragma once

// The game's remote-admin sockets: the listener on game.cfg's remote_admin_port and the
// connections it accepts, pumped once per frame into the engine's connection machine
// (net/admin/admin_server.h). The device half only, in apps/common AdminTcpServer's shape (one
// accept, then one read per connection of at most its buffer's room, the replies sent, a
// connection closed on an error, on the peer's close or when the engine says so) over Godot's
// TCP classes; the console and the protocol are the engine's (runtime/inmatch/remote_admin.h
// and net/admin/admin_server.h carry the witnesses).

#include <godot_cpp/classes/stream_peer_tcp.hpp>
#include <godot_cpp/classes/tcp_server.hpp>

#include <net/admin/admin_server.h>

#include <cstdint>
#include <vector>

class AdminTcpPump {
public:
	explicit AdminTcpPump(opennova::AdminServer &server) : server_(server) {}
	~AdminTcpPump() { close(); }
	AdminTcpPump(const AdminTcpPump &) = delete;
	AdminTcpPump &operator=(const AdminTcpPump &) = delete;

	// Listen on `port` on every IPv4 interface (retail's AF_INET INADDR_ANY). False when the
	// listen fails, which the caller ignores as retail ignores Listen's result.
	bool listen(uint16_t port);
	bool listening() const { return server_socket_.is_valid() && server_socket_->is_listening(); }
	// One frame of the device: one accept, one read per connection.
	void pump();
	void close();

private:
	struct Client {
		godot::Ref<godot::StreamPeerTCP> peer;
		uint32_t id = 0;
	};

	opennova::AdminServer &server_;
	godot::Ref<godot::TCPServer> server_socket_;
	std::vector<Client> clients_;
};
