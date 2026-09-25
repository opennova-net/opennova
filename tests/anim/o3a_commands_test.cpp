// Exercise the same anim command handlers the CLI dispatches: the clip set
// round trip (build -> scene -> build is byte-identical), what `compare` calls
// the same animation and what it does not, and the scenes `build` refuses.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "../../apps/threedi_cli/anim_cli.h"

namespace {
int failures = 0;
void check(bool ok, const char *what) {
	if (!ok) {
		std::fprintf(stderr, "FAIL: %s\n", what);
		++failures;
	}
}

// A three-bone rig: the root, a spine one metre up and a hand out to its left,
// four keys over three frames, translations and one event per key.
const std::string head = "o3a 1\nadm CHECK.adm\nrow anim_reset \"walk\"\n"
						 "row anim_walk_forward \"walk.bad\" \"walk\"\n"
						 "clip walk\nfps 30\nflags 0x3\nframes 3\n";
const std::string root = "bone -1 0 0 0 0.5 \"BN01 Pelvis\"\n"
						 " k 0 0 0 1\n k 0 0 0.0871557427 0.996194698\n"
						 " k 0 0 0.173648178 0.984807753\n k 0 0 0.258819045 0.965925826\n"
						 " tr 0 0 0\n tr 0 0 0\n tr 0 0 0\n tr 0 0 0\n";
const std::string spine = "bone 0 0 0 1 0.4 \"BN02 Spine\"\n"
						  " k 0 0 0 1\n k 0 0 0 1\n k 0 0 0 1\n k 0 0 0 1\n"
						  " tr 0 0 0\n tr 0 0 0.01 \n tr 0 0 0.02\n tr 0 0 0.03\n";
const std::string hand = "bone 1 0 0.25 1 0.1 \"BN03 L Hand\"\n"
						 " k 0.258819045 0 0 0.965925826\n k 0.258819045 0 0 0.965925826\n"
						 " k 0.258819045 0 0 0.965925826\n k 0.258819045 0 0 0.965925826\n"
						 " tr 0 0 0\n tr 0 0 0\n tr 0 0 0\n tr 0 0 0\n";
const std::string events = "event 0 0 0 0x0\nevent 0.06 0 0 0x1\nevent 0.06 0 0 0x0\n"
						   "event 0.06 0 0 0x2\n";

std::string replace(std::string s, const std::string &from, const std::string &to) {
	const size_t at = s.find(from);
	if (at == std::string::npos) return s;
	s.replace(at, from.size(), to);
	return s;
}

bool read_file(const std::string &path, std::vector<char> &out) {
	std::ifstream in(path, std::ios::binary);
	if (!in) return false;
	out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
	return true;
}
} // namespace

