// The player body's (org2) view phase, the kernel the authority's body tick and a client's
// decoded rows share (world/infantry_view.h), and the authority's seated body running it.
// - D-NET-NEW-L5-47: the skip branch (Flags 0x100060) writes the legs alone; the canopy's chase
//   is what moves the re-plant targets.
// - D-NET-NEW-L5-48: a seated player body runs the phase: its Flags 0x40 holds the Yaw 120 degrees
//   about the body heading its last seat carry left, unless its mount's config is 3 or its ground
//   (the mount) is a crashed vehicle; its prone local clears.
// - D-NET-NEW-L5-49: the pitch clamp is skipped while the mount is a crashed vehicle.
// [orig: Entity_UpdateInfantryPlayerBody @0x4B40E0 - the canopy chase @0x4B4945..0x4B496C, the
//  skip test @0x4B4972..0x4B4978, the mount config test @0x4B4AC6..0x4B4ADE, the ground test
//  @0x4B4AE0..0x4B4AF9, the clamp @0x4B4AFB..0x4B4B59, the legs @0x4B4B5F..0x4B4B6B, the leg model
//  @0x4B497E..0x4B4ABB, the limit select @0x4B4B71..0x4B4B7D, the mount's crash test
//  @0x4B4B82..0x4B4BA0, the pitch clamp @0x4B4BA2..0x4B4BC6; the prone local @0x4B416C..0x4B4183,
//  its clear @0x4B46FC..0x4B471B; the ground = mount store @0x4B41A2..0x4B41B4]
// Core-only: every value is the test's own; the World is heap-held.
#include <base/io/bam.h>
#include <runtime/world/ai.h>
#include <runtime/world/entity.h>
#include <runtime/world/entity_commands.h>
#include <runtime/world/infantry.h>
#include <runtime/world/infantry_view.h>
#include <runtime/world/world.h>

#include <cstdint>
#include <cstdio>
#include <memory>

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

// D-NET-NEW-L5-47: on a ladder (or seated, or under a canopy) both legs take the body heading and
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

// D-NET-NEW-L5-47: under a canopy the body heading chases a sixteenth of the way to the Yaw and both
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

// D-NET-NEW-L5-48 / D-NET-NEW-L5-49 through the authority's body tick: a local player seated in a
// vehicle's passenger seat (the attach sets Flags 0x40). The first tick's seat carry leaves the
// body heading and savedLivePitch; the second tick's phase clamps the look about them, ahead of the
// seat pose that reads it.
void test_seated_player_runs_the_phase(Mount kind) {
	auto wp = std::make_unique<w::World>();
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
	world.registry.spawn(1, vehicle);
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
	CHECK(world.commands.mount(100, 200));
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
	if (failures == 0) std::printf("infantry_org2_view: all passed\n");
	return failures == 0 ? 0 : 1;
}
