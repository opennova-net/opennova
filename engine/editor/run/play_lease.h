#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <editor/run/process_platform.h>

namespace opennova::editor {

// A running game's claim on the build directory it runs from (ADR 0046 d8, S13 A1). Play writes
// <output_root>/<build-id>.lease beside the directory (which stays immutable), naming the game's
// process and executable, and removes it when the game stops. Before a build prunes the older
// build directories under its output root, every lease there whose process still runs protects
// its directory (ProcessPlatform::process_alive), so a game left running across an editor
// restart keeps its files; a lease whose process is gone is deleted. Only a file that proves it
// is a lease (a build id's name, a lease record) is ever read or deleted: the output root may be
// any folder.
inline constexpr int kPlayLeaseSchemaVersion = 1;
inline constexpr const char *kPlayLeaseSuffix = ".lease";

struct PlayLease {
	std::string build_dir; // the directory the game runs from: <output_root>/<build-id>
	int64_t pid = -1;
	std::string executable;
};

// Writes the lease of the game `lease.pid` runs from `lease.build_dir`, beside that directory;
// false with `error` when it cannot be written.
bool write_play_lease(const PlayLease &lease, std::string &error);
// Removes the lease of `build_dir`, when there is one.
void remove_play_lease(const std::string &build_dir);
// The build directories under `output_root` whose lease names a process that still runs, the
// others' leases deleted. `running_pid` is the session's own game, which it holds by its handle:
// its lease counts as live without asking.
std::vector<std::string> live_leased_dirs(const std::string &output_root, ProcessPlatform &platform,
		int64_t running_pid = -1);

} // namespace opennova::editor
