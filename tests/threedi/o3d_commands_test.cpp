// Exercise the same command handlers the CLI dispatches, including negative
// cases: successful readback alone does not establish semantic equivalence.
// Each negative compare case changes exactly one stored field (by scene text,
// or by a hand edit of the minted bytes for a value the builder derives)
// while the CMDL and section bounds stay put, so only the check under test
// can catch it.
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <string>
#include <vector>

#include <base/io/le.h>
#include <formats/threedi/threedi.h>
#include <formats/threedi/threedi_3di3.h>

#include "../../apps/threedi_cli/threedi_cli.h"

using namespace opennova::threedi;

namespace {
int failures = 0;
void check(bool ok, const char *what) {
	if (!ok) {
		std::fprintf(stderr, "FAIL: %s\n", what);
		++failures;
	}
}

const std::string prefix = "o3d 1\nmodel CHECK\nskinned 1\nuv1 1\nmaterial VS_SKBASIC\n"
		"lod 0 gnrc\npart 0 0 0 0\npart 0 0 0 1\npart 0 0 0 0\nstrip 0\nbones 0 1\n";
const std::string verts =
		"v 0 0 0 0 0 1 0 0 0 0 0 1 0 0.75 0.25 0\n"
		"v 1 0 0 0 0 1 1 0 1 0 0 1 0 0.5 0.5 0\n"
		"v 0 1 0 0 0 1 0 1 0 1 0 1 0 0.25 0.75 0\n";
const std::string suffix = "t 0 1 2\npanm 0 0\npanm 1 0\npanm 2 0\n";

std::string replace(std::string s, const std::string &from, const std::string &to) {
	const size_t at = s.find(from);
	if (at == std::string::npos) {
		std::fprintf(stderr, "FAIL: test text lacks \"%s\"\n", from.c_str());
		++failures;
		return s;
	}
	s.replace(at, from.size(), to);
	return s;
}

// A rigid model with something of every kind compare reads: two parts (one
// turning in an MTRX frame on a register), two materials (one generator
// driven), a user point, a light, an occlusion record with faces on two
// parallel planes, and two collision sections (the second gets a CXLT row)
// with bullet faces, a box volume, a ladder and a convex volume. Section 0's
// bounds and the CMDL are set by its corner vertices (0 0 0)..(20 20 0) and
// the volumes, so the small face 3 4 5 can move without changing them.
const std::string rich =
		"o3d 1\nmodel RICH\n"
		"register FLICKER\nregister TEX_CAMO1\nregister TEX_CAMO2\n"
		"mtrx 0 1 0 -1 0 0 0 0 1\n"
		"material FF_ST_OP\ntexture rich.tga\n"
		"material FF_ST_OP_LUM\ntexture bulb.tga\nrgbgen 113 0 2 255 255 255 0 0 0\n"
		"lod 200 gnrc\n"
		"part 0 0 0 0\nstrip 0\nv 0 0 0 0 0 1 0 0\nv 2 0 0 0 0 1 1 0\nv 0 2 0 0 0 1 0 1\nt 0 1 2\n"
		"part 0 1 0 1\nstrip 1\nv 1 0 1 0 0 1 0 0\nv 2 0 1 0 0 1 1 0\nv 1 1 1 0 0 1 0 1\nt 0 1 2\n"
		"panm 0 0\npanm 1 0 0x00000200 1\ntrack rotz 113 FLICKER 0 0 4096\n"
		"userpoint muzzle 1 0 1 1 0 0 1 71\n"
		"light 1 1 0 1.5 0 4 55 0.296875 0.5 255 0 34 240 235 230 0x40\n"
		"occ 0 0 0\nov 0 0 0\nov 1 0 0\nov 0 1 0\nov 0 0 2\nov 1 0 2\nov 0 1 2\n"
		"op 0 0 1 0\nop 0 0 1 -2\nof 0 1 2 0\nof 3 4 5 1\n"
		"cobj 0\ncv 0 0 0\ncv 20 20 0\ncv 0 20 0\ncv 5 5 0\ncv 6 5 0\ncv 5 6 0\ncf 0 1 2\ncf 3 4 5\n"
		"cvol 1 0 -1 -1 -1 1 1 1\n"
		"cvolume 4 0 2 2 0 3 3 1\ncp -1 0 0 2\ncp 1 0 0 -3\ncp 0 1 0 -3\ncp 0 -1 0 2\ncp 0 0 1 -1\ncp 0 0 -1 0\n"
		"cvolume 1 0 4 4 0 5 5 1\ncp 1 0 0 -5\ncp -1 0 0 4\ncp 0 1 0 -5\ncp 0 -1 0 4\ncp 0 0 1 -1\ncp 0 0 -1 0\n"
		"cobj 0 1 0 1\ncv 1 0 1\ncv 2 0 1\ncv 1 1 1\ncf 0 1 2\n";

std::vector<uint8_t> slurp(const std::string &path) {
	std::ifstream in(path, std::ios::binary);
	return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

const ThreediChunk *find_chunk(const ThreediChunk *c, const char *tag) {
	if (c == nullptr) return nullptr;
	if (std::string(c->id) == tag && !c->is_parent) return c;
	for (size_t i = 0; i < c->child_count; ++i)
		if (const ThreediChunk *f = find_chunk(&c->children[i], tag)) return f;
	return nullptr;
}

// Copy the model at `from` to `to` with `edit` applied to the payload of the
// first leaf chunk named `tag` (for a table: its count, record size, then the
// records): a hand edit of stored bytes the scene text does not carry.
bool patch(const std::string &from, const std::string &to, const char *tag,
		const std::function<void(uint8_t *payload)> &edit) {
	std::vector<uint8_t> bytes = slurp(from);
	ThreediFile file{};
	if (bytes.empty() || threedi_read_memory(bytes.data(), bytes.size(), &file) != 0) return false;
	const ThreediChunk *chunk = find_chunk(file.root, tag);
	const size_t offset = chunk != nullptr ? static_cast<size_t>(chunk->data - file.buffer) : 0;
	threedi_free_file(&file);
	if (chunk == nullptr) return false;
	edit(bytes.data() + offset);
	std::ofstream(to, std::ios::binary).write(reinterpret_cast<const char *>(bytes.data()),
			static_cast<std::streamsize>(bytes.size()));
	return true;
}

void add_s32(uint8_t *p, int32_t delta) {
	opennova::io::write_u32_le(p, static_cast<uint32_t>(opennova::io::read_s32_le(p) + delta));
}
void add_f32(uint8_t *p, float delta) { opennova::io::write_f32_le(p, opennova::io::read_f32_le(p) + delta); }
} // namespace

int main(int argc, char **argv) {
	if (argc != 2) return 2;
	const auto dir = std::filesystem::path(argv[1]) / "o3d-commands";
	std::filesystem::create_directories(dir);
	const auto build = [&](const char *name, const std::string &text, bool valid = true) {
		const auto scene = (dir / (std::string(name) + ".o3d")).string();
		const auto model = (dir / (std::string(name) + ".3di")).string();
		std::ofstream(scene) << text;
		const int result = threedi_cli::cmd_build(scene.c_str(), model.c_str());
		check(valid ? result == 0 : result != 0, name);
		return model;
	};
	const auto original = build("original", prefix + verts + suffix);
	const auto compare = [&](const char *name, const std::string &text, bool same) {
		const auto path = build(name, text);
		check(threedi_cli::cmd_compare(original.c_str(), path.c_str()) == (same ? 0 : 1), name);
	};
	compare("identical", prefix + verts + suffix, true);
	compare("cyclic-corners", prefix + verts + replace(suffix, "t 0 1 2", "t 1 2 0"), true);
	compare("winding", prefix + verts + replace(suffix, "t 0 1 2", "t 0 2 1"), false);
	compare("weights", prefix + replace(verts, "0.75 0.25 0", "0.25 0.75 0") + suffix, false);
	compare("bone-table", replace(prefix, "bones 0 1", "bones 1 0") + verts + suffix, false);
	// UV swaps preserve each mean, but move the texture to different corners.
	compare("uv0", prefix + replace(replace(verts,
			"v 1 0 0 0 0 1 1 0", "v 1 0 0 0 0 1 0 1"),
			"v 0 1 0 0 0 1 0 1", "v 0 1 0 0 0 1 1 0") + suffix, false);
	compare("uv1", prefix + replace(replace(verts,
			"v 1 0 0 0 0 1 1 0 1 0", "v 1 0 0 0 0 1 1 0 0 1"),
			"v 0 1 0 0 0 1 0 1 0 1", "v 0 1 0 0 0 1 0 1 1 0") + suffix, false);
	compare("remapped-bones", replace(prefix, "bones 0 1", "bones 1 0") +
			replace(replace(replace(verts, "0.75 0.25 0", "0.25 0.75 0"),
					"v 1 0 0 0 0 1 1 0 1 0 0 1 0", "v 1 0 0 0 0 1 1 0 1 0 1 0 0"),
					"v 0 1 0 0 0 1 0 1 0 1 0 1 0", "v 0 1 0 0 0 1 0 1 0 1 1 0 0") + suffix, true);
	build("undeclared-track-register", prefix + verts + suffix + "track rotx 113 0 0 0 100\n", false);
	build("wrapped-panm-part", prefix + verts + "t 0 1 2\npanm 256 0\n", false);
	build("wrapped-panm-parent", prefix + verts + "t 0 1 2\npanm 0 256\n", false);
	build("undeclared-flipbook-register", replace(prefix, "material VS_SKBASIC\n",
			"material VS_SKBASIC\ntexanim 2 1 0\n") + verts + suffix, false);
	const std::string flipbook = replace(replace(prefix, "material VS_SKBASIC\n",
			"register TEX_CAMO1\nregister TEX_CAMO2\nmaterial VS_SKBASIC\ntexanim 2 1 1\n"),
			"model CHECK", "model FLIPBOOK") + verts + suffix;
	const auto flip_a = build("flipbook", flipbook);
	const auto flip_b = build("flipbook-remapped", replace(replace(flipbook,
			"register TEX_CAMO1\nregister TEX_CAMO2", "register TEX_CAMO2\nregister TEX_CAMO1"),
			"texanim 2 1 1", "texanim 2 1 0"));
	check(threedi_cli::cmd_compare(flip_a.c_str(), flip_b.c_str()) == 0, "remapped flipbook register");
	// The same register order, the flipbook pointed at the other register.
	const auto flip_c = build("flipbook-elsewhere", replace(flipbook, "texanim 2 1 1", "texanim 2 1 0"));
	check(threedi_cli::cmd_compare(flip_a.c_str(), flip_c.c_str()) == 1, "flipbook register pointed elsewhere");
	std::string nearby = "o3d 1\nmodel NEARBY\nmaterial FF_ST_OP\nlod 0\npart 0 0 0 0\n";
	for (double x : {0.0, 0.00006, -0.00006})
		nearby += "strip 0\nv " + std::to_string(x) + " 0 0 0 0 1 0 0\nv " + std::to_string(x + 1) +
				" 0 0 0 0 1 1 0\nv " + std::to_string(x) + " 1 0 0 0 1 0 1\nt 0 1 2\n";
	const auto nearby_path = build("nearby", nearby);
	check(threedi_cli::cmd_compare(nearby_path.c_str(), nearby_path.c_str()) == 0, "nearly coincident faces");
	const std::string plain = "o3d 1\nmodel FACES\nlod 0\npart 0 0 0 0\n";
	const std::string collision = "cobj 0\ncv 0 0 0\ncv 1 0 0\ncv 0 1 0\ncf 0 1 2\n";
	const auto normal_a = build("stored-normal", plain + replace(collision, "cf 0 1 2", "cf 0 1 2 14 0 0.5 0 0.75"));
	const auto normal_scene = (dir / "stored-normal.rt.o3d").string();
	const auto normal_b = (dir / "stored-normal.rt.3di").string();
	check(threedi_cli::cmd_scene(normal_a.c_str(), normal_scene.c_str()) == 0, "scene with stored normal");
	check(threedi_cli::cmd_build(normal_scene.c_str(), normal_b.c_str()) == 0, "rebuild stored normal");
	check(threedi_cli::cmd_compare(normal_a.c_str(), normal_b.c_str()) == 0, "preserve stored collision normal");
	const std::string occlusion = "occ 0 0 0\nov 0 0 0\nov 1 0 0\nov 0 1 0\nop 0 0 1 0\nof 0 1 2 0\n";
	const auto occ_a = build("occlusion", plain + occlusion);
	const auto occ_b = build("occlusion-reversed", plain + replace(occlusion, "of 0 1 2", "of 0 2 1"));
	check(threedi_cli::cmd_compare(occ_a.c_str(), occ_b.c_str()) == 1, "reversed occlusion faces");

	// Every check of compare, one field at a time against the rich model.
	const auto rich_a = build("rich", rich);
	check(threedi_cli::cmd_compare(rich_a.c_str(), rich_a.c_str(), true) == 0, "a model is the same as itself, drift-free");
	const auto rich_case = [&](const char *name, const std::string &text, bool same) {
		const auto path = build(name, text);
		check(threedi_cli::cmd_compare(rich_a.c_str(), path.c_str()) == (same ? 0 : 1), name);
	};
	// Moving a bullet face inside section 0 keeps the CMDL and the section
	// bounds (read back below): only the face check sees it.
	const std::string moved = replace(rich, "cv 5 5 0\ncv 6 5 0\ncv 5 6 0", "cv 6 5 0\ncv 7 5 0\ncv 6 6 0");
	rich_case("moved collision face", moved, false);
	{
		Threedi3di3 x{}, y{};
		const auto moved_path = (dir / "moved collision face.3di").string();
		if (threedi_3di3_read(rich_a.c_str(), &x) == 0 && threedi_3di3_read(moved_path.c_str(), &y) == 0 &&
				x.collision != nullptr && y.collision != nullptr) {
			check(std::memcmp(x.collision->model_data.bbox_fp16, y.collision->model_data.bbox_fp16, 24) == 0 &&
							std::memcmp(x.collision->model_data.radii, y.collision->model_data.radii, 12) == 0,
					"the moved face keeps the CMDL");
			check(std::memcmp(x.collision->objects[0].min, y.collision->objects[0].min, 40) == 0,
					"the moved face keeps section 0's bounds");
		} else {
			check(false, "read back the moved face");
		}
		threedi_3di3_free(&x);
		threedi_3di3_free(&y);
	}
	// A render corner 1.5 mm off; a vertex normal 3 degrees off (past what
	// Blender's normal storage moves), and 1 degree off (within: drift).
	rich_case("render position", replace(rich, "v 0 2 0 0 0 1 0 1", "v 0.0015 2 0 0 0 1 0 1"), false);
	rich_case("render normal", replace(rich, "v 0 2 0 0 0 1 0 1", "v 0 2 0 0 0.052336 0.99863 0 1"), false);
	{
		const auto path = build("render normal within storage",
				replace(rich, "v 0 2 0 0 0 1 0 1", "v 0 2 0 0 0.017452 0.99985 0 1"));
		check(threedi_cli::cmd_compare(rich_a.c_str(), path.c_str()) == 0, "render normal within storage");
		check(threedi_cli::cmd_compare(rich_a.c_str(), path.c_str(), true) == 1, "render normal within storage (--strict)");
	}
	rich_case("lod threshold", replace(rich, "lod 200 gnrc", "lod 100 gnrc"), false);
	rich_case("lod type", replace(rich, "lod 200 gnrc", "lod 200 bldg"), false);
	rich_case("part pivot", replace(rich, "part 0 1 0 1\n", "part 0 1 0 1.25\n"), false);
	rich_case("part parent", replace(rich, "part 0 1 0 1\n", "part -1 1 0 1\n"), false);
	rich_case("panm parent", replace(rich, "panm 1 0 0x00000200 1", "panm 1 1 0x00000200 1"), false);
	rich_case("panm track", replace(rich, "track rotz 113 FLICKER 0 0 4096", "track rotz 113 FLICKER 0 0 2048"), false);
	rich_case("panm frame", replace(rich, "mtrx 0 1 0 -1 0 0 0 0 1", "mtrx 1 0 0 0 0 1 0 -1 0"), false);
	rich_case("material texture", replace(rich, "texture rich.tga", "texture rich2.tga"), false);
	rich_case("material generator", replace(rich, "rgbgen 113 0 2 ", "rgbgen 113 0 3 "), false);
	rich_case("user point position", replace(rich, "userpoint muzzle 1 0 1 ", "userpoint muzzle 1 0 1.0015 "), false);
	rich_case("user point name", replace(rich, "userpoint muzzle ", "userpoint muzzle2 "), false);
	rich_case("light position", replace(rich, "light 1 1 0 1.5 ", "light 1 1 0 1.5015 "), false);
	rich_case("light colour", replace(rich, "255 0 34 240", "255 0 35 240"), false);
	rich_case("extra register", replace(rich, "register FLICKER\n", "register FLICKER\nregister DOOR_00\n"), false);
	rich_case("missing register", replace(rich, "register TEX_CAMO2\n", ""), false);
	// Faces on two parallel planes that trade planes keep every normal.
	rich_case("occlusion face plane", replace(replace(rich, "of 0 1 2 0", "of 0 1 2 1"), "of 3 4 5 1", "of 3 4 5 0"),
			false);
	// A ladder's facing is its plane 0: the same solid led by another plane.
	rich_case("ladder facing", replace(rich, "cp -1 0 0 2\ncp 1 0 0 -3\n", "cp 1 0 0 -3\ncp -1 0 0 2\n"), false);
	// One plane of a convex volume moved in, its box as authored: the solid
	// shrinks, the bounds do not.
	rich_case("volume plane", replace(rich, "cp 1 0 0 -5\n", "cp 1 0 0 -4.5\n"), false);
	// Volumes in another order, and a (non-ladder) volume's planes in another
	// order, are the same collision.
	const std::string ladder = "cvolume 4 0 2 2 0 3 3 1\ncp -1 0 0 2\ncp 1 0 0 -3\ncp 0 1 0 -3\ncp 0 -1 0 2\ncp 0 0 1 -1\n"
			"cp 0 0 -1 0\n";
	rich_case("volume order", replace(replace(rich, ladder, ""), "cvol 1 0 -1", ladder + "cvol 1 0 -1"), true);
	rich_case("volume plane order", replace(rich, "cp 1 0 0 -5\ncp -1 0 0 4\n", "cp -1 0 0 4\ncp 1 0 0 -5\n"), true);
	// A volume box one 8.8 step plus a Blender placement's noise off (APLFP1's
	// two CB boxes after an import and export: 3.9978e-3 m) is drift; two
	// steps off is a different box.
	{
		const auto path = build("volume box within storage",
				replace(rich, "cvolume 1 0 4 4 0 5 5 1\n", "cvolume 1 0 4 4 0 5.00395625 5 1\n"));
		check(threedi_cli::cmd_compare(rich_a.c_str(), path.c_str()) == 0, "volume box within storage");
		check(threedi_cli::cmd_compare(rich_a.c_str(), path.c_str(), true) == 1, "volume box within storage (--strict)");
	}
	rich_case("volume box", replace(rich, "cvolume 1 0 4 4 0 5 5 1\n", "cvolume 1 0 4 4 0 5.0078125 5 1\n"), false);

	// Hand edits of derived values, one stored field each.
	const auto edited = [&](const char *name, const char *tag, const std::function<void(uint8_t *)> &edit, int expect,
			int expect_strict) {
		const auto path = (dir / (std::string(name) + ".3di")).string();
		check(patch(rich_a, path, tag, edit), name);
		check(threedi_cli::cmd_compare(rich_a.c_str(), path.c_str()) == expect, name);
		check(threedi_cli::cmd_compare(rich_a.c_str(), path.c_str(), true) == expect_strict,
				(std::string(name) + " (--strict)").c_str());
	};
	// CFAC records: vert_index[3], normal_index, plane_dist @8, box @12. A
	// plane 1.5 cm off still passes a 2 cm check of a face against itself.
	edited("plane distance", "CFAC", [](uint8_t *p) { add_s32(p + 8 + 8, 65536 * 3 / 200); }, 1, 1);
	edited("plane distance by one step", "CFAC", [](uint8_t *p) { add_s32(p + 8 + 8, 1); }, 0, 1);
	edited("face box", "CFAC", [](uint8_t *p) { add_s32(p + 8 + 12, -65536); }, 1, 1);
	// CNRM records: Q14 x y z, then the dominant axis (1 z for this +z face).
	edited("dominant axis", "CNRM", [](uint8_t *p) { opennova::io::write_u16_le(p + 8 + 6, 4); }, 1, 1);
	// CMDL: the box (16.16), then the radii.
	edited("CMDL box", "CMDL", [](uint8_t *p) { add_s32(p + 12, 65536); }, 1, 1);
	edited("CMDL box by one step", "CMDL", [](uint8_t *p) { add_s32(p + 12, 1); }, 0, 1);
	edited("CMDL radius", "CMDL", [](uint8_t *p) { add_s32(p + 24, 65536); }, 1, 1);
	// COBJ records: ..., offset @36, min @48, max @60, med @72, radius @84.
	edited("section box", "COBJ", [](uint8_t *p) { add_s32(p + 8 + 48, -65536); }, 1, 1);
	edited("section midpoint", "COBJ", [](uint8_t *p) { add_s32(p + 8 + 72, 65536); }, 1, 1);
	edited("section radius", "COBJ", [](uint8_t *p) { add_s32(p + 8 + 84, 65536); }, 1, 1);
	// CXLT rows (section 1's pivot).
	edited("CXLT row", "CXLT", [](uint8_t *p) { add_s32(p + 8, 65536); }, 1, 1);
	edited("CXLT row by one step", "CXLT", [](uint8_t *p) { add_s32(p + 8, 1); }, 0, 1);
	// ROBJ records (52 bytes): ..., rel @12, abs @24, sphere centre @36, radius @48.
	edited("part sphere radius", "ROBJ", [](uint8_t *p) { add_f32(p + 8 + 48, 0.5f); }, 1, 1);
	edited("part sphere centre", "ROBJ", [](uint8_t *p) { add_f32(p + 8 + 36, 0.5f); }, 1, 1);
	edited("part rel", "ROBJ", [](uint8_t *p) { add_f32(p + 8 + 52 + 12, 0.5f); }, 1, 1);
	// GHDR: the model radius at +24.
	edited("model radius", "GHDR", [](uint8_t *p) { add_s32(p + 24, 65536); }, 1, 1);
	// OOBJ records: type bytes, then the sphere centre and radius.
	edited("occlusion sphere", "OOBJ", [](uint8_t *p) { add_f32(p + 8 + 4 + 12, 0.5f); }, 1, 1);
	// PANM rows 0 and 1 swapped: the same rows keyed by part, in another order.
	edited("panm row order", "PANM", [](uint8_t *p) {
		uint8_t row[0x44];
		std::memcpy(row, p + 8, 0x44);
		std::memmove(p + 8, p + 8 + 0x44, 0x44);
		std::memcpy(p + 8 + 0x44, row, 0x44);
	}, 1, 1);
	// A second row for part 0 (row 1 transforms part 0 too): part 0 poses by
	// its last row and part 1 by none.
	edited("panm duplicate part", "PANM", [](uint8_t *p) { p[8 + 0x44 + 5] = 0; }, 1, 1);
	// A bullet face naming a vertex outside its section: malformed, never read.
	edited("malformed collision index", "CFAC", [](uint8_t *p) { opennova::io::write_u16_le(p + 8, 100); }, 1, 1);

	// A hit sphere (a bone section's `csphere`): its radius alone, then its
	// bounds alone.
	const std::string sphere = "o3d 1\nmodel SPH\nlod 0\npart 0 0 0 0\ncobj 0\n"
			"csphere 0 0 1 0.5 -0.2 -0.2 0.8 0.2 0.2 1.2\n";
	const auto sphere_a = build("hit sphere", sphere);
	const auto sphere_b = build("hit sphere radius", replace(sphere, "0 0 1 0.5 ", "0 0 1 0.6 "));
	const auto sphere_c = build("hit sphere bounds", replace(sphere, "0.2 0.2 1.2", "0.2 0.2 1.3"));
	check(threedi_cli::cmd_compare(sphere_a.c_str(), sphere_b.c_str()) == 1, "hit sphere radius");
	check(threedi_cli::cmd_compare(sphere_a.c_str(), sphere_c.c_str()) == 1, "hit sphere bounds");

	// The vertex layout: tangents on a shader that does not read them.
	const std::string layout = "o3d 1\nmodel T\nmaterial FF_ST_OP\nlod 0\npart 0 0 0 0\nstrip 0\n"
			"v 0 0 0 0 0 1 0 0\nv 1 0 0 0 0 1 1 0\nv 0 1 0 0 0 1 0 1\nt 0 1 2\n";
	const auto layout_a = build("layout", layout);
	const auto layout_b = build("layout-tangents", replace(layout, "model T\n", "model T\ntangents 1\n"));
	check(threedi_cli::cmd_compare(layout_a.c_str(), layout_b.c_str()) == 1, "tangent layout");
	// Tangent values are derived by a heuristic of ours: DRIFT only.
	const auto tangent_a = build("tangent-values", replace(layout, "FF_ST_OP", "VS_PHONGT"));
	const auto tangent_b = (dir / "tangent-values-edited.3di").string();
	check(patch(tangent_a, tangent_b, "VERT", [](uint8_t *p) { add_f32(p + 12 + 40, 0.5f); }), "edit a tangent");
	check(threedi_cli::cmd_compare(tangent_a.c_str(), tangent_b.c_str()) == 0, "tangent values are drift");
	check(threedi_cli::cmd_compare(tangent_a.c_str(), tangent_b.c_str(), true) == 1, "tangent values under --strict");

	// A vertex without weight is drawn wholly on its first slot's bone.
	const std::string rigid = "o3d 1\nmodel ZW\nskinned 1\nmaterial VS_SKBASIC\nlod 0 gnrc\npart 0 0 0 0\npart 0 0 0 1\n"
			"strip 0\nbones 0 1\nv 0 0 0 0 0 1 0 0 0 0 0 0 0 0\nv 1 0 0 0 0 1 1 0 0 0 0 1 0 0\n"
			"v 0 1 0 0 0 1 0 1 0 0 0 1 0 0\nt 0 1 2\npanm 0 0\npanm 1 0\n";
	const auto rigid_a = build("zero-weight", rigid);
	const auto rigid_b = build("zero-weight-slot", replace(rigid, "v 0 0 0 0 0 1 0 0 0 0 0", "v 0 0 0 0 0 1 0 0 1 0 0"));
	check(threedi_cli::cmd_compare(rigid_a.c_str(), rigid_b.c_str()) == 1, "zero-weight vertex slot");

	// A file that cannot be read is an error, not a usage mistake.
	check(threedi_cli::cmd_compare((dir / "missing.3di").string().c_str(), rich_a.c_str()) == 1, "unreadable file");

	// Matching scales: a flat 20,000-triangle grid (one centroid x for every
	// face) and 2,000 stacked copies of one face, each against itself.
	std::string grid = "o3d 1\nmodel GRID\nmaterial FF_ST_OP\nlod 0\npart 0 0 0 0\nstrip 0\n";
	for (int i = 0; i <= 100; ++i)
		for (int j = 0; j <= 100; ++j)
			grid += "v 0 " + std::to_string(i * 0.1) + " " + std::to_string(j * 0.1) + " 1 0 0 " + std::to_string(i * 0.01) +
					" " + std::to_string(j * 0.01) + "\n";
	for (int i = 0; i < 100; ++i)
		for (int j = 0; j < 100; ++j) {
			const int v = i * 101 + j;
			grid += "t " + std::to_string(v) + " " + std::to_string(v + 101) + " " + std::to_string(v + 1) + "\n";
			grid += "t " + std::to_string(v + 1) + " " + std::to_string(v + 101) + " " + std::to_string(v + 102) + "\n";
		}
	std::string stack = "o3d 1\nmodel STACK\nmaterial FF_ST_OP\nlod 0\npart 0 0 0 0\nstrip 0\n";
	for (int i = 0; i < 2000; ++i)
		stack += "v 0 0 0 0 0 1 0 0\nv 1 0 0 0 0 1 1 0\nv 0 1 0 0 0 1 0 1\nt " + std::to_string(3 * i) + " " +
				std::to_string(3 * i + 1) + " " + std::to_string(3 * i + 2) + "\n";
	for (const auto &big : {build("grid", grid), build("stack", stack)}) {
		const auto start = std::chrono::steady_clock::now();
		check(threedi_cli::cmd_compare(big.c_str(), big.c_str()) == 0, "a large model is the same as itself");
		const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
		// Bucketing on the centroid x alone took 16.8 s (grid) and 4.1 s
		// (stack); the grid and the copy classes take well under a second.
		check(seconds < 8.0, "large models compare in linear time");
	}

	// The derived collision rules, read back from the minted bytes.
	const auto read = [&](const std::string &path, Threedi3di3 &m) {
		m = Threedi3di3{};
		const bool ok = threedi_3di3_read(path.c_str(), &m) == 0;
		check(ok, ("read back " + path).c_str());
		return ok;
	};
	Threedi3di3 m{};
	// A bullet face on z = 1 facing +z: the runtime tests n . p + plane_dist.
	if (read(build("plane-distance", plain + "cobj 0\ncv 0 0 1\ncv 1 0 1\ncv 0 1 1\ncf 0 1 2\n"), m)) {
		check(m.collision != nullptr && m.collision->face_count == 1 && m.collision->faces[0].plane_dist_fp16 == -65536,
				"bullet-face plane distance is -(n . v0)");
		threedi_3di3_free(&m);
	}
	// A unit box as triangles, counter-clockwise from outside; `last` names
	// the side whose triangles come last.
	const auto box = [](double x0, double y0, double z0, double x1, double y1, double z1, int last) {
		std::string s;
		for (int i = 0; i < 8; ++i)
			s += "vv " + std::to_string((i & 1) ? x1 : x0) + " " + std::to_string((i & 2) ? y1 : y0) + " " +
					std::to_string((i & 4) ? z1 : z0) + "\n";
		// +x -x +y -y +z -z, each side counter-clockwise from outside.
		const int sides[6][4] = {{1, 3, 7, 5}, {0, 4, 6, 2}, {2, 6, 7, 3}, {0, 1, 5, 4}, {4, 5, 7, 6}, {0, 2, 3, 1}};
		for (int k = 0; k < 6; ++k) {
			const int *q = sides[k == 5 ? last : (k >= last ? k + 1 : k)];
			s += "vf " + std::to_string(q[0]) + " " + std::to_string(q[1]) + " " + std::to_string(q[2]) + "\n";
			s += "vf " + std::to_string(q[0]) + " " + std::to_string(q[2]) + " " + std::to_string(q[3]) + "\n";
		}
		return s;
	};
	// A ladder faces the plane of its last triangle (plane 0 after the swap);
	// a box's faces fold into its six box planes.
	if (read(build("ladder", plain + "cobj 0\ncvmesh 4 0\n" + box(0, 0, 0, 1, 1, 2, 3) + "cvmesh 1 0\n" +
			box(2, 0, 0, 3, 1, 1, 5)), m)) {
		check(m.collision != nullptr && m.collision->volume_count == 2 && m.collision->volumes[0].plane_count == 6 &&
				m.collision->volumes[1].plane_count == 6, "a box volume keeps its six planes");
		check(m.collision != nullptr && m.collision->planes[0].normal[1] == -1.0f && m.collision->planes[3].normal[0] == 1.0f,
				"a ladder's plane 0 is its last triangle's plane");
		check(m.collision != nullptr && m.collision->objects[0].radius == 0, "a volume-only section has radius 0");
		threedi_3di3_free(&m);
	}
	// Seams: a CB inside another CB flags the planes of its faces; the outer
	// one's faces lie inside nothing.
	if (read(build("seams", plain + "cobj 0\ncvmesh 1 0\n" + box(0, 0, 0, 1, 1, 1, 5) + "cvmesh 1 0\n" +
			box(-1, -1, -1, 2, 2, 2, 5)), m)) {
		int inner = 0, outer = 0;
		for (int p = 0; m.collision != nullptr && p < 6; ++p) {
			inner += m.collision->planes[p].flags;
			outer += m.collision->planes[6 + p].flags;
		}
		check(inner == 6 && outer == 0, "seam flags follow the OED overlap rule");
		threedi_3di3_free(&m);
	}
	// No section at all: the CMDL box still bounds LOD 0 while its radii,
	// taken from the collision LOD's faces, stay empty (retail CNet01: 0 0
	// and a height of -20000).
	if (read(build("no-collision", "o3d 1\nmodel NOCOLL\nmaterial FF_ST_OP\nlod 0\npart 0 0 0 0\nstrip 0\n"
			"v 0 0 0 0 0 1 0 0\nv 2 0 0 0 0 1 1 0\nv 0 1 3 0 0 1 0 1\nt 0 1 2\n"), m)) {
		check(m.collision != nullptr && m.collision->object_count == 0 && m.collision->model_data.bbox[3] == 2.0f &&
				m.collision->model_data.radii[0] == 0.0f && m.collision->model_data.radii[2] == -20000.0f,
				"a model without sections bounds LOD 0 in its CMDL box only");
		threedi_3di3_free(&m);
	}
	// A tangent-space shader lays out tangents, derived from the UVs: u runs
	// along mission +x, model (0 0 1); v (down) along mission -y, model (1 0 0).
	if (read(build("tangents", "o3d 1\nmodel TANGENT\nmaterial VS_PHONGT\nlod 0\npart 0 0 0 0\nstrip 0\n"
			"v 0 0 0 0 0 1 0 0\nv 1 0 0 0 0 1 1 0\nv 0 -1 0 0 0 1 0 1\nt 0 1 2\n"), m)) {
		const ThreediVertex *v = m.lod_count > 0 && m.lods[0].vertices.count > 0 ? &m.lods[0].vertices.items[0] : nullptr;
		check(v != nullptr && (m.lods[0].vertices.flags & THREEDI_VERTEX_FLAG_TANGENTS) == THREEDI_VERTEX_FLAG_TANGENTS,
				"a tangent-space shader lays out tangents");
		check(v != nullptr && v->tangent[2] > 0.999f && v->bitangent[0] > 0.999f, "tangents follow the UVs");
		threedi_3di3_free(&m);
	}
	std::printf("o3d_commands: %d failures\n", failures);
	return failures ? 1 : 0;
}
