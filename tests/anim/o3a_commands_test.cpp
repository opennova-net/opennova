// Exercise the same anim command handlers the CLI dispatches: the clip set
// round trip (build -> scene -> build is byte-identical), what `compare` calls
// the same animation and what it does not, and the scenes `build` refuses.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "../../apps/3di/anim_cli.h"
#include "../common/file_io.h"

namespace threedi_cli = opennova::threedi_cli;

namespace {
using test_io::read_file;
using test_io::read_file_text;

int failures = 0;
void check(bool ok, const char *what) {
	if (!ok) {
		std::fprintf(stderr, "FAIL: %s\n", what);
		++failures;
	}
}

// A three-bone rig: the root, a spine one metre up and a hand out to its left,
// four keys over three frames, translations and one event per key, each with
// the hips' and the head's height above the ground.
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
const std::string events = "event 0 0 0 0x0 0.9 1.7\nevent 0.06 0 0 0x1 0.92 1.71\n"
						   "event 0.06 0 0 0x0 0.95 1.73\nevent 0.06 0 0 0x2 0.93 1.72\n";

std::string replace(std::string s, const std::string &from, const std::string &to) {
	const size_t at = s.find(from);
	if (at == std::string::npos) return s;
	s.replace(at, from.size(), to);
	return s;
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
		std::vector<uint8_t> a;
		std::vector<uint8_t> b;
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
	// The hips' and the head's height above the ground.
	compare("bottom", replace(text, "0x1 0.92 1.71", "0x1 0.8 1.71"), false);
	compare("top", replace(text, "0x1 0.92 1.71", "0x1 0.92 1.5"), false);
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
	// A table binds every clip to its reset row's clip, and the game cannot load
	// one without it.
	build("no-reset-row", replace(text, "row anim_reset \"walk\"\n", ""), false);
	build("short-translations", replace(text, " tr 0 0 0.02\n", ""), false);
	build("forward-parent", replace(text, "bone 0 0 0 1 0.4", "bone 2 0 0 1 0.4"), false);
	build("root-with-parent", replace(text, "bone -1 0 0 0 0.5", "bone 0 0 0 0 0.5"), false);
	build("bad-unit-key", replace(text, " k 0 0 0 1\n k 0 0 0.0871557427", " k 0 0 0 0\n k 0 0 0.0871557427"),
			false);
	build("short-event-list", replace(text, "event 0.06 0 0 0x2 0.93 1.72\n", ""), false);
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
	// Every event states the hips' and the head's height above the ground: the
	// clip does not hold the ground, so nothing derives them, and no record
	// states one pair for a whole clip.
	build("event-without-heights", replace(text, "0x1 0.92 1.71", "0x1"), false);
	build("event-without-top", replace(text, "0x1 0.92 1.71", "0x1 0.92"), false);
	build("event-trailing-token", replace(text, "0x1 0.92 1.71", "0x1 0.92 1.71 2"), false);
	build("capsule-record", replace(text, "frames 3\n", "frames 3\ncapsule 0 0.6\n"), false);
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
		std::vector<uint8_t> a;
		std::vector<uint8_t> b;
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
		std::vector<uint8_t> a;
		std::vector<uint8_t> b;
		check(read_file((std::filesystem::path(table).parent_path() / "walk.bad").string(), a) &&
						read_file((home / "walk.bad").string(), b) && a == b,
				"sparse clip byte-identical");
		check(threedi_cli::cmd_anim_compare(table.c_str(), rebuilt.c_str()) == 0,
				"sparse compares the same");
		check(threedi_cli::cmd_anim_compare(original.c_str(), table.c_str()) == 1,
				"sparse differs from the dense clip");
	}

