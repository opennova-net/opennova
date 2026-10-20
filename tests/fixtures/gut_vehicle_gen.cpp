// Generator + guard for the GUT vehicle model set fixtures/gut/*.3di: the
// mounts, door guns, carriers and tanks the GUT vehicle suites (mounted_view,
// vehicle_emplacement_alignment, usegun_mount_alignment, tank_parity,
// tank_training, mounted_weapon_switch, virtual_display_present,
// hud_installed_assets) board, crew and fire, authored from small
// integer-friendly data through tests/fixtures/minimal_3di_builder.h and
// minted by our own parity writer (threedi_3di3_write), so every file is
// reproduced byte-for-byte and no retail bytes live in the tree (ADR 0003).
//
// Each model stands in for the retail one those suites once loaded and keeps
// only what the suites key on (mission axes, x forward / y left / z up, origin
// on the ground):
//   tripod_usegun_cradle  the E50triB shape: base + cradle + gun, CTRL
//            EWEAP_GUNYAW / EWEAP_GUNPITCH (the cradle yaws on 0, the gun yaws
//            on 0 and pitches on 1), `Usegun` owned by the articulated cradle
//            (part 1), `Camera`, `BCasing`, `Bullet`, `MFlash01` on the gun
//   minigun  the Minigun shape: base + cradle + gun + barrel, CTRL
//            EWEAP_GUNYAW / EWEAP_GUNPITCH / WEAP_SPIN; the barrel turns with
//            the gun and rolls about its own axis on WEAP_SPIN; `MFlash01` on the
//            gun at the barrel's far end, on its axis; `UseGun` on the base
//   fifty    the H50cal shape: cradle + gun, CTRL EWEAP_GUNYAW /
//            EWEAP_GUNPITCH (both parts yaw, the gun pitches), `Camera`,
//            `Bullet`, `MFlash01` on the gun, `Usegun` on the cradle
//   cannon   the M1trret shape: base + cradle + gun, CTRL EWEAP_GUNYAW /
//            EWEAP_GUNPITCH, `camera`, `bullet01`, `mflash01` on the gun,
//            `usegun` on the base
//   twingun  the Ka50gun shape: mount + cradle + twin-barrel gun, `bullet01` /
//            `bullet02` (left and right), `camera`, `usegun` well behind the
//            gun (the gunner sits in the cockpit)
//   helo     the Dblkhwk1 / DAH62 shape: hull + main rotor + tail rotor, CTRL
//            HELO_ROTOR / HELO_TAILROTOR spinning the rotors; `ctrlx05` (the
//            pilot), two `sitex` passenger points, `Ewep01` / `Ewep02` door
//            anchors facing out of the left and right doors, the carrier
//            weapon's pods `BULLET01..04` / `MFLASH01..04` / `bcasing01..02`,
//            `ground`, `FX00`
//   gunboat  the Dmrk51 shape: one hull part, `ctrlx22`, four `ewep01..04`
//            anchors, ewep02 and ewep03 tilted off the hull plane (the
//            row-matrix conversion witnesses), `FX00`
//   tank_turret  the Dm1a1 shape: hull + turret ring part on VEHICLE_GUNYAW;
//            `ctrlx25`, `ewep01` on the hull (the turret child's mount),
//            `ewep02` on the ring (the roof gun's), `fx00`, `ground`
//   lcac     the Dlcac1 shape: one flat-decked hull a tank and its crew stand
//            on, the pilot's cab on the left bow; `ctrlx11` (the pilot, as
//            Dlcac1's), `sitex00`, `FX00` astern
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
constexpr Vec3 kForward{1.0, 0.0, 0.0};
constexpr Vec3 kBackward{-1.0, 0.0, 0.0};
constexpr Vec3 kLeft{0.0, 1.0, 0.0};
constexpr Vec3 kRight{0.0, -1.0, 0.0};

// One collision section per drawing part, a face box and a CB volume.
void solid_cobj(Model &m, int parent, Vec3 offset, const Box &b) {
	const int cobj = m.add_cobj(parent, offset);
	m.add_face_box(cobj, b);
	m.add_volume(cobj, 1, 0, b);
}

