// Exercise the same anim command handlers the CLI dispatches: the clip set
// round trip (build -> scene -> build is byte-identical), what `compare` calls
// the same animation and what it does not, and the scenes `build` refuses.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
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
	// A row key names its slot past its first five characters, whatever they
	// are: the table keeps the keys as written and they are the same slots.
	compare("slot-prefix", replace(replace(text, "row anim_reset", "row ANIM_RESET"), "row anim_walk_forward",
								   "row xxxx_walk_forward"),
			true);

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
	// A key list one short of `frames + 1` with no durations is malformed, not
	// sparse: only a bone that states its durations may key sparsely.
	build("short-key-list", replace(text, " k 0 0 0.258819045 0.965925826\n", ""), false);
	build("mixed-durations", replace(text, " k 0 0 0.0871557427 0.996194698\n",
									 " k 0 0 0.0871557427 0.996194698 1\n"),
			false);
	// Every field is whole and in range, and nothing trails a record.
	build("partial-duration", replace(text, " k 0 0 0.0871557427 0.996194698\n",
									  " k 0 0 0.0871557427 0.996194698 5x\n"),
			false);
	build("nan-pivot", replace(text, "bone 0 0 0 1 0.4", "bone 0 nan 0 1 0.4"), false);
	build("inf-translation", replace(text, " tr 0 0 0.02", " tr 0 0 inf"), false);
	build("inf-velocity", replace(text, "event 0.06 0 0 0x1", "event -inf 0 0 0x1"), false);
	build("trailing-token", replace(text, "fps 30", "fps 30 60"), false);
	build("trailing-key-token", replace(text, " k 0 0 0.0871557427 0.996194698\n",
										" k 0 0 0.0871557427 0.996194698 1 2\n"),
			false);
	build("wrapping-frames", replace(text, "frames 3", "frames 4294967299"), false);
	build("wrapping-parent", replace(text, "bone 0 0 0 1 0.4", "bone 4294967296 0 0 1 0.4"), false);
	build("wrapping-trigger", replace(text, "event 0.06 0 0 0x1", "event 0.06 0 0 0x100000001"),
			false);
	build("lone-capsule-value", replace(text, "event 0.06 0 0 0x1", "event 0.06 0 0 0x1 0.5"), false);
	build("unterminated-quote", replace(text, "\"BN02 Spine\"", "\"BN02 Spine"), false);
	// What the writer would drop: translations the flags do not carry, and a
	// trigger on a version 0 event; a version the loader does not know.
	build("tr-without-flag", replace(text, "flags 0x3", "flags 0x1"), false);
	build("version-0-trigger", replace(text, "frames 3\n", "frames 3\nversion 0\n"), false);
	build("version-2", replace(text, "frames 3\n", "frames 3\nversion 2\n"), false);
	build("empty-variant", replace(text, "\"walk.bad\" \"walk\"", "\"walk.bad\" \"\" \"walk\""), false);

	// A clip name or a row variant is a bare file stem: `build` writes each
	// clip beside the table, so a path would write outside it.
	{
		const auto named = [&](const std::string &name) {
			return replace(replace(replace(text, "clip walk", "clip \"" + name + "\""),
								   "row anim_reset \"walk\"", "row anim_reset \"" + name + "\""),
					"row anim_walk_forward \"walk.bad\" \"walk\"",
					"row anim_walk_forward \"" + name + "\"");
		};
		build("clip-escape", named("../../escape"), false);
		check(!std::filesystem::exists(dir.parent_path() / "escape.bad"), "no clip escapes the table");
		const std::string absolute = std::filesystem::absolute(dir / "absolute").generic_string();
		build("clip-absolute", named(absolute), false);
		check(!std::filesystem::exists(absolute + ".bad"), "no clip lands on an absolute path");
		build("clip-pipe", named("Armature|Walk"), false);
		build("variant-escape", replace(text, "row anim_reset \"walk\"", "row anim_reset \"../walk\""),
				false);

		// A set that fails anywhere writes nothing: the first clip mints, the
		// second does not, and neither lands nor does the table.
		const std::string broken = text + "clip broken\nframes 1\nbone -1 0 0 0 0 Root\n k 0 0 0 0\n"
										  " k 0 0 0 1\n";
		const std::string table = build("partial", broken, false);
		check(!std::filesystem::exists(std::filesystem::path(table).parent_path() / "walk.bad") &&
						!std::filesystem::exists(table),
				"a failed set writes no file");

		// A table read back refuses a variant that names a path.
		const std::filesystem::path home = dir / "read-escape" / "table";
		std::filesystem::remove_all(dir / "read-escape");
		std::filesystem::create_directories(home);
		std::filesystem::copy_file(std::filesystem::path(original).parent_path() / "walk.bad",
				dir / "read-escape" / "walk.bad");
		std::ofstream(home / "ESCAPE.adm", std::ios::binary) << "\r\nanim_reset\t\"../walk\"\r\n";
		check(threedi_cli::cmd_anim_info((home / "ESCAPE.adm").string().c_str(), 0) != 0,
				"a table variant that names a path is refused");
	}

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

	// What compare must not call the same: a value that is not a number, a
	// variant whose clip is absent, a bind that turns differently; and what it
	// must: two lone clips that are one animation under two names.
	{
		const std::filesystem::path home = dir / "compare-cases";
		std::filesystem::remove_all(home);
		std::filesystem::create_directories(home / "a");
		std::filesystem::create_directories(home / "b");
		std::vector<char> walk;
		check(read_file((std::filesystem::path(original).parent_path() / "walk.bad").string(), walk),
				"read the built clip");
		const auto word = [&](const std::vector<char> &bytes, size_t at) {
			uint32_t v = 0;
			std::memcpy(&v, bytes.data() + at, sizeof(v));
			return v;
		};
		const auto put = [&](std::vector<char> bytes, size_t at, float v) {
			std::memcpy(bytes.data() + at, &v, sizeof(v));
			return bytes;
		};
		const auto write = [&](const std::filesystem::path &path, const std::vector<char> &bytes) {
			std::ofstream(path, std::ios::binary).write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
			return path.string();
		};
		// The first event's forward step, NaN in both clips.
		const size_t event_at = word(walk, 0x40);
		const std::vector<char> nan_walk = put(walk, event_at, std::nanf(""));
		check(threedi_cli::cmd_anim_compare(write(home / "a" / "walk.bad", nan_walk).c_str(),
					  write(home / "b" / "walk.bad", nan_walk).c_str()) == 1,
				"a NaN is never the same");
		// Bone 0's bind rotation turned a quarter turn about y.
		const size_t bind_at = word(walk, 0x18) + 64;
		std::vector<char> turned = put(put(walk, bind_at, 0.0f), bind_at + 8, 1.0f);
		turned = put(put(turned, bind_at + 24, -1.0f), bind_at + 32, 0.0f);
		check(threedi_cli::cmd_anim_compare(write(home / "a" / "walk.bad", walk).c_str(),
					  write(home / "b" / "walk.bad", turned).c_str()) == 1,
				"a bind that turns differently differs");
		// Two lone clips under two names.
		check(threedi_cli::cmd_anim_compare(write(home / "a" / "one.bad", walk).c_str(),
					  write(home / "b" / "two.bad", walk).c_str()) == 0,
				"two lone clips compare with each other");
		// Two tables that name a clip neither directory holds.
		const std::string absent = "\r\nanim_reset\t\t\t\t\"walk\"\r\nanim_idle\t\t\t\t\"absent\"\r\n";
		std::ofstream(home / "a" / "T.adm", std::ios::binary) << absent;
		std::ofstream(home / "b" / "T.adm", std::ios::binary) << absent;
		check(threedi_cli::cmd_anim_compare((home / "a" / "T.adm").string().c_str(),
					  (home / "b" / "T.adm").string().c_str()) == 1,
				"a variant with no clip differs");
	}

	// A clip the text cannot express is noted and left out, not written for
	// build to refuse: a clip one event short of its frame count.
	{
		const std::filesystem::path home = dir / "inexpressible";
		std::filesystem::remove_all(home);
		std::filesystem::create_directories(home);
		std::vector<char> walk;
		check(read_file((std::filesystem::path(original).parent_path() / "walk.bad").string(), walk),
				"read the built clip");
		const uint32_t short_events = 3;
		std::memcpy(walk.data() + 0x3C, &short_events, sizeof(short_events));
		std::ofstream((home / "walk.bad").string(), std::ios::binary)
				.write(walk.data(), static_cast<std::streamsize>(walk.size()));
		const std::string scene = (home / "set.o3a").string();
		check(threedi_cli::cmd_anim_scene((home / "walk.bad").string().c_str(), scene.c_str()) == 0,
				"scene of a clip it cannot express");
		std::vector<char> text_out;
		check(read_file(scene, text_out), "read the scene");
		const std::string written(text_out.begin(), text_out.end());
		check(written.find("# note: left out clip 'walk'") != std::string::npos &&
						written.find("\nclip ") == std::string::npos,
				"the clip is noted and left out");
	}

	// `info` prints a ring from its last variant back, and a reset row, which
	// is no ring, in file order with its bind named.
	check(threedi_cli::anim_info_row(opennova::bad::BadBuildRow{"anim_walk", {"a", "b"}})
							.find(" b a   (ring order)") != std::string::npos,
			"info prints a ring last to first");
	check(threedi_cli::anim_info_row(opennova::bad::BadBuildRow{"anim_reset", {"a", "b"}})
							.find(" a b   (no ring: the last is the rig's bind)") != std::string::npos,
			"info prints a reset row as no ring");

	// `info` reads a table and a lone clip.
	check(threedi_cli::cmd_anim_info(original.c_str(), 2) == 0, "info");

	if (failures == 0) std::printf("o3a commands: every case holds\n");
	return failures == 0 ? 0 : 1;
}
