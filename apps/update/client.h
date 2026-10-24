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
};

// Read-only discovery: runtime, selected content, and installed expansions with a wizard.
bool discover_products(const Options &options, std::vector<Product> &out, std::string &error);
// The same worker drives the dialog, --check and loopback tests. Checks write no files.
// Downloads are validated before a product is changed; a failed write can leave a partial
// install, which a subsequent run rechecks. Cancellation is checked before installation.
bool run(const Options &options, const std::vector<Product> &products, Result &result,
		std::string &error, const ProgressSink &progress = {}, const std::atomic_bool *cancel = nullptr);

} // namespace opennova::updater