// The retail emplaced-gun articulation: yaw on register `yaw` about the
// authored yaw axis (rotation x), pitch on `pitch` (rotation y), a full turn
// per register wrap, a positive pitch word dipping the muzzle (the shipped
// H50cal: EWEAP_GUNPITCH 4096 turns the gun's forward to (0, -0.383, 0.924)),
// so the look that raises the view raises the gun.
void yaw_row(Model &m, int lod, int part, int yaw) {
	ThreediPartAnimation &r = m.add_panm(lod, part, 0, threedi_panm_pack_flags(0, 2, 1, 0));
	r.rotation_x = track(THREEDI_PANM_STYLE_CONTROL_REGISTER, static_cast<uint8_t>(yaw), 0, 0, 16384);
}

void yaw_pitch_row(Model &m, int lod, int part, int parent, int yaw, int pitch) {
	ThreediPartAnimation &r = m.add_panm(lod, part, parent, threedi_panm_pack_flags(0, 2, 1, 0));
	r.rotation_x = track(THREEDI_PANM_STYLE_CONTROL_REGISTER, static_cast<uint8_t>(yaw), 0, 0, 16384);
	r.rotation_y = track(THREEDI_PANM_STYLE_CONTROL_REGISTER, static_cast<uint8_t>(pitch), 0, 0, 16384);
}

// ---------------------------------------------------------------------------
Model make_tripod_usegun_cradle() {
	Model m;
	m.name = "tripod";
	const int lod = m.add_lod();
	const int mat = m.add_material("FF_ST_OP", "tripod.tga");
	const Box legs = box(-0.5, -0.5, 0.0, 0.5, 0.5, 0.1);
	const Box post = box(-0.1, -0.1, 0.1, 0.1, 0.1, 0.6);
	const Box cradle = box(-0.4, -0.2, 0.6, 0.2, 0.2, 0.8);
	const Box gun = box(-0.6, -0.08, 0.8, 1.4, 0.08, 0.95);
	const int p_base = m.add_part(lod, 0, Vec3{});
	m.add_box(lod, p_base, mat, legs);
	m.add_box(lod, p_base, mat, post);
	const int p_cradle = m.add_part(lod, 0, Vec3{0.0, 0.0, 0.6});
	m.add_box(lod, p_cradle, mat, cradle);
	const int p_gun = m.add_part(lod, 0, Vec3{0.0, 0.0, 0.85});
	m.add_box(lod, p_gun, mat, gun);
	m.add_control_register("EWEAP_GUNYAW");
	m.add_control_register("EWEAP_GUNPITCH");
	m.add_panm(lod, p_base, 0);
	yaw_row(m, lod, p_cradle, 0);
	yaw_pitch_row(m, lod, p_gun, 0, 0, 1);
	// E50triB's Usegun rides the cradle (part 1): the seat turns with the gun.
	m.add_user_point("Camera", Vec3{-0.45, 0.0, 1.3}, kForward, p_gun, kUserPointGameplay);
	m.add_user_point("Usegun", Vec3{-0.7, 0.0, 0.0}, kUp, p_cradle, kUserPointGameplay);
	m.add_user_point("BCasing", Vec3{0.15, -0.06, 0.9}, kRight, p_gun, kUserPointEffect);
	m.add_user_point("Bullet", Vec3{1.45, 0.0, 0.88}, kForward, p_gun, kUserPointEffect);
	m.add_user_point("MFlash01", Vec3{1.35, 0.0, 0.88}, kForward, p_gun, kUserPointEffect);
	solid_cobj(m, 0, Vec3{}, legs);
	solid_cobj(m, 0, Vec3{0.0, 0.0, 0.6}, cradle);
	solid_cobj(m, 0, Vec3{0.0, 0.0, 0.85}, gun);
	return m;
}

