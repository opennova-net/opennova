#pragma once

#include <string>

namespace opennova::serve {

// The --credentials file: KEY=VALUE lines, NOVAWORLD_USER / NOVAWORLD_PASS for the account the
// listing logs in with to fetch its HOSTKEY. A line starting with `#` or `;` is a comment, a value
// may be quoted, and other keys are ignored. The caller registers every value as a log secret.
struct Credentials {
	std::string user;
	std::string pass;
	bool present() const { return !user.empty() && !pass.empty(); }
};
// On failure returns false and sets `error`.
bool load_credentials(const std::string &path, Credentials &out, std::string &error);

} // namespace opennova::serve
