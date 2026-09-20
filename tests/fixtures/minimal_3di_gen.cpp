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
// Default: rebuild every file in memory and byte-compare the committed
// copies; `--write` (re)writes them. Every run re-reads the committed bytes
// and checks the authored facts the consumers pin.
#include "fixtures/minimal_3di_builder.h"

#include <formats/threedi/threedi_panm_pose.h>

#include "common/test_paths.h"

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
	// the pelvis, feet a little over a unit below it.
	const int parents[19] = {0, 0, 1, 2, 2, 3, 4, 0, 0, 5, 6, 7, 8, 2, 13, 10, 9, 11, 12};
	const Vec3 pivots[19] = {{0, 0, 0}, {0, 0, 0.125}, {0, 0, 0.375}, {0, -0.0625, 0.625},
		{0, 0.0625, 0.625}, {0, -0.25, 0.625}, {0, 0.25, 0.625}, {0, -0.125, 0}, {0, 0.125, 0},
		{0, -0.4375, 0.4375}, {0, 0.4375, 0.4375}, {0, -0.125, -0.4375}, {0, 0.125, -0.4375},
		{0, 0, 0.6875}, {0.03125, 0, 0.75}, {0, 0.6875, 0.3125}, {0, -0.6875, 0.3125},
		{-0.0625, -0.125, -0.875}, {-0.0625, 0.125, -0.875}};
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
	// yaws on 1 and pitches on 2 (rotation x = the authored yaw axis).
	ThreediPartAnimation &r0 = m.add_panm(lod, 0, 0, threedi_panm_pack_flags(0, 2, 1, 0));
	r0.rotation_x = track(0, 0, 0, 0, 16384);
	ThreediPartAnimation &r1 = m.add_panm(lod, 1, 0, threedi_panm_pack_flags(0, 2, 1, 0));
	r1.rotation_x = track(THREEDI_PANM_STYLE_CONTROL_REGISTER, 1, 0, 0, 16384);
	r1.rotation_y = track(0, 0, 0, 16384, 0);
	ThreediPartAnimation &r2 = m.add_panm(lod, 2, 0, threedi_panm_pack_flags(0, 2, 1, 0));
	r2.rotation_x = track(THREEDI_PANM_STYLE_CONTROL_REGISTER, 1, 0, 0, 16384);
	r2.rotation_y = track(THREEDI_PANM_STYLE_CONTROL_REGISTER, 2, 0, 16384, 0);
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

// ---------------------------------------------------------------------------
// The variant recipes (fixtures/README.md "threedi/synth"): one authored edit
// each, spelled with the same runtime vocabulary the retired ObjectData
// editing surface used.
using Edit = std::function<void(Model &)>;

