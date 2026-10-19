#pragma once

// The slots' working directories on the machine: <root>/slot-<k> (root
// /srv/slots), each its container's /srv/serve. A start writes one from
// scratch: whatever the slot's previous container left is removed, the
// directory is made again 0700 for the slot's user, and each of the service's
// files (the host file, game.cfg, admin.cfg, the NovaWorld credentials) is
// written 0600 for that user, opened with O_EXCL and O_NOFOLLOW. A container's
// removal wipes its directory, its secrets with it. Removal never follows a
// link the container left (std::filesystem::remove_all removes the link). What
// a server writes there for its agent (its --exit-file, its --stats-file) is
// read as root reads a file the slot's user controls (read_written). The
// agent runs as root; `chown` false (a development run as another user) leaves
// the files that user's. Platform plumbing, not a port.

#include <chrono>
#include <cstddef>
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

	// The most read_written reads: the files a server writes for the agent are a
	// line or two.
	static constexpr size_t kMaxWrittenBytes = 4096;
	// The file `name` (a bare file name) the server of slot `slot` wrote in its
	// directory, and its last write: opened from the directory, never through a
	// link (O_NOFOLLOW) nor waiting on a FIFO (O_NONBLOCK), a regular file alone,
	// at most kMaxWrittenBytes of it, the last write its open file's own. false
	// for none, or for anything else (a link, a FIFO, a directory, a file past
	// the bound), which reads as nothing written.
	bool read_written(int slot, const char *name, std::string &out,
			std::chrono::system_clock::time_point &written) const;

private:
	std::string root_;
	bool chown_;
};

} // namespace opennova::hostd
