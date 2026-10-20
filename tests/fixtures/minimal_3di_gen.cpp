// Generator + guard for the synthetic 3DI model set fixtures/threedi/synth
// (fixtures/README.md): eleven base models and the one-edit variants the GUT
// suite loads, authored from small integer-friendly data in
// minimal_3di_builder.h and minted through our own parity writer
// (threedi_3di3_write), so every file is reproduced byte-for-byte on every
// platform and no retail bytes live in the tree (ADR 0003).
//
// The set stands in for the retail models the tests once loaded, shaped after
// what those tests key on (mission axes, x forward / y left / z up, origin on
// the ground unless noted):
//   crate    one part, one box, a CB volume + face box, a `ground` point
//   gun      receiver + barrel parts, no collision block (a held weapon),
//            `MFlash01` on the barrel, `Bullet01`/`bcasing` on the root
//   shed     one part, an opaque hut + one alpha bulb strip, ONE light,
//            CTRL `FLICKER`, two CB volumes (the compare/roundtrip witness
//            and the effect-light host)
//   house    one inert part, three opaque strips, 3 CB + one octagonal prism,
//            no OOBJ (the collision-backed building without a section map)
//   bird     nine skinned parts (the Bird1 hierarchy), two collision faces per
//            COBJ, no BVOL (the face-only projectile witness)
//   person   nineteen skinned parts (the retail person rig order: 14 = head,
//            16 = left hand), origin at the pelvis, COBJ 0 a face box and
//            every other COBJ a bone sphere (14: parent 13, med (1/16, 0,
//            13/16), r 5/32)
//   pump     two LODs x five parts (base, beam, head, rod, weight); LOD0's
//            beam row is a free-running sine on rotation z, every other row
//            inert; each part owns a collision face box
//   armory   four parts (exterior, east, west, middle rooms), material 3 =
//            the FLICKER-driven bulb, two spatially separated alpha strips,
//            two lights, the retail floor plan's OCCL topology (open box,
//            four wall occluders, the east room's south window, portals
//            east<->middle and middle<->west), blink boxes 0x2E/0x28/0x2E,
//            collision faces [12, 8, 2, 2]
//   mount    base + cradle + gun; CTRL HEAT_GLOW/EWEAP_GUNYAW/EWEAP_GUNPITCH;
//            the cradle yaws on register 1, the gun yaws and pitches on 1/2;
//            user points BCasing, Bullet, Camera, heat, MFlash01, Usegun (row 6)
//   carrier  hull + cabin; the dsuv1 user-point order (ctrlx13 first, then
//            ewep01, sitex00d, sitex08c, FX01, ground, sitex06b, sitex12a);
//            the cabin COBJ carries its own CVRT run and CXLT (2, 0, 1/2)
//   tank     hull + turret; ctrlx25 (row 1), ewep01 on the turret, ground;
//            the turret yaws on VEHICLE_GUNYAW
//   vm40     the first-person viewmodel: forty skinned parts on the stock
//            first-person rig's 40 bones (vm40_rig.h: two arms of five
//            fingers, the receiver, magazine and bolt), each part a cube on
//            its bone and a collision section of its own carrying one face
//            quad on its pivot, the fingers' quads under the 8.8 grid's step
//            so the grid collapses them (ArmsG's and Mp5b_1st's); the arms on
//            a material of two camouflage frames CTRL TEX_CAMO1 picks (ArmsG's
//            animated texture rows); `bullet` and
//            `MFlash01` on the receiver; the fixtures/anim/vm40* clip set plays
//            on it
//   heli     Dblkhwk1's layout: tangent frames on every vertex (vflags 0x15),
//            a hull, a main rotor and its blur sharing one pivot, a tail rotor
//            and its blur on MTRX frame 1 (the tail rotor's tilted axes), the
//            rotors register-driven (HELO_ROTOR, HELO_TAILROTOR), three LODs of
//            which the last holds no part
//   person_slot2  the person with every limb skinned to two bones by a
//            strip bone table (vflags 0x55, tangents too), each vertex naming
//            a third slot past its strip's two-entry table with no weight on it
//            (FSldr03's), and a CXLT table whose rows are not its sections'
//            offsets (US01's, ArmsG's, Mp5b_1st's)
//   person_rest4  the same with an eighth of each vertex's weight, the
//            remainder the fourth slot takes, on that slot past the table
//            (SASBODY1's and sasmg's)
// Outside the set, fixtures/threedi/crate_alphagen24.3di: the crate wearing
// retail's constant AlphaGen on an opaque material, the MTRL row JOTAC's
// SCrate1.3di carries (FF_ST_OP, two-sided, alphagen style 24 reg -1 start 128,
// no alpha test: witnessed 2026-10-19, the retired threedi_retail_material_facts
// row), which the projected-shadow witness keys on.
// Default: rebuild every file in memory and byte-compare the committed
// copies; `--write` (re)writes them. Every run re-reads the committed bytes
// and checks the authored facts the consumers pin.
#include "fixtures/minimal_3di_builder.h"
#include "fixtures/person_rig.h"
#include "fixtures/vm40_rig.h"

#include <formats/threedi/threedi_panm_pose.h>

#include "common/test_paths.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

using namespace synth3di;

#include "common/file_io.h"

using namespace opennova::threedi;

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
constexpr Vec3 kForward{1.0, 0.0, 0.0};
constexpr Vec3 kBackward{-1.0, 0.0, 0.0};
constexpr int kWhite[3] = {255, 255, 255};
constexpr int kBlack[3] = {0, 0, 0};
constexpr int kWarm[3] = {255, 246, 222};
constexpr int kBulb[3] = {235, 203, 137};
constexpr int kBulbEnd[3] = {250, 235, 214};
constexpr int kGlow[3] = {115, 46, 0};

// A part whose box also carries a collision face box + CB volume in its own COBJ.
int solid_part(Model &m, int lod, int parent, Vec3 pivot, int material, const Box &b) {
	const int part = m.add_part(lod, parent, pivot);
	m.add_box(lod, part, material, b);
	return part;
}

// The register-driven slide every "slide" recipe writes: translation x on
// CTRL register `reg`, 0..4 units, no speed (control 113 = control register).
ThreediPartAnimation slide_row(int part, int reg) {
	ThreediPartAnimation row = inert_panm(part, 0);
	row.flags = threedi_panm_pack_flags(0, 0, 0, THREEDI_TRANS_X);
	row.translation = track(THREEDI_PANM_STYLE_CONTROL_REGISTER, static_cast<uint8_t>(reg), 0, 0, 4 * THREEDI_PANM_VALUE_ONE);
	return row;
}

// A sine rotation 0..90 degrees at speed 1 on one axis (the House/pump recipes).
ThreediPartAnimation sine_rotation_row(int part, int axis) {
	ThreediPartAnimation row = inert_panm(part, 0);
	row.flags = threedi_panm_pack_flags(0, 2, 0, 0);
	const ThreediTransform t = track(THREEDI_PANM_STYLE_SINE_WAVE, 0, THREEDI_PANM_VALUE_ONE, 0,
			static_cast<int16_t>(threedi_panm_rotation_raw_from_deg(90.0)));
	if (axis == 0) row.rotation_x = t;
	if (axis == 1) row.rotation_y = t;
	if (axis == 2) row.rotation_z = t;
	return row;
}

// ---------------------------------------------------------------------------
Model make_crate() {
	Model m;
	m.name = "crate";
	const int lod = m.add_lod();
	const int mat = m.add_material("FF_ST_OP", "crate.tga");
	const Box b = box(-0.5, -0.5, 0.0, 0.5, 0.5, 1.0);
	solid_part(m, lod, 0, Vec3{}, mat, b);
	m.add_panm(lod, 0, 0);
	m.add_user_point("ground", Vec3{}, kUp, 0, kUserPointGameplay);
	const int cobj = m.add_cobj(0);
	m.add_face_box(cobj, b);
	m.add_volume(cobj, 1, 0, b);
	return m;
}

Model make_gun() {
	Model m;
	m.name = "gun";
	const int lod = m.add_lod();
	const int mat = m.add_material("FF_ST_OP", "gun.tga");
	const int receiver = m.add_part(lod, 0, Vec3{});
	m.add_box(lod, receiver, mat, box(-0.25, -0.03, 0.10, 0.25, 0.03, 0.20));
	const int barrel = m.add_part(lod, receiver, Vec3{0.25, 0.0, 0.15});
	m.add_box(lod, barrel, mat, box(0.25, -0.02, 0.13, 0.75, 0.02, 0.17));
	m.add_panm(lod, 0, 0);
	m.add_panm(lod, 1, 0);
	// The flash point first: the user-point overlay pins its FIRST row to a
	// drawing part whose pivot is off the model root. The muzzle (`Bullet01`)
	// rides the root so a one-bone rig can carry it.
	m.add_user_point("MFlash01", Vec3{0.72, 0.0, 0.15}, kForward, barrel, kUserPointEffect);
	m.add_user_point("Bullet01", Vec3{0.75, 0.0, 0.15}, kForward, 0, kUserPointEffect);
	m.add_user_point("bcasing", Vec3{0.0, -0.05, 0.18}, Vec3{0.0, -1.0, 0.0}, 0, kUserPointEffect);
	m.add_user_point("ground", Vec3{}, kUp, 0, kUserPointGameplay);
	return m;
}

Model make_shed() {
	Model m;
	m.name = "shed";
	const int lod = m.add_lod();
	const int walls = m.add_material("FF_ST_OP", "shed.tga");
	const int bulb = m.add_material("FF_ST_AB", "bulb.tga");
	const Box hut = box(-2.0, -3.0, 0.0, 2.0, 3.0, 3.0);
	const Box step = box(2.0, -0.5, 0.0, 2.4, 0.5, 0.3);
	const int part = m.add_part(lod, 0, Vec3{});
	m.add_box(lod, part, walls, hut);
	m.add_box(lod, part, bulb, box(-0.1, -0.1, 2.4, 0.1, 0.1, 2.6), true);
	m.add_panm(lod, 0, 0);
	m.add_control_register("FLICKER");
	m.add_user_point("ground", Vec3{}, kUp, 0, kUserPointGameplay);
	m.add_light(Vec3{0.0, 0.0, 1.25}, 0.0, 3.0, 24, 0, kWarm, kBlack);
	const int cobj = m.add_cobj(0);
	m.add_face_box(cobj, hut);
	m.add_volume(cobj, 1, 0, hut);
	m.add_volume(cobj, 1, 0, step);
	return m;
}

Model make_house() {
	Model m;
	m.name = "house";
	const int lod = m.add_lod();
	const int wall = m.add_material("FF_ST_OP", "wall.tga");
	const int roof = m.add_material("FF_ST_OP", "roof.tga");
	const int wood = m.add_material("FF_ST_OP", "wood.tga");
	const Box walls = box(-4.0, -5.0, 0.0, 4.0, 5.0, 4.0);
	const Box roof_slab = box(-4.5, -5.5, 4.0, 4.5, 5.5, 4.4);
	const Box chimney = box(2.0, 3.0, 4.4, 2.6, 3.6, 5.4);
	const int part = m.add_part(lod, 0, Vec3{});
	m.add_box(lod, part, wall, walls);
	m.add_box(lod, part, roof, roof_slab);
	m.add_box(lod, part, wood, chimney);
	m.add_panm(lod, 0, 0);
	m.add_user_point("ground", Vec3{}, kUp, 0, kUserPointGameplay);
	const int cobj = m.add_cobj(0);
	m.add_face_box(cobj, walls);
	m.add_volume(cobj, 1, 0, walls);
	m.add_volume(cobj, 1, 0, roof_slab);
	m.add_volume(cobj, 1, 0, chimney);
	m.add_prism8(cobj, 1, 0, 6.0, 0.0, 0.6, 0.0, 2.5); // the water tank beside the house
	return m;
}

