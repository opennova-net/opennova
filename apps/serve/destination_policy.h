#pragma once

#include "net_sockets.h"

#include <string>

namespace opennova::serve {

// Which NovaWorld destinations opennova-serve's listing may reach (ADR 0051 d6): loopback and any
// host off NovaLogic's domain, the OpenNova service being configured per deployment; a name under
// novaworld.net (NovaLogic's live service, is_novaworld_domain_host) is refused, unresolved,
// until --allow-public. The rule is by name, so it is applied where a name resolves (the gate,
// the NWU session's host, the web hosts of the login and the HOSTKEY); a datagram to an address
// passes.

// The name-level rule, before any lookup: may `host` be reached?
bool destination_permitted(const std::string &host, bool allow_public);

// Resolve `host` into `out` (its port left alone) under the rule. `what` names the destination in
// the refusal's log line.
bool resolve_destination(
		const std::string &host, net::Endpoint &out, bool allow_public, const char *what);

} // namespace opennova::serve
