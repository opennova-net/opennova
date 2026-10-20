// Generator + guard for the GUT world model set fixtures/gut/*.3di: the
// buildings the GUT render/world suites (blink_reopen_edge,
// static_decoration_lighting, mission_present_pass) stand a player in, place
// decorations inside and open, authored from small integer-friendly data
// through tests/fixtures/minimal_3di_builder.h and minted by our own parity
// writer (threedi_3di3_write), so every file is reproduced byte-for-byte and
// no retail bytes live in the tree (ADR 0003).
//
// Each model stands in for the retail one those suites once loaded and keeps
// only what the suites key on (mission axes, x forward / y left / z up, origin
// on the ground):
//   armory_floor  the synthetic armory's floor plan (minimal_3di_gen's
//            `armory`: four parts, the shell's walls, roof, armory zone,
//            vehicle hull and ladder, the blink boxes east 0x2E / west 0x28 /
//            middle 0x2E, the OCCL topology) standing on the solid floor slab
//            00TRa's Armry01 authors as its COBJ 0 volume 0 (a CB under the
//            whole plan, 1.2 deep, its top at the rooms' floor). A body
//            whose blink letters say indoors probes its ground through the
//            candidate solids alone, the heightfield skipped
//            (CollisionWorld::clip_segment_to_nearest_collision [orig:
//            Entity_RaycastCollision @ 0x413760]), so a room with no solid
//            under it drops its occupant through the floor; the slab holds
//            it. The armory's lights, bulb and glass strips are left out.
//   door_block  the IBlock01 shape: LOD 0 of type `door`, the block (part 0)
//            and its door (part 1, pivoted on its hinge); CTRL 0 `DOOR_00`;
//            the door's row turns rotation x (Euler, flags 0x200) on control
//            113 register 0 from 0 to -5688 (IBlock01's LOD 0 row 1); one
//            collision section per part
//   light_post  the Lpost01 shape, a decoration tall enough to show over a
//            window sill: one inert part, a 4-unit post and its lamp head,
//            one CB volume, no light record
// Default: rebuild every file in memory and byte-compare the committed copies;
// `--write` (re)writes them. Every run re-reads the committed bytes and checks
// the authored facts the consumers pin.
#include "fixtures/minimal_3di_builder.h"

#include "common/file_io.h"
#include "common/test_paths.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace opennova::threedi;
using namespace synth3di;