Model make_minigun() {
	Model m;
	m.name = "minigun";
	const int lod = m.add_lod();
	const int mat = m.add_material("FF_ST_OP", "minigun.tga");
	const Box post = box(-0.08, -0.08, 0.0, 0.08, 0.08, 0.3);
	const Box cradle = box(-0.2, -0.1, 0.3, 0.1, 0.1, 0.4);
	const Box receiver = box(-0.35, -0.09, 0.4, 0.25, 0.09, 0.6);
	// The barrel cluster around its own axis (y 0, z 0.5): an off-axis box, so a
	// roll about that axis turns it visibly.
	const Box barrel = box(0.25, -0.06, 0.44, 1.05, 0.06, 0.56);
	const Box rib = box(0.25, 0.06, 0.48, 1.05, 0.1, 0.52);
	const int p_base = m.add_part(lod, 0, Vec3{});
	m.add_box(lod, p_base, mat, post);
	const int p_cradle = m.add_part(lod, 0, Vec3{0.0, 0.0, 0.3});
	m.add_box(lod, p_cradle, mat, cradle);
	const int p_gun = m.add_part(lod, 0, Vec3{0.0, 0.0, 0.5});
	m.add_box(lod, p_gun, mat, receiver);
	const int p_barrel = m.add_part(lod, 0, Vec3{0.0, 0.0, 0.5});
	m.add_box(lod, p_barrel, mat, barrel);
	m.add_box(lod, p_barrel, mat, rib);
	m.add_control_register("EWEAP_GUNYAW");
	m.add_control_register("EWEAP_GUNPITCH");
	m.add_control_register("WEAP_SPIN");
	m.add_panm(lod, p_base, 0);
	yaw_row(m, lod, p_cradle, 0);
	yaw_pitch_row(m, lod, p_gun, 0, 0, 1);
	// The barrel turns with the gun (its own yaw and pitch tracks about the
	// gun's pivot, as every part of a flat rig carries its whole articulation)
	// and rolls a full turn per register wrap about its own axis (rotation z)
	// on WEAP_SPIN, the ewep class update's wrapping angle word.
	ThreediPartAnimation &spin = m.add_panm(lod, p_barrel, 0, threedi_panm_pack_flags(0, 2, 1, 0));
	spin.rotation_x = track(THREEDI_PANM_STYLE_CONTROL_REGISTER, 0, 0, 0, 16384);
	spin.rotation_y = track(THREEDI_PANM_STYLE_CONTROL_REGISTER, 1, 0, 0, 16384);
	spin.rotation_z = track(THREEDI_PANM_STYLE_CONTROL_REGISTER, 2, 0, 0, 16384);
	m.add_user_point("Camera", Vec3{-0.45, 0.0, 0.7}, kForward, p_gun, kUserPointGameplay);
	m.add_user_point("UseGun", Vec3{-0.85, 0.0, 0.0}, kUp, p_base, kUserPointGameplay);
	m.add_user_point("BCasing", Vec3{0.0, -0.1, 0.5}, kRight, p_gun, kUserPointEffect);
	m.add_user_point("Bullet", Vec3{1.1, 0.0, 0.5}, kForward, p_gun, kUserPointEffect);
	m.add_user_point("MFlash01", Vec3{1.06, 0.0, 0.5}, kForward, p_gun, kUserPointEffect);
	// Only the post collides: the gunner seated at UseGun stands clear of it,
	// so the seated body's eight-tick mounted resolve has nothing to push it
	// out of (the gun's swinging parts carry empty sections).
	solid_cobj(m, 0, Vec3{}, post);
	m.add_cobj(0, Vec3{0.0, 0.0, 0.3});
	m.add_cobj(0, Vec3{0.0, 0.0, 0.5});
	m.add_cobj(0, Vec3{0.0, 0.0, 0.5});
	return m;
}

