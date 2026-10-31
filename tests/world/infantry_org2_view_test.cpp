// The player body's (org2) view phase, the kernel the authority's body tick and a client's
// decoded rows share (world/infantry_view.h), and the authority's seated body running it.
// - D-NET-1196: the skip branch (Flags 0x100060) writes the legs alone; the canopy's chase
//   is what moves the re-plant targets.
// - D-NET-1197: a seated player body runs the phase: its Flags 0x40 holds the Yaw 120 degrees
//   about the body heading its last seat carry left, unless its mount's config is 3 or its ground
//   (the mount) is a crashed vehicle; its prone local clears.
// - D-NET-1198: the pitch clamp is skipped while the mount is a crashed vehicle.
// - D-NET-1199: the local body in a control seat halves its Pitch under the chase camera.
// - D-NET-1225: the dead body's corpse leg after its dead tail (its timer, its 186 decay, the
//   LeaveCorpse keep, section bit 0's destroy).
// - D-NET-1227 / -58: the local body's rain ambient ahead of the phase, dead or alive; the
//   stance sound after the on-foot site.
// - The local body's reverb preset on the 16-tick slow pass.
// [orig: Entity_UpdateInfantryPlayerBody @0x4B40E0 - the canopy chase @0x4B4945..0x4B496C, the
//  skip test @0x4B4972..0x4B4978, the mount config test @0x4B4AC6..0x4B4ADE, the ground test
//  @0x4B4AE0..0x4B4AF9, the clamp @0x4B4AFB..0x4B4B59, the legs @0x4B4B5F..0x4B4B6B, the leg model
//  @0x4B497E..0x4B4ABB, the limit select @0x4B4B71..0x4B4B7D, the mount's crash test
//  @0x4B4B82..0x4B4BA0, the pitch clamp @0x4B4BA2..0x4B4BC6; the prone local @0x4B416C..0x4B4183,
//  its clear @0x4B46FC..0x4B471B; the ground = mount store @0x4B41A2..0x4B41B4; the chase camera's
//  halving @0x4B4921..0x4B4942]
// Core-only: every value is the test's own; the World is heap-held.
#include <base/io/bam.h>
#include "common/effect_producers.h"
#include <formats/def/def.h>
#include <runtime/world/ai.h>
#include <runtime/world/entity.h>
#include <runtime/world/entity_commands.h>
#include <runtime/world/infantry.h>
#include <runtime/world/infantry_internal.h> // player_body_dead_tail
#include <runtime/world/infantry_view.h>
#include <runtime/world/local_player.h>
#include <runtime/world/vehicle_motor.h> // stamp_saved_live_pose
#include <runtime/world/world.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include "../common/title_fixture.h"

using namespace opennova;
namespace w = opennova::world;

static int failures = 0;
#define CHECK(c)                                                                       \
	do {                                                                               \
		if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
	} while (0)

