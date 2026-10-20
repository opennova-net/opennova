// A test program's cases in named parts, so ctest runs a row per part and the parts run side by
// side, on as many cores as there are parts, where one row ran them one after another
// (tests/CMakeLists.txt opennova_add_parts registers the rows):
//
//   <program>                               every part, in order, each headed `-- <name>`: a
//                                           by-hand run, as before
//   <program> --part <name> --parts <all>   that part alone, once <all> is checked to name the
//                                           program's parts exactly, in order
//
// Each row checks its own registration that way, so a part added to the program and not to the
// CMake list fails every row rather than never running in CI. Infrastructure only.
#pragma once

#include <cstdio>
#include <string>
#include <vector>

namespace test_parts {

struct Part {
	const char *name;
	std::vector<int (*)()> cases; // in order; each returns its failures
};

// Takes `--part <name> --parts <all...>` (the last arguments, when given) off argv and runs the
// part, or every part when there are none; returns the failures, 1 for a selection that is not
// the program's. Any argument before them is the caller's.
inline int run(int &argc, char **argv, const std::vector<Part> &parts) {
	int at = 1;
	while (at < argc && std::string(argv[at]) != "--part") ++at;
	const bool by_part = at < argc;
	std::string wanted;
	std::vector<std::string> listed;
	if (by_part) {
		if (at + 2 >= argc || std::string(argv[at + 2]) != "--parts") {
			std::fprintf(stderr, "FAIL: usage: --part <name> --parts <every part>\n");
			return 1;
		}
		wanted = argv[at + 1];
		for (int i = at + 3; i < argc; ++i) listed.emplace_back(argv[i]);
		argc = at;
		std::vector<std::string> own;
		for (const Part &part : parts) own.emplace_back(part.name);
		if (listed != own) {
			std::string names;
			for (const std::string &name : own) names += " " + name;
			std::fprintf(stderr, "FAIL: the rows' parts are not the program's (its parts:%s)\n", names.c_str());
			return 1;
		}
	}
	int failures = 0;
	bool ran = false;
	for (const Part &part : parts) {
		if (by_part && wanted != part.name) continue;
		ran = true;
		// A run of every part heads each part's output with its name; a row is its part's alone.
		if (!by_part) {
			std::printf("-- %s\n", part.name);
			std::fflush(stdout);
		}
		for (int (*each)() : part.cases) failures += each();
	}
	if (by_part && !ran) {
		std::fprintf(stderr, "FAIL: no part %s\n", wanted.c_str());
		return 1;
	}
	return failures;
}

} // namespace test_parts