namespace {

int failures = 0;

bool expect(bool cond, const std::string &msg) {
	if (!cond) {
		std::fprintf(stderr, "FAIL: %s\n", msg.c_str());
		++failures;
	}
	return cond;
}

Box box(double x0, double y0, double z0, double x1, double y1, double z1) {
	return Box{Vec3{x0, y0, z0}, Vec3{x1, y1, z1}};
}

constexpr Vec3 kUp{0.0, 0.0, 1.0};
constexpr Vec3 kBackward{-1.0, 0.0, 0.0};

// ---------------------------------------------------------------------------
Model make_armory_floor() {
	Model m;
	m.name = "armory_floor";
	const int lod = m.add_lod(0, "bldg");
	const int exterior = m.add_material("FF_MT_OP", "armry.tga");
	m.add_detail_texture(exterior, "armry_o.tga");
	const int interior = m.add_material("FF_ST_OP", "armryin.tga");
	// The synthetic armory's floor plan (entity-local, mission axes): east
	// room x 1.1..5.7, west room x -6.8..-0.6, middle x -0.5..1.1, all y
	// -3.2..2.8 (west to 3.7).
	const Box shell = box(-7.0, -3.5, 0.0, 6.0, 4.0, 3.0);
	const Box east = box(1.1, -3.2, 0.0, 5.7, 2.8, 3.0);
	const Box west = box(-6.8, -3.2, 0.0, -0.6, 3.7, 3.0);
	const Box middle = box(-0.5, -3.2, 0.0, 1.1, 2.8, 3.0);
	const int p_exterior = m.add_part(lod, 0, Vec3{});
	m.add_box(lod, p_exterior, exterior, shell);
	m.add_box(lod, p_exterior, exterior, box(-7.2, -3.7, 3.0, 6.2, 4.2, 3.3)); // the roof slab
	const int p_east = m.add_part(lod, 0, Vec3{});
	m.add_box(lod, p_east, interior, east);
	const int p_west = m.add_part(lod, 0, Vec3{});
	m.add_box(lod, p_west, interior, west);
	const int p_middle = m.add_part(lod, 0, Vec3{});
	m.add_box(lod, p_middle, interior, middle);
	for (int part = 0; part < 4; ++part) m.add_panm(lod, part, 0);
	m.add_user_point("Armory", Vec3{-3.7, 0.3, 0.9}, kUp, 0, kUserPointGameplay);
	m.add_user_point("Ground", Vec3{}, kUp, 0, kUserPointEffect);
	// Collision: COBJ 0 = the shell: first the floor slab (Armry01's volume 0,
	// a CB 1.2 deep under the whole plan, its top at the floor), then the
	// armory's four walls, roof, the armory trigger, the vehicle hull (down
	// over the slab, as Armry01's VC encloses its own) and the ladder on the
	// east wall; COBJ 1..3 = one blink box per room (east 0x2E, west 0x28,
	// middle 0x2E), each reaching 0.1 below the floor as Armry01's reach
	// 0.11 below its slab top, so a decoration resting on the floor stands
	// inside its room's box.
	const int c_shell = m.add_cobj(0);
	m.add_face_box(c_shell, shell);
	m.add_volume(c_shell, 1, 0, box(-7.0, -3.5, -1.2, 6.0, 4.0, 0.0));
	m.add_volume(c_shell, 1, 0, box(-7.0, -3.5, 0.0, -6.8, 4.0, 3.0));
	m.add_volume(c_shell, 1, 0, box(5.7, -3.5, 0.0, 6.0, 4.0, 3.0));
	m.add_volume(c_shell, 1, 0, box(-7.0, 3.7, 0.0, 6.0, 4.0, 3.0));
	m.add_volume(c_shell, 1, 0, box(-7.0, -3.5, 0.0, 6.0, -3.2, 3.0));
	m.add_volume(c_shell, 1, 0, box(-7.2, -3.7, 3.0, 6.2, 4.2, 3.3));
	m.add_volume(c_shell, 6, 0, box(1.5, -3.0, 0.0, 5.5, 2.5, 2.5));
	m.add_volume(c_shell, 7, 0, box(-7.0, -3.5, -1.2, 6.0, 4.0, 3.3));
	m.add_volume(c_shell, 4, 0, box(6.0, -0.5, 0.0, 6.4, 0.5, 3.3));
	const int c_east = m.add_cobj(0);
	{
		const Vec3 walls[4][4] = {
			{{1.1, -3.2, 0.0}, {1.1, 2.8, 0.0}, {1.1, 2.8, 3.0}, {1.1, -3.2, 3.0}},
			{{5.7, -3.2, 0.0}, {5.7, -3.2, 3.0}, {5.7, 2.8, 3.0}, {5.7, 2.8, 0.0}},
			{{1.1, -3.2, 0.0}, {1.1, -3.2, 3.0}, {5.7, -3.2, 3.0}, {5.7, -3.2, 0.0}},
			{{1.1, 2.8, 0.0}, {5.7, 2.8, 0.0}, {5.7, 2.8, 3.0}, {1.1, 2.8, 3.0}}};
		for (const auto &quad : walls) m.add_face_quad(c_east, quad);
	}
	m.add_volume(c_east, 8, 0x2E, box(1.05, -3.2, -0.1, 5.7, 2.8, 3.0));
	const int c_west = m.add_cobj(0);
	{
		const Vec3 floor[4] = {{-6.8, -3.2, 0.0}, {-0.6, -3.2, 0.0}, {-0.6, 3.7, 0.0}, {-6.8, 3.7, 0.0}};
		m.add_face_quad(c_west, floor);
	}
	m.add_volume(c_west, 8, 0x28, box(-6.8, -3.2, -0.1, -0.6, 3.7, 3.0));
	const int c_middle = m.add_cobj(0);
	{
		const Vec3 floor[4] = {{-0.5, -3.2, 0.0}, {1.1, -3.2, 0.0}, {1.1, 2.8, 0.0}, {-0.5, 2.8, 0.0}};
		m.add_face_quad(c_middle, floor);
	}
	m.add_volume(c_middle, 8, 0x2E, box(-0.5, -3.2, -0.1, 1.15, 2.8, 3.0));
	// Occlusion (the armory's record order): the whole-building open slot,
	// four wall occluders (the south one stops short of the window), the east
	// room's south window, the east<->middle and middle<->west portals.
	m.add_occ_box(1, 0, 0, shell);
	m.add_occ_box(0, 0, 0, box(-7.0, -3.5, 0.0, 1.5, -3.2, 3.0));
	m.add_occ_box(0, 0, 0, box(-7.0, 3.7, 0.0, 6.0, 4.0, 3.0));
	m.add_occ_box(0, 0, 0, box(-7.0, -3.5, 0.0, -6.8, 4.0, 3.0));
	m.add_occ_box(0, 0, 0, box(5.7, -3.5, 0.0, 6.0, 4.0, 3.0));
	{
		const Vec3 window[4] = {{2.1, -3.5, 1.0}, {3.1, -3.5, 1.0}, {3.1, -3.5, 2.5}, {2.1, -3.5, 2.5}};
		m.add_occ_quad(2, 1, 0, window, Vec3{0.0, -1.0, 0.0});
		const Vec3 door_east[4] = {{1.1, 0.75, 0.0}, {1.1, 1.75, 0.0}, {1.1, 1.75, 2.2}, {1.1, 0.75, 2.2}};
		m.add_occ_quad(3, 1, 3, door_east, kBackward);
		const Vec3 door_west[4] = {{-0.54, -2.75, 0.0}, {-0.54, -1.75, 0.0}, {-0.54, -1.75, 2.2}, {-0.54, -2.75, 2.2}};
		m.add_occ_quad(3, 3, 2, door_west, kBackward);
	}
	return m;
}

// IBlock01's door row: LOD 0 row 1, flags 0x200 (Euler rotation, not
// reversed), rotation x on control 113 (the control register) register 0
// `DOOR_00`, 0 to -5688 (-125 degrees at 16384 counts per turn).
constexpr int16_t kDoorOpenRaw = -5688;

Model make_door_block() {
	Model m;
	m.name = "door_block";
	const int lod = m.add_lod(0, "door");
	const int mat = m.add_material("FF_ST_OP", "block.tga");
	const Box block = box(-4.0, -3.0, 0.0, 0.0, 3.0, 3.0);
	// The door hangs in the block's front face on a hinge at y -0.5.
	const Box door = box(0.0, -0.5, 0.0, 0.1, 0.5, 2.2);
	const Vec3 hinge{0.0, -0.5, 0.0};
	const int p_block = m.add_part(lod, 0, Vec3{});
	m.add_box(lod, p_block, mat, block);
	const int p_door = m.add_part(lod, 0, hinge);
	m.add_box(lod, p_door, mat, door);
	m.add_control_register("DOOR_00");
	m.add_panm(lod, p_block, 0);
	ThreediPartAnimation &row = m.add_panm(lod, p_door, 0, threedi_panm_pack_flags(0, 2, 0, 0));
	row.rotation_x = track(THREEDI_PANM_STYLE_CONTROL_REGISTER, 0, 0, 0, kDoorOpenRaw);
	m.add_user_point("Ground", Vec3{}, kUp, 0, kUserPointGameplay);
	const int c_block = m.add_cobj(0);
	m.add_face_box(c_block, block);
	m.add_volume(c_block, 1, 0, block);
	const int c_door = m.add_cobj(0, hinge);
	m.add_face_box(c_door, door);
	m.add_volume(c_door, 1, 0, door);
	return m;
}

Model make_light_post() {
	Model m;
	m.name = "light_post";
	const int lod = m.add_lod();
	const int mat = m.add_material("FF_ST_OP", "lpost.tga");
	const Box post = box(-0.1, -0.1, 0.0, 0.1, 0.1, 4.0);
	const Box lamp = box(-0.25, -0.25, 4.0, 0.25, 0.25, 4.4);
	const int part = m.add_part(lod, 0, Vec3{});
	m.add_box(lod, part, mat, post);
	m.add_box(lod, part, mat, lamp);
	m.add_panm(lod, part, 0);
	m.add_user_point("ground", Vec3{}, kUp, 0, kUserPointGameplay);
	const int cobj = m.add_cobj(0);
	m.add_face_box(cobj, post);
	m.add_volume(cobj, 1, 0, post);
	return m;
}

struct Recipe {
	const char *file;
	Model (*make)();
};

const std::vector<Recipe> &recipes() {
	static const std::vector<Recipe> kRecipes = {
			{"armory_floor", make_armory_floor},
			{"door_block", make_door_block},
			{"light_post", make_light_post},
	};
	return kRecipes;
}

// ---------------------------------------------------------------------------
// The authored facts the consumers pin, checked on the committed bytes.
struct Parsed {
	Threedi3di3 model{};
	bool ok = false;
	explicit Parsed(const std::vector<uint8_t> &bytes) {
		ok = threedi_3di3_read_memory(bytes.data(), bytes.size(), &model) == 0;
	}
	~Parsed() {
		if (ok) threedi_3di3_free(&model);
	}
	const char *reg(size_t i) const { return i < model.ctrl.count ? model.ctrl.registers[i].name : ""; }
	size_t parts() const { return model.lod_count ? model.lods[0].render_object_count : 0; }
	size_t cobjs() const { return model.collision ? model.collision->object_count : 0; }
	bool safe() const { return model.collision == nullptr || threedi_3di3_collision_is_runtime_safe(model.collision) == 1; }
	const ThreediBoundingVolume *volume(size_t i) const {
		return model.collision != nullptr && i < model.collision->volume_count ? &model.collision->volumes[i] : nullptr;
	}
};

void check_facts(const std::string &name, const std::vector<uint8_t> &bytes) {
	Parsed p(bytes);
	if (!expect(p.ok, name + ": committed bytes parse")) return;
	expect(p.safe(), name + ": collision block is runtime safe");
	if (name == "armory_floor") {
		// The slab leads COBJ 0 and tops out at the floor; the blink boxes
		// keep the armory's flags in section order.
		const ThreediBoundingVolume *slab = p.volume(0);
		const ThreediBoundingVolume *west = p.volume(10);
		expect(p.parts() == 4 && p.cobjs() == 4 && p.model.collision->objects[0].num_bounding_volumes == 9 &&
						slab != nullptr && slab->collidable_type == 1 && slab->max_z_fp16 == 0 && slab->min_z_fp16 < 0 &&
						west != nullptr && west->collidable_type == 8 && west->flags == 0x28 && p.volume(9)->flags == 0x2E &&
						p.volume(11)->flags == 0x2E && p.model.occlusion_object_count == 8,
				name + ": the floor slab under the armory's rooms");
	} else if (name == "door_block") {
		bool ok = p.parts() == 2 && p.cobjs() == 2 && !std::strcmp(p.reg(0), "DOOR_00") &&
				!std::strcmp(p.model.lods[0].model_type, "door") && p.model.lods[0].part_animation_count == 2;
		if (ok) {
			const ThreediPartAnimation &door = p.model.lods[0].part_animations[1];
			ok = door.subobject_index == 1 && door.flags == 0x200 &&
					door.rotation_x.control == THREEDI_PANM_STYLE_CONTROL_REGISTER && door.rotation_x.control_param == 0 &&
					door.rotation_x.start == 0 && door.rotation_x.end == kDoorOpenRaw;
		}
		expect(ok, name + ": the door turns on DOOR_00");
	} else if (name == "light_post") {
		expect(p.parts() == 1 && p.cobjs() == 1 && p.model.light_count == 0 && p.model.collision->volume_count == 1 &&
						p.model.collision->volumes[0].max_z_fp16 == 4 * 65536,
				name + ": a 4-unit post, no light");
	}
}

} // namespace

