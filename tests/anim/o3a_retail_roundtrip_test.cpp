// Retail clip sets through opennova-3di and back: `anim scene` writes each one
// as .o3a, `anim build` mints it again, and `anim compare` must call the two the
// same animation (the dead bone fields and float noise aside). The sets cover
// what the clip text carries: US01.ADM (the person rig's table, translated body
// clips with footstep and foley triggers), mp5_1st.adm (the first-person rig's
// nine keys), 357_1st.adm (a 40-bone viewmodel rig whose every event stands
// still at bottom = top = 1.07), plus the lone clips DT1RST (a channel that
// keys fewer times than its frame count) and DVFLEE1E (the one retail clip that
// keys its bones sparsely, with a duration table per bone). Every event's
// bottom and top ride the text as stored.
// Gated on OPENNOVA_JO_ASSETS (docs/asset-gated-tests.md).
//
//   o3a_retail_roundtrip_test <opennova-3di> <scratch dir>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "common/retail_paths.h"

namespace {

int run(const std::string &cmd) {
	std::fflush(stdout);
#ifdef _WIN32
	// cmd.exe strips one pair of outer quotes: wrap the whole command.
	const std::string line = "\"" + cmd + "\"";
	return std::system(line.c_str());
#else
	return std::system(cmd.c_str());
#endif
}

std::string quoted(const std::string &s) { return "\"" + s + "\""; }

// The `.bad` clips a table names sit beside it, so a round trip writes the
// rebuilt set into a directory of its own.
bool make_dir(const std::string &path) {
#ifdef _WIN32
	return run("if not exist " + quoted(path) + " mkdir " + quoted(path)) == 0;
#else
	return run("mkdir -p " + quoted(path)) == 0;
#endif
}

} // namespace

int main(int argc, char **argv) {
	if (argc != 3) {
		std::fprintf(stderr, "usage: o3a_retail_roundtrip_test <opennova-3di> <scratch dir>\n");
		return 2;
	}
	const std::string cli = quoted(argv[1]);
	const std::string dir = argv[2];
	int failures = 0;
	int ran = 0;
	for (const char *name : {"US01.ADM", "mp5_1st.adm", "357_1st.adm", "DT1RST.bad",
				 "DVFLEE1E.BAD"}) {
		const std::string source = retail::asset_file(name);
		if (source.empty()) {
			std::printf("SKIP-LEG: needs %s in OPENNOVA_JO_ASSETS\n", name);
			continue;
		}
		++ran;
		const std::string stem = std::string(name).substr(0, std::string(name).find('.'));
		const std::string home = dir + "/" + stem + ".rt";
		if (!make_dir(home)) {
			std::fprintf(stderr, "%s: cannot make %s\n", name, home.c_str());
			++failures;
			continue;
		}
		const std::string o3a = home + "/set.o3a";
		const std::string rebuilt = home + "/" + std::string(name);
		if (run(cli + " anim scene " + quoted(source) + " -o " + quoted(o3a)) != 0 ||
				run(cli + " anim build " + quoted(o3a) + " -o " + quoted(rebuilt)) != 0 ||
				run(cli + " anim compare " + quoted(source) + " " + quoted(rebuilt)) != 0) {
			std::fprintf(stderr, "%s: the clip round trip is not the same animation\n", name);
			++failures;
		}
	}
	if (ran == 0)
		return retail::skip("OPENNOVA_JO_ASSETS with US01.ADM, mp5_1st.adm, 357_1st.adm, "
							"DT1RST.bad, DVFLEE1E.BAD");
	if (failures == 0) std::printf("o3a_retail_roundtrip_test: ok (%d clip sets)\n", ran);
	return failures == 0 ? 0 : 1;
}
