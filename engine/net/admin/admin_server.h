#pragma once

// The retail remote-admin protocol, server side: the TCP console a game server opens on
// `remote_admin_port` (CAdminServer), the counterpart of admin_protocol.h's client codec. It
// owns each connection's state machine (the challenge, the encrypted login or the plaintext
// QUERY, then one command packet per read) and nothing else: socket I/O stays in the
// embedder, which hands each accepted connection's source address and each read's bytes in and
// sends the packets that come back; the command verbs run in the embedder's handler
// (runtime/inmatch/admin_console.h). The witness record is docs/net/novaworld-net-re.md §6.9.
// [orig: CAdminServer_ProcessFrame @0x406f50; CAdminServer_AcceptConnection @0x405580;
//  CAdminServer_ProcessClientData @0x406ec0; CAdminServer_HandleLogin @0x405870;
//  CAdminServer_DisconnectClient @0x402cb0]

#include <base/io/crt_rand.h>
#include <formats/admincfg/admin_cfg.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace opennova {

// What a logged-in connection carries into the dispatcher: the matched admin.cfg user's stored
// name (the log's `User command (%s)` names it) and its rights word.
// [orig: HandleLogin @0x4059AD (slot +24 = the entry's rights), @0x4059C0 (slot +20 = the
//  entry's name)]
struct AdminSession {
	std::string user;
	uint32_t rights = 0;
};

// The command half: the embedder's verb dispatcher over the live server state.
class AdminCommandHandler {
public:
	virtual ~AdminCommandHandler() = default;
	// One authenticated command line (the packet's text before its NUL): append the replies in
	// order, each sent as one packet. False is QUIT, whose 0 closes the connection; every other
	// line returns 1. [orig: CAdminServer_DispatchCommand @0x406720 — QUIT's `jz` @0x406811
	//  keeps stricmp's 0, every handler path ends at `mov eax, 1` @0x406D5D]
	virtual bool dispatch(const AdminSession &session, std::string_view line,
			std::vector<std::string> &replies) = 0;
	// The plaintext QUERY's status report. [orig: CAdminServer_HandleStatus @0x402e30]
	virtual std::string status_report() = 0;
};

// The 1024-byte receive buffer each accepted connection gets. [orig: AcceptConnection
// @0x405765 (operator new 0x400), @0x40577B (capacity 1024)]
inline constexpr size_t ADMIN_SERVER_BUFFER_BYTES = 1024;

class AdminServer {
public:
	// One admin_log.txt line, with its trailing "\n" (the embedder writes it as given). The
	// log is the process's: retail opens admin_log.txt for writing at static construction, so
	// each launch truncates it. [orig: CAdminServer_Construct @0x402c10, the fopen @0x402c8c]
	using LogSink = std::function<void(std::string_view line)>;

	// `rand` is the CRT stream the challenge draws from (retail's process rand()).
	AdminServer(admincfg::AdminConfig config, AdminCommandHandler &handler, io::CrtRand &rand,
			LogSink log = {});

	struct Accepted {
		bool admitted = false;
		uint32_t connection = 0;
		std::vector<uint8_t> send; // the challenge packet
	};
	// A connection the listener accepted from `source_ip`, the raw in_addr dword (`a | b<<8 |
	// c<<16 | d<<24` for a.b.c.d). A whitelist miss logs and is refused: the embedder closes the
	// socket before anything is sent. [orig: CAdminServer_AcceptConnection @0x405580]
	Accepted accept(uint32_t source_ip);

	// How many bytes the connection's next read may take: the buffer's room. A packet that
	// declares more than the buffer holds leaves it full and waiting for good, so the room
	// stays 0 (retail's wedged slot). [orig: ProcessFrame's recv size @0x406f8a..0x406f9e]
	size_t receive_room(uint32_t connection) const;

	struct Received {
		bool keep_open = true;
		std::vector<std::vector<uint8_t>> send; // reply packets, in order
	};
	// One read's bytes (at most receive_room()). The buffer handles one packet and is then
	// emptied, so bytes past it in the same read are lost: a client sends one request at a time.
	// [orig: ProcessFrame @0x406fbc..0x406fc7; ProcessClientData @0x406ec0..0x406f3d]
	Received receive(uint32_t connection, const uint8_t *data, size_t size);

	// The embedder's close: a reset, a recv error, or the peer's FIN (D-NET-360: retail reads a
	// FIN as data and leaks the slot). [orig: ProcessFrame @0x406fa6..0x406fd0 ->
	// CAdminServer_DisconnectClient @0x402cb0]
	void close(uint32_t connection);

	size_t connection_count() const { return connections_.size(); }
	bool authenticated(uint32_t connection) const;

private:
	struct Connection {
		uint32_t id = 0;
		std::array<uint8_t, ADMIN_SERVER_BUFFER_BYTES> buffer{};
		size_t received = 0;
		bool authenticated = false;
		AdminSession session;
		std::array<uint8_t, 33> challenge{};
	};

	Connection *find(uint32_t connection);
	const Connection *find(uint32_t connection) const;
	// ProcessClientData's verdict once a whole packet is buffered: false closes.
	bool handle_packet(Connection &conn, std::vector<std::vector<uint8_t>> &send);
	bool handle_login(Connection &conn, size_t data_len, std::vector<std::vector<uint8_t>> &send);
	void log(const std::string &line) const;

	admincfg::AdminConfig config_;
	AdminCommandHandler &handler_;
	io::CrtRand &rand_;
	LogSink log_;
	std::vector<Connection> connections_;
	uint32_t next_id_ = 1;
};

} // namespace opennova