Model make_fifty() {
	Model m;
	m.name = "fifty";
	const int lod = m.add_lod();
	const int mat = m.add_material("FF_ST_OP", "fifty.tga");
	const Box cradle = box(-0.25, -0.15, 0.0, 0.15, 0.15, 0.6);
	const Box gun = box(-0.4, -0.06, 0.6, 1.5, 0.06, 0.74);
	const int p_cradle = m.add_part(lod, 0, Vec3{});
	m.add_box(lod, p_cradle, mat, cradle);
	const int p_gun = m.add_part(lod, 0, Vec3{0.0, 0.0, 0.67});
	m.add_box(lod, p_gun, mat, gun);
	m.add_control_register("EWEAP_GUNYAW");
	m.add_control_register("EWEAP_GUNPITCH");
	yaw_row(m, lod, p_cradle, 0);
	yaw_pitch_row(m, lod, p_gun, 0, 0, 1);
	m.add_user_point("Camera", Vec3{0.03, 0.0, 1.5}, kForward, p_gun, kUserPointGameplay);
	m.add_user_point("Usegun", Vec3{-0.23, 0.0, 0.71}, kUp, p_cradle, kUserPointGameplay);
	m.add_user_point("BCasing", Vec3{0.59, -0.07, 0.68}, kRight, p_gun, kUserPointEffect);
	m.add_user_point("Bullet", Vec3{1.6, 0.0, 0.67}, kForward, p_gun, kUserPointEffect);
	m.add_user_point("MFlash01", Vec3{1.5, 0.0, 0.67}, kForward, p_gun, kUserPointEffect);
	solid_cobj(m, 0, Vec3{}, cradle);
	solid_cobj(m, 0, Vec3{0.0, 0.0, 0.67}, gun);
	return m;
}

Model make_cannon() {
	Model m;
	m.name = "cannon";
	const int lod = m.add_lod();
	const int mat = m.add_material("FF_ST_OP", "cannon.tga");
	const Box base = box(-0.3, -0.3, 0.0, 0.3, 0.3, 0.1);
	const Box cradle = box(-0.4, -0.3, 0.2, 0.4, 0.3, 0.45);
	const Box gun = box(-0.6, -0.12, 0.38, 2.6, 0.12, 0.58);
	const int p_base = m.add_part(lod, 0, Vec3{});
	m.add_box(lod, p_base, mat, base);
	const int p_cradle = m.add_part(lod, 0, Vec3{0.0, 0.0, 0.3});
	m.add_box(lod, p_cradle, mat, cradle);
	const int p_gun = m.add_part(lod, 0, Vec3{0.0, 0.0, 0.48});
	m.add_box(lod, p_gun, mat, gun);
	m.add_control_register("EWEAP_GUNYAW");
	m.add_control_register("EWEAP_GUNPITCH");
	m.add_panm(lod, p_base, 0);
	yaw_row(m, lod, p_cradle, 0);
	yaw_pitch_row(m, lod, p_gun, 0, 0, 1);
	m.add_user_point("bcasing", Vec3{-0.3, -0.13, 0.48}, kRight, p_gun, kUserPointEffect);
	m.add_user_point("bullet01", Vec3{2.7, 0.0, 0.48}, kForward, p_gun, kUserPointEffect);
	m.add_user_point("camera", Vec3{-0.85, 0.0, 0.6}, kForward, p_gun, kUserPointGameplay);
	m.add_user_point("mflash01", Vec3{2.62, 0.0, 0.48}, kForward, p_gun, kUserPointEffect);
	m.add_user_point("usegun", Vec3{-1.13, 0.0, 0.0}, kUp, p_base, kUserPointGameplay);
	solid_cobj(m, 0, Vec3{}, base);
	m.add_cobj(0, Vec3{0.0, 0.0, 0.3});
	m.add_cobj(0, Vec3{0.0, 0.0, 0.48});
	return m;
}

