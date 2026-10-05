#pragma once

#include "net_datagram_socket.h"
#include "net_sockets.h"

#include <net/npwire/idatagram_socket.h>

#include <cstdint>
#include <set>
#include <string>

namespace opennova::nw_lister {

// The destination policy: until --allow-public every destination must be on 127.0.0.0/8, and a
// name does not resolve at all (the lookup would itself be traffic); "localhost" is loopback.
// `what` names the destination in the refusal's log line.
bool resolve_destination(const std::string &host, net::Endpoint &out, bool allow_public, const char *what);

// The policy at the datagram level: the gate response and the session name further hosts, so
// every send is checked, not just the first resolve. Non-owning: `inner` outlives it.
class PolicySocket final : public IDatagramSocket {
public:
	PolicySocket(IDatagramSocket &inner, bool allow_public) : inner_(inner), allow_public_(allow_public) {}

	int recv_from(uint8_t *buf, std::size_t cap, PeerAddr &from) override {
		return inner_.recv_from(buf, cap, from);
	}
	void send_to(const PeerAddr &to, const uint8_t *data, std::size_t len) override;

private:
	IDatagramSocket &inner_;
	bool allow_public_;
	std::set<uint32_t> refused_;
};

} // namespace opennova::nw_lister
