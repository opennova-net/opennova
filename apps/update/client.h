#pragma once

#include "options.h"

#include <atomic>
#include <chrono>
#include <cstdint>
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
	bool app = false; // an app's folder's own (its OPENNOVA.WIZ naming another login than the runtime's)
};

// What UPDATE.EXE's dialog (IDD 102) shows of a product's run, as its state machine and its
// passes write each control: the product line (control 1007, the command file's PRODUCT), the
// status line (1005), the current file (1002), the bar (1003, 0..range at position) and the
// three counter lines (1000, 1001, 1006) as `lines` says; `start` is the run's START, whence the
// elapsed line (1004) and the throughput. A run is a product's: each of retail's is a process of
// its own, nested for an expansion (D-UPD-12). [orig: Cupdate2Dlg_StateTick @ 0x408170
// (UPDATE.EXE); Cupdate2Dlg_ReadCommandFile @ 0x4076e0, Cupdate2Dlg_PrecheckCommands @ 0x407820,
// Cupdate2Dlg_OnGetReply @ 0x406ec0, Cupdate2Dlg_InstallPffRows @ 0x406380 (UPDATE.EXE)]
struct Progress {
	enum class Lines {
		Blank, // the three lines empty: the run's start, and PACK's clearing @0x408cb8..0x408cd8
		Commands, // the precheck's "%d/%d Commands": `done` rows of `total` @0x40791b
		Download, // the fetch's: `done` bytes of the rows fetched of the `total` to fetch @0x408bd2
		Files, // the install's and the integration's "%d/%d Files" @0x40648e, @0x408d34
	};
	std::string product;
	std::string status;
	std::string file;
	Lines lines = Lines::Blank;
	uint64_t done = 0;
	uint64_t total = 0;
	uint32_t position = 0;
	uint32_t range = 0;
	std::chrono::steady_clock::time_point start{};
};
using ProgressSink = std::function<void(const Progress &)>;

// The dialog's three counter lines and its elapsed line for `progress`, `elapsed_ms` after its
// start, as StateTick writes them each tick: "Downloading:  %d/%d KBytes", the throughput (the
// fetched bytes over the elapsed seconds, "%d BPS" to 1024, "%d KBPS" past it) and the estimate
// (all the bytes to fetch over that rate, "-" at none), from the CRT's truncation (a rate no int
// holds, the first millisecond's infinity among them, is INT_MIN); "Elapsed time: %2.1f
// seconds". [orig: Cupdate2Dlg_StateTick @ 0x408170 (UPDATE.EXE) @0x408bd2..0x408c7c, the
// elapsed line @0x408dc4]
struct DialogLines {
	std::string downloaded; // control 1000
	std::string throughput; // control 1001
	std::string estimated;  // control 1006
	std::string elapsed;    // control 1004
};
DialogLines dialog_lines(const Progress &progress, uint64_t elapsed_ms);

// How long an install's connect to a gateway or an update server may take before the next one is
// tried: retail's 10 s from the connect's start. A check keeps its own bound (at most 2 s each,
// within its 10 s in all: D-UPD-7). [orig: UpdateSocket_Connect @ 0x403570 (UPDATE.EXE), the start
// tick @0x4036c4, GetTickCount() - it >= 0x2710 @0x403726..0x403731]
inline constexpr int kInstallConnectTimeoutMs = 10000;

// The result box's MessageBox type: retail's 0, MB_OK with no icon, a failure's as a success's.
// [orig: Cupdate2Dlg_ReportResult @ 0x405140 (UPDATE.EXE), uType 0 pushed @0x4053ce]
inline constexpr unsigned kResultBoxType = 0;

// The installed command file of a product's folder (its UPDATE.WIZ's CMD, else an app's folder's
// OPENNOVA.WIZ's, else OP: Acid Gambit's), as the result box reads it:
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
	std::string launch_exe; // a legacy run's or an app's command file's EXE, if present
	bool app = false; // the run was an app's folder's (relaunch_command)
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

// Read-only discovery: runtime, selected content, and installed expansions with a wizard; in an
// app's folder (its OPENNOVA.WIZ naming another login than ONRUNTIME) that app alone, /game and
// --no-runtime refused there.
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