Model make_twingun() {
	Model m;
	m.name = "twingun";
	const int lod = m.add_lod();
	const int mat = m.add_material("FF_ST_OP", "twingun.tga");
	const Box mount = box(-0.15, -0.15, 0.0, 0.15, 0.15, 0.25);
	const Box cradle = box(-0.2, -0.3, 0.25, 0.2, 0.3, 0.4);
	const Box guns = box(-0.3, -0.28, 0.4, 1.7, 0.28, 0.6);
	const int p_mount = m.add_part(lod, 0, Vec3{});
	m.add_box(lod, p_mount, mat, mount);
	const int p_cradle = m.add_part(lod, 0, Vec3{0.0, 0.0, 0.3});
	m.add_box(lod, p_cradle, mat, cradle);
	const int p_gun = m.add_part(lod, 0, Vec3{0.0, 0.0, 0.5});
	m.add_box(lod, p_gun, mat, guns);
	m.add_control_register("EWEAP_GUNYAW");
	m.add_control_register("EWEAP_GUNPITCH");
	m.add_panm(lod, p_mount, 0);
	yaw_row(m, lod, p_cradle, 0);
	yaw_pitch_row(m, lod, p_gun, 0, 0, 1);
	m.add_user_point("bullet01", Vec3{1.75, -0.19, 0.5}, kForward, p_gun, kUserPointEffect);
	m.add_user_point("bullet02", Vec3{1.75, 0.19, 0.5}, kForward, p_gun, kUserPointEffect);
	m.add_user_point("mflash01", Vec3{1.72, -0.19, 0.5}, kForward, p_gun, kUserPointEffect);
	m.add_user_point("mflash02", Vec3{1.72, 0.19, 0.5}, kForward, p_gun, kUserPointEffect);
	m.add_user_point("bcasing02", Vec3{0.03, 0.35, 0.45}, kLeft, p_gun, kUserPointEffect);
	m.add_user_point("camera", Vec3{1.34, 0.0, 0.8}, kForward, p_gun, kUserPointGameplay);
	m.add_user_point("bcasing01", Vec3{0.03, -0.36, 0.45}, kRight, p_gun, kUserPointEffect);
	// The gunner sits in the cockpit well behind the chin turret.
	m.add_user_point("usegun", Vec3{-2.3, 0.4, 0.77}, kForward, p_mount, kUserPointGameplay);
	solid_cobj(m, 0, Vec3{}, mount);
	m.add_cobj(0, Vec3{0.0, 0.0, 0.3});
	m.add_cobj(0, Vec3{0.0, 0.0, 0.5});
	return m;
}

