#pragma once

#include "options.h"

#include <atomic>
#include <functional>
#include <string>
#include <vector>

namespace opennova::updater {

struct Product {
	std::filesystem::path dir;
	std::string login;
	std::string command;
	std::vector<std::string> gateways;
	bool native = false;
	bool expansion = false; // a registered expansion's (expansion/<name>/UPDATE.WIZ)
};

struct Progress {
	std::string status;
	std::string file;
	uint64_t bytes = 0;
	uint64_t total = 0;
};
using ProgressSink = std::function<void(const Progress &)>;

// How long an install's connect to a gateway or an update server may take before the next one is
// tried: retail's 10 s from the connect's start. A check keeps its own bound (at most 2 s each,
// within its 10 s in all: D-UPD-7). [orig: UpdateSocket_Connect @ 0x403570 (UPDATE.EXE), the start
// tick @0x4036c4, GetTickCount() - it >= 0x2710 @0x403726..0x403731]
inline constexpr int kInstallConnectTimeoutMs = 10000;

// The installed command file of a product's folder (its wizard's CMD), as the result box reads it:
// its CRC and its VERSION, or none when it does not open. Retail reads version.txt here; ours reads
// the command file the release wrote, whose VERSION is the release's (D-UPD-11).
struct VersionMark {
	bool present = false;
	uint32_t crc = 0;
	std::string version;
};
VersionMark version_mark(const std::filesystem::path &dir);

struct Result {
	bool available = false;
	bool applied = false;
	bool revision_pending = false;
	size_t products = 0;
	size_t files = 0;
	std::string launch_exe; // legacy command file's EXE, if present
	// An install's expansions in their order, each run as retail runs one, a nested updater with a
	// result box of its own: its folder, its command file as its run found it, and its outcome
	// (an install goes on past a failed one). A check records none.
	struct Expansion {
		std::filesystem::path dir;
		VersionMark start;
		bool ok = false;
		std::string error;
	};
	std::vector<Expansion> expansions;
};

// Read-only discovery: runtime, selected content, and installed expansions with a wizard.
bool discover_products(const Options &options, std::vector<Product> &out, std::string &error);
// The same worker drives the dialog, --check and loopback tests. Checks write no files.
// A product's fetched files land in its update\ directory as they arrive, the install reads
// them back one at a time, and the directory goes when the product is done, installed or not.
// Downloads are validated before a product is changed; a failed write can leave a partial
// install, which a subsequent run rechecks. Cancellation is checked before installation.
// An install stops at a failing runtime or game product; an expansion's failure is recorded
// and the install goes on, as retail's nested expansion runs do. A check goes on past any
// failing product, reporting an update another product has, and fails only when it found
// none and a product failed.
bool run(const Options &options, const std::vector<Product> &products, Result &result,
		std::string &error, const ProgressSink &progress = {}, const std::atomic_bool *cancel = nullptr);

} // namespace opennova::updater