int main(int argc, char **argv) {
	bool write_mode = false;
	for (int i = 1; i < argc; ++i)
		if (std::strcmp(argv[i], "--write") == 0) write_mode = true;
	const std::string dir = std::string(test_paths_repo_root(__FILE__)) + "/fixtures/gut";
	std::filesystem::create_directories(dir);

	for (const Recipe &recipe : recipes()) {
		const std::string path = dir + "/" + recipe.file + ".3di";
		const Model model = recipe.make();
		std::vector<uint8_t> bytes;
		if (!expect(mint(model, bytes), std::string(recipe.file) + ": the writer accepts the model")) continue;
		if (write_mode) {
			std::ofstream o(path, std::ios::binary);
			o.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
			std::printf("wrote %s (%zu bytes)\n", path.c_str(), bytes.size());
			check_facts(recipe.file, bytes);
			continue;
		}
		std::vector<uint8_t> committed;
		if (!expect(test_io::read_file(path, committed), path + " missing; run with --write")) continue;
		if (test_io::is_lfs_pointer(committed)) {
			std::printf("[skip] %s is an unpulled LFS pointer\n", path.c_str());
			continue;
		}
		expect(committed == bytes, path + " differs from the generator output; regenerate with --write");
		check_facts(recipe.file, committed);
		std::printf("%-24s %6zu bytes\n", recipe.file, committed.size());
	}
	if (failures == 0 && !write_mode) std::printf("OK: fixtures/gut world models byte-reproducible\n");
	return failures == 0 ? 0 : 1;
}