Model make_bird() {
	Model m;
	m.name = "bird";
	m.skinned = true;
	const int lod = m.add_lod();
	const int mat = m.add_material("VS_SKBASIC", "bird.tga");
	// The Bird1 hierarchy: body; wing roots; wing mids; wing tips; head; tail.
	const int parents[9] = {0, 0, 0, 1, 2, 3, 4, 0, 0};
	const Vec3 pivots[9] = {{0, 0, 0}, {0.05, 0.1, 0.05}, {0.05, -0.1, 0.05}, {0.05, 0.35, 0.05},
		{0.05, -0.35, 0.05}, {0.05, 0.6, 0.05}, {0.05, -0.6, 0.05}, {0.3, 0.0, 0.08}, {-0.3, 0.0, 0.02}};
	const Box boxes[9] = {box(-0.25, -0.1, 0.0, 0.25, 0.1, 0.12), box(0.0, 0.1, 0.04, 0.1, 0.35, 0.06),
		box(0.0, -0.35, 0.04, 0.1, -0.1, 0.06), box(0.0, 0.35, 0.04, 0.1, 0.6, 0.06),
		box(0.0, -0.6, 0.04, 0.1, -0.35, 0.06), box(0.0, 0.6, 0.04, 0.1, 0.8, 0.06),
		box(0.0, -0.8, 0.04, 0.1, -0.6, 0.06), box(0.25, -0.04, 0.05, 0.4, 0.04, 0.12),
		box(-0.45, -0.05, 0.02, -0.25, 0.05, 0.05)};
	for (int i = 0; i < 9; ++i) {
		const int part = m.add_part(lod, parents[i], pivots[i]);
		m.add_box(lod, part, mat, boxes[i], false, part);
		m.add_panm(lod, part, parents[i]);
		const int cobj = m.add_cobj(parents[i], pivots[i]);
		const Box &b = boxes[i];
		const Vec3 quad[4] = {{b.min.x, b.min.y, b.max.z}, {b.max.x, b.min.y, b.max.z},
			{b.max.x, b.max.y, b.max.z}, {b.min.x, b.max.y, b.max.z}};
		m.add_face_quad(cobj, quad);
	}
	return m;
}

Model make_person() {
	Model m;
	m.name = "person";
	m.skinned = true;
	const int lod = m.add_lod();
	const int mat = m.add_material("VS_SKBASIC", "person.tga");
	// The retail person rig order (CharModel): pelvis; spine; chest; clavicles;
	// upper arms; thighs; forearms; shins; neck; head; hands; feet. Origin at
	// the pelvis, feet a little over a unit below it (person_rig.h, which the
	// rig19 clip set shares).
	const int *parents = person_rig::kParents;
	Vec3 pivots[person_rig::kBones];
	for (int i = 0; i < person_rig::kBones; ++i)
		pivots[i] = Vec3{person_rig::kPivots[i][0], person_rig::kPivots[i][1], person_rig::kPivots[i][2]};
	// Each bone's box spans from its pivot toward its child (or a stub).
	const Box boxes[19] = {box(-0.125, -0.125, -0.0625, 0.125, 0.125, 0.125),
		box(-0.1, -0.1, 0.125, 0.1, 0.1, 0.375), box(-0.125, -0.15, 0.375, 0.125, 0.15, 0.625),
		box(-0.05, -0.25, 0.575, 0.05, -0.0625, 0.675), box(-0.05, 0.0625, 0.575, 0.05, 0.25, 0.675),
		box(-0.05, -0.3, 0.4375, 0.05, -0.2, 0.625), box(-0.05, 0.2, 0.4375, 0.05, 0.3, 0.625),
		box(-0.07, -0.2, -0.4375, 0.07, -0.05, 0.0), box(-0.07, 0.05, -0.4375, 0.07, 0.2, 0.0),
		box(-0.04, -0.48, 0.3125, 0.04, -0.4, 0.4375), box(-0.04, 0.4, 0.3125, 0.04, 0.48, 0.4375),
		box(-0.06, -0.18, -0.875, 0.06, -0.07, -0.4375), box(-0.06, 0.07, -0.875, 0.06, 0.18, -0.4375),
		box(-0.04, -0.04, 0.6875, 0.04, 0.04, 0.75), box(-0.09, -0.09, 0.75, 0.15, 0.09, 0.95),
		box(-0.03, 0.66, 0.25, 0.09, 0.72, 0.3125), box(-0.03, -0.72, 0.25, 0.09, -0.66, 0.3125),
		box(-0.08, -0.17, -1.0, 0.17, -0.08, -0.875), box(-0.08, 0.08, -1.0, 0.17, 0.17, -0.875)};
	for (int i = 0; i < 19; ++i) {
		const int part = m.add_part(lod, parents[i], pivots[i]);
		m.add_box(lod, part, mat, boxes[i], false, part);
		m.add_panm(lod, part, parents[i]);
	}
	m.add_user_point("bullet", Vec3{0.3, -0.6875, 0.3125}, kForward, 16, kUserPointEffect);
	m.add_user_point("MFlash01", Vec3{0.35, -0.6875, 0.3125}, kForward, 16, kUserPointEffect);
	m.add_user_point("LOOK", Vec3{0.125, 0.0, 0.85}, kForward, 14, kUserPointEffect);
	// COBJ 0 carries the body's face box; every other bone is a sphere section.
	const int body = m.add_cobj(0);
	m.add_face_box(body, box(-0.125, -0.15, -0.0625, 0.125, 0.15, 0.625));
	for (int i = 1; i < 19; ++i) {
		const Box &b = boxes[i];
		Vec3 center{(b.min.x + b.max.x) * 0.5, (b.min.y + b.max.y) * 0.5, (b.min.z + b.max.z) * 0.5};
		double radius = 0.125;
		if (i == 14) {
			center = Vec3{0.0625, 0.0, 0.8125};
			radius = 0.15625;
		}
		m.add_sphere_cobj(parents[i], pivots[i], center, radius);
	}
	return m;
}

// The pump jack: shared by both LODs (LOD1 drops the base slab's post).
void pump_parts(Model &m, int lod, bool detailed) {
	const int mat = 0;
	const int post_mat = 1;
	const int base = m.add_part(lod, 0, Vec3{});
	m.add_box(lod, base, mat, box(-1.0, -1.0, 0.0, 1.0, 1.0, 0.5));
	if (detailed) m.add_box(lod, base, post_mat, box(-0.2, -0.2, 0.5, 0.2, 0.2, 3.0));
	const int beam = m.add_part(lod, base, Vec3{0.0, 0.0, 3.0});
	m.add_box(lod, beam, mat, box(-2.0, -0.15, 2.85, 2.0, 0.15, 3.15));
	const int head = m.add_part(lod, beam, Vec3{2.0, 0.0, 3.0});
	m.add_box(lod, head, mat, box(2.0, -0.3, 2.7, 2.6, 0.3, 3.3));
	const int rod = m.add_part(lod, head, Vec3{2.3, 0.0, 2.7});
	m.add_box(lod, rod, mat, box(2.2, -0.1, 0.5, 2.4, 0.1, 2.7));
	const int weight = m.add_part(lod, base, Vec3{-2.0, 0.0, 3.0});
	m.add_box(lod, weight, mat, box(-2.6, -0.4, 2.6, -2.0, 0.4, 3.4));
}

Model make_pump() {
	Model m;
	m.name = "pump";
	m.add_material("FF_ST_OP", "pump.tga");
	const int post = m.add_material("FF_MT_OP", "pump.tga");
	m.add_detail_texture(post, "pump_o.tga");
	const int lod0 = m.add_lod(96);
	pump_parts(m, lod0, true);
	const int lod1 = m.add_lod(0);
	pump_parts(m, lod1, false);
	// LOD0: the beam rocks on a free-running sine (rotation z, retail's
	// control 50); every other row inert. LOD1: every row inert.
	const int parents[5] = {0, 0, 1, 2, 0};
	for (int lod = 0; lod < 2; ++lod) {
		for (int part = 0; part < 5; ++part) {
			ThreediPartAnimation &row = m.add_panm(lod, part, parents[part]);
			if (lod == 0 && part == 1) {
				row.flags = threedi_panm_pack_flags(0, 2, 0, 0);
				row.rotation_z = track(THREEDI_PANM_STYLE_SINE_WAVE, 0, 128, 682, -682);
			}
			if (lod == 0 && part == 0) row.flags = threedi_panm_pack_flags(0, 0, 0, THREEDI_TRANS_Z);
		}
	}
	m.add_user_point("Noname", Vec3{}, kForward, 0, kUserPointGameplay);
	m.add_user_point("Sound", Vec3{0.0, 0.0, 2.5}, kForward, 0, kUserPointGameplay);
	m.add_user_point("ground", Vec3{}, kUp, 0, kUserPointGameplay);
	const Box slab = box(-1.0, -1.0, 0.0, 1.0, 1.0, 0.5);
	const int base = m.add_cobj(0);
	m.add_face_box(base, slab);
	m.add_volume(base, 1, 0, slab);
	m.add_volume(base, 1, 0, box(-0.2, -0.2, 0.5, 0.2, 0.2, 3.0));
	const int beam = m.add_cobj(0, Vec3{0.0, 0.0, 3.0});
	m.add_face_box(beam, box(-2.0, -0.15, 2.85, 2.0, 0.15, 3.15));
	const int head = m.add_cobj(1, Vec3{2.0, 0.0, 3.0});
	m.add_face_box(head, box(2.0, -0.3, 2.7, 2.6, 0.3, 3.3));
	const int rod = m.add_cobj(2, Vec3{2.3, 0.0, 2.7});
	m.add_face_box(rod, box(2.2, -0.1, 0.5, 2.4, 0.1, 2.7));
	const int weight = m.add_cobj(0, Vec3{-2.0, 0.0, 3.0});
	m.add_face_box(weight, box(-2.6, -0.4, 2.6, -2.0, 0.4, 3.4));
	return m;
}

