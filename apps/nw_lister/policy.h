#pragma once

#include "net_datagram_socket.h"
#include "net_sockets.h"

#include <net/npwire/idatagram_socket.h>

#include <cstdint>
#include <set>
#include <string>

namespace opennova::nw_lister {

// Which NovaWorld destinations a process may reach before --allow-public.
enum class DestinationPolicy {
	// opennova-nw-lister: every destination must be on 127.0.0.0/8, and a name does not resolve at
	// all (the lookup would itself be traffic); "localhost" is loopback.
	LoopbackOnly,
	// opennova-serve (ADR 0051 d6): loopback and any host off NovaLogic's domain, the OpenNova
	// service being configured per deployment; a name under novaworld.net (NovaLogic's live
	// service, is_novaworld_domain_host) is refused, unresolved. The rule is by name, so it is
	// applied where a name resolves; a datagram to an address passes.
	NovaLogicGated,
};

// The name-level rule, before any lookup: may `host` be reached under `policy`?
bool destination_permitted(const std::string &host, DestinationPolicy policy, bool allow_public);

// Resolve `host` into `out` (its port left alone) under the policy. `what` names the
// destination in the refusal's log line.
bool resolve_destination(const std::string &host, net::Endpoint &out, DestinationPolicy policy, bool allow_public,
                         const char *what);

// The policy at the datagram level: the gate response and the session name further hosts, so
// every send is checked, not just the first resolve (LoopbackOnly; NovaLogicGated's rule is by
// name). Non-owning: `inner` outlives it.
class PolicySocket final : public IDatagramSocket {
public:
	PolicySocket(IDatagramSocket &inner, DestinationPolicy policy, bool allow_public)
	    : inner_(inner), policy_(policy), allow_public_(allow_public) {}

	int recv_from(uint8_t *buf, std::size_t cap, PeerAddr &from) override {
		return inner_.recv_from(buf, cap, from);
	}
	void send_to(const PeerAddr &to, const uint8_t *data, std::size_t len) override;

private:
	IDatagramSocket &inner_;
	DestinationPolicy policy_;
	bool allow_public_;
	std::set<uint32_t> refused_;
};

} // namespace opennova::nw_lister
