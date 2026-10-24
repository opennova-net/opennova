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

struct Result {
	bool available = false;
	bool applied = false;
	bool revision_pending = false;
	size_t products = 0;
	size_t files = 0;
	std::string launch_exe; // legacy command file's EXE, if present
	// An install's expansions that failed while the install went on, with the error of each.
	struct Failure {
		std::filesystem::path dir;
		std::string error;
	};
	std::vector<Failure> expansion_failures;
};

// Read-only discovery: runtime, selected content, and installed expansions with a wizard.
bool discover_products(const Options &options, std::vector<Product> &out, std::string &error);
// The same worker drives the dialog, --check and loopback tests. Checks write no files.
// Downloads are validated before a product is changed; a failed write can leave a partial
// install, which a subsequent run rechecks. Cancellation is checked before installation.
// An install stops at a failing runtime or game product; an expansion's failure is recorded
// and the install goes on, as retail's nested expansion runs do. A check goes on past any
// failing product, reporting an update another product has, and fails only when it found
// none and a product failed.
bool run(const Options &options, const std::vector<Product> &products, Result &result,
		std::string &error, const ProgressSink &progress = {}, const std::atomic_bool *cancel = nullptr);

} // namespace opennova::updater