Model make_armory() {
	Model m;
	m.name = "armory";
	const int lod = m.add_lod(0, "bldg");
	const int exterior = m.add_material("FF_MT_OP", "armry.tga");
	m.add_detail_texture(exterior, "armry_o.tga");
	const int interior = m.add_material("FF_ST_OP", "armryin.tga");
	const int lamp = m.add_material("FF_ST_OP", "bulb.tga");
	const int bulb = m.add_material("FF_ST_OP_LUM", "bulb.tga");
	m.materials[bulb].emissive_type = THREEDI_EMISSIVE_FULL;
	m.set_rgb_gen(bulb, THREEDI_PANM_STYLE_CONTROL_REGISTER, 0, 0.0, kBulb, kBulbEnd);
	const int glass = m.add_material("FFP_GLASS", "");
	m.materials[glass].is_glass = 1;
	for (int k = 0; k < 3; ++k) m.materials[glass].reflect_color[k] = byte_unit(128);
	const int pane = m.add_material("FF_ST_AB", "pane.tga");
	// The retail floor plan (entity-local, mission axes): east room x 1.1..5.7,
	// west room x -6.8..-0.6, middle x -0.5..1.1, all y -3.2..2.8 (west to 3.7).
	const Box shell = box(-7.0, -3.5, 0.0, 6.0, 4.0, 3.0);
	const Box east = box(1.1, -3.2, 0.0, 5.7, 2.8, 3.0);
	const Box west = box(-6.8, -3.2, 0.0, -0.6, 3.7, 3.0);
	const Box middle = box(-0.5, -3.2, 0.0, 1.1, 2.8, 3.0);
	const int p_exterior = m.add_part(lod, 0, Vec3{});
	m.add_box(lod, p_exterior, exterior, shell);
	m.add_box(lod, p_exterior, exterior, box(-7.2, -3.7, 3.0, 6.2, 4.2, 3.3)); // the roof slab
	const int p_east = m.add_part(lod, 0, Vec3{});
	m.add_box(lod, p_east, interior, east);
	m.add_box(lod, p_east, bulb, box(3.3, -0.4, 2.7, 3.5, -0.2, 2.9));
	m.add_box(lod, p_east, glass, box(2.1, -3.45, 1.0, 3.1, -3.4, 2.5), true); // the south window
	const int p_west = m.add_part(lod, 0, Vec3{});
	m.add_box(lod, p_west, interior, west);
	m.add_box(lod, p_west, pane, box(-4.0, 3.6, 1.0, -3.0, 3.65, 2.5), true); // a north pane
	const int p_middle = m.add_part(lod, 0, Vec3{});
	m.add_box(lod, p_middle, interior, middle);
	m.add_box(lod, p_middle, lamp, box(0.2, -0.3, 2.7, 0.4, -0.1, 2.9));
	for (int part = 0; part < 4; ++part) m.add_panm(lod, part, 0);
	m.add_control_register("FLICKER");
	m.add_user_point("Armory", Vec3{-3.7, 0.3, 0.9}, kUp, 0, kUserPointGameplay);
	m.add_user_point("Ground", Vec3{}, kUp, 0, kUserPointEffect);
	m.add_light(Vec3{3.4, -0.3, 2.8}, 0.0, 4.0, 24, 1, kWarm, kBlack);
	m.add_light(Vec3{-3.7, 0.3, 2.8}, 0.0, 4.0, 55, 2, kWarm, kWarm, 0, 179, 76);
	// Collision: COBJ 0 = the shell (four walls, roof, the armory trigger, the
	// vehicle hull, a ladder on the east wall); COBJ 1..3 = one blink box per
	// room (east 0x2E, west 0x28, middle 0x2E).
	const int c_shell = m.add_cobj(0);
	m.add_face_box(c_shell, shell);
	m.add_volume(c_shell, 1, 0, box(-7.0, -3.5, 0.0, -6.8, 4.0, 3.0));
	m.add_volume(c_shell, 1, 0, box(5.7, -3.5, 0.0, 6.0, 4.0, 3.0));
	m.add_volume(c_shell, 1, 0, box(-7.0, 3.7, 0.0, 6.0, 4.0, 3.0));
	m.add_volume(c_shell, 1, 0, box(-7.0, -3.5, 0.0, 6.0, -3.2, 3.0));
	m.add_volume(c_shell, 1, 0, box(-7.2, -3.7, 3.0, 6.2, 4.2, 3.3));
	m.add_volume(c_shell, 6, 0, box(1.5, -3.0, 0.0, 5.5, 2.5, 2.5));
	m.add_volume(c_shell, 7, 0, box(-7.0, -3.5, 0.0, 6.0, 4.0, 3.3));
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
	m.add_volume(c_east, 8, 0x2E, box(1.05, -3.2, 0.0, 5.7, 2.8, 3.0));
	const int c_west = m.add_cobj(0);
	{
		const Vec3 floor[4] = {{-6.8, -3.2, 0.0}, {-0.6, -3.2, 0.0}, {-0.6, 3.7, 0.0}, {-6.8, 3.7, 0.0}};
		m.add_face_quad(c_west, floor);
	}
	m.add_volume(c_west, 8, 0x28, west);
	const int c_middle = m.add_cobj(0);
	{
		const Vec3 floor[4] = {{-0.5, -3.2, 0.0}, {1.1, -3.2, 0.0}, {1.1, 2.8, 0.0}, {-0.5, 2.8, 0.0}};
		m.add_face_quad(c_middle, floor);
	}
	m.add_volume(c_middle, 8, 0x2E, box(-0.5, -3.2, 0.0, 1.15, 2.8, 3.0));
	// Occlusion (the retail record order): the whole-building open slot, four
	// wall occluders (the south one stops short of the window), the east
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

Model make_mount() {
	Model m;
	m.name = "mount";
	const int lod = m.add_lod();
	const int body = m.add_material("FF_ST_OP", "mount.tga");
	const int cutout = m.add_material("FF_ST_OP", "mount.tga");
	m.materials[cutout].material_flags = THREEDI_MATERIAL_FLAG_ALPHA_TEST;
	m.materials[cutout].alpha_test_value_byte = 32;
	const int glow = m.add_material("FF_ST_AD_LUM", "glow.tga");
	m.materials[glow].emissive_type = THREEDI_EMISSIVE_FULL;
	m.set_rgb_gen(glow, 114, 0, 0.5, kBlack, kGlow);
	const Box plate = box(-0.4, -0.4, 0.0, 0.4, 0.4, 0.1);
	const Box post = box(-0.1, -0.1, 0.1, 0.1, 0.1, 0.3);
	const Box cradle = box(-0.3, -0.15, 0.3, 0.1, 0.15, 0.45);
	const Box sight = box(-0.29, -0.05, 0.45, -0.15, 0.05, 0.5);
	const Box gun = box(-0.7, -0.1, 0.4, 1.6, 0.1, 0.55);
	const Box heat = box(0.2, -0.06, 0.42, 1.2, 0.06, 0.53);
	const int p_base = m.add_part(lod, 0, Vec3{});
	m.add_box(lod, p_base, body, plate);
	m.add_box(lod, p_base, body, post);
	const int p_cradle = m.add_part(lod, 0, Vec3{0.0, 0.0, 0.3});
	m.add_box(lod, p_cradle, body, cradle);
	m.add_box(lod, p_cradle, cutout, sight);
	const int p_gun = m.add_part(lod, 0, Vec3{0.0, 0.0, 0.5});
	m.add_box(lod, p_gun, body, gun);
	m.add_box(lod, p_gun, glow, heat, true);
	m.add_control_register("HEAT_GLOW");
	m.add_control_register("EWEAP_GUNYAW");
	m.add_control_register("EWEAP_GUNPITCH");
	// The retail B50Cal articulation: the cradle yaws on register 1, the gun
	// yaws on 1 and pitches on 2 (rotation x = the authored yaw axis), the gun's
	// row in MTRX frame 1, a half turn about the model's y (diag(-1, 1, -1)),
	// which turns its pitch the shipped way (B50Cal.3di's and H50Cal.3di's gun
	// rows and frame, witnessed 2026-10-19).
	ThreediMatrix4x4 half_turn{};
	half_turn.m[0] = -1.0f;
	half_turn.m[5] = 1.0f;
	half_turn.m[10] = -1.0f;
	half_turn.m[15] = 1.0f;
	m.frames.push_back(half_turn);
	ThreediPartAnimation &r0 = m.add_panm(lod, 0, 0, threedi_panm_pack_flags(0, 2, 1, 0));
	r0.rotation_x = track(0, 0, 0, 0, 16384);
	ThreediPartAnimation &r1 = m.add_panm(lod, 1, 0, threedi_panm_pack_flags(0, 2, 1, 0));
	r1.rotation_x = track(THREEDI_PANM_STYLE_CONTROL_REGISTER, 1, 0, 0, 16384);
	r1.rotation_y = track(0, 0, 0, 16384, 0);
	ThreediPartAnimation &r2 = m.add_panm(lod, 2, 0, threedi_panm_pack_flags(0, 2, 1, 0));
	r2.rotation_x = track(THREEDI_PANM_STYLE_CONTROL_REGISTER, 1, 0, 0, 16384);
	r2.rotation_y = track(THREEDI_PANM_STYLE_CONTROL_REGISTER, 2, 0, 16384, 0);
	r2.matrix_index = 1;
	m.add_user_point("BCasing", Vec3{-0.3, -0.07, 0.48}, Vec3{0.0, -1.0, 0.0}, 2, kUserPointEffect);
	m.add_user_point("Bullet", Vec3{1.7, 0.0, 0.48}, kForward, 2, kUserPointEffect);
	m.add_user_point("Camera", Vec3{-0.85, 0.0, 0.6}, kForward, 2, kUserPointGameplay);
	m.add_user_point("heat", Vec3{0.4, 0.0, 0.48}, kUp, 2, kUserPointEffect);
	m.add_user_point("MFlash01", Vec3{1.58, 0.0, 0.48}, kForward, 2, kUserPointEffect);
	m.add_user_point("Usegun", Vec3{-1.13, 0.0, 0.0}, kUp, 0, kUserPointGameplay);
	const int c_base = m.add_cobj(0);
	m.add_face_box(c_base, plate);
	m.add_volume(c_base, 1, 0, plate);
	const int c_cradle = m.add_cobj(0, Vec3{0.0, 0.0, 0.3});
	m.add_face_box(c_cradle, cradle);
	m.add_volume(c_cradle, 1, 0, cradle);
	const int c_gun = m.add_cobj(0, Vec3{0.0, 0.0, 0.5});
	m.add_face_box(c_gun, gun);
	m.add_volume(c_gun, 1, 0, gun);
	return m;
}

Model make_carrier() {
	Model m;
	m.name = "carrier";
	const int lod = m.add_lod();
	const int paint = m.add_material("FF_ST_OP", "carrier.tga");
	const int cabin_mat = m.add_material("FF_ST_OP", "cabin.tga");
	const Box wheels = box(-2.5, -1.2, 0.0, 2.5, 1.2, 0.6);
	const Box body = box(-2.5, -1.0, 0.6, 2.5, 1.0, 1.4);
	const Box cabin = box(1.0, -1.0, 1.4, 2.5, 1.0, 2.2);
	const int p_hull = m.add_part(lod, 0, Vec3{});
	m.add_box(lod, p_hull, paint, wheels);
	m.add_box(lod, p_hull, paint, body);
	const int p_cabin = m.add_part(lod, 0, Vec3{2.0, 0.0, 0.5});
	m.add_box(lod, p_cabin, cabin_mat, cabin);
	m.add_control_register("VEHICLE_STEERING");
	m.add_control_register("VEHICLE_WHEELS");
	m.add_control_register("VEHICLE_TIRE00");
	m.add_control_register("VEHICLE_TIRE01");
	m.add_control_register("VEHICLE_TIRE02");
	m.add_control_register("VEHICLE_TIRE03");
	m.add_panm(lod, 0, 0);
	ThreediPartAnimation &row = m.add_panm(lod, 1, 0, threedi_panm_pack_flags(0, 2, 1, THREEDI_TRANS_Y));
	row.rotation_x = track(THREEDI_PANM_STYLE_CONTROL_REGISTER, 0, 0, 0, 16338);
	row.rotation_y = track(THREEDI_PANM_STYLE_CONTROL_REGISTER, 1, 0, 0, 16338);
	row.translation = track(THREEDI_PANM_STYLE_CONTROL_REGISTER, 2, 0, 0, 256);
	// The dsuv1 user-point order: the controller first, then the eweap
	// anchor, the passengers, the exhaust effect, the ground point.
	m.add_user_point("ctrlx13", Vec3{0.08, 0.63, 1.09}, kForward, 0, kUserPointGameplay);
	m.add_user_point("ewep01", Vec3{-0.78, -0.49, 1.39}, kForward, 0, kUserPointGameplay);
	m.add_user_point("sitex00d", Vec3{-2.52, 0.0, 0.9}, kBackward, 0, kUserPointGameplay);
	m.add_user_point("sitex08c", Vec3{-1.7, 0.59, 1.21}, kBackward, 0, kUserPointGameplay);
	m.add_user_point("FX01", Vec3{-2.69, 0.0, 0.18}, kBackward, 0, kUserPointEffect);
	m.add_user_point("ground", Vec3{}, kUp, 0, kUserPointGameplay);
	m.add_user_point("sitex06b", Vec3{-0.81, 0.46, 1.09}, kBackward, 0, kUserPointGameplay);
	m.add_user_point("sitex12a", Vec3{0.08, -0.58, 1.09}, kForward, 0, kUserPointGameplay);
	const int c_hull = m.add_cobj(0);
	m.add_face_box(c_hull, body);
	m.add_volume(c_hull, 1, 0, wheels);
	m.add_volume(c_hull, 1, 0, body);
	const int c_cabin = m.add_cobj(0, Vec3{2.0, 0.0, 0.5});
	m.add_face_box(c_cabin, cabin);
	m.add_volume(c_cabin, 1, 0, cabin);
	return m;
}

Model make_tank() {
	Model m;
	m.name = "tank";
	const int lod = m.add_lod();
	const int armor = m.add_material("FF_ST_OP", "tank.tga");
	const int tread = m.add_material("FF_ST_OP", "tread.tga");
	const Box hull = box(-3.0, -1.5, 0.3, 3.0, 1.5, 1.3);
	const Box track_l = box(-3.0, 1.5, 0.0, 3.0, 1.9, 1.0);
	const Box track_r = box(-3.0, -1.9, 0.0, 3.0, -1.5, 1.0);
	const Box turret = box(-1.0, -1.0, 1.3, 1.2, 1.0, 2.0);
	const Box barrel = box(1.2, -0.1, 1.55, 4.0, 0.1, 1.75);
	const int p_hull = m.add_part(lod, 0, Vec3{});
	m.add_box(lod, p_hull, armor, hull);
	m.add_box(lod, p_hull, tread, track_l);
	m.add_box(lod, p_hull, tread, track_r);
	const int p_turret = m.add_part(lod, 0, Vec3{0.0, 0.0, 1.3});
	m.add_box(lod, p_turret, armor, turret);
	m.add_box(lod, p_turret, armor, barrel);
	m.add_control_register("VEHICLE_GUNYAW");
	m.add_control_register("VEHICLE_WHEELS00");
	m.add_control_register("VEHICLE_TIRE00");
	m.add_panm(lod, 0, 0);
	ThreediPartAnimation &row = m.add_panm(lod, 1, 0, threedi_panm_pack_flags(0, 2, 0, 0));
	row.rotation_x = track(THREEDI_PANM_STYLE_CONTROL_REGISTER, 0, 0, 0, 16384);
	m.add_user_point("ctrlx25", Vec3{1.5, 0.0, 0.7}, kForward, 0, kUserPointGameplay);
	m.add_user_point("ewep01", Vec3{0.0, 0.0, 1.65}, kForward, p_turret, kUserPointGameplay);
	m.add_user_point("fx00", Vec3{-3.7, 0.9, 0.6}, kBackward, 0, kUserPointEffect);
	m.add_user_point("ground", Vec3{}, kUp, 0, kUserPointGameplay);
	const int c_hull = m.add_cobj(0);
	m.add_face_box(c_hull, hull);
	m.add_volume(c_hull, 1, 0, hull);
	m.add_volume(c_hull, 19, 0, track_l);
	m.add_volume(c_hull, 19, 0, track_r);
	const int c_turret = m.add_cobj(0, Vec3{0.0, 0.0, 1.3});
	m.add_face_box(c_turret, turret);
	m.add_volume(c_turret, 1, 0, turret);
	return m;
}

Model make_vm40() {
	Model m;
	m.name = "vm40";
	m.skinned = true;
	const int lod = m.add_lod();
	const int mat = m.add_material("VS_SKBASIC", "vm40.tga");
	// The arms' material: two camouflage diffuse frames a CTRL register picks
	// (ArmsG's TEX_CAMO1 rows: animated texture rows, a register-driven
	// texture animation).
	const int arms = m.add_material("VS_SKBASIC", "arms_wd.tga");
	{
		ThreediMaterial &a = m.materials[arms];
		a.textures[0].flags = THREEDI_TEX_FLAG_ANIMATED;
		ThreediMaterialTexture &t = a.textures[a.texture_count++];
		std::snprintf(t.name, sizeof(t.name), "%s", "arms_ds.tga");
		t.slot = THREEDI_TEX_SLOT_DIFFUSE;
		t.type = THREEDI_TEX_TYPE_DIFFUSE;
		t.flags = THREEDI_TEX_FLAG_ANIMATED;
		t.frame = 1;
		a.animation.num_frames = 2;
		a.animation.animation_type = 1;  // control register driven
		a.animation.cycle_frame_time = 0; // register 0
	}
	m.add_control_register("TEX_CAMO1");
	const std::vector<vm40_rig::Bone> rig = vm40_rig::bones();
	for (size_t i = 0; i < rig.size(); ++i) {
		const vm40_rig::Bone &b = rig[i];
		const int parent = b.parent < 0 ? 0 : b.parent;
		const Vec3 pivot{b.pivot[0], b.pivot[1], b.pivot[2]};
		const int part = m.add_part(lod, parent, pivot);
		const double h = 1.0 / 64.0;
		const bool arm = i >= 1 && i < static_cast<size_t>(vm40_rig::kReceiver);
		m.add_box(lod, part, arm ? arms : mat, box(pivot.x - h, pivot.y - h, pivot.z - h, pivot.x + h, pivot.y + h, pivot.z + h), false,
				part);
		m.add_panm(lod, part, parent);
		const int cobj = m.add_cobj(parent, pivot);
		// A finger's quad (bones 4..18, 22..36) spans a quarter of the 8.8 grid's step.
		const bool finger = (i >= 4 && i <= 18) || (i >= 22 && i <= 36);
		const double q = finger ? 1.0 / 1024.0 : h;
		const Vec3 quad[4] = {{pivot.x - q, pivot.y - q, pivot.z}, {pivot.x + q, pivot.y - q, pivot.z},
			{pivot.x + q, pivot.y + q, pivot.z}, {pivot.x - q, pivot.y + q, pivot.z}};
		m.add_face_quad(cobj, quad);
	}
	const vm40_rig::Bone &receiver = rig[vm40_rig::kReceiver];
	m.add_user_point("bullet", Vec3{receiver.pivot[0] + 0.375, receiver.pivot[1], receiver.pivot[2]}, kForward,
			vm40_rig::kReceiver, kUserPointEffect);
	m.add_user_point("MFlash01", Vec3{receiver.pivot[0] + 0.40625, receiver.pivot[1], receiver.pivot[2]}, kForward,
			vm40_rig::kReceiver, kUserPointEffect);
	return m;
}

Model make_heli() {
	Model m;
	m.name = "heli";
	m.tangents = true;
	const int paint = m.add_material("VS_PHONGT", "heli.tga");
	{
		// VS_PHONGT's normal map row (slot 3, type 4), as the stock models carry it.
		ThreediMaterial &mat = m.materials[paint];
		ThreediMaterialTexture &t = mat.textures[mat.texture_count++];
		std::snprintf(t.name, sizeof(t.name), "%s", "heli_n.mdt");
		t.slot = THREEDI_TEX_SLOT_NORMAL;
		t.type = THREEDI_TEX_TYPE_NORMAL_MDT;
	}
	const int blur = m.add_material("FF_ST_AB", "blur.tga");
	m.add_control_register("HELO_ROTOR");
	m.add_control_register("HELO_TAILROTOR");
	// The tail rotor's frame: its axes turned a quarter about the model's z
	// (row-major, p' = p * M).
	ThreediMatrix4x4 tail{};
	tail.m[1] = 1.0f;
	tail.m[4] = -1.0f;
	tail.m[10] = 1.0f;
	tail.m[15] = 1.0f;
	m.frames.push_back(tail);
	const Box hull = box(-6.0, -1.0, 0.0, 4.0, 1.0, 2.5);
	const Vec3 mast{0.0, 0.0, 3.0}, tail_hub{-8.0, 0.25, 2.0};
	for (int l = 0; l < 3; ++l) {
		const int lod = m.add_lod(l == 0 ? 64 : 0);
		if (l == 2) continue; // the last LOD holds no part
		const int body = m.add_part(lod, 0, Vec3{});
		m.add_box(lod, body, paint, hull);
		const int rotor = m.add_part(lod, 0, mast);
		m.add_box(lod, rotor, paint, box(-0.25, -0.25, 2.75, 0.25, 0.25, 3.1));
		const int rotor_blur = m.add_part(lod, 0, mast);
		m.add_box(lod, rotor_blur, blur, box(-7.0, -7.0, 3.1, 7.0, 7.0, 3.15), true);
		const int tail_rotor = m.add_part(lod, 0, tail_hub);
		m.add_box(lod, tail_rotor, paint, box(-8.1, 0.15, 1.9, -7.9, 0.35, 2.1));
		const int tail_blur = m.add_part(lod, 0, tail_hub);
		m.add_box(lod, tail_blur, blur, box(-9.0, 0.35, 1.0, -7.0, 0.4, 3.0), true);
		m.add_panm(lod, body, 0);
		for (int part : {rotor, rotor_blur}) {
			ThreediPartAnimation &row = m.add_panm(lod, part, 0, threedi_panm_pack_flags(0, 2, 0, 0));
			row.rotation_x = track(THREEDI_PANM_STYLE_CONTROL_REGISTER, 0, 0, 0, 16338);
		}
		for (int part : {tail_rotor, tail_blur}) {
			ThreediPartAnimation &row = m.add_panm(lod, part, 0, threedi_panm_pack_flags(0, 2, 0, 0));
			row.rotation_y = track(THREEDI_PANM_STYLE_CONTROL_REGISTER, 1, 0, 0, 16338);
			row.matrix_index = 1;
		}
	}
	m.add_user_point("ground", Vec3{}, kUp, 0, kUserPointGameplay);
	const int c_hull = m.add_cobj(0);
	m.add_face_box(c_hull, hull);
	m.add_volume(c_hull, 1, 0, hull);
	return m;
}

// The person with every limb on a two-bone strip table, `rest` of each vertex's
// weight on slot 2, past the table (see the set's list above).
Model make_person_two_bone(const char *name, float rest) {
	Model m = make_person();
	m.name = name;
	m.tangents = true;
	for (ThreediBuildPart &part : m.lods[0].parts)
		for (ThreediBuildStrip &strip : part.strips) {
			const int bone = strip.bone;
			if (bone <= 0) continue;
			strip.bone = -1;
			strip.bone_table = {static_cast<uint8_t>(bone), static_cast<uint8_t>(person_rig::kParents[bone])};
			for (ThreediVertex &v : strip.vertices) {
				// Three quarters on the bone, the rest but `rest` on its parent,
				// `rest` (the remainder) on slot 2, past the table.
				v.bone_weights[0] = 0.75f;
				v.bone_weights[1] = 0.25f - rest;
				v.bone_weights[2] = 0.0f;
				v.bone_indices[0] = 0;
				v.bone_indices[1] = 1;
				v.bone_indices[2] = 0;
				v.bone_indices[3] = 2;
				v.is_skinned = 1;
			}
		}
	// CXLT: one row a section, a sixteenth above its offset.
	m.translations_given = true;
	for (const opennova::threedi::ThreediBuildCollisionObject &c : m.collision)
		m.translations.push_back(Vec3{c.offset.x, c.offset.y, c.offset.z + 1.0 / 16.0});
	return m;
}

// ---------------------------------------------------------------------------
// The variant recipes (fixtures/README.md "threedi/synth"): one authored edit
// each, spelled with the same runtime vocabulary the retired ObjectData
// editing surface used.
using Edit = std::function<void(Model &)>;

struct Recipe {
	const char *file;
	Model (*base)();
	Edit edit;
	// The directory under fixtures/ the file lands in.
	const char *dir = "threedi/synth";
	// A form the shipped models carry that no author states: an edit of the
	// model as the file holds it, between the mint and the writer.
	std::function<void(Threedi3di3 &)> raw;
};

// The quiet NaN word the shipped MTRX rows carry.
float nan_word() {
	const uint32_t bits = 0x7FC00000u;
	float f;
	std::memcpy(&f, &bits, sizeof(f));
	return f;
}

void rename_register(Model &m, int index, const char *name) { m.control_registers[index] = name; }

void delete_lod0_rows(Model &m) { m.lods[0].panm.clear(); }

// The first LOD0 track with control 113 on register `param` (the mount's yaw).
ThreediTransform *controlled_track(Model &m, uint8_t param) {
	for (ThreediPartAnimation &row : m.lods[0].panm) {
		ThreediTransform *tracks[7] = {&row.rotation_x, &row.rotation_y, &row.rotation_z, &row.scale_x, &row.scale_y, &row.scale_z, &row.translation};
		for (ThreediTransform *t : tracks)
			if (t->control == THREEDI_PANM_STYLE_CONTROL_REGISTER && t->control_param == param) return t;
	}
	return nullptr;
}

void set_light_position_model(ThreediLight &l, float x, float y, float z) {
	l.offset[0] = x;
	l.offset[1] = y;
	l.offset[2] = z;
}

Model make_liveness_case(uint32_t flags, const std::vector<int> &live_tracks) {
	Model m = make_shed();
	delete_lod0_rows(m);
	ThreediPartAnimation row = inert_panm(0, 0);
	row.flags = flags;
	ThreediTransform *tracks[7] = {&row.rotation_x, &row.rotation_y, &row.rotation_z, &row.scale_x, &row.scale_y, &row.scale_z, &row.translation};
	for (int index : live_tracks) tracks[index]->control = THREEDI_PANM_STYLE_SLIDE;
	m.lods[0].panm.push_back(row);
	return m;
}

const std::vector<Recipe> &recipes() {
	static const std::vector<Recipe> kRecipes = {
		{"crate", make_crate, nullptr},
		{"gun", make_gun, nullptr},
		{"shed", make_shed, nullptr},
		{"house", make_house, nullptr},
		{"bird", make_bird, nullptr},
		{"person", make_person, nullptr},
		{"pump", make_pump, nullptr},
		// Authored projected-sphere LOD witnesses: identical geometry and CMDL,
		// distinct LOD0 thresholds in retail's RMDL form (integer pixels in the
		// level's own slot: LOD0 draws above 20/80 px, the zero LOD1 row below),
		// written through the same parity writer.
		{"pump_lod20", make_pump, [](Model &m) { m.lods[0].threshold = 20; }},
		{"pump_lod80", make_pump, [](Model &m) { m.lods[0].threshold = 80; }},
        {"pump_minefield", make_pump, [](Model &m) {
            m.add_user_point("ignored", Vec3{}, kUp, 0, kUserPointGameplay);
            const char *names[] = {"SMLMARKED", "small", "LrgMarked", "large"};
            for (int i = 0; i < 16; ++i)
                m.add_user_point(names[i % 4], Vec3{double(i) / 4, -0.5, 2.0},
                        kUp, i % 5, kUserPointGameplay);
        }},
		{"armory", make_armory, nullptr},
		{"mount", make_mount, nullptr},
		{"carrier", make_carrier, nullptr},
		{"tank", make_tank, nullptr},
		{"vm40", make_vm40, nullptr},
		{"heli", make_heli, nullptr},
		{"person_slot2", [] { return make_person_two_bone("person_slot2", 0.0f); }, nullptr},
		{"person_rest4", [] { return make_person_two_bone("person_rest4", 0.125f); }, nullptr},
		// --- CTRL bus (object_data_ctrl_bus_test.gd) ---
		{"mount_ctrl1_heat_glow", make_mount, [](Model &m) { rename_register(m, 1, "HEAT_GLOW"); }},
		{"mount_ctrl1_not_retail", make_mount, [](Model &m) { rename_register(m, 1, "NOT_RETAIL"); }},
		{"mount_yaw_style114", make_mount, [](Model &m) { controlled_track(m, 1)->control = 114; }},
		{"mount_ctrl1_lod_frac_yaw_style114", make_mount, [](Model &m) {
			rename_register(m, 1, "LOD_FRAC");
			controlled_track(m, 1)->control = 114;
		}},
		// The RgbGen writes SelfLumColor, so the row wears the SELFLUM effect
		// with emissive_type 2 (Material_ApplyShaderParameters skips an RgbGen whose
		// routed colour the effect never reads).
		{"mount_mtrl0_rgbgen113_reg1", make_mount, [](Model &m) {
			std::snprintf(m.materials[0].shader_name, sizeof(m.materials[0].shader_name), "FF_ST_OP_LUM");
			m.materials[0].emissive_type = THREEDI_EMISSIVE_FULL;
			m.set_rgb_gen(0, THREEDI_PANM_STYLE_CONTROL_REGISTER, 1, 0.0, kBlack, kWhite);
		}},
		// --- Q3 bloom source (framefx_test.gd): the heat slab as an AlphaBlend
		// LUM (its SELFLUM copy carries alpha 0 into the Q3 target) ---
		{"mount_mtrl2_ab_lum", make_mount, [](Model &m) {
			std::snprintf(m.materials[2].shader_name, sizeof(m.materials[2].shader_name), "FF_ST_AB_LUM");
		}},
		// --- Q3 bloom source (framefx_test.gd): a per-vertex skinned model wearing
		// a LUM material; retail's bone path never copies it into Q3 ---
		// --- celestial bodies (celestial_test.gd): the crate as a stock-style
		// sky body, an FF_ST_AD_LUM surface whose RGB generator style 113 reads
		// CTRL UPL_INTENSITY black to white (the msun/fmoon4/mglare authoring) ---
		{"crate_mtrl0_ad_lum_upl113", make_crate, [](Model &m) {
			std::snprintf(m.materials[0].shader_name, sizeof(m.materials[0].shader_name), "FF_ST_AD_LUM");
			m.materials[0].emissive_type = THREEDI_EMISSIVE_FULL;
			m.add_control_register("UPL_INTENSITY");
			m.set_rgb_gen(0, THREEDI_PANM_STYLE_CONTROL_REGISTER, 0, 0.0, kBlack, kWhite);
		}},
		// ... and the same body authored AlphaBlend (its Q3 glow must not add)
		{"crate_mtrl0_ab_lum_upl113", make_crate, [](Model &m) {
			std::snprintf(m.materials[0].shader_name, sizeof(m.materials[0].shader_name), "FF_ST_AB_LUM");
			m.materials[0].emissive_type = THREEDI_EMISSIVE_FULL;
			m.add_control_register("UPL_INTENSITY");
			m.set_rgb_gen(0, THREEDI_PANM_STYLE_CONTROL_REGISTER, 0, 0.0, kBlack, kWhite);
		}},
		{"person_mtrl0_ad_lum", make_person, [](Model &m) {
			std::snprintf(m.materials[0].shader_name, sizeof(m.materials[0].shader_name), "FF_ST_AD_LUM");
			m.materials[0].emissive_type = THREEDI_EMISSIVE_FULL;
		}},
		// --- the clip-posed PANM layer (object_model_skeletal_panm_test.gd): the
		// left upper arm (part 9) scales 1 -> 0 from CTRL 0 `WPN_TRIGGER` ---
		{"person_part9_trigger_scale", make_person, [](Model &m) {
			m.add_control_register("WPN_TRIGGER");
			ThreediPartAnimation &row = m.lods[0].panm[9];
			row.flags = threedi_panm_pack_flags(1, threedi_panm_rotation_type(row.flags),
					static_cast<uint8_t>(threedi_panm_rotation_reversed(row.flags)),
					threedi_panm_translate_type(row.flags));
			row.scale_x.control = THREEDI_PANM_STYLE_CONTROL_REGISTER;
			row.scale_x.control_param = 0;
			row.scale_x.start = THREEDI_PANM_VALUE_ONE;
			row.scale_x.end = 0;
		}},
		{"armory_lght0_colorgen113_flicker", make_armory, [](Model &m) {
			ThreediLight &l = m.lights[0];
			l.flags = static_cast<uint8_t>(l.flags & ~THREEDI_LIGHT_FLAG_DISABLE_OBJECTS);
			l.style = THREEDI_PANM_STYLE_CONTROL_REGISTER;
			l.phase = 0;
			std::memset(l.color_start, 0, sizeof(l.color_start));
			l.color_end[0] = l.color_end[1] = l.color_end[2] = 255;
			l.color_end[3] = 0;
		}},
		// --- PANM apply (object_data_panm_apply_test.gd): a same-time noise track ---
		{"pump_anim0_noise_translation", make_pump, [](Model &m) {
			ThreediPartAnimation &row = m.lods[0].panm[0];
			row.flags = threedi_panm_pack_flags(threedi_panm_scale_type(row.flags), threedi_panm_rotation_type(row.flags),
					static_cast<uint8_t>(threedi_panm_rotation_reversed(row.flags)), THREEDI_TRANS_Z);
			row.translation = track(0x36, 0, 0, 0, 32767);
		}},
		// --- effect lights / per-model isolation ---
		{"shed_lght0_sub2_origin_atten100", make_shed, [](Model &m) {
			ThreediLight &l = m.lights[0];
			l.subobj_index = 2;
			set_light_position_model(l, 0.0f, 0.0f, 0.0f);
			l.atten_end = 100.0f;
		}},
		{"armory_lght0_sub1_offset", make_armory, [](Model &m) {
			ThreediLight &l = m.lights[0];
			l.subobj_index = 1;
			set_light_position_model(l, -0.25f, 0.5f, -0.75f); // Godot (0.25, 0.5, -0.75)
			l.atten_end = 1000.0f;
			l.flags = static_cast<uint8_t>(l.flags & ~THREEDI_LIGHT_FLAG_DISABLE_OBJECTS);
		}},
		// --- terrain static shadow (terrain_static_shadow_runtime_test.gd) ---
		{"house_lod0_sine_rotx", make_house, [](Model &m) { m.lods[0].panm.push_back(sine_rotation_row(0, 0)); }},
		{"house_lod0_sine_rotx_uv1", make_house, [](Model &m) {
			m.lods[0].panm.push_back(sine_rotation_row(0, 0));
			m.materials[0].u_params.style = 1;
		}},
		// Only the #UV twin evaluates MatTexCoord1, so the scrolling row wears it.
		{"house_mtrl0_uvscroll16_alphatest", make_house, [](Model &m) {
			std::snprintf(m.materials[0].shader_name, sizeof(m.materials[0].shader_name), "FF_ST_OP#UV");
			m.materials[0].material_flags |= THREEDI_MATERIAL_FLAG_ALPHA_TEST;
			m.materials[0].u_params.style = 16;
			m.materials[0].u_params.gen_rate = 1.0f;
		}},
		// --- render_swatch projshadow (render_swatch_pass_modes.gd): the _MT
		// post alpha-tested so the slot capture's Diffuse2.a coverage is
		// observable at the discard boundary ---
		{"pump_mtrl1_mt_alphatest", make_pump, [](Model &m) {
			m.materials[1].material_flags |= THREEDI_MATERIAL_FLAG_ALPHA_TEST;
			m.materials[1].alpha_test_value_byte = 32;
		}},
		// --- simulation_test.gd ---
		{"mount_heat_glow_slide_part1", make_mount, [](Model &m) {
			rename_register(m, 0, "HEAT_GLOW");
			delete_lod0_rows(m);
			m.lods[0].panm.push_back(slide_row(1, 0));
		}},
		{"armory_special1_slide_part1", make_armory, [](Model &m) {
			rename_register(m, 0, "VEHICLE_SPECIAL1");
			delete_lod0_rows(m);
			m.lods[0].panm.push_back(slide_row(1, 0));
		}},
		{"armory_special2_slide_part1", make_armory, [](Model &m) {
			rename_register(m, 0, "VEHICLE_SPECIAL2");
			delete_lod0_rows(m);
			m.lods[0].panm.push_back(slide_row(1, 0));
		}},
		{"tank_special1_slide_ewep01", make_tank, [](Model &m) {
			rename_register(m, 0, "VEHICLE_SPECIAL1");
			int anchor = -1;
			for (const ThreediUserPoint &p : m.user_points)
				if (std::strcmp(p.name, "ewep01") == 0) anchor = p.subobject_index;
			delete_lod0_rows(m);
			m.lods[0].panm.push_back(slide_row(anchor, 0));
		}},
		{"pump_lod0_inert_lod1_sine_rotz", make_pump, [](Model &m) {
			m.lods[0].panm.clear();
			m.lods[0].panm.push_back(inert_panm(0, 0));
			m.lods[1].panm.clear();
			m.lods[1].panm.push_back(sine_rotation_row(0, 2));
		}},
		// --- the PANM liveness family (simulation_test.gd): one LOD0 row on the shed ---
		{"panm_live_01_spinner", nullptr, nullptr},
		{"panm_live_02_view3", nullptr, nullptr},
		{"panm_live_03_view4", nullptr, nullptr},
		{"panm_live_04_rotz", nullptr, nullptr},
		{"panm_inert_05_rot_scalex", nullptr, nullptr},
		{"panm_inert_06_uniform_scaley", nullptr, nullptr},
		{"panm_live_07_uniform_scalex", nullptr, nullptr},
		{"panm_live_08_axis_scaley", nullptr, nullptr},
		{"panm_live_09_translation", nullptr, nullptr},
		{"panm_inert_10_rotrev", nullptr, nullptr},
		// --- the forms the shipped models carry past what an author states (the
		// retired threedi_retail_rewrite row read each back as its own bytes;
		// witnessed 2026-10-19 in the model named) ---
		// CFAC pad bytes that are not zero.
		{"crate_cfac_pad", make_crate, nullptr, "threedi/synth", [](Threedi3di3 &t) {
			for (size_t f = 0; f < t.collision->face_count; ++f) {
				t.collision->faces[f].pad[0] = 0x5A;
				t.collision->faces[f].pad[1] = static_cast<uint8_t>(f);
				t.collision->faces[f].pad[2] = 0x80;
			}
		}},
		// Face flags 0x100, 0x101, 0x400, 0x401, 0x500, 0x501.
		{"armory_cfac_flags", make_armory, nullptr, "threedi/synth", [](Threedi3di3 &t) {
			const uint32_t flags[6] = {0x100, 0x101, 0x400, 0x401, 0x500, 0x501};
			for (size_t f = 0; f < 6; ++f) t.collision->faces[f].material_flags = flags[f];
		}},
		// LOD 1's PANM chunk present and empty beside LOD 0's rows (CNet01, Flsh_1st).
		{"pump_panm_empty_lod1", make_pump, nullptr, "threedi/synth",
				[](Threedi3di3 &t) { t.lods[1].part_animation_count = 0; }},
		// LOD 1 holds no part, strip or vertex (CNet01).
		{"pump_lod1_empty", make_pump, nullptr, "threedi/synth", [](Threedi3di3 &t) {
			ThreediLod &l = t.lods[1];
			l.render_object_count = 0;
			l.strip_count = 0;
			l.vertices.count = 0;
			l.indices.count = 0;
			l.part_animation_count = 0;
		}},
		// A strip naming a material one past the table's end (Tent1).
		{"house_strip_material_past", make_house, nullptr, "threedi/synth",
				[](Threedi3di3 &t) { t.lods[0].strips[2].material_index = static_cast<int32_t>(t.material_count); }},
		// A texture row with an empty name (Det_1st).
		{"crate_texture_unnamed", make_crate, nullptr, "threedi/synth",
				[](Threedi3di3 &t) { std::memset(t.materials[0].textures[0].name, 0, sizeof(t.materials[0].textures[0].name)); }},
		// MTRX rows carrying -0 words and a translation, row 0 not the identity's bytes (B50Cal).
		{"mount_mtrx_translated", make_mount, nullptr, "threedi/synth", [](Threedi3di3 &t) {
			ThreediMatrix4x4 &row0 = t.mtrx.matrices[0];
			row0.m[1] = row0.m[4] = row0.m[6] = row0.m[9] = row0.m[12] = row0.m[13] = row0.m[14] = -0.0f;
			ThreediMatrix4x4 &row1 = t.mtrx.matrices[1];
			row1.m[1] = -0.0f;
			row1.m[12] = 0.0625f;
			row1.m[14] = 0.125f;
		}},
		// An MTRX row of NaN words (US01's; B50Cal's row 2).
		{"person_mtrx_nan", make_person, [](Model &m) {
			ThreediMatrix4x4 row{};
			for (int r = 0; r < 3; ++r)
				for (int c = 0; c < 3; ++c) row.m[r * 4 + c] = nan_word();
			row.m[15] = 1.0f;
			m.frames.push_back(row);
		}},
		// Omni lights flagged 2, 3, 6, 16, 18, 64, 65, 66, 146, 160, one axis word -0.0.
		{"shed_lght_omni_flags", make_shed, [](Model &m) {
			const uint8_t flags[10] = {2, 3, 6, 16, 18, 64, 65, 66, 146, 160};
			m.lights[0].flags = flags[0];
			for (int i = 1; i < 10; ++i)
				m.add_light(Vec3{0.25 * i - 1.0, 0.0, 1.25}, 0.0, 3.0, 24, 0, kWarm, kBlack, flags[i]);
		}, "threedi/synth", [](Threedi3di3 &t) { t.lights[0].rotation[0] = -0.0f; }},
		// Volume flags above 0x3E on two blink boxes (Armry02).
		{"armory_bvol_flags_high", make_armory, nullptr, "threedi/synth", [](Threedi3di3 &t) {
			int set = 0;
			for (size_t v = 0; v < t.collision->volume_count && set < 2; ++v)
				if (t.collision->volumes[v].collidable_type == 8) t.collision->volumes[v].flags = set++ == 0 ? 0x6E : 0x12E;
		}},
		// A flipbook on the register clock over flagged, framed rows (ArmGlove's: TEX_CAMO1
		// picks among four diffuse and four detail frames; the normal map stays).
		{"person_flipbook_register", make_person, [](Model &m) {
			m.add_control_register("TEX_CAMO1");
			ThreediMaterial &mat = m.materials[0];
			mat.texture_count = 0;
			const char *names[2][4] = {{"body_a.tga", "body_b.tga", "body_c.tga", "body_d.tga"},
				{"pat_a.tga", "pat_b.tga", "pat_c.tga", "pat_d.tga"}};
			for (int slot = 0; slot < 2; ++slot)
				for (int f = 0; f < 4; ++f) {
					ThreediMaterialTexture &t = mat.textures[mat.texture_count++];
					std::snprintf(t.name, sizeof(t.name), "%s", names[slot][f]);
					t.slot = static_cast<uint8_t>(slot == 0 ? THREEDI_TEX_SLOT_DIFFUSE : THREEDI_TEX_SLOT_DETAIL);
					t.type = THREEDI_TEX_TYPE_DIFFUSE;
					t.flags = THREEDI_TEX_FLAG_ANIMATED;
					t.frame = static_cast<uint8_t>(f);
				}
			ThreediMaterialTexture &n = mat.textures[mat.texture_count++];
			std::snprintf(n.name, sizeof(n.name), "%s", "body.mdt");
			n.slot = THREEDI_TEX_SLOT_NORMAL;
			n.type = THREEDI_TEX_TYPE_NORMAL_MDT;
			mat.animation.num_frames = 4;
			mat.animation.animation_type = 1;
			mat.animation.cycle_frame_time = 0;
		}},
		// Volume types 0, 5, 9, 10, 12, 13 and 16 beside the crate's own.
		{"crate_bvol_types", make_crate, [](Model &m) {
			const Box b = box(-0.5, -0.5, 0.0, 0.5, 0.5, 1.0);
			for (int32_t type : {0, 5, 9, 10, 12, 13, 16}) m.add_volume(0, type, 0, b);
		}},
		// A user point on part -1 (Bed01).
		{"crate_usrp_no_part", make_crate, nullptr, "threedi/synth",
				[](Threedi3di3 &t) { t.user_points[0].subobject_index = -1; }},
		// The bulb self-lit on its UV twin: an emissive material named _LUM#UV, its U
		// scrolling (the shipped _LUM#UV rows the reader accepts as emissive).
		{"shed_mtrl1_ab_lum_uv", make_shed, [](Model &m) {
			std::snprintf(m.materials[1].shader_name, sizeof(m.materials[1].shader_name), "FF_ST_AB_LUM#UV");
			m.materials[1].emissive_type = THREEDI_EMISSIVE_FULL;
			m.materials[1].u_params.style = 16;
			m.materials[1].u_params.gen_rate = 1.0f;
		}},
		// --- the constant AlphaGen crate (fixtures/threedi/crate_alphagen24.3di) ---
		{"crate_alphagen24", make_crate, [](Model &m) {
			ThreediMaterial &mat = m.materials[0];
			mat.material_flags = THREEDI_MATERIAL_FLAG_TWO_SIDED;
			mat.alpha_gen.style = 24;
			mat.alpha_gen.reg = -1;
			mat.alpha_gen.start = 128;
			mat.alpha_gen.end = 0;
		}, "threedi"},
	};
	return kRecipes;
}

Model build_recipe(const Recipe &r) {
	if (r.base == nullptr) {
		// (flags, live tracks): rx ry rz sx sy sz tr = 0..6
		if (!std::strcmp(r.file, "panm_live_01_spinner")) return make_liveness_case(1u << 8, {});
		if (!std::strcmp(r.file, "panm_live_02_view3")) return make_liveness_case(3u << 8, {});
		if (!std::strcmp(r.file, "panm_live_03_view4")) return make_liveness_case(4u << 8, {});
		if (!std::strcmp(r.file, "panm_live_04_rotz")) return make_liveness_case(2u << 8, {2});
		if (!std::strcmp(r.file, "panm_inert_05_rot_scalex")) return make_liveness_case(2u << 8, {3});
		if (!std::strcmp(r.file, "panm_inert_06_uniform_scaley")) return make_liveness_case(1u, {4});
		if (!std::strcmp(r.file, "panm_live_07_uniform_scalex")) return make_liveness_case(1u, {3});
		if (!std::strcmp(r.file, "panm_live_08_axis_scaley")) return make_liveness_case(2u, {4});
		if (!std::strcmp(r.file, "panm_live_09_translation")) return make_liveness_case(1u << 24, {6});
		return make_liveness_case(1u << 16, {});
	}
	Model m = r.base();
	if (r.edit) r.edit(m);
	return m;
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
	const char *point(size_t i) const { return i < model.user_point_count ? model.user_points[i].name : ""; }
	size_t parts(size_t lod = 0) const { return lod < model.lod_count ? model.lods[lod].render_object_count : 0; }
	size_t rows(size_t lod = 0) const { return lod < model.lod_count ? model.lods[lod].part_animation_count : 0; }
	const ThreediPartAnimation &row(size_t lod, size_t i) const { return model.lods[lod].part_animations[i]; }
	bool live(int lod) const { return threedi_panm_lod_has_live(model, lod); }
	size_t cobjs() const { return model.collision ? model.collision->object_count : 0; }
	int faces(size_t cobj) const { return model.collision->objects[cobj].num_faces; }
	int volumes(size_t cobj) const { return model.collision->objects[cobj].num_bounding_volumes; }
	size_t bvols() const { return model.collision ? model.collision->volume_count : 0; }
	int bvol_type_count(int32_t type) const {
		int n = 0;
		for (size_t i = 0; i < bvols(); ++i) n += model.collision->volumes[i].collidable_type == type ? 1 : 0;
		return n;
	}
	bool safe() const { return model.collision == nullptr || threedi_3di3_collision_is_runtime_safe(model.collision) == 1; }
};

// The quirk each form-carrying variant holds, read back off the committed
// bytes; false for every other recipe.
bool check_quirk(const std::string &name, const Parsed &p) {
	const Threedi3di3 &t = p.model;
	if (name == "crate_cfac_pad") {
		expect(t.collision->face_count == 12 && t.collision->faces[3].pad[0] == 0x5A && t.collision->faces[3].pad[1] == 3,
				name + ": CFAC pad bytes");
	} else if (name == "armory_cfac_flags") {
		expect(t.collision->faces[0].material_flags == 0x100 && t.collision->faces[5].material_flags == 0x501,
				name + ": face flags");
	} else if (name == "pump_panm_empty_lod1") {
		expect(t.lod_count == 2 && t.lods[1].part_animation_count == 0 && t.lods[0].part_animation_count == 5,
				name + ": an empty LOD 1 PANM");
	} else if (name == "pump_lod1_empty") {
		expect(t.lod_count == 2 && t.lods[1].render_object_count == 0 && t.lods[1].strip_count == 0 &&
						t.lods[1].vertices.count == 0,
				name + ": an empty LOD 1");
	} else if (name == "house_strip_material_past") {
		expect(t.lods[0].strips[2].material_index == static_cast<int32_t>(t.material_count), name + ": a strip past the table");
	} else if (name == "crate_texture_unnamed") {
		expect(t.materials[0].texture_count == 1 && t.materials[0].textures[0].name[0] == '\0', name + ": an unnamed row");
	} else if (name == "mount_mtrx_translated") {
		expect(t.mtrx.count == 2 && std::signbit(t.mtrx.matrices[0].m[1]) && t.mtrx.matrices[1].m[12] == 0.0625f,
				name + ": MTRX -0 words and a translation");
	} else if (name == "person_mtrx_nan") {
		expect(t.mtrx.count == 2 && std::isnan(t.mtrx.matrices[1].m[0]) && t.mtrx.matrices[1].m[15] == 1.0f,
				name + ": an MTRX row of NaN");
	} else if (name == "shed_lght_omni_flags") {
		expect(t.light_count == 10 && t.lights[0].flags == 2 && t.lights[8].flags == 146 && t.lights[9].flags == 160 &&
						std::signbit(t.lights[0].rotation[0]) && t.lights[0].rotation[0] == 0.0f,
				name + ": light flags and a -0.0 axis word");
	} else if (name == "armory_bvol_flags_high") {
		int high = 0;
		for (size_t v = 0; v < t.collision->volume_count; ++v) high += t.collision->volumes[v].flags > 0x3E ? 1 : 0;
		expect(high == 2, name + ": two volume flags above 0x3E");
	} else if (name == "person_flipbook_register") {
		const ThreediMaterial &m = t.materials[0];
		expect(m.texture_count == 9 && m.animation.num_frames == 4 && m.animation.animation_type == 1 &&
						m.textures[5].slot == THREEDI_TEX_SLOT_DETAIL && m.textures[5].frame == 1 &&
						(m.textures[5].flags & THREEDI_TEX_FLAG_ANIMATED) != 0 && !std::strcmp(p.reg(0), "TEX_CAMO1"),
				name + ": a register flipbook");
	} else if (name == "crate_bvol_types") {
		const int32_t types[7] = {0, 5, 9, 10, 12, 13, 16};
		bool all = p.bvols() == 8;
		for (int32_t type : types) all = all && p.bvol_type_count(type) == 1;
		expect(all, name + ": volume types");
	} else if (name == "crate_usrp_no_part") {
		expect(t.user_points[0].subobject_index == -1, name + ": a user point on part -1");
	} else {
		return false;
	}
	return true;
}

int slide_moves_part(const Parsed &p, int part, int reg) {
	if (p.rows(0) != 1) return 0;
	const ThreediPartAnimation &r = p.row(0, 0);
	return r.subobject_index == part && r.translation.control == THREEDI_PANM_STYLE_CONTROL_REGISTER &&
			r.translation.control_param == reg && r.translation.end == 4 * THREEDI_PANM_VALUE_ONE;
}

void check_facts(const std::string &name, const std::vector<uint8_t> &bytes) {
	Parsed p(bytes);
	if (!expect(p.ok, name + ": committed bytes parse")) return;
	expect(p.safe(), name + ": collision block is runtime safe");
	expect(p.model.header.has_header && p.model.header.lod_count_decl == static_cast<int32_t>(p.model.lod_count), name + ": GHDR");
	if (check_quirk(name, p)) return;
	const std::string base = name.substr(0, name.find('_'));
	if (base == "crate") {
		expect(p.parts() == 1 && p.cobjs() == 1 && p.faces(0) == 12 && p.bvols() == 1 && !std::strcmp(p.point(0), "ground"), name + ": crate facts");
	} else if (base == "gun") {
		expect(p.parts() == 2 && p.bvols() == 0 && p.model.collision->face_count == 0 && !std::strcmp(p.point(0), "MFlash01") && p.model.user_points[0].subobject_index == 1 && !std::strcmp(p.point(1), "Bullet01") && p.model.user_points[1].subobject_index == 0, name + ": gun facts");
	} else if (base == "shed" || base == "panm") {
		expect(p.parts() == 1 && p.model.light_count == 1 && p.model.lods[0].render_objects[0].num_alpha_strips == 1 && p.bvols() == 2 && !std::strcmp(p.reg(0), "FLICKER"), name + ": shed facts");
	} else if (base == "house") {
		expect(p.parts() == 1 && p.bvols() == 4 && p.model.occlusion_object_count == 0 && p.model.collision->planes != nullptr && p.model.collision->volumes[3].plane_count == 10, name + ": house facts");
	} else if (base == "bird") {
		expect(p.model.header.mesh_type == THREEDI_MESH_SKINNED && p.parts() == 9 && p.cobjs() == 9 && p.model.collision->face_count == 18 && p.bvols() == 0, name + ": bird facts");
	} else if (name == "person_slot2" || name == "person_rest4") {
		const ThreediLod &lod = p.model.lods[0];
		expect(lod.vertices.flags == 0x55u && lod.strips[1].bone_table_length == 2 &&
						lod.vertices.items[lod.strips[1].start_vertex].bone_indices[3] == 2 &&
						p.model.collision->translation_count == 19 &&
						p.model.collision->translations[1].translation[2] != p.model.collision->objects[1].offset[2],
				name + ": two-bone strips, a slot past the table, CXLT off the offsets");
	} else if (base == "person") {
		bool ok = p.model.header.mesh_type == THREEDI_MESH_SKINNED && p.parts() == 19 && p.cobjs() == 19 && p.bvols() == 0;
		if (ok) {
			const ThreediCollisionObject &head = p.model.collision->objects[14];
			ok = head.parent_subobject_index == 13 && head.num_faces == 0 && head.med[0] == 4096 && head.med[1] == 0 && head.med[2] == 53248 && head.radius == 10240;
		}
		expect(ok && !std::strcmp(p.point(0), "bullet") && p.model.user_points[0].subobject_index == 16, name + ": person facts");
	} else if (base == "pump") {
		expect(p.model.lod_count == 2 && p.parts(0) == 5 && p.parts(1) == 5 && p.cobjs() == 5 && p.bvols() == 2 && p.model.ctrl.count == 0, name + ": pump facts");
	} else if (base == "armory") {
		expect(p.parts() == 4 && p.cobjs() == 4 && p.faces(0) == 12 && p.faces(1) == 8 && p.faces(2) == 2 && p.faces(3) == 2 &&
						p.bvol_type_count(1) == 5 && p.bvol_type_count(4) == 1 && p.bvol_type_count(6) == 1 && p.bvol_type_count(7) == 1 && p.bvol_type_count(8) == 3 &&
						p.model.occlusion_object_count == 8 && p.model.light_count == 2 && p.model.material_count == 6 &&
						p.model.materials[3].rgb_gen.style == THREEDI_PANM_STYLE_CONTROL_REGISTER && p.model.materials[3].rgb_gen.reg == 0 &&
						!std::strcmp(p.point(0), "Armory") && !std::strcmp(p.point(1), "Ground"),
				name + ": armory facts");
	} else if (base == "mount") {
		expect(p.parts() == 3 && p.cobjs() == 3 && p.bvols() == 3 && p.model.user_point_count == 6 && !std::strcmp(p.point(5), "Usegun") &&
						!std::strcmp(p.point(4), "MFlash01") && !std::strcmp(p.reg(2), "EWEAP_GUNPITCH"),
				name + ": mount facts");
	} else if (base == "carrier") {
		expect(p.parts() == 2 && p.cobjs() == 2 && p.model.collision->translation_count == 1 &&
						p.model.collision->translations[0].translation[0] == 131072 && p.model.collision->translations[0].translation[2] == 32768 &&
						!std::strcmp(p.point(0), "ctrlx13") && !std::strcmp(p.point(2), "sitex00d") && p.model.user_point_count == 8 && p.model.ctrl.count == 6,
				name + ": carrier facts");
	} else if (base == "vm40") {
		expect(p.model.header.mesh_type == THREEDI_MESH_SKINNED && p.parts() == 40 && p.cobjs() == 40 &&
						p.model.collision->face_count == 80 && p.model.lods[0].render_objects[38].parent_index == 37 &&
						!std::strcmp(p.point(0), "bullet") && p.model.user_points[0].subobject_index == 37,
				name + ": vm40 facts");
	} else if (base == "heli") {
		expect(p.model.lod_count == 3 && p.parts(0) == 5 && p.parts(2) == 0 && p.model.mtrx.count == 2 &&
						p.model.lods[0].vertices.flags == 0x15u && p.row(0, 3).matrix_index == 1 &&
						!std::strcmp(p.reg(1), "HELO_TAILROTOR"),
				name + ": heli facts");
	} else if (base == "tank") {
		expect(p.parts() == 2 && p.cobjs() == 2 && !std::strcmp(p.point(0), "ctrlx25") && !std::strcmp(p.point(1), "ewep01") &&
						p.model.user_points[1].subobject_index == 1 && p.bvol_type_count(19) == 2,
				name + ": tank facts");
	}
	// The per-variant read-back guards of the old minting probe.
	if (name == "mount") expect(p.live(0) && !std::strcmp(p.reg(0), "HEAT_GLOW") && !std::strcmp(p.reg(1), "EWEAP_GUNYAW"), name + ": registers/live");
	// The first LOD0 track on register 1 (the cradle's yaw: row 1, rotation x,
	// part 1) — object_data_ctrl_bus_test.gd keys its poses on these facts.
	if (base == "mount" && name != "mount_heat_glow_slide_part1")
		expect(p.rows(0) == 3 && p.row(0, 1).subobject_index == 1 &&
						p.row(0, 0).rotation_x.control != THREEDI_PANM_STYLE_CONTROL_REGISTER &&
						p.row(0, 1).rotation_x.control_param == 1 &&
						(name == "mount_yaw_style114" || name == "mount_ctrl1_lod_frac_yaw_style114" ||
								p.row(0, 1).rotation_x.control == THREEDI_PANM_STYLE_CONTROL_REGISTER),
				name + ": cradle yaw row");
	if (name == "mount_ctrl1_heat_glow") expect(!std::strcmp(p.reg(1), "HEAT_GLOW"), name + ": ctrl1");
	if (name == "mount_ctrl1_not_retail") expect(!std::strcmp(p.reg(1), "NOT_RETAIL"), name + ": ctrl1");
	if (name == "mount_yaw_style114" || name == "mount_ctrl1_lod_frac_yaw_style114")
		expect(p.row(0, 1).rotation_x.control == 114, name + ": yaw style 114");
	if (name == "mount_ctrl1_lod_frac_yaw_style114") expect(!std::strcmp(p.reg(1), "LOD_FRAC"), name + ": ctrl1");
	if (name == "mount_mtrl0_rgbgen113_reg1")
		expect(p.model.materials[0].rgb_gen.style == 113 && p.model.materials[0].rgb_gen.reg == 1 &&
						!std::strcmp(p.model.materials[0].shader_name, "FF_ST_OP_LUM") &&
						p.model.materials[0].emissive_type == THREEDI_EMISSIVE_FULL,
				name + ": material alias");
	if (name == "crate_mtrl0_ad_lum_upl113" || name == "crate_mtrl0_ab_lum_upl113")
		expect(!std::strcmp(p.model.materials[0].shader_name,
						name == "crate_mtrl0_ad_lum_upl113" ? "FF_ST_AD_LUM" : "FF_ST_AB_LUM") &&
						p.model.materials[0].rgb_gen.style == 113 &&
						p.model.materials[0].rgb_gen.reg == 0 && !std::strcmp(p.reg(0), "UPL_INTENSITY"),
				name + ": UPL_INTENSITY sky body");
	if (name == "mount_heat_glow_slide_part1") expect(slide_moves_part(p, 1, 0) && !std::strcmp(p.reg(0), "HEAT_GLOW"), name + ": slide");
	if (name == "armory") expect(!p.live(0), name + ": inert");
	if (name == "armory_lght0_colorgen113_flicker") expect(p.model.lights[0].style == 113 && p.model.lights[0].phase == 0, name + ": light gen");
	if (name == "armory_lght0_sub1_offset") expect(p.model.lights[0].subobj_index == 1 && p.model.lights[0].atten_end == 1000.0f, name + ": light offset");
	if (name == "armory_special1_slide_part1") expect(slide_moves_part(p, 1, 0) && !std::strcmp(p.reg(0), "VEHICLE_SPECIAL1"), name + ": slide");
	if (name == "armory_special2_slide_part1") expect(slide_moves_part(p, 1, 0) && !std::strcmp(p.reg(0), "VEHICLE_SPECIAL2"), name + ": slide");
	if (name == "tank_special1_slide_ewep01") expect(slide_moves_part(p, 1, 0) && !std::strcmp(p.reg(0), "VEHICLE_SPECIAL1"), name + ": slide");
	if (name == "pump") expect(p.live(0) && !p.live(1) && p.rows(0) == 5, name + ": LOD0 live, LOD1 inert");
	if (name == "pump_lod20" || name == "pump_lod80")
		expect(p.model.lods[0].lod_threshold == (name == "pump_lod20" ? 20 : 80) &&
						p.model.lods[1].lod_threshold == 0,
				name + ": authored LOD0 pixel threshold");
	if (name == "pump_anim0_noise_translation") expect(p.row(0, 0).translation.control == 0x36 && p.row(0, 0).translation.end == 32767, name + ": noise");
	if (name == "pump_lod0_inert_lod1_sine_rotz") expect(p.rows(0) == 1 && !p.live(0) && p.rows(1) == 1 && p.live(1), name + ": LOD liveness");
	if (name == "shed_lght0_sub2_origin_atten100") expect(p.model.lights[0].subobj_index == 2 && p.model.lights[0].atten_end == 100.0f, name + ": light");
	if (name == "house") expect(!p.live(0), name + ": inert");
	if (name == "house_lod0_sine_rotx") expect(p.live(0) && p.model.materials[0].u_params.style == 0, name + ": live, uv 0");
	if (name == "house_lod0_sine_rotx_uv1") expect(p.live(0) && p.model.materials[0].u_params.style == 1, name + ": live, uv 1");
	if (name == "shed_mtrl1_ab_lum_uv")
		expect(!std::strcmp(p.model.materials[1].shader_name, "FF_ST_AB_LUM#UV") &&
						p.model.materials[1].emissive_type == THREEDI_EMISSIVE_FULL && p.model.materials[1].u_params.style == 16,
				name + ": an emissive _LUM#UV row");
	if (name == "crate_alphagen24")
		expect(!std::strcmp(p.model.materials[0].shader_name, "FF_ST_OP") && p.model.materials[0].alpha_gen.style == 24 &&
						p.model.materials[0].alpha_gen.start == 128 &&
						(p.model.materials[0].material_flags & THREEDI_MATERIAL_FLAG_ALPHA_TEST) == 0,
				name + ": constant AlphaGen on an opaque row");
	if (name == "house_mtrl0_uvscroll16_alphatest")
		expect((p.model.materials[0].material_flags & THREEDI_MATERIAL_FLAG_ALPHA_TEST) != 0 && p.model.materials[0].u_params.style == 16 && p.model.materials[0].u_params.gen_rate == 1.0f &&
						!std::strcmp(p.model.materials[0].shader_name, "FF_ST_OP#UV"),
				name + ": material");
	if (base == "panm") {
		const bool expected_live = name.rfind("panm_live", 0) == 0;
		expect(p.rows(0) == 1 && p.live(0) == expected_live, name + ": liveness");
	}
}

using test_io::read_file;

} // namespace

