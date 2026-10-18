#pragma once

// A static address's name (ADR 0066): its owner's (or an admin's) label, which
// names its A record in the deployment's play zone (<name>.play.<domain>; on
// staging play.staging.<domain>) and is unique for good among the addresses
// (hosted_addresses.name: a released one keeps it, until an admin renames it).
// 3 to 32 of lower-case letters, digits and '-', a letter or digit first and
// last, no "--" (an IDNA label's mark), not all digits, and none of the names
// the service keeps for itself (its hosts and programs, the words a person
// would take for the service's own) or that start "srv-" (a hosted server's
// service account). Pure. Service policy, not a port.

#include <string>
#include <string_view>

namespace opennova::novaworld_server::hosted {

inline constexpr size_t kAddressNameMin = 3;
inline constexpr size_t kAddressNameMax = 32;

// "" when `name` may name a static address, else why not (an owner reads it).
std::string address_name_refusal(std::string_view name);

// The name's host under `zone` ("smoke" under "play.opennova.net":
// "smoke.play.opennova.net"), without a trailing dot.
std::string address_hostname(std::string_view name, std::string_view zone);

} // namespace opennova::novaworld_server::hosted
