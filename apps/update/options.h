#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace opennova::updater {

struct Options {
	bool check = false;
	bool no_runtime = false;
	bool run = false;
	bool verbose = false;
	bool legacy = false;
	bool help = false;
	bool game_explicit = false;
	std::string game = "opennova";
	std::filesystem::path runtime_dir;
	std::filesystem::path resource_dir;
	std::string server;
	uint16_t port = 0;
	uint64_t parent_pid = 0;
	uintptr_t parent_window = 0;
	std::vector<std::string> run_args;
};

// Arguments exclude argv[0]. Defaults are based on the executable, never the caller's cwd.
bool parse_options(const std::vector<std::string> &args,
		const std::filesystem::path &executable, Options &out, std::string &error);
const char *usage();

} // namespace opennova::updater