Model make_helo() {
	Model m;
	m.name = "helo";
	const int lod = m.add_lod();
	const int paint = m.add_material("FF_ST_OP", "helo.tga");
	const Box skids = box(-2.0, -1.2, 0.0, 2.0, 1.2, 0.3);
	const Box cabin = box(-2.5, -1.0, 0.3, 2.5, 1.0, 2.3);
	const Box boom = box(-7.0, -0.25, 1.5, -2.5, 0.25, 2.0);
	const Box rotor = box(-5.0, -0.2, 2.6, 5.0, 0.2, 2.7);
	const Box tail = box(-7.1, -0.4, 1.0, -6.9, 0.4, 2.6);
	const int p_hull = m.add_part(lod, 0, Vec3{});
	m.add_box(lod, p_hull, paint, skids);
	m.add_box(lod, p_hull, paint, cabin);
	m.add_box(lod, p_hull, paint, boom);
	const int p_rotor = m.add_part(lod, 0, Vec3{0.0, 0.0, 2.65});
	m.add_box(lod, p_rotor, paint, rotor);
	const int p_tail = m.add_part(lod, 0, Vec3{-7.0, 0.0, 1.8});
	m.add_box(lod, p_tail, paint, tail);
	m.add_control_register("HELO_ROTOR");
	m.add_control_register("HELO_TAILROTOR");
	m.add_panm(lod, p_hull, 0);
	yaw_row(m, lod, p_rotor, 0);
	ThreediPartAnimation &t = m.add_panm(lod, p_tail, 0, threedi_panm_pack_flags(0, 2, 1, 0));
	t.rotation_y = track(THREEDI_PANM_STYLE_CONTROL_REGISTER, 1, 0, 0, 16384);
	m.add_user_point("ctrlx05", Vec3{1.5, 0.45, 0.9}, kForward, p_hull, kUserPointGameplay);
	// The door anchors stand out of the cabin as Dblkhwk1's do (x +-1.9): the
	// two gunners at their guns' UseGun points sit 2.2 apart, clear of each other.
	m.add_user_point("Ewep01", Vec3{0.3, 1.95, 1.5}, kLeft, p_hull, kUserPointGameplay);
	m.add_user_point("Ewep02", Vec3{0.3, -1.95, 1.5}, kRight, p_hull, kUserPointGameplay);
	m.add_user_point("ground", Vec3{}, kUp, p_hull, kUserPointGameplay);
	m.add_user_point("sitex00A", Vec3{1.5, -0.45, 0.9}, kForward, p_hull, kUserPointGameplay);
	m.add_user_point("sitex00B", Vec3{-1.2, 0.6, 0.6}, kLeft, p_hull, kUserPointGameplay);
	m.add_user_point("sitex00C", Vec3{-1.2, -0.6, 0.6}, kRight, p_hull, kUserPointGameplay);
	m.add_user_point("BULLET01", Vec3{2.9, -1.2, 0.6}, kForward, p_hull, kUserPointEffect);
	m.add_user_point("MFLASH01", Vec3{2.8, -1.2, 0.6}, kForward, p_hull, kUserPointEffect);
	m.add_user_point("bcasing01", Vec3{2.4, -1.2, 0.6}, kRight, p_hull, kUserPointEffect);
	m.add_user_point("FX00", Vec3{-2.0, 0.0, 2.2}, kBackward, -1, kUserPointEffect);
	// The other rocket pods: the attack helicopters fire their own weapon from
	// two (Dka501: bullet01/02) or four (Dapche1: bullet01..04) stub-wing points.
	m.add_user_point("BULLET02", Vec3{2.9, 1.2, 0.6}, kForward, p_hull, kUserPointEffect);
	m.add_user_point("MFLASH02", Vec3{2.8, 1.2, 0.6}, kForward, p_hull, kUserPointEffect);
	m.add_user_point("bcasing02", Vec3{2.4, 1.2, 0.6}, kLeft, p_hull, kUserPointEffect);
	m.add_user_point("BULLET03", Vec3{2.9, -0.9, 0.5}, kForward, p_hull, kUserPointEffect);
	m.add_user_point("MFLASH03", Vec3{2.8, -0.9, 0.5}, kForward, p_hull, kUserPointEffect);
	m.add_user_point("BULLET04", Vec3{2.9, 0.9, 0.5}, kForward, p_hull, kUserPointEffect);
	m.add_user_point("MFLASH04", Vec3{2.8, 0.9, 0.5}, kForward, p_hull, kUserPointEffect);
	// The hull's collision is the cabin floor, so the seats above it stand clear
	// of the hull's own volumes.
	solid_cobj(m, 0, Vec3{}, box(-2.5, -1.0, 0.3, 2.5, 1.0, 0.8));
	solid_cobj(m, 0, Vec3{0.0, 0.0, 2.65}, rotor);
	solid_cobj(m, 0, Vec3{-7.0, 0.0, 1.8}, tail);
	return m;
}

Model make_gunboat() {
	Model m;
	m.name = "gunboat";
	const int lod = m.add_lod();
	const int paint = m.add_material("FF_ST_OP", "gunboat.tga");
	const Box hull = box(-8.0, -2.0, 0.0, 9.0, 2.0, 1.8);
	const Box house = box(-1.0, -1.2, 1.8, 4.0, 1.2, 3.2);
	const int p_hull = m.add_part(lod, 0, Vec3{});
	m.add_box(lod, p_hull, paint, hull);
	m.add_box(lod, p_hull, paint, house);
	m.add_panm(lod, p_hull, 0);
	m.add_user_point("ctrlx22", Vec3{3.0, 0.8, 2.0}, kForward, p_hull, kUserPointGameplay);
	m.add_user_point("ewep01", Vec3{6.0, 1.6, 1.9}, kLeft, p_hull, kUserPointGameplay);
	// Two anchors tilted off the hull plane (Dmrk51's carry up to 0.0176 of up;
	// these carry more, so a yaw-only conversion misses them by degrees).
	m.add_user_point("ewep02", Vec3{-3.0, 1.6, 1.9}, Vec3{0.0, 0.9950, 0.0998}, p_hull, kUserPointGameplay);
	m.add_user_point("ewep03", Vec3{6.0, -1.6, 1.9}, Vec3{0.0995, -0.9900, 0.0995}, p_hull, kUserPointGameplay);
	m.add_user_point("ewep04", Vec3{-3.0, -1.6, 1.9}, kRight, p_hull, kUserPointGameplay);
	m.add_user_point("FX00", Vec3{-8.1, 0.0, 0.2}, kBackward, -1, kUserPointEffect);
	solid_cobj(m, 0, Vec3{}, hull);
	return m;
}

