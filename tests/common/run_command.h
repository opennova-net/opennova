// Running a command line from a test: the one home for the run/quoted pair the
// tests that drive the opennova-3di executable used to carry per file.
// Header-only, infrastructure only (no retail counterpart to cite).
#pragma once

#include <cstdio>
#include <cstdlib>
#include <string>

namespace test_cmd {

// Run `cmd` through the shell and return its status. stdout is flushed first
// so the test's own lines stay ahead of the child's.
inline int run(const std::string &cmd) {
	std::fflush(stdout);
#ifdef _WIN32
	// cmd.exe strips one pair of outer quotes: wrap the whole command.
	const std::string line = "\"" + cmd + "\"";
	return std::system(line.c_str());
#else
	return std::system(cmd.c_str());
#endif
}

// `s` as one double-quoted shell argument.
inline std::string quoted(const std::string &s) { return "\"" + s + "\""; }

} // namespace test_cmd
