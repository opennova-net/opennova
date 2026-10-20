// Clip sets through opennova-3di and back: `anim scene` writes each one as
// .o3a, `anim build` mints it again, and `anim compare` must call the two the
// same animation (the dead bone fields and float noise aside). The sets cover
// what the clip text carries, each minted at the shape of the shipped set it
// stands for (tests/fixtures/minimal_bad_gen.cpp): rig19.adm (US01.ADM's, the
// person rig's table, its walk with footstep and foley triggers), vm40.adm
// (mp5_1st.adm's and 357_1st.adm's: the first-person rig's nine keys, translated
// clips, every event standing still at bottom = top), plus the lone clips
// rig19_short.bad (DT1RST's, a channel that keys fewer times than its frame
// count), rig19_sprs.bad (DVFLEE1E's, the one shipped clip that keys its
// bones sparsely, with a duration table per bone), rig19_v0.bad (m60_rst's
// version 0 events), vm40_long.bad (M60_1i's channels keyed past the frame
// count), vm40_brk.bad (ak47_1st's X-triplicated positions, which the text
// states as stored: `bonepos`), rig19_junk.bad (the exporter's leftovers),
// vm40_nopad.bad (stgr_RST's missing pad row) and the flag bit 3 clips
// rig19_b3.bad and vm40_b3lp.bad. Every event's bottom and top ride the text as
// stored.
//
//   o3a_roundtrip_test <opennova-3di> <scratch dir>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "common/run_command.h"
#include "common/test_paths.h"

namespace {

using test_cmd::quoted;
using test_cmd::run;

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
		std::fprintf(stderr, "usage: o3a_roundtrip_test <opennova-3di> <scratch dir>\n");
		return 2;
	}
	const std::string cli = quoted(argv[1]);
	const std::string dir = argv[2];
	int failures = 0;
	int ran = 0;
	for (const char *name : {"rig19.adm", "vm40.adm", "rig19_short.bad", "rig19_sprs.bad", "rig19_v0.bad",
				 "vm40_long.bad", "vm40_brk.bad", "rig19_junk.bad", "vm40_nopad.bad", "rig19_b3.bad", "vm40_b3lp.bad"}) {
		const std::string source = test_paths_fixture(std::string("anim/") + name);
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
	if (failures == 0) std::printf("o3a_roundtrip_test: ok (%d clip sets)\n", ran);
	return failures == 0 ? 0 : 1;
}