Model make_tank_turret() {
	Model m;
	m.name = "tank_turret";
	const int lod = m.add_lod();
	const int armor = m.add_material("FF_ST_OP", "tank.tga");
	const Box hull = box(-3.0, -1.5, 0.3, 3.0, 1.5, 1.3);
	const Box track_l = box(-3.0, 1.5, 0.0, 3.0, 1.9, 1.0);
	const Box track_r = box(-3.0, -1.9, 0.0, 3.0, -1.5, 1.0);
	const Box ring = box(-1.0, -1.0, 1.3, 1.0, 1.0, 1.5);
	const int p_hull = m.add_part(lod, 0, Vec3{});
	m.add_box(lod, p_hull, armor, hull);
	m.add_box(lod, p_hull, armor, track_l);
	m.add_box(lod, p_hull, armor, track_r);
	const int p_ring = m.add_part(lod, 0, Vec3{0.0, 0.0, 1.3});
	m.add_box(lod, p_ring, armor, ring);
	m.add_control_register("VEHICLE_GUNYAW");
	m.add_control_register("VEHICLE_WHEELS00");
	m.add_panm(lod, p_hull, 0);
	yaw_row(m, lod, p_ring, 0);
	m.add_user_point("ctrlx25", Vec3{1.5, 0.0, 0.7}, kForward, p_hull, kUserPointGameplay);
	// The turret child rides the hull at its ring; the roof gun rides the ring.
	m.add_user_point("ewep01", Vec3{0.0, 0.0, 1.5}, kForward, p_hull, kUserPointGameplay);
	m.add_user_point("ewep02", Vec3{-0.6, 0.5, 2.9}, kForward, p_ring, kUserPointGameplay);
	m.add_user_point("fx00", Vec3{-3.7, 0.9, 0.6}, kBackward, p_hull, kUserPointEffect);
	m.add_user_point("ground", Vec3{}, kUp, p_hull, kUserPointGameplay);
	const int c_hull = m.add_cobj(0);
	m.add_face_box(c_hull, hull);
	m.add_volume(c_hull, 1, 0, hull);
	m.add_volume(c_hull, 19, 0, track_l);
	m.add_volume(c_hull, 19, 0, track_r);
	solid_cobj(m, 0, Vec3{0.0, 0.0, 1.3}, ring);
	return m;
}

Model make_lcac() {
	Model m;
	m.name = "lcac";
	const int lod = m.add_lod();
	const int paint = m.add_material("FF_ST_OP", "lcac.tga");
	const Box hull = box(-13.0, -6.0, 0.0, 13.0, 6.0, 1.2);
	const Box cab = box(6.0, 4.5, 1.2, 10.0, 6.0, 4.0);
	const int p_hull = m.add_part(lod, 0, Vec3{});
	m.add_box(lod, p_hull, paint, hull);
	m.add_box(lod, p_hull, paint, cab);
	m.add_panm(lod, p_hull, 0);
	m.add_user_point("ctrlx11", Vec3{8.7, 5.25, 1.4}, kForward, p_hull, kUserPointGameplay);
	m.add_user_point("sitex00", Vec3{7.6, 5.25, 1.4}, kForward, p_hull, kUserPointGameplay);
	m.add_user_point("FX00", Vec3{-13.1, 0.0, 0.5}, kBackward, -1, kUserPointEffect);
	const int c_hull = m.add_cobj(0);
	m.add_face_box(c_hull, hull);
	m.add_volume(c_hull, 1, 0, hull);
	m.add_face_box(c_hull, cab);
	m.add_volume(c_hull, 1, 0, cab);
	return m;
}