int main(int argc, char **argv) {
	if (argc != 2) return 2;
	const std::filesystem::path dir = std::filesystem::path(argv[1]) / "o3a-commands";
	const std::string text = head + root + spine + hand + events;

	// Each set gets its own directory: `build` writes every clip beside the
	// table it names.
	const auto build = [&](const char *name, const std::string &body, bool valid = true) {
		const std::filesystem::path home = dir / name;
		std::filesystem::remove_all(home);
		std::filesystem::create_directories(home);
		const std::string scene = (home / "set.o3a").string();
		const std::string table = (home / "CHECK.adm").string();
		std::ofstream(scene) << body;
		const int result = threedi_cli::cmd_anim_build(scene.c_str(), table.c_str());
		check(valid ? result == 0 : result != 0, name);
		return table;
	};
	const std::string original = build("original", text);
	const auto compare = [&](const char *name, const std::string &body, bool same) {
		const std::string table = build(name, body);
		check(threedi_cli::cmd_anim_compare(original.c_str(), table.c_str()) == (same ? 0 : 1),
				name);
	};

	// build -> scene -> build is byte-identical, table and clip alike.
	{
		const std::filesystem::path home = dir / "roundtrip";
		std::filesystem::create_directories(home);
		const std::string scene = (home / "set.o3a").string();
		const std::string table = (home / "CHECK.adm").string();
		check(threedi_cli::cmd_anim_scene(original.c_str(), scene.c_str()) == 0, "scene");
		check(threedi_cli::cmd_anim_build(scene.c_str(), table.c_str()) == 0, "rebuild");
		std::vector<char> a;
		std::vector<char> b;
		check(read_file(original, a) && read_file(table, b) && a == b, "table byte-identical");
		check(read_file((std::filesystem::path(original).parent_path() / "walk.bad").string(), a) &&
						read_file((home / "walk.bad").string(), b) && a == b,
				"clip byte-identical");
		check(threedi_cli::cmd_anim_compare(original.c_str(), table.c_str()) == 0,
				"roundtrip compares the same");
	}

	// The same animation, differently expressed.
	compare("identical", text, true);
	// A negated quaternion is the same rotation, and retail stores both.
	compare("negated-key", replace(text, " k 0 0 0.173648178 0.984807753",
								   " k 0 0 -0.173648178 -0.984807753"),
			true);
	// The table may name a clip with or without the extension it carries.
	compare("row-extension", replace(text, "row anim_reset \"walk\"", "row anim_reset \"walk.bad\""),
			true);
	// Dead fields: the bone length, and a position the pivot does not derive.
	compare("length", replace(text, "0.4 \"BN02 Spine\"", "0.9 \"BN02 Spine\""), true);
	compare("explicit-position", replace(text, " k 0 0 0 1\n k 0 0 0 1\n k 0 0 0 1\n k 0 0 0 1\n",
								  "bonepos 3 4 5\n k 0 0 0 1\n k 0 0 0 1\n k 0 0 0 1\n k 0 0 0 1\n"),
			true);

	// Not the same animation.
	compare("rotated-key", replace(text, " k 0 0 0.173648178 0.984807753",
								   " k 0 0 0.258819045 0.965925826"),
			false);
	// A bone that keys fewer times than the frame count is a sparse channel, not
	// a malformed one: its keys hold past their durations, which is a different
	// animation from the dense clip.
	compare("short-key-list", replace(text, " k 0 0 0.258819045 0.965925826\n", ""), false);
	compare("fps", replace(text, "fps 30", "fps 15"), false);
	compare("loop-flag", replace(text, "flags 0x3", "flags 0x2"), false);
	compare("bone-name", replace(text, "BN03 L Hand", "BN03 R Hand"), false);
	compare("bone-parent", replace(text, "bone 1 0 0.25 1 0.1", "bone 0 0 0.25 1 0.1"), false);
	compare("translation", replace(text, " tr 0 0 0.02", " tr 0 0 0.5"), false);
	// Row frame_count is a translation the runtime reads, not a hold of the one
	// before it.
	compare("last-translation", replace(text, " tr 0 0 0.03", " tr 0 0 0.5"), false);
	compare("trigger", replace(text, "event 0.06 0 0 0x1", "event 0.06 0 0 0x4"), false);
	compare("velocity", replace(text, "event 0.06 0 0 0x1", "event 0.6 0 0 0x1"), false);
	compare("row-order", replace(replace(text, "row anim_reset \"walk\"\n", ""),
								 "row anim_walk_forward \"walk.bad\" \"walk\"\n",
								 "row anim_walk_forward \"walk.bad\" \"walk\"\nrow anim_reset "
								 "\"walk\"\n"),
			false);
	compare("ring-order", replace(text, "row anim_walk_forward \"walk.bad\" \"walk\"",
								  "row anim_walk_forward \"walk\" \"walk.bad\""),
			true); // both variants name one clip, so the ring is the same
	// A capsule the author states, against the one the rig derives.
	compare("capsule", replace(text, "frames 3\n", "frames 3\ncapsule 0 0.6\n"), false);

	// What `build` refuses, by name.
	build("no-header", replace(text, "o3a 1", "o3d 1"), false);
	build("unknown-record", text + "bones 0 1\n", false);
	build("row-without-clip", replace(text, "row anim_reset \"walk\"", "row anim_reset \"stand\""),
			false);
	build("key-outside-namespace", replace(text, "row anim_reset", "row reset"), false);
	build("short-translations", replace(text, " tr 0 0 0.02\n", ""), false);
	build("forward-parent", replace(text, "bone 0 0 0 1 0.4", "bone 2 0 0 1 0.4"), false);
	build("root-with-parent", replace(text, "bone -1 0 0 0 0.5", "bone 0 0 0 0 0.5"), false);
	build("bad-unit-key", replace(text, " k 0 0 0 1\n k 0 0 0.0871557427", " k 0 0 0 0\n k 0 0 0.0871557427"),
			false);
	build("short-event-list", replace(text, "event 0.06 0 0 0x2\n", ""), false);
	build("no-frames", replace(text, "frames 3", "frames 0"), false);
	build("long-bone-name", replace(text, "BN02 Spine", "BN02 SpineWithAVeryLongNameIndeedYes"),
			false);
	build("two-clips-one-name", text + "clip walk\nfps 30\nframes 1\n" + root, false);
	build("record-before-clip", replace(text, "clip walk\nfps 30", "fps 30\nclip walk"), false);
	build("key-before-bone", replace(text, "bone -1 0 0 0 0.5 \"BN01 Pelvis\"\n", ""), false);

	// A lone clip writes one `.bad` and takes no table row.
	{
		const std::filesystem::path home = dir / "lone";
		std::filesystem::create_directories(home);
		const std::string scene = (home / "set.o3a").string();
		const std::string clip = (home / "walk.bad").string();
		std::ofstream(scene) << replace(replace(text, "adm CHECK.adm\n", ""),
				"row anim_reset \"walk\"\nrow anim_walk_forward \"walk.bad\" \"walk\"\n", "");
		check(threedi_cli::cmd_anim_build(scene.c_str(), clip.c_str()) == 0, "lone clip");
		const std::string again = (home / "again.o3a").string();
		const std::string rebuilt = (home / "again.bad").string();
		check(threedi_cli::cmd_anim_scene(clip.c_str(), again.c_str()) == 0, "lone scene");
		check(threedi_cli::cmd_anim_build(again.c_str(), rebuilt.c_str()) == 0, "lone rebuild");
		std::vector<char> a;
		std::vector<char> b;
		check(read_file(clip, a) && read_file(rebuilt, b) && a == b, "lone byte-identical");
		// A table row with no table, and a table with no row, are both refused.
		const std::string rows = (home / "rows.o3a").string();
		std::ofstream(rows) << text;
		check(threedi_cli::cmd_anim_build(rows.c_str(), (home / "rows.bad").string().c_str()) != 0,
				"rows into a .bad");
		std::ofstream(rows + ".2") << replace(replace(text, "adm CHECK.adm\n", ""),
				"row anim_reset \"walk\"\nrow anim_walk_forward \"walk.bad\" \"walk\"\n", "");
		check(threedi_cli::cmd_anim_build((rows + ".2").c_str(),
					   (home / "norows.adm").string().c_str()) != 0,
				"no row into a .adm");
	}

	// A bone may key sparsely when it says how long each key lasts (retail's
	// DVFLEE1E.BAD is the one clip that does).
	{
		const std::string sparse = head + root +
				"bone 0 0 0 1 0.4 \"BN02 Spine\"\n k 0 0 0 1 2\n k 0 0 0.0871557427 0.996194698 2\n"
				" tr 0 0 0\n tr 0 0 0\n tr 0 0 0\n tr 0 0 0\n" +
				hand + events;
		const std::string table = build("sparse", sparse);
		const std::filesystem::path home = dir / "sparse-roundtrip";
		std::filesystem::create_directories(home);
		const std::string scene = (home / "set.o3a").string();
		const std::string rebuilt = (home / "CHECK.adm").string();
		check(threedi_cli::cmd_anim_scene(table.c_str(), scene.c_str()) == 0, "sparse scene");
		check(threedi_cli::cmd_anim_build(scene.c_str(), rebuilt.c_str()) == 0, "sparse rebuild");
		std::vector<char> a;
		std::vector<char> b;
		check(read_file((std::filesystem::path(table).parent_path() / "walk.bad").string(), a) &&
						read_file((home / "walk.bad").string(), b) && a == b,
				"sparse clip byte-identical");
		check(threedi_cli::cmd_anim_compare(table.c_str(), rebuilt.c_str()) == 0,
				"sparse compares the same");
		check(threedi_cli::cmd_anim_compare(original.c_str(), table.c_str()) == 1,
				"sparse differs from the dense clip");
	}

	// A table's clips measure their capsule against its reset clip, the bind
	// the runtime composes them against: a clip that holds its root pitched a
	// quarter turn lays the tip (a metre up in the reset) flat, so it derives
	// no height, where measured against its own first key it would stand a
	// metre tall.
	{
		const std::string rig = "bone -1 0 0 0 0.5 Root\n K0\n K0\nbone 0 0 0 1 0.5 Tip\n k 0 0 0 1\n"
								" k 0 0 0 1\n";
		const std::string pitched = "0 0.707106781 0 0.707106781";
		const std::string set = "o3a 1\nrow anim_reset rest\nrow anim_walk_forward pitch\n"
								"clip rest\nframes 1\n" +
				replace(replace(rig, "K0", "k 0 0 0 1"), "K0", "k 0 0 0 1") +
				"event 0 0 0 0\nevent 0 0 0 0\nclip pitch\nframes 1\n" +
				replace(replace(rig, "K0", "k " + pitched), "K0", "k " + pitched) +
				"event 0 0 0 0\nevent 0 0 0 0\n";
		const std::string table = build("reset-bind", set);
		opennova::bad::BadFile clip{};
		const std::string path = (std::filesystem::path(table).parent_path() / "pitch.bad").string();
		check(opennova::bad::bad_parse(path.c_str(), &clip) == 0 && clip.num_events == 2 &&
						clip.events[0].top < 1e-4f,
				"the capsule is measured against the reset clip");
		opennova::bad::bad_free(&clip);
	}

	// `info` reads a table and a lone clip.
	check(threedi_cli::cmd_anim_info(original.c_str(), 2) == 0, "info");

	if (failures == 0) std::printf("o3a commands: every case holds\n");
	return failures == 0 ? 0 : 1;
}