int main(int argc, char **argv) {
	bool write_mode = false;
	for (int i = 1; i < argc; ++i)
		if (std::strcmp(argv[i], "--write") == 0) write_mode = true;
	const std::string fixtures = std::string(test_paths_repo_root(__FILE__)) + "/fixtures/";

	for (const Recipe &recipe : recipes()) {
		const std::string dir = fixtures + recipe.dir;
		std::filesystem::create_directories(dir);
		const std::string path = dir + "/" + recipe.file + ".3di";
		const Model model = build_recipe(recipe);
		std::vector<uint8_t> bytes;
		if (!expect(mint(model, bytes), std::string(recipe.file) + ": the writer accepts the model")) continue;
		if (recipe.raw) {
			Threedi3di3 t{};
			if (!expect(threedi_3di3_read_memory(bytes.data(), bytes.size(), &t) == 0, std::string(recipe.file) + ": reads"))
				continue;
			recipe.raw(t);
			const int rc = threedi_3di3_write_memory(&t, bytes);
			threedi_3di3_free(&t);
			if (!expect(rc == 0, std::string(recipe.file) + ": the writer takes the edited model")) continue;
		}
		if (write_mode) {
			std::ofstream o(path, std::ios::binary);
			o.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
			std::printf("wrote %s (%zu bytes)\n", path.c_str(), bytes.size());
			check_facts(recipe.file, bytes);
			continue;
		}
		std::vector<uint8_t> committed;
		if (!expect(read_file(path, committed), path + " missing; run with --write")) continue;
		if (test_io::is_lfs_pointer(committed)) {
			std::printf("[skip] %s is an unpulled LFS pointer\n", path.c_str());
			continue;
		}
		expect(committed == bytes, path + " differs from the generator output; regenerate with --write");
		check_facts(recipe.file, committed);
		std::printf("%-40s %6zu bytes\n", recipe.file, committed.size());
	}
	if (failures == 0 && !write_mode) std::printf("OK: fixtures/threedi/synth byte-reproducible\n");
	return failures == 0 ? 0 : 1;
}
