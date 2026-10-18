#pragma once

// The service's random secrets: the machines' one-time tokens and agent
// secrets, the hosted servers' passwords, the owner-only scratch directories'
// names. Service policy, not a port.

#include <cstddef>
#include <string>

namespace opennova::novaworld_server {

// `length` letters and digits from the OS CSPRNG.
std::string random_secret(size_t length);

} // namespace opennova::novaworld_server