	// What compare must not call the same: a value that is not a number, a
	// variant whose clip is absent, a bind that turns differently; and what it
	// must: two lone clips that are one animation under two names.
	{
		const std::filesystem::path home = dir / "compare-cases";
		std::filesystem::remove_all(home);
		std::filesystem::create_directories(home / "a");
		std::filesystem::create_directories(home / "b");
		std::vector<uint8_t> walk;
		check(read_file((std::filesystem::path(original).parent_path() / "walk.bad").string(), walk),
				"read the built clip");
		const auto word = [&](const std::vector<uint8_t> &bytes, size_t at) {
			uint32_t v = 0;
			std::memcpy(&v, bytes.data() + at, sizeof(v));
			return v;
		};
		const auto put = [&](std::vector<uint8_t> bytes, size_t at, float v) {
			std::memcpy(bytes.data() + at, &v, sizeof(v));
			return bytes;
		};
		const auto write = [&](const std::filesystem::path &path, const std::vector<uint8_t> &bytes) {
			test_io::write_file(path.string(), bytes);
			return path.string();
		};
		// The first event's forward step, NaN in both clips.
		const size_t event_at = word(walk, 0x40);
		const std::vector<uint8_t> nan_walk = put(walk, event_at, std::nanf(""));
		check(threedi_cli::cmd_anim_compare(write(home / "a" / "walk.bad", nan_walk).c_str(),
					  write(home / "b" / "walk.bad", nan_walk).c_str()) == 1,
				"a NaN is never the same");
		// Bone 0's bind rotation turned a quarter turn about y.
		const size_t bind_at = word(walk, 0x18) + 64;
		std::vector<uint8_t> turned = put(put(walk, bind_at, 0.0f), bind_at + 8, 1.0f);
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

	// A bone count that differs is one difference and the rest still compares:
	// a clip with a dead fourth bone and its root turned twice as far at key 1
	// reports both.
	{
		const std::string extra = head +
				replace(root, " k 0 0 0.0871557427 0.996194698\n", " k 0 0 0.173648178 0.984807753\n") +
				spine + hand +
				"bone 2 0 0.5 1 0.1 \"BN04 Dead\"\n k 0 0 0 1\n k 0 0 0 1\n k 0 0 0 1\n k 0 0 0 1\n"
				" tr 0 0 0\n tr 0 0 0\n tr 0 0 0\n tr 0 0 0\n" +
				events;
		const std::string table = build("extra-bone", extra);
		threedi_cli::AnimLoadedSet a;
		threedi_cli::AnimLoadedSet b;
		std::string error;
		const bool loaded =
				threedi_cli::anim_load((std::filesystem::path(original).parent_path() / "walk.bad").string(), a,
						error) &&
				threedi_cli::anim_load((std::filesystem::path(table).parent_path() / "walk.bad").string(), b,
						error);
		check(loaded && a.clips.size() == 1 && b.clips.size() == 1, "load the two clips");
		if (loaded && a.clips.size() == 1 && b.clips.size() == 1) {
			const std::vector<std::string> found = threedi_cli::anim_compare_clips(a.clips[0], b.clips[0]);
			const auto has = [&](const char *what) {
				return std::any_of(found.begin(), found.end(),
						[&](const std::string &d) { return d.find(what) != std::string::npos; });
			};
			check(has("3 bones vs 4"), "compare names the bone count");
			check(has("bone 0 key 1"), "compare goes on over the bones both clips hold");
		}
		threedi_cli::anim_free(a);
		threedi_cli::anim_free(b);
	}

	// A row whose key names no anim slot: build refuses it and scene notes and
	// drops it, since the game registers nothing under it. And an output the
	// game could not pack (over 15 bytes with its extension) is refused.
	{
		build("no-slot-row", replace(text, "row anim_walk_forward", "row anim_notaslot \"walk\"\nrow anim_walk_forward"),
				false);
		const std::filesystem::path home = dir / "no-slot-scene";
		std::filesystem::remove_all(home);
		std::filesystem::create_directories(home);
		std::filesystem::copy_file(std::filesystem::path(original).parent_path() / "walk.bad", home / "walk.bad");
		std::ofstream(home / "SLOTS.adm", std::ios::binary)
				<< "\r\nanim_reset\t\t\t\t\"walk\"\r\nanim_notaslot\t\t\t\t\"walk\"\r\n";
		const std::string scene = (home / "set.o3a").string();
		check(threedi_cli::cmd_anim_scene((home / "SLOTS.adm").string().c_str(), scene.c_str()) == 0,
				"scene of a table with a row naming no slot");
		std::string written;
		check(read_file_text(scene, written) &&
						written.find("# dropped: row 'anim_notaslot' (its key names no anim slot") !=
								std::string::npos &&
						written.find("row anim_notaslot") == std::string::npos,
				"a row naming no slot is noted and dropped");
		std::ofstream(home / "long.o3a") << text;
		check(threedi_cli::cmd_anim_build((home / "long.o3a").string().c_str(),
					  (home / "CHECK_TOO_LONG.adm").string().c_str()) != 0,
				"an output name over 15 bytes is refused");
	}

	// A table with no reset row is noted: build refuses it.
	{
		const std::filesystem::path home = dir / "no-reset-scene";
		std::filesystem::remove_all(home);
		std::filesystem::create_directories(home);
		std::filesystem::copy_file(std::filesystem::path(original).parent_path() / "walk.bad", home / "walk.bad");
		std::ofstream(home / "IDLE.adm", std::ios::binary) << "\r\nanim_idle\t\t\t\t\"walk\"\r\n";
		const std::string scene = (home / "set.o3a").string();
		check(threedi_cli::cmd_anim_scene((home / "IDLE.adm").string().c_str(), scene.c_str()) == 0,
				"scene of a table with no reset row");
		std::string text_out;
		check(read_file_text(scene, text_out) &&
						text_out.find("# dropped: the table's binding (it has no reset row") != std::string::npos,
				"a table with no reset row is noted");
	}

	// A clip the text cannot express is noted and left out, not written for
	// build to refuse: a clip one event short of its frame count.
	{
		const std::filesystem::path home = dir / "inexpressible";
		std::filesystem::remove_all(home);
		std::filesystem::create_directories(home);
		std::vector<uint8_t> walk;
		check(read_file((std::filesystem::path(original).parent_path() / "walk.bad").string(), walk),
				"read the built clip");
		const uint32_t short_events = 3;
		std::memcpy(walk.data() + 0x3C, &short_events, sizeof(short_events));
		test_io::write_file((home / "walk.bad").string(), walk);
		const std::string scene = (home / "set.o3a").string();
		check(threedi_cli::cmd_anim_scene((home / "walk.bad").string().c_str(), scene.c_str()) == 0,
				"scene of a clip it cannot express");
		std::string written;
		check(read_file_text(scene, written), "read the scene");
		check(written.find("# dropped: clip 'walk' (") != std::string::npos &&
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

	// `scene` writes its text whole or not at all, LF on every platform: it
	// replaces an existing file, fails onto a folder, and leaves no part file.
	{
		const std::filesystem::path home = dir / "write-safety";
		std::filesystem::remove_all(home);
		std::filesystem::create_directories(home / "a-folder.o3a");
		const std::string scene = (home / "set.o3a").string();
		std::ofstream(scene, std::ios::binary) << "stale";
		std::string written;
		check(threedi_cli::cmd_anim_scene(original.c_str(), scene.c_str()) == 0 && read_file_text(scene, written) &&
						written.rfind("o3a 1\n", 0) == 0 &&
						std::find(written.begin(), written.end(), '\r') == written.end(),
				"scene replaces a file with LF text");
		check(threedi_cli::cmd_anim_scene(original.c_str(), (home / "a-folder.o3a").string().c_str()) != 0,
				"scene onto a folder fails");
		check(!std::filesystem::exists(scene + ".part") &&
						!std::filesystem::exists(home / "a-folder.o3a.part"),
				"scene leaves no part file");
	}

	// `info` reads a table and a lone clip.
	check(threedi_cli::cmd_anim_info(original.c_str(), 2) == 0, "info");

	if (failures == 0) std::printf("o3a commands: every case holds\n");
	return failures == 0 ? 0 : 1;
}
