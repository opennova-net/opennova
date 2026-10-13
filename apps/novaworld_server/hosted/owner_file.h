#pragma once

// A file only the service's own account may read, written from scratch: the
// hosted servers' secret files (LocalProcessProvider's working directories)
// and the cloud provider's per-launch user-data (Ec2Provider, which carries a
// bootstrap token). Platform plumbing, not a port.

#include <filesystem>
#include <string>

namespace opennova::novaworld_server::hosted {

// Writes `content` to a new `path` (replacing a file there). A `secret` file is
// made the owner's alone before a byte is written: mode 0600 on POSIX, a
// protected DACL granting the service's own account alone on Windows; any
// other file takes 0644 or what the directory hands down. False with `error`
// (naming the file, never its content) when it could not.
bool write_owner_file(const std::filesystem::path &path, const std::string &content, bool secret,
		std::string &error);

// A directory only the service's own account may enter, made when missing
// (0700 on POSIX; on Windows it keeps what it inherits, and each secret file
// in it is protected by write_owner_file). False with `error` when it could
// not be made or restricted.
bool make_owner_directory(const std::filesystem::path &dir, std::string &error);

} // namespace opennova::novaworld_server::hosted
