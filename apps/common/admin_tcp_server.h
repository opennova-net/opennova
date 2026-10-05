#pragma once

// The remote-admin console's sockets: the listener on `remote_admin_port` and the
// connections it accepts, pumped once per frame into the engine's connection machine
// (net/admin/admin_server.h), as CAdminServer_ProcessFrame pumps them from the game loop
// and the menu. Socket I/O stays here; the engine takes and returns bytes.
// [orig: CAdminServer_Listen @0x406E00 (INADDR_ANY, non-blocking, backlog 5);
//  CAdminServer_ProcessFrame @0x406F50]

#include "net_sockets.h"

#include <net/admin/admin_server.h>

#include <cstdint>
#include <vector>

namespace opennova::net {

class AdminTcpServer {
public:
	explicit AdminTcpServer(AdminServer &server) : server_(server) {}
	~AdminTcpServer() { close(); }
	AdminTcpServer(const AdminTcpServer &) = delete;
	AdminTcpServer &operator=(const AdminTcpServer &) = delete;

	// Listen on `port` (0: ephemeral) on every interface, retail's INADDR_ANY, or on loopback
	// alone. The game listens only for a nonzero port [orig: Game_InitSubsystems
	// @0x4A72C7..0x4A72D9]; that test is the embedder's.
	bool listen(uint16_t port, bool loopback_only = false);
	uint16_t port() const { return port_; }
	bool listening() const { return listener_.is_valid(); }

	// One frame: one accept, then one read per connection (at most its buffer's room), the
	// replies sent, and a connection closed on an error, on the peer's FIN (retail reads a
	// FIN as data and keeps the slot, D-NET-360) or when the engine says so.
	// [orig: CAdminServer_ProcessFrame @0x406F50 — AcceptConnection @0x406F61, the recv
	//  @0x406F9E, WSAEWOULDBLOCK kept @0x406FB2, DisconnectClient @0x406FD0]
	void pump();

	void close();
	size_t connection_count() const { return clients_.size(); }

private:
	struct Client {
		Socket socket;
		uint32_t id = 0;
	};

	AdminServer &server_;
	Socket listener_;
	uint16_t port_ = 0;
	std::vector<Client> clients_;
};

} // namespace opennova::net