struct Recipe {
	const char *file;
	Model (*base)();
	Edit edit;
};

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
		// distinct coarse thresholds, written through the same parity writer.
		{"pump_lod20", make_pump, [](Model &m) { m.lods[1].threshold = 20 << 16; }},
		{"pump_lod80", make_pump, [](Model &m) { m.lods[1].threshold = 80 << 16; }},
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
		// --- CTRL bus (object_data_ctrl_bus_test.gd) ---
		{"mount_ctrl1_heat_glow", make_mount, [](Model &m) { rename_register(m, 1, "HEAT_GLOW"); }},
		{"mount_ctrl1_not_retail", make_mount, [](Model &m) { rename_register(m, 1, "NOT_RETAIL"); }},
		{"mount_yaw_style114", make_mount, [](Model &m) { controlled_track(m, 1)->control = 114; }},
		{"mount_ctrl1_lod_frac_yaw_style114", make_mount, [](Model &m) {
			rename_register(m, 1, "LOD_FRAC");
			controlled_track(m, 1)->control = 114;
		}},
		{"mount_mtrl0_rgbgen113_reg1", make_mount, [](Model &m) {
			m.set_rgb_gen(0, THREEDI_PANM_STYLE_CONTROL_REGISTER, 1, 0.0, kBlack, kWhite);
		}},
		// --- Q3 bloom source (framefx_test.gd): the heat slab as an AlphaBlend
		// LUM (its SELFLUM copy carries alpha 0 into the Q3 target) ---
		{"mount_mtrl2_ab_lum", make_mount, [](Model &m) {
			std::snprintf(m.materials[2].shader_name, sizeof(m.materials[2].shader_name), "FF_ST_AB_LUM");
		}},
		// --- Q3 bloom source (framefx_test.gd): a per-vertex skinned model wearing
		// a LUM material; retail's bone path never copies it into Q3 ---
		{"person_mtrl0_ad_lum", make_person, [](Model &m) {
			std::snprintf(m.materials[0].shader_name, sizeof(m.materials[0].shader_name), "FF_ST_AD_LUM");
			m.materials[0].emissive_type = THREEDI_EMISSIVE_FULL;
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
		{"house_mtrl0_uvscroll16_alphatest", make_house, [](Model &m) {
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
	if (name == "mount_mtrl0_rgbgen113_reg1") expect(p.model.materials[0].rgb_gen.style == 113 && p.model.materials[0].rgb_gen.reg == 1, name + ": material alias");
	if (name == "mount_heat_glow_slide_part1") expect(slide_moves_part(p, 1, 0) && !std::strcmp(p.reg(0), "HEAT_GLOW"), name + ": slide");
	if (name == "armory") expect(!p.live(0), name + ": inert");
	if (name == "armory_lght0_colorgen113_flicker") expect(p.model.lights[0].style == 113 && p.model.lights[0].phase == 0, name + ": light gen");
	if (name == "armory_lght0_sub1_offset") expect(p.model.lights[0].subobj_index == 1 && p.model.lights[0].atten_end == 1000.0f, name + ": light offset");
	if (name == "armory_special1_slide_part1") expect(slide_moves_part(p, 1, 0) && !std::strcmp(p.reg(0), "VEHICLE_SPECIAL1"), name + ": slide");
	if (name == "armory_special2_slide_part1") expect(slide_moves_part(p, 1, 0) && !std::strcmp(p.reg(0), "VEHICLE_SPECIAL2"), name + ": slide");
	if (name == "tank_special1_slide_ewep01") expect(slide_moves_part(p, 1, 0) && !std::strcmp(p.reg(0), "VEHICLE_SPECIAL1"), name + ": slide");
	if (name == "pump") expect(p.live(0) && !p.live(1) && p.rows(0) == 5, name + ": LOD0 live, LOD1 inert");
	if (name == "pump_lod20" || name == "pump_lod80")
		expect(p.model.lods[1].lod_threshold == ((name == "pump_lod20" ? 20 : 80) << 16),
				name + ": authored coarse threshold");
	if (name == "pump_anim0_noise_translation") expect(p.row(0, 0).translation.control == 0x36 && p.row(0, 0).translation.end == 32767, name + ": noise");
	if (name == "pump_lod0_inert_lod1_sine_rotz") expect(p.rows(0) == 1 && !p.live(0) && p.rows(1) == 1 && p.live(1), name + ": LOD liveness");
	if (name == "shed_lght0_sub2_origin_atten100") expect(p.model.lights[0].subobj_index == 2 && p.model.lights[0].atten_end == 100.0f, name + ": light");
	if (name == "house") expect(!p.live(0), name + ": inert");
	if (name == "house_lod0_sine_rotx") expect(p.live(0) && p.model.materials[0].u_params.style == 0, name + ": live, uv 0");
	if (name == "house_lod0_sine_rotx_uv1") expect(p.live(0) && p.model.materials[0].u_params.style == 1, name + ": live, uv 1");
	if (name == "house_mtrl0_uvscroll16_alphatest")
		expect((p.model.materials[0].material_flags & THREEDI_MATERIAL_FLAG_ALPHA_TEST) != 0 && p.model.materials[0].u_params.style == 16 && p.model.materials[0].u_params.gen_rate == 1.0f, name + ": material");
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
	const std::string dir = std::string(test_paths_repo_root(__FILE__)) + "/fixtures/threedi/synth";
	std::filesystem::create_directories(dir);
	const std::string scratch = std::string(test_paths_temp_dir()) + "/minimal_3di_gen_scratch.3di";

	for (const Recipe &recipe : recipes()) {
		const std::string path = dir + "/" + recipe.file + ".3di";
		const Model model = build_recipe(recipe);
		std::vector<uint8_t> bytes;
		if (!expect(mint(model, scratch, bytes), std::string(recipe.file) + ": the writer accepts the model")) continue;
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
