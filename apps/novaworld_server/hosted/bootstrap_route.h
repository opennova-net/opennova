#pragma once

// The cloud machines' bootstrap route (bootstrap.h): POST
// /hosted/bootstrap/<server id> with "Authorization: Bearer <token>", the
// token the machine's user-data carries, answered with its bundle once.
//
// Its exposure. The route is on the HTTP listener's port (8080), which is open
// to the internet for the retail client, but outside /api/, so the portal's
// nginx (which forwards /api/ alone) never reaches it, and Cloudflare (which
// fronts nginx) neither. It answers only a TCP peer inside
// ONNET_HOSTED_BOOTSTRAP_CIDRS (the VPC's private range: the machines fetch
// over the service's private address), read from the socket itself, never from
// X-Real-IP or X-Forwarded-For, which anyone can write; every other peer gets
// the same 404 as an unknown path, with nothing logged. A peer inside the range
// still needs a live token bound to its own address (BootstrapStore::redeem).
// Every refusal is that one 404; a refusal inside the range is logged without
// the token, at most once per peer per minute (LogThrottle). The reply carries Cache-Control: no-store, and is never logged.

#include "bootstrap.h"
#include "web_access.h"

#include <vector>

namespace opennova::novaworld_server::hosted {

// Registers the route on `app`; `store` must outlive it.
void register_bootstrap_route(WebApp &app, BootstrapStore &store, std::vector<Ipv4Cidr> admitted);

} // namespace opennova::novaworld_server::hosted