struct Recipe {
	const char *file;
	Model (*make)();
};

const std::vector<Recipe> &recipes() {
	static const std::vector<Recipe> kRecipes = {
			{"tripod_usegun_cradle", make_tripod_usegun_cradle},
			{"minigun", make_minigun},
			{"fifty", make_fifty},
			{"cannon", make_cannon},
			{"twingun", make_twingun},
			{"helo", make_helo},
			{"gunboat", make_gunboat},
			{"tank_turret", make_tank_turret},
			{"lcac", make_lcac},
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
	int point(const char *name) const {
		for (size_t i = 0; i < model.user_point_count; ++i)
			if (!std::strcmp(model.user_points[i].name, name)) return static_cast<int>(i);
		return -1;
	}
	int sub(const char *name) const {
		const int i = point(name);
		return i < 0 ? -2 : model.user_points[i].subobject_index;
	}
	size_t parts() const { return model.lod_count ? model.lods[0].render_object_count : 0; }
	bool safe() const { return model.collision == nullptr || threedi_3di3_collision_is_runtime_safe(model.collision) == 1; }
};

void check_facts(const std::string &name, const std::vector<uint8_t> &bytes) {
	Parsed p(bytes);
	if (!expect(p.ok, name + ": committed bytes parse")) return;
	expect(p.safe(), name + ": collision block is runtime safe");
	if (name == "tripod_usegun_cradle") {
		expect(p.parts() == 3 && !std::strcmp(p.reg(0), "EWEAP_GUNYAW") && p.sub("Usegun") == 1 && p.sub("MFlash01") == 2,
				name + ": Usegun on the cradle, the muzzle on the gun");
	} else if (name == "minigun") {
		expect(p.parts() == 4 && !std::strcmp(p.reg(2), "WEAP_SPIN") && p.sub("MFlash01") == 2 && p.sub("UseGun") == 0,
				name + ": the spinning barrel's gun");
	} else if (name == "fifty") {
		expect(p.parts() == 2 && !std::strcmp(p.reg(1), "EWEAP_GUNPITCH") && p.sub("Camera") == 1 && p.sub("Usegun") == 0,
				name + ": the H50cal shape");
	} else if (name == "cannon") {
		expect(p.parts() == 3 && !std::strcmp(p.reg(1), "EWEAP_GUNPITCH") && p.sub("camera") == 2 && p.sub("usegun") == 0,
				name + ": the M1trret shape");
	} else if (name == "twingun") {
		expect(p.parts() == 3 && p.sub("bullet01") == 2 && p.sub("bullet02") == 2 && p.sub("usegun") == 0,
				name + ": the Ka50gun shape");
	} else if (name == "helo") {
		expect(p.parts() == 3 && !std::strcmp(p.reg(0), "HELO_ROTOR") && !std::strcmp(p.reg(1), "HELO_TAILROTOR") &&
						p.point("ctrlx05") == 0 && p.point("Ewep01") >= 0 && p.point("Ewep02") >= 0,
				name + ": rotors, pilot and door anchors");
	} else if (name == "gunboat") {
		expect(p.parts() == 1 && p.point("ewep01") >= 0 && p.point("ewep04") >= 0 && p.model.ctrl.count == 0,
				name + ": four anchors, no CTRL");
	} else if (name == "tank_turret") {
		expect(p.parts() == 2 && p.sub("ewep01") == 0 && p.sub("ewep02") == 1 && !std::strcmp(p.reg(0), "VEHICLE_GUNYAW"),
				name + ": the hull ring anchor and the ring's roof anchor");
	} else if (name == "lcac") {
		expect(p.parts() == 1 && p.point("ctrlx11") == 0 && p.sub("ctrlx11") == 0 && p.model.ctrl.count == 0,
				name + ": one deck hull, the pilot on it");
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
	if (failures == 0 && !write_mode) std::printf("OK: fixtures/gut byte-reproducible\n");
	return failures == 0 ? 0 : 1;
}
