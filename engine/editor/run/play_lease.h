#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <editor/run/process_platform.h>

namespace opennova::editor {

// A running game's claim on the build directory it runs from (ADR 0046 d8, S13 A1). Play writes
// <output_root>/<build-id>.<pid>.lease beside the directory (which stays immutable), naming the
// game's process by its pid and the creation time the OS reported for it (process_identity),
// and removes that lease, its own and no other, when the game stops: two games may run from one
// build. When a build publishes, every directory a lease names whose process may still run
// (Alive, or Unknown: a lease the platform cannot check is kept) is protected from the prune, so
// a game left running across an editor restart keeps its files; a lease whose process is gone
// (Dead: none of that pid, one that exited, one created at another time) is deleted. Only a file
// that proves it is a lease (a build id's and a pid's name, a lease record naming both) is ever
// read or deleted: the output root may be any folder.
inline constexpr int kPlayLeaseSchemaVersion = 2;
inline constexpr const char *kPlayLeaseSuffix = ".lease";

struct PlayLease {
	std::string build_dir; // the directory the game runs from: <output_root>/<build-id>
	int64_t pid = -1;
	std::string image;   // the image the OS reported for the game ("" when it could not say)
	std::string created; // when the OS created it, as the platform writes it ("" when unknown)
};

// Whether the process a lease names still runs: `pid`, created at `created`.
using LeaseLiveness = std::function<ProcessLiveness(int64_t pid, const std::string &created)>;

// Writes the lease of the game `lease.pid` runs from `lease.build_dir`, beside that directory;
// false with `error` when it cannot be written, or `build_dir` is not a build's (its name a build
// id) or the pid is none.
bool write_play_lease(const PlayLease &lease, std::string &error);
// Removes the lease the game `pid` holds on `build_dir`, when there is one; another game's lease on
// the same directory stays.
void remove_play_lease(const std::string &build_dir, int64_t pid);
// The build directories under `output_root` a lease protects: those whose process `liveness` says
// is Alive or cannot tell (Unknown); the leases it says are Dead are deleted.
std::vector<std::string> leased_build_dirs(const std::string &output_root, const LeaseLiveness &liveness);

} // namespace opennova::editor