namespace {

constexpr int32_t kYawLimit = 0x55555500;
constexpr int32_t kPitchLimit = 0x38E38E00;
constexpr int32_t kPronePitchLimit = 0x1C71C700;

struct Body {
	int32_t yaw = 0;
	int32_t pitch = 0;
	int32_t body_heading = 0;
	int32_t leg_yaw[2] = {};
	int32_t leg_target[2] = {};
	int32_t body_pitch = 0;
	w::Org2ViewWords words() {
		return w::Org2ViewWords{yaw, pitch, body_heading, leg_yaw, leg_target, body_pitch};
	}
};

// D-NET-1196: on a ladder (or seated, or under a canopy) both legs take the body heading and
// the re-plant targets stay as they stand [orig: @0x4B4B5F..0x4B4B6B].
void test_skip_branch_writes_the_legs_alone() {
	for (const uint32_t flags : {w::kEntityFlagLadderContact, w::kEntityFlagMounted}) {
		Body b;
		b.body_heading = 0x10000000;
		b.yaw = 0x20000000;
		b.leg_yaw[0] = 0x01000000;
		b.leg_yaw[1] = 0x02000000;
		b.leg_target[0] = 0x03000000;
		b.leg_target[1] = 0x04000000;
		w::Org2ViewGates g;
		g.flags = flags;
		w::infantry_org2_view_phase(b.words(), g);
		CHECK(b.leg_yaw[0] == 0x10000000 && b.leg_yaw[1] == 0x10000000);
		CHECK(b.leg_target[0] == 0x03000000 && b.leg_target[1] == 0x04000000);
		CHECK(b.body_heading == 0x10000000);
		CHECK(b.yaw == 0x20000000); // 45 degrees off: inside the clamp
	}
}

// D-NET-1196: under a canopy the body heading chases a sixteenth of the way to the Yaw and both
// targets take it; the skip branch then puts the legs on it [orig: @0x4B494D..0x4B496C].
void test_canopy_moves_the_targets() {
	Body b;
	b.body_heading = 0;
	b.yaw = 0x10000000;
	b.leg_target[0] = 0x03000000;
	b.leg_target[1] = 0x04000000;
	w::Org2ViewGates g;
	g.flags = w::kEntityFlagParachute;
	w::infantry_org2_view_phase(b.words(), g);
	static constexpr int32_t kChased = (0x10000000 + 8) >> 4;
	CHECK(b.body_heading == kChased);
	CHECK(b.leg_target[0] == kChased && b.leg_target[1] == kChased);
	CHECK(b.leg_yaw[0] == kChased && b.leg_yaw[1] == kChased);
}

// The skip branch's two-sided clamp about the body heading, and its skip
// [orig: @0x4B4AFB..0x4B4B59].
void test_yaw_clamp_and_its_skip() {
	for (const int32_t off : {0x70000000, -0x70000000}) {
		for (const bool skipped : {false, true}) {
			Body b;
			b.body_heading = 0x08000000;
			b.yaw = io::bam_add(b.body_heading, off);
			w::Org2ViewGates g;
			g.flags = w::kEntityFlagMounted;
			g.yaw_clamp_skipped = skipped;
			w::infantry_org2_view_phase(b.words(), g);
			const int32_t want = skipped ? io::bam_add(0x08000000, off)
					: off > 0 ? io::bam_add(0x08000000, kYawLimit)
					          : io::bam_sub(0x08000000, kYawLimit);
			CHECK(b.yaw == want);
		}
	}
}

// The pitch clamp about savedLivePitch, its prone limit, both sides, and the crashed mount's skip
// [orig: @0x4B4B71..0x4B4BC6].
void test_pitch_clamp_and_its_skip() {
	for (const int32_t off : {0x40000000, -0x40000000}) {
		for (const bool prone : {false, true}) {
			for (const bool skipped : {false, true}) {
				Body b;
				b.body_pitch = 0x02000000;
				b.pitch = io::bam_add(b.body_pitch, off);
				w::Org2ViewGates g;
				g.flags = w::kEntityFlagMounted;
				g.prone = prone;
				g.pitch_clamp_skipped = skipped;
				w::infantry_org2_view_phase(b.words(), g);
				const int32_t limit = prone ? kPronePitchLimit : kPitchLimit;
				const int32_t want = skipped ? io::bam_add(0x02000000, off)
						: off > 0 ? io::bam_add(0x02000000, limit)
						          : io::bam_sub(0x02000000, limit);
				CHECK(b.pitch == want);
			}
		}
	}
}

// On foot the legs chase the Yaw (a movement state re-plants both targets on it) and the body
// heading is their midpoint [orig: @0x4B497E..0x4B4ABB].
void test_on_foot_leg_model() {
	Body b;
	b.yaw = 0x01000000;
	w::Org2ViewGates g;
	g.anim_state = 1; // flag-table bit 0: a movement state
	w::infantry_org2_view_phase(b.words(), g);
	CHECK(b.leg_target[0] == 0x01000000 && b.leg_target[1] == 0x01000000);
	static constexpr int32_t kStep = (0x01000000 + 2) >> 2;
	CHECK(b.leg_yaw[0] == kStep && b.leg_yaw[1] == kStep);
	CHECK(b.body_heading == kStep);
}

// The prologue's prone local: the prone order with none of Flags 0x10A000, cleared by a living
// body's def'd mount or by Flags 0x40 [orig: @0x4B416C..0x4B4183, @0x4B46FC..0x4B471B].
void test_prone_local() {
	CHECK(w::infantry_org2_prone_local(true, 0u, false));
	CHECK(!w::infantry_org2_prone_local(false, 0u, false));
	CHECK(!w::infantry_org2_prone_local(true, w::kEntityFlagInAir, false));
	CHECK(!w::infantry_org2_prone_local(true, w::kEntityFlagDrowning, false));
	CHECK(!w::infantry_org2_prone_local(true, w::kEntityFlagLadderContact, false));
	CHECK(!w::infantry_org2_prone_local(true, 0u, true));
	CHECK(!w::infantry_org2_prone_local(true, w::kEntityFlagMounted, false));
}

enum class Mount { kPlain, kConfig3, kCrashed };

// D-NET-1197 / D-NET-1198 through the authority's body tick: a local player seated in a
// vehicle's passenger seat (the attach sets Flags 0x40). The first tick's seat carry leaves the
// body heading and savedLivePitch; the second tick's phase clamps the look about them, ahead of the
// seat pose that reads it.
void test_seated_player_runs_the_phase(Mount kind) {
	auto wp = std::make_unique<w::World>();
	test_titles::jo_world(*wp);
	w::World &world = *wp;
	world.registry.configure_pool(0, 16);
	world.registry.configure_pool(1, 16);
	w::Entity vehicle;
	vehicle.net_id = 200;
	vehicle.kind = w::EntityKind::Item;
	vehicle.has_item_def = true;
	vehicle.has_graphic_model = true;
	vehicle.item_type = 1;
	vehicle.position = {10.f, 20.f, 5.f};
	w::Seat seat;
	seat.type = w::SeatType::Passenger;
	vehicle.seats.push_back(seat);
	if (kind == Mount::kConfig3) {
		vehicle.emplaced_config_valid = true;
		vehicle.emplaced_config = 3;
	}
	if (kind == Mount::kCrashed) vehicle.veh.crashed = 1;
	const w::EntityHandle vh = world.registry.spawn(1, vehicle);
	w::Entity body;
	body.net_id = 100;
	body.kind = w::EntityKind::Organic;
	body.health = 100;
	body.alive = true;
	const w::EntityHandle bh = world.registry.spawn(0, body);
	w::AiSystem &ai = world.ai;
	w::AiEntity *ae = ai.at(ai.attach(bh));
	CHECK(ae != nullptr);
	if (ae == nullptr) return;
	ae->inf.active = true;
	ae->inf.is_local_player = true;
	ae->inf.stance = w::InfantryState::Stance::kProne;
	// By handle: a local player body answers an SSN lookup by its DcbId (10000 + its slot byte),
	// not its record word (entity_dcb_id, D-NET-899).
	CHECK(world.commands.mount(bh, vh));
	const w::Entity *occ = world.registry.get(bh);
	CHECK(occ != nullptr && occ->mounted &&
			((occ->flags | occ->engine_flags) & w::kEntityFlagMounted) != 0);
	ai.tick_infantry(*ae, world, 1);
	const int32_t seat_heading = ae->inf.body_heading;
	const int32_t seat_pitch = ae->body_pitch;
	ae->inf.target_heading = io::bam_add(seat_heading, 0x70000000);
	ae->inf.look_pitch = io::bam_add(seat_pitch, 0x40000000);
	ai.tick_infantry(*ae, world, 2);
	const int32_t yaw_want = kind == Mount::kPlain
			? io::bam_add(seat_heading, kYawLimit)
			: io::bam_add(seat_heading, 0x70000000);
	// Seated, the prone local is cleared: the 80 degree limit.
	const int32_t pitch_want = kind == Mount::kCrashed ? io::bam_add(seat_pitch, 0x40000000)
	                                                   : io::bam_add(seat_pitch, kPitchLimit);
	CHECK(ae->inf.target_heading == yaw_want);
	CHECK(ae->heading == yaw_want);
	CHECK(ae->inf.look_pitch == pitch_want);
	CHECK(ae->pitch == pitch_want);
	occ = world.registry.get(bh);
	CHECK(occ != nullptr && occ->heading == yaw_want);
}

// D-NET-1211: the ride's turn. The yaw delta turns the Yaw and the local look unless the
// mount is IsTurret, then the body heading and the four leg words either way; the turned pitch
// delta goes into savedLivePitch, the pitch lag and the aim pitch, the roll delta into Roll
// [orig: @0x4B5683..0x4B5723].
void test_ride_turn() {
	for (const bool turret : {false, true}) {
		for (const bool local : {false, true}) {
			int32_t yaw = 0x10000000, look = 0x20000000, body_heading = 0x30000000;
			int32_t legs[2] = {1, 2}, targets[2] = {3, 4};
			int32_t body_pitch = 5, lag = 6, aim = 7, roll = 8;
			const w::Org2RideWords words{yaw, local ? &look : nullptr, body_heading, legs, targets,
					body_pitch, lag, aim, roll};
			static constexpr int32_t kDyaw = 0x01000000;
			w::infantry_org2_ride_turn(words, kDyaw, 0x100, 0x200, turret);
			CHECK(yaw == (turret ? 0x10000000 : 0x11000000));
			CHECK(look == (turret || !local ? 0x20000000 : 0x21000000));
			CHECK(body_heading == 0x31000000);
			CHECK(legs[0] == kDyaw + 1 && legs[1] == kDyaw + 2);
			CHECK(targets[0] == kDyaw + 3 && targets[1] == kDyaw + 4);
			CHECK(body_pitch == 0x105 && lag == 0x106 && aim == 0x107 && roll == 0x208);
		}
	}
}

// D-NET-1211: the driver's hold gate and the hold [orig: @0x4B572E..0x4B57AA].
void test_ride_hold() {
	static constexpr uint32_t kPlayerControl = 0x40u;
	CHECK(w::infantry_org2_ride_holds(true, kPlayerControl, 0u, false));
	CHECK(!w::infantry_org2_ride_holds(false, kPlayerControl, 0u, false));
	CHECK(!w::infantry_org2_ride_holds(true, 0u, 0u, false));
	CHECK(!w::infantry_org2_ride_holds(true, kPlayerControl, 0x10u, false));
	CHECK(w::infantry_org2_ride_holds(true, kPlayerControl, 0x08u, false));
	CHECK(!w::infantry_org2_ride_holds(true, kPlayerControl, 0u, true));
	int32_t yaw = 0x11000000, look = 0x21000000, body_heading = 0, body_pitch = 0, lag = 0;
	int32_t aim = 0, roll = 0, legs[2] = {}, targets[2] = {};
	const w::Org2RideWords words{yaw, &look, body_heading, legs, targets, body_pitch, lag, aim, roll};
	w::infantry_org2_ride_hold(words, 0x10000000, 0x20000000, 0x00400000);
	CHECK(yaw == 0x10400000 && look == 0x20400000);
}

// D-NET-1212: the pitch lag's step, a rounded thirty-second, its remainder pulled one toward
// zero when negative [orig: @0x4B57CD..0x4B57E5].
void test_pitch_lag_step() {
	int32_t lag = 0x40000000;
	CHECK(w::infantry_org2_pitch_lag_step(lag) == 0x02000000 && lag == 0x3E000000);
	lag = -3200;
	CHECK(w::infantry_org2_pitch_lag_step(lag) == -100 && lag == -3099);
	lag = 15;
	CHECK(w::infantry_org2_pitch_lag_step(lag) == 0 && lag == 15);
}

enum class Ride { kPassenger, kTurret, kDriver, kDriverFreeLook, kCrashed };

// D-NET-1211 / D-NET-1212 through the authority's body tick: a local player seated on
// a vehicle that turns 0x1000000 and pitches 0x1000000 between its first and second ticks (its
// mover-entry stamp at yaw and pitch 0). The second tick's ride, its ground the mount, turns the
// look with the vehicle; under an IsTurret mount it does not; in the drvrx seat of a PlayerControl
// def it holds the look unless the MoveOrder carries the free-look bit. The look faces the
// vehicle's turned Yaw, so the whole pitch delta lands in the pitch lag and the aim pitch; the
// lag's first thirty-second steps into the Pitch, unless the vehicle is crashed.
void test_seated_player_rides_its_mount(Ride kind) {
	auto wp = std::make_unique<w::World>();
	test_titles::jo_world(*wp);
	w::World &world = *wp;
	world.registry.configure_pool(0, 16);
	world.registry.configure_pool(1, 16);
	w::Entity vehicle;
	vehicle.net_id = 200;
	vehicle.kind = w::EntityKind::Item;
	vehicle.has_item_def = true;
	vehicle.has_graphic_model = true;
	vehicle.item_type = 1;
	vehicle.position = {10.f, 20.f, 5.f};
	const bool driver = kind == Ride::kDriver || kind == Ride::kDriverFreeLook;
	if (driver) vehicle.item_attrib = w::kItemAttribPlayerControl;
	if (kind == Ride::kTurret) vehicle.item_attrib2 = def::DEF_ITEM_ATTRIB2_ISTURRET;
	if (kind == Ride::kCrashed) vehicle.veh.crashed = 1;
	w::Seat seat;
	seat.type = driver ? w::SeatType::Driver : w::SeatType::Passenger;
	vehicle.seats.push_back(seat);
	const w::EntityHandle vh = world.registry.spawn(1, vehicle);
	w::Entity body;
	body.net_id = 100;
	body.kind = w::EntityKind::Organic;
	body.health = 100;
	body.alive = true;
	const w::EntityHandle bh = world.registry.spawn(0, body);
	w::AiSystem &ai = world.ai;
	w::AiEntity *ae = ai.at(ai.attach(bh));
	CHECK(ae != nullptr);
	if (ae == nullptr) return;
	ae->inf.active = true;
	ae->inf.is_local_player = true;
	CHECK(world.commands.mount(bh, vh));
	ai.tick_infantry(*ae, world, 1);
	w::Entity *veh = world.registry.get(vh);
	w::Entity *occ = world.registry.get(bh);
	CHECK(veh != nullptr && occ != nullptr && occ->ground_target == vh);
	if (veh == nullptr || occ == nullptr) return;
	CHECK((veh->primary_occupant == bh) == driver);
	w::stamp_saved_live_pose(*veh);
	veh->heading = io::bam_add(veh->heading, 0x01000000);
	veh->pitch_bam = io::bam_add(veh->pitch_bam, 0x01000000);
	// The look faces the turned vehicle: the ride turns the pitch delta by the vehicle's Yaw less
	// the body's, 0, so the whole delta lands.
	const int32_t look0 = veh->heading;
	ae->inf.target_heading = look0;
	ae->inf.look_pitch = 0;
	ae->inf.carrier_pitch_lag = 0;
	ae->inf.aim_pitch = 0;
	if (kind == Ride::kDriverFreeLook) occ->net_move_input |= w::Entity::kMoveOrderFreeLook;
	ai.tick_infantry(*ae, world, 2);
	const bool turned = kind == Ride::kPassenger || kind == Ride::kDriverFreeLook ||
			kind == Ride::kCrashed;
	const int32_t yaw_want = turned ? io::bam_add(look0, 0x01000000) : look0;
	CHECK(ae->inf.target_heading == yaw_want);
	CHECK(ae->heading == yaw_want);
	CHECK(occ->heading == yaw_want);
	const bool crashed = kind == Ride::kCrashed;
	CHECK(ae->inf.carrier_pitch_lag == (crashed ? 0x01000000 : 0x00F80000));
	CHECK(ae->inf.look_pitch == (crashed ? 0 : 0x00080000));
	CHECK(ae->pitch == ae->inf.look_pitch);
	CHECK(ae->inf.aim_pitch == 0x01000000);
}

// D-NET-1213: the recoil follows the view phase. A seated local player looking past both of
// the skip branch's clamps with a recoil accumulator of 1 << 18 is clamped first, then kicked: the
// Pitch ends 0x1000 past its 80 degree clamp and the Yaw 0x4000 off its 120 degree one (the sign
// the PRNG draw picks). The old order (kick, then clamp) left both on the clamp.
// [orig: Entity_UpdateInfantryPlayerBody @0x4B40E0 - the phase @0x4B4945..0x4B4BC6, the recoil
//  @0x4B57EB..0x4B5848]
void test_kick_follows_the_view_clamp() {
	auto wp = std::make_unique<w::World>();
	test_titles::jo_world(*wp);
	w::World &world = *wp;
	world.registry.configure_pool(0, 16);
	world.registry.configure_pool(1, 16);
	w::Entity vehicle;
	vehicle.net_id = 200;
	vehicle.kind = w::EntityKind::Item;
	vehicle.has_item_def = true;
	vehicle.has_graphic_model = true;
	vehicle.item_type = 1;
	w::Seat seat;
	seat.type = w::SeatType::Passenger;
	vehicle.seats.push_back(seat);
	const w::EntityHandle vh = world.registry.spawn(1, vehicle);
	w::Entity body;
	body.net_id = 100;
	body.kind = w::EntityKind::Organic;
	body.health = 100;
	body.alive = true;
	const w::EntityHandle bh = world.registry.spawn(0, body);
	w::AiSystem &ai = world.ai;
	w::AiEntity *ae = ai.at(ai.attach(bh));
	CHECK(ae != nullptr);
	if (ae == nullptr) return;
	ae->inf.active = true;
	ae->inf.is_local_player = true;
	CHECK(world.commands.mount(bh, vh));
	ai.tick_infantry(*ae, world, 1);
	const int32_t seat_heading = ae->inf.body_heading;
	const int32_t seat_pitch = ae->body_pitch;
	ae->inf.target_heading = io::bam_add(seat_heading, 0x70000000);
	ae->inf.look_pitch = io::bam_add(seat_pitch, 0x40000000);
	ae->inf.recoil_pitch = 1 << 18;
	ai.tick_infantry(*ae, world, 2);
	// The step (0x40000 + 4) >> 3 = 0x8000: the Pitch takes 0x1000, the Yaw the half 0x4000.
	CHECK(ae->inf.look_pitch == io::bam_add(io::bam_add(seat_pitch, kPitchLimit), 0x1000));
	CHECK(ae->pitch == ae->inf.look_pitch);
	const int32_t off = io::bam_sub(ae->inf.target_heading, io::bam_add(seat_heading, kYawLimit));
	CHECK(off == 0x4000 || off == -0x4000);
	CHECK(ae->heading == ae->inf.target_heading);
	CHECK(ae->inf.recoil_pitch == (1 << 18) - 0x4000);
}

// D-NET-1214: the player body's dead tail. A latched-dead body's aim pitch takes
// savedLivePitch, its Yaw (and the local look) the body heading, and Flags drops 0x60; a live
// body keeps all of them [orig: @0x4B4D30..0x4B4D5F].
void test_dead_tail() {
	for (const bool dead : {false, true}) {
		for (const bool local : {false, true}) {
			auto wp = std::make_unique<w::World>();
			test_titles::jo_world(*wp);
			w::World &world = *wp;
			world.registry.configure_pool(0, 16);
			w::Entity body;
			body.kind = w::EntityKind::Organic;
			const w::EntityHandle bh = world.registry.spawn(0, body);
			w::AiEntity *ae = world.ai.at(world.ai.attach(bh));
			w::Entity *ent = world.registry.get(bh);
			CHECK(ae != nullptr && ent != nullptr);
			if (ae == nullptr || ent == nullptr) continue;
			ae->inf.is_local_player = local;
			ae->inf.body_heading = 0x10000000;
			ae->heading = 0x30000000;
			ae->inf.target_heading = 0x20000000;
			ae->body_pitch = 0x00123400;
			ae->inf.aim_pitch = -5;
			static constexpr uint32_t kKept = 0x1000u;
			ent->flags = kKept | w::kEntityFlagParachute | w::kEntityFlagMounted |
					(dead ? w::kEntityFlagDead : 0u);
			ent->engine_flags = ent->flags;
			w::player_body_dead_tail(*ae, *ent);
			CHECK(ae->inf.aim_pitch == (dead ? 0x00123400 : -5));
			CHECK(ae->heading == (dead ? 0x10000000 : 0x30000000));
			CHECK(ae->inf.target_heading == (dead && local ? 0x10000000 : 0x20000000));
			const uint32_t flags_want = dead ? (kKept | w::kEntityFlagDead)
					: (kKept | w::kEntityFlagParachute | w::kEntityFlagMounted);
			CHECK(ent->flags == flags_want && ent->engine_flags == flags_want);
		}
	}
}

// D-NET-1214 through the authority's body tick: a dead local player (its edge latched) under
// a canopy and in a seat's Flags runs its view phase (the canopy chase moves its body heading),
// then its dead tail: its Yaw and look take that body heading and the 0x60 bits drop.
void test_dead_tail_through_the_tick() {
	auto wp = std::make_unique<w::World>();
	test_titles::jo_world(*wp);
	w::World &world = *wp;
	world.registry.configure_pool(0, 16);
	world.registry.configure_pool(1, 16);
	w::Entity body;
	body.kind = w::EntityKind::Organic;
	body.health = 0;
	body.flags = body.engine_flags =
			w::kEntityFlagDead | w::kEntityFlagParachute | w::kEntityFlagMounted;
	const w::EntityHandle bh = world.registry.spawn(0, body);
	w::AiEntity *ae = world.ai.at(world.ai.attach(bh));
	CHECK(ae != nullptr);
	if (ae == nullptr) return;
	ae->inf.active = true;
	ae->inf.is_local_player = true;
	ae->inf.body_heading = 0;
	ae->inf.target_heading = 0x10000000;
	world.ai.tick_infantry(*ae, world, 1);
	CHECK(ae->inf.body_heading == (0x10000000 + 8) >> 4); // the canopy chase ran first
	CHECK(ae->heading == ae->inf.body_heading);
	CHECK(ae->inf.target_heading == ae->inf.body_heading);
	const w::Entity *ent = world.registry.get(bh);
	CHECK(ent != nullptr &&
			((ent->flags | ent->engine_flags) & (w::kEntityFlagParachute | w::kEntityFlagMounted)) ==
					0u);
}

// D-NET-1225: the player body's corpse leg, after its dead tail, every tick its dead bit is
// latched. The timer drops by one a tick. At 186 the def's decay spawns once into the corpse's
// +0x1CC slot (oriented (0, 0, -0.5), tagged with the corpse, positioned), where the leg runs, into
// the world's scene (world::ItemEmitterSystem::store_positioned_emitter). At 0 the timer stays
// 0 and the body stays. A LeaveCorpse def keeps its timer. Section bit 0 overrides LeaveCorpse,
// spawns no decay and destroys the body at 0. A live body's timer is left alone.
// [orig: Entity_UpdateInfantryPlayerBody @0x4B40E0 - @0x4B4D63..0x4B4E71]
void test_corpse_leg_through_the_tick() {
	enum class Kind { kDecay, kLeaveCorpse, kSilent, kLive };
	for (const Kind kind : {Kind::kDecay, Kind::kLeaveCorpse, Kind::kSilent, Kind::kLive}) {
		auto wp = std::make_unique<w::World>();
		test_titles::jo_world(*wp);
		w::World &world = *wp;
		world.registry.configure_pool(0, 16);
		static constexpr uint16_t kItem = 0x14B9;
		static constexpr int32_t kTimer = 188;
		w::ItemDeathTraits traits;
		traits.particledeath = "corpse_decay";
		world.tables.item_death_traits.set(kItem, traits);
		const bool dead = kind != Kind::kLive;
		w::Entity body;
		body.kind = w::EntityKind::Organic;
		body.item_id = kItem;
		body.health = dead ? 0 : 100;
		body.alive = !dead;
		body.flags = body.engine_flags = dead ? w::kEntityFlagDead : 0u;
		body.leave_corpse = kind == Kind::kLeaveCorpse || kind == Kind::kSilent;
		body.section_mask = kind == Kind::kSilent ? 1u : 0u;
		body.corpse_timer = kTimer;
		const w::EntityHandle bh = world.registry.spawn(0, body);
		const auto scene = test_effects::open_producers(world, {"corpse_decay"});
		w::AiEntity *ae = world.ai.at(world.ai.attach(bh));
		CHECK(ae != nullptr);
		if (ae == nullptr) continue;
		ae->inf.active = true;
		ae->inf.is_local_player = true;
		int decays = 0, decay_tick = -1;
		bool destroyed = false;
		for (int t = 1; t <= kTimer + 2 && !destroyed; ++t) {
			const int before = test_effects::groups_named(*scene, "corpse_decay");
			world.ai.tick_infantry(*ae, world, static_cast<uint32_t>(t));
			if (test_effects::groups_named(*scene, "corpse_decay") > before) {
				++decays;
				decay_tick = t;
				const auto row = test_effects::slot_row(*scene, bh);
				CHECK(row.has_value() && row->effect_name == "corpse_decay");
				CHECK(row && row->pose.forward.x == 0.0f && row->pose.forward.y == -1.0f &&
				      row->pose.forward.z == 0.0f);
				CHECK(row && row->section_gate.tagged &&
				      row->admission == opennova::particle::EffectAdmission::StoreOwned);
			}
			world.out.destruction.effects.clear();
			destroyed = world.registry.get(bh) == nullptr;
			if (kind == Kind::kSilent && !destroyed) CHECK(t < kTimer);
		}
		const w::Entity *ent = world.registry.get(bh);
		switch (kind) {
		case Kind::kDecay:
			CHECK(decays == 1 && decay_tick == kTimer - 186);
			CHECK(ent != nullptr && ent->corpse_timer == 0 && ent->death_effect_active[0] == 1);
			break;
		case Kind::kLeaveCorpse:
		case Kind::kLive:
			CHECK(decays == 0 && ent != nullptr && ent->corpse_timer == kTimer);
			break;
		case Kind::kSilent:
			CHECK(decays == 0 && destroyed && world.ai.for_handle(bh) == nullptr);
			break;
		}
	}
}

// D-NET-1227: the local player body registers its rain ambient ahead of its view phase with no
// Health test, so a dead local body keeps its two rain loops; a player body not the local one
// registers none. [orig: Entity_UpdateInfantryPlayerBody @0x4B40E0 - @0x4B473B..0x4B4906]
void test_rain_ambient_dead_or_alive() {
	for (const bool dead : {false, true}) {
		for (const bool local : {false, true}) {
			auto wp = std::make_unique<w::World>();
			test_titles::jo_world(*wp);
			w::World &world = *wp;
			world.registry.configure_pool(0, 16);
			world.weather.core.scalar_channels.rain_pct_fp = 0x8000;
			world.weather.precipitation_kind = static_cast<uint32_t>(w::PrecipitationKind::Rain);
			w::Entity body;
			body.kind = w::EntityKind::Organic;
			body.health = dead ? 0 : 100;
			body.alive = !dead;
			body.position = {10.f, 20.f, 0.f};
			// A player body not the local one is org2 by its player bit.
			body.flags = body.engine_flags =
					(dead ? w::kEntityFlagDead : 0u) | (local ? 0u : w::kEntityFlagPlayer);
			const w::EntityHandle bh = world.registry.spawn(0, body);
			w::AiEntity *ae = world.ai.at(world.ai.attach(bh));
			CHECK(ae != nullptr);
			if (ae == nullptr) continue;
			ae->inf.active = true;
			ae->inf.is_local_player = local;
			world.ai.tick_infantry(*ae, world, 1);
			int left = 0, right = 0;
			for (const w::SoundEmitterEvent &ev : world.out.sound_emitters.pending()) {
				if (ev.set_name == "LPNV_RAIN_L") ++left;
				if (ev.set_name == "LPNV_RAIN_R") ++right;
			}
			CHECK(left == (local ? 1 : 0) && right == (local ? 1 : 0));
		}
	}
}

// The local live body sets its reverb preset on the slow pass alone: the raw tick's low nibble 0,
// whatever region it stood in between; a player body not the local one sets none.
// [orig: Entity_UpdateInfantryPlayerBody @0x4B40E0 - the gate @0x4B5D6D..0x4B5D71 on the key
//  stored @0x4B4E79, the local test @0x4B6234..0x4B623A, Audio_SetReverbPreset @0x4B633F]
void test_reverb_rides_the_slow_pass() {
	for (const bool local : {false, true}) {
		auto wp = std::make_unique<w::World>();
		test_titles::jo_world(*wp);
		w::World &world = *wp;
		world.registry.configure_pool(0, 16);
		world.reverb.mission = world.reverb.selected = 3;
		w::ReverbRegion region;
		region.min[0] = 5 << 16;
		region.min[1] = 15 << 16;
		region.min[2] = -30000 * 65536;
		region.max[0] = 15 << 16;
		region.max[1] = 25 << 16;
		region.max[2] = 30000 * 65536;
		region.value = 7;
		world.reverb.regions.push_back(region);
		w::Entity body;
		body.kind = w::EntityKind::Organic;
		body.health = 100;
		body.alive = true;
		body.position = {10.f, 20.f, 0.f};
		body.flags = body.engine_flags = local ? 0u : w::kEntityFlagPlayer;
		const w::EntityHandle bh = world.registry.spawn(0, body);
		w::AiEntity *ae = world.ai.at(world.ai.attach(bh));
		CHECK(ae != nullptr);
		if (ae == nullptr) continue;
		ae->inf.active = true;
		ae->inf.is_local_player = local;
		world.ai.tick_infantry(*ae, world, 1);
		CHECK(world.reverb.selected == 3); // inside the region, off the pass
		world.ai.tick_infantry(*ae, world, 16);
		CHECK(world.reverb.selected == (local ? 7 : 3));
		ae->self->position.x.q16 = 100 << 16; // out of the region
		world.ai.tick_infantry(*ae, world, 17);
		CHECK(world.reverb.selected == (local ? 7 : 3)); // held to the next pass
		world.ai.tick_infantry(*ae, world, 32);
		CHECK(world.reverb.selected == 3);
	}
}

// D-NET-1228: the stance sound plays after the on-foot site. A seated body's mounted site
// returns first, so it plays none and its latch holds; the same body on foot plays its change.
// [orig: Entity_UpdateInfantryPlayerBody @0x4B40E0 - the mounted returns @0x4B6907 / @0x4B6977,
//  the stance block @0x4B703D..0x4B709D]
void test_stance_sound_after_the_on_foot_site() {
	for (const bool seated : {true, false}) {
		auto wp = std::make_unique<w::World>();
		test_titles::jo_world(*wp);
		w::World &world = *wp;
		world.registry.configure_pool(0, 16);
		world.registry.configure_pool(1, 16);
		w::Entity vehicle;
		vehicle.net_id = 200;
		vehicle.kind = w::EntityKind::Item;
		vehicle.has_item_def = true;
		vehicle.has_graphic_model = true;
		vehicle.item_type = 1;
		vehicle.position = {10.f, 20.f, 5.f};
		w::Seat seat;
		seat.type = w::SeatType::Passenger;
		vehicle.seats.push_back(seat);
		const w::EntityHandle vh = world.registry.spawn(1, vehicle);
		w::Entity body;
		body.net_id = 100;
		body.kind = w::EntityKind::Organic;
		body.health = 100;
		body.alive = true;
		const w::EntityHandle bh = world.registry.spawn(0, body);
		w::AiEntity *ae = world.ai.at(world.ai.attach(bh));
		CHECK(ae != nullptr);
		if (ae == nullptr) continue;
		ae->inf.active = true;
		ae->inf.is_local_player = true;
		ae->inf.stance = w::InfantryState::Stance::kCrouch; // the crouch key: no seat clears it
		if (seated) CHECK(world.commands.mount(bh, vh));
		world.ai.tick_infantry(*ae, world, 1);
		int crouch_sounds = 0;
		for (const w::SoundSlotEvent &ev : world.out.slot_sounds)
			if (std::strcmp(ev.set_name, "TO_CROUCH") == 0) ++crouch_sounds;
		CHECK(crouch_sounds == (seated ? 0 : 1));
		CHECK(ae->inf.stance_sound_state == (seated ? 0 : 1));
	}
}

// D-NET-1199: the local body in a control seat (ctrlx / drvrx) under the chase camera
// (g_CameraMode 1) halves its Pitch ahead of the phase, every tick; a passenger seat, the first
// person camera or a body not the local one keeps it [orig: @0x4B4921..0x4B4942].
void test_chase_camera_halves_a_drivers_pitch() {
	struct Case {
		w::SeatType seat;
		int camera_mode;
		bool local;
		bool halved;
	};
	for (const Case c : {Case{w::SeatType::Controller, 1, true, true},
				Case{w::SeatType::Driver, 1, true, true},
				Case{w::SeatType::Passenger, 1, true, false},
				Case{w::SeatType::Driver, 0, true, false},
				Case{w::SeatType::Driver, 1, false, false}}) {
		auto wp = std::make_unique<w::World>();
		test_titles::jo_world(*wp);
		w::World &world = *wp;
		world.registry.configure_pool(0, 16);
		auto local = std::make_unique<w::LocalPlayer>(world);
		local->view.camera_mode = c.camera_mode;
		world.local_player_state = local.get();
		w::Entity body;
		body.kind = w::EntityKind::Organic;
		body.health = 100;
		body.alive = true;
		body.mount_type = c.seat; // +0x168 alone: no mount, so no Flags 0x40 skip and no clamp skips
		const w::EntityHandle bh = world.registry.spawn(0, body);
		w::AiEntity *ae = world.ai.at(world.ai.attach(bh));
		CHECK(ae != nullptr);
		if (ae == nullptr) continue;
		ae->inf.active = true;
		ae->inf.is_local_player = c.local;
		static constexpr int32_t kPitch = -0x10000001;
		ae->inf.look_pitch = kPitch;
		ae->pitch = kPitch;
		w::infantry_org2_view_tick(world, *ae, world.registry.get(bh), 1);
		const int32_t want = c.halved ? io::bam_sar(kPitch, 1) : kPitch;
		CHECK((c.local ? ae->inf.look_pitch : ae->pitch) == want);
		world.local_player_state = nullptr;
	}
}

} // namespace

int main() {
	test_skip_branch_writes_the_legs_alone();
	test_canopy_moves_the_targets();
	test_yaw_clamp_and_its_skip();
	test_pitch_clamp_and_its_skip();
	test_on_foot_leg_model();
	test_prone_local();
	test_seated_player_runs_the_phase(Mount::kPlain);
	test_seated_player_runs_the_phase(Mount::kConfig3);
	test_seated_player_runs_the_phase(Mount::kCrashed);
	test_chase_camera_halves_a_drivers_pitch();
	test_ride_turn();
	test_ride_hold();
	test_pitch_lag_step();
	for (const Ride kind : {Ride::kPassenger, Ride::kTurret, Ride::kDriver, Ride::kDriverFreeLook,
				Ride::kCrashed})
		test_seated_player_rides_its_mount(kind);
	test_kick_follows_the_view_clamp();
	test_dead_tail();
	test_dead_tail_through_the_tick();
	test_corpse_leg_through_the_tick();
	test_rain_ambient_dead_or_alive();
	test_reverb_rides_the_slow_pass();
	test_stance_sound_after_the_on_foot_site();
	if (failures == 0) std::printf("infantry_org2_view: all passed\n");
	return failures == 0 ? 0 : 1;
}
