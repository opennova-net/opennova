#pragma once

// Role partitioning for nw_replay: split a captured NovaWorld session into one
// role per participant, derived purely from the NAPI session opcode + UDP ports.
// No SCRK, no inner decode, no items.def — the replay tool forwards raw bytes, so
// it only needs to know which port is the host and which are clients.

#include <cstdint>
#include <map>
#include <vector>

#include <pcapio/pcap_reader.h>

namespace opennova::replay {

// A captured datagram, classified by its NAPI session opcode. Client opcodes
// (0x42 auth / 0x43 protocol / 0x46 goodbye) flow client->host; server opcodes
// (0x81 hello / 0x82 auth / 0x83 protocol) flow host->client. Anything that
// isn't a valid NAPI envelope with a session opcode is NotSession (gate/lobby/
// SSDP/mDNS noise the capture also contains).
enum class SessionKind {
	NotSession,
	ClientAuth,
	ClientProtocol,
	ServerAuth,
	ServerProtocol,
};

inline bool is_server_kind(SessionKind k) {
	return k == SessionKind::ServerAuth || k == SessionKind::ServerProtocol;
}
inline bool is_auth_kind(SessionKind k) {
	return k == SessionKind::ClientAuth || k == SessionKind::ServerAuth;
}

// Classify one captured UDP payload by its NAPI session opcode.
SessionKind classify_session(const std::vector<uint8_t> &payload);

// The client-side (session-key) port for a session datagram, or 0 if it is not
// session traffic. Client-kind -> src port; server-kind -> dst port (matches
// wire_capture's per-session keying).
int session_port_of(const net::PcapDatagram &d);

struct Roles {
	int host_port = 0;               // host UDP port (0 if no session traffic)
	std::vector<int> client_ports;   // distinct client ports, ascending
	bool ok() const { return host_port != 0 && !client_ports.empty(); }
	// role 0 = host; roles 1..N = client_ports[role-1].
	int role_count() const { return host_port ? int(client_ports.size()) + 1 : 0; }
};

// Incremental partition tally: feed datagrams one at a time (so a multi-GB capture
// can be partitioned by streaming, not loading it all), then finish() to derive
// the host + client ports. The host is the port on the host side of every session
// flow; each client is a distinct port on the other side. Noise is ignored.
class RoleTally {
public:
	void add(const net::PcapDatagram &d);
	Roles finish() const;

private:
	std::map<int, int> host_votes_;
	std::map<int, int> client_seen_;
};

// Derive the role partition from all of `pkts` (a convenience over RoleTally).
Roles partition_roles(const std::vector<net::PcapDatagram> &pkts);

} // namespace opennova::replay
