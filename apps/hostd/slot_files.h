#pragma once

// The slots' working directories on the machine: <root>/slot-<k> (root
// /srv/slots), each its container's /srv/serve. A start writes one from
// scratch: whatever the slot's previous container left is removed, the
// directory is made again 0700 for the slot's user, and each of the service's
// files (the host file, game.cfg, admin.cfg, the NovaWorld credentials) is
// written 0600 for that user, opened with O_EXCL and O_NOFOLLOW. A container's
// removal wipes its directory, its secrets with it. Removal never follows a
// link the container left (std::filesystem::remove_all removes the link). The
// agent runs as root; `chown` false (a development run as another user) leaves
// the files that user's. Platform plumbing, not a port.

#include <cstdint>
#include <map>
#include <string>

namespace opennova::hostd {

class SlotFiles {
public:
	SlotFiles(std::string root, bool chown);

	const std::string &root() const { return root_; }
	// <root>/slot-<k>.
	std::string dir(int slot) const;
	// The root, 0755, where it is missing.
	bool ensure_root(std::string &error);
	// Slot `slot`'s directory from scratch, for `uid`, holding `files` (bare
	// name -> bytes).
	bool prepare(int slot, uint32_t uid, const std::map<std::string, std::string> &files,
			std::string &error);
	// Removes slot `slot`'s directory and everything in it; no directory is no error.
	bool wipe(int slot, std::string &error);

private:
	std::string root_;
	bool chown_;
};

} // namespace opennova::hostd
