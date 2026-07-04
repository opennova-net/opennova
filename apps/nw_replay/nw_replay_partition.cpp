#include "nw_replay_partition.h"

#include <napi/envelope.h>
#include <npwire/session_keys.h>

#include <algorithm>
#include <map>

namespace opennova::replay {

SessionKind classify_session(const std::vector<uint8_t> &payload) {
	if (payload.empty()) return SessionKind::NotSession;
	std::vector<uint8_t> stripped(payload.size());
	size_t out = 0;
	if (napi_envelope_decode(payload.data(), payload.size(), stripped.data(),
	                         stripped.size(), &out) != 0 ||
	    out == 0)
		return SessionKind::NotSession;
	switch (stripped[0]) {
	case SESSION_OPCODE_CLIENT_AUTH:
		return SessionKind::ClientAuth;
	case SESSION_OPCODE_PROTOCOL_MESSAGE:
	case SESSION_OPCODE_CLIENT_GOODBYE:
		return SessionKind::ClientProtocol;
	case SESSION_OPCODE_SERVER_AUTH:
		return SessionKind::ServerAuth;
	case SESSION_OPCODE_SERVER_HELLO:
	case SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE:
		return SessionKind::ServerProtocol;
	default:
		return SessionKind::NotSession;
	}
}

int session_port_of(const net::PcapDatagram &d) {
	const SessionKind k = classify_session(d.payload);
	if (k == SessionKind::NotSession) return 0;
	return is_server_kind(k) ? d.dstport : d.srcport;
}

void RoleTally::add(const net::PcapDatagram &d) {
	const SessionKind k = classify_session(d.payload);
	if (k == SessionKind::NotSession) return;
	const int client = is_server_kind(k) ? d.dstport : d.srcport;
	const int host = is_server_kind(k) ? d.srcport : d.dstport;
	host_votes_[host]++;
	client_seen_[client]++;
}

Roles RoleTally::finish() const {
	Roles r;
	int best = 0;
	for (const auto &[port, n] : host_votes_)
		if (n > best) {
			best = n;
			r.host_port = port;
		}
	for (const auto &[port, n] : client_seen_)
		if (port != r.host_port) r.client_ports.push_back(port);
	std::sort(r.client_ports.begin(), r.client_ports.end());
	return r;
}

Roles partition_roles(const std::vector<net::PcapDatagram> &pkts) {
	RoleTally t;
	for (const auto &d : pkts) t.add(d);
	return t.finish();
}

} // namespace opennova::replay
