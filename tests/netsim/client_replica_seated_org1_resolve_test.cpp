// D-COL-15: a joiner's live seated org1 row runs its update's head and mounted tail. The head runs
// its dual update (the channel, armed across its records) and stamps savedLivePose from the pose
// its last update left; the seat branch clears the chute, in-air, afloat, ladder, dive and armory
// bits and the vertical velocity, keeping Indoors (0x800000), then poses the body on its seat; the
// mounted tail runs the movement resolver on the eight-tick key over the channel's capsule, its
// Flags out the row's, so the Indoors bit is re-derived. A seated player row runs no resolve; its
// channel runs at its head too (a starved tick advances it), and its update poses it at its
// mounted site, after its head's stamp. A dead body its record still seats resolves
// free-standing with the mounted latch its seat attach's Flags 0x40 sets. Both replica resolves
// read the body's current state (+0x2BC): the arbitrated current, or the state its death edge
// stored that tick, never the record's raw byte.
// [orig: Entity_UpdateInfantryAI @0x4B9910 - the dual update @0x4B9A48, the stamp
//  @0x4B9A53..0x4B9A6E, the key `& 7` @0x4BA066..0x4BA072, the seat block @0x4BEBEC..0x4BEC15,
//  the mounted tail @0x4BF4B3..0x4BF5BE, the free-standing call @0x4BF7F2;
//  Entity_UpdateInfantryPlayerBody @0x4B40E0 - the dual update @0x4B41DF ahead of the starved
//  exit @0x4B4669..0x4B4670; Entity_MovementCollisionResolver @0x4B2BD0 - the latch
//  @0x4B2D2B..0x4B2D34, the clear @0x4B2D2E..0x4B2D36; Entity_AttachToVehicleSlot @0x4946D0 -
//  `or ecx, 40h` @0x494760; the resolver's +0x2BC reads @0x4B2C05 / @0x4B3A77, the death edges'
//  stores @0x4B4CA3 / @0x4B9CFE..0x4B9D06; the org2 stamp @0x4B4190..0x4B419C, its mounted
//  site's pose @0x4B63C7 / @0x4B6510]

#include <runtime/replication/client_replica_pipeline.h>

#include <net/npwire/entity_class.h>
#include <net/npwire/ingame_decode.h>
#include <net/npwire/ingame_encode.h>
#include <net/npwire/ingame_message_id.h>
#include <runtime/world/entity.h>
#include <runtime/world/infantry.h>

#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <vector>
#include "../common/test_expect.h"

namespace {

namespace nw = opennova;
namespace ns = opennova::replication;
namespace as = opennova::world::anim_state;

int &failures = test_expect::failures;
using test_expect::expect;

constexpr uint16_t kPlayerType = 0x14B9;
constexpr uint16_t kInfantryType = 0x0042;
constexpr uint16_t kHullType = 0x0248;
constexpr uint16_t kHull = 0x1006;
constexpr uint16_t kSelf = 0x0002;
constexpr uint16_t kRider = 0x0004;
constexpr uint16_t kGround = 0x2001; // the probe's hit the stub resolver answers

int32_t fixed_of(double v) { return static_cast<int32_t>(v * 65536.0); }

// Looping clips over one adm, every state authored, the capsule 1..2 u.
struct BodySource final : public opennova::world::IRootMotionSource {
	bool has_clip(int, int) const override { return true; }
	bool advance(int, int, int32_t &phase, opennova::world::RootMotionFrame &out) override {
		++phase;
		out = {};
		out.capsule_bottom = 1 << 16;
		out.capsule_top = 2 << 16;
		return true;
	}
	int32_t clip_length_ticks(int, int, int) const override { return 62; }
	bool clip_loops(int, int, int) const override { return true; }
};

// One 0x0A from the anchor (100, 200, 10): the hull free-standing at x 50, and the rider on it,
// an org1 row on bone 2 (or free-standing, `seated` false) at the local x +1, or a player row on
// bone 1.
std::vector<uint8_t> frame(nw::EntityClass rider_class, bool seated = true) {
	nw::FrameUpdate fu;
	fu.flags2 = 0;
	fu.carried_handle = 0xFFFF;
	fu.health = 100;
	fu.anchor_x = fixed_of(100.0);
	fu.anchor_y = fixed_of(200.0);
	fu.anchor_z = fixed_of(10.0);
	nw::FrameUpdateRecord hull;
	hull.handle = kHull;
	hull.type_id = kHullType;
	hull.cls = nw::EntityClass::Vehicle;
	hull.vehicle.parent_slot_handle = 0xFFFF;
	hull.vehicle.pos_x_compressed = nw::network_compress_fixedpoint(fixed_of(-50.0));
	hull.vehicle.health_word = 100;
	fu.records.push_back(hull);
	nw::FrameUpdateRecord rec;
	rec.handle = kRider;
	rec.cls = rider_class;
	if (rider_class == nw::EntityClass::Infantry) {
		rec.type_id = kInfantryType;
		rec.infantry.seat_bone_idx = seated ? 2 : 0;
		rec.infantry.vehicle_slot_handle = seated ? kHull : 0xFFFF;
		rec.infantry.pos_x_compressed = nw::network_compress_fixedpoint(fixed_of(1.0));
		rec.infantry.yaw_byte = 0x10;
		rec.infantry.anim_byte = as::kIdle;
	} else {
		rec.type_id = kPlayerType;
		rec.player.carrier_handle = kHull;
		rec.player.vehicle_bone = 1;
		rec.player.pos_x_compressed = nw::network_compress_fixedpoint(fixed_of(1.0));
		rec.player.yaw_byte = 0x40;
		rec.player.anim_state_id = as::kIdle;
	}
	fu.records.push_back(rec);
	return nw::encode_frame_update(fu);
}

// A remote-motion view over BodySource with the rider seated (one record landed) and a stub
// resolver that records each query and answers the Indoors bit `*indoors` and the ground kGround.
struct Rig {
	BodySource src;
	std::unique_ptr<ns::ClientReplicaPipeline> view = std::make_unique<ns::ClientReplicaPipeline>();
	std::vector<ns::ClientReplicaPipeline::ReplicaContactQuery> queries;
	bool indoors = false;

	explicit Rig(nw::EntityClass rider_class, bool split = false, bool seated = true) {
		view->set_remote_motion_mode(true);
		view->set_root_motion_source(&src);
		view->set_item_class_resolver(
				[](uint16_t type) -> ns::ClientReplicaPipeline::ItemClassResolution {
					if (type == kHullType) return nw::EntityClass::Vehicle;
					if (type == kPlayerType) return nw::EntityClass::Player;
					if (type == kInfantryType) return nw::EntityClass::Infantry;
					return std::nullopt;
				});
		view->set_replica_contact_resolver(
				[this](ns::ClientReplicaPipeline::ReplicaContactQuery &q) -> int32_t {
					queries.push_back(q);
					q.entity_flags = (q.entity_flags & ~nw::world::kEntityFlagIndoors) |
							(indoors ? nw::world::kEntityFlagIndoors : 0u);
					q.out_ground = kGround;
					return 0x10000; // a clearance the seated call discards
				});
		view->state().upsert(kHull).type_id = kHullType;
		view->state().upsert(kRider).type_id =
				rider_class == nw::EntityClass::Infantry ? kInfantryType : kPlayerType;
		if (split) {
			// The own body splits the walk: every pool-0 row parks for its visit.
			view->set_self_body_split(true);
			view->state().upsert(kSelf).type_id = kPlayerType;
		}
		view->apply(0x0A, frame(rider_class, seated));
		view->state().find(kRider)->rm_adm_id = 0;
		split_ = split;
	}

	ns::ClientEntityState &rider() { return *view->state().find(kRider); }
	ns::ClientEntityState &hull() { return *view->state().find(kHull); }

	// One tick in the embedder's order (no gun channels here).
	void tick() {
		if (split_) {
			view->tick_remote_motion(kSelf);
			view->tick_remote_motion_through_self();
			view->tick_remote_motion_after_self();
		} else {
			view->tick_remote_motion(0xFFFF);
		}
		view->tick_org1_looks();
	}

	size_t rider_queries() const {
		size_t n = 0;
		for (const auto &q : queries) n += q.row_handle == kRider ? 1u : 0u;
		return n;
	}

private:
	bool split_ = false;
};

// The mounted tail resolves on the eight-tick key alone, over the channel's capsule, as an org1
// body with the mounted latch, and its Flags out re-derive the row's Indoors bit; the probe's
// ground stands until the next update's head stores the parent back.
void test_seated_org1_resolves_on_its_eight_tick_key() {
	Rig rig(nw::EntityClass::Infantry);
	ns::ClientEntityState &rider = rig.rider();
	expect(rider.carrier_handle == kHull && rider.mount_bone == 2 && rider.net_seat_valid,
			"(the org1 row is seated on the hull)");
	rider.rm_entity_flags |= nw::world::kEntityFlagIndoors; // its spawn word's bit, say
	for (int i = 0; i < 16; ++i) {
		rig.tick();
		if (rig.view->frame_tick() == 8) {
			expect(rig.rider_queries() == 1, "the eighth tick resolves the seated row");
			const auto &q = rig.queries.back();
			expect(!q.is_player_class && q.mounted_latch && q.has_saved_live && !q.has_eye &&
							q.tick == 8,
					"the seated resolve is an org1 body's, the mounted latch set");
			expect(q.capsule_bottom == (1 << 16) && q.capsule_top == (2 << 16),
					"the resolve reads the capsule the head's dual update returned");
			expect(q.vel_z == 0, "the seat block zeroed the vertical velocity ahead of it");
			expect((rider.rm_entity_flags & nw::world::kEntityFlagIndoors) == 0,
					"the resolve's Flags out drop the stale Indoors bit");
			expect(rider.resolved_ground == kGround, "the probe's ground stands after the resolve");
		} else if (rig.view->frame_tick() == 9) {
			expect(rig.rider_queries() == 1, "no resolve off the key");
			expect(rider.resolved_ground == kHull, "the next head stores the parent back");
			rig.indoors = true;
		}
	}
	expect(rig.rider_queries() == 2, "sixteen ticks resolve twice");
	expect((rider.rm_entity_flags & nw::world::kEntityFlagIndoors) != 0,
			"the sixteenth tick's resolve sets the Indoors bit");
}

// The head stamps savedLivePose from the pose the body's last update left, ahead of this
// update's seat pose: the hull's move this tick shows as the resolve's pos less its saved pose,
// in the walk's first half and in its parked half alike.
void test_seated_org1_stamp_reads_the_last_seat_pose() {
	for (const bool split : {false, true}) {
		Rig rig(nw::EntityClass::Infantry, split);
		for (int i = 0; i < 7; ++i) rig.tick();
		const int32_t rider_x = rig.rider().x;
		expect(rider_x == rig.hull().x + fixed_of(1.0), "(the rider sits at the hull's x + 1)");
		rig.hull().x += fixed_of(2.0);
		rig.tick();
		expect(rig.rider_queries() == 1, "(the eighth tick resolves)");
		const auto &q = rig.queries.back();
		expect(q.saved_live_pos[0] == rider_x,
				split ? "a parked rider's stamp reads its last seat pose"
				      : "the rider's stamp reads its last seat pose");
		expect(q.pos[0] == rig.hull().x + fixed_of(1.0) &&
						q.pos[0] - q.saved_live_pos[0] == fixed_of(2.0),
				"the resolve reads this tick's seat pose, the hull's 2 u past the stamp");
	}
}

// The seat block clears the in-air (0x2000) and afloat (0x8000) bits and the vertical velocity
// every tick, keeping Indoors.
void test_seated_org1_seat_block_scrubs_its_flags() {
	Rig rig(nw::EntityClass::Infantry);
	ns::ClientEntityState &rider = rig.rider();
	rider.rm_entity_flags = nw::world::kEntityFlagInAir | nw::world::kEntityFlagDrowning |
			nw::world::kEntityFlagIndoors;
	rider.rm_vel_z = -5000;
	rig.tick();
	expect(rig.rider_queries() == 0, "(no resolve on the first tick)");
	expect(rider.rm_entity_flags == nw::world::kEntityFlagIndoors && rider.rm_vel_z == 0,
			"the seat block keeps Indoors alone and zeroes the vertical velocity");
}

// A seated org1 row's channel runs at its head and keeps its phase across its records: the fold
// no longer disarms it.
void test_seated_org1_channel_survives_its_records() {
	Rig rig(nw::EntityClass::Infantry);
	ns::ClientEntityState &rider = rig.rider();
	for (int i = 0; i < 3; ++i) rig.tick();
	expect(rider.rm_state == as::kIdle && rider.rm_phase == 3,
			"three ticks advance the seated channel");
	rig.view->apply(0x0A, frame(nw::EntityClass::Infantry));
	expect(rider.rm_state == as::kIdle && rider.rm_phase == 3,
			"a seated record keeps the channel armed");
	rig.tick();
	expect(rider.rm_phase == 4, "the next tick plays on");
}

// A seated player row runs no resolve; its channel runs at its update's head, a starved tick's
// included.
void test_seated_player_row_runs_no_resolve() {
	Rig rig(nw::EntityClass::Player);
	ns::ClientEntityState &rider = rig.rider();
	expect(rider.carrier_handle == kHull && rider.mount_bone == 1, "(the player row is seated)");
	for (int i = 0; i < 16; ++i) rig.tick();
	expect(rig.rider_queries() == 0, "a seated player row never resolves");
	rider.net_interp_progress = 512;
	const int32_t phase = rider.rm_phase;
	rig.tick();
	expect(rider.rm_starved && rider.rm_phase == phase + 1,
			"a starved seated player's head still advances its channel");
}

// A dead body its record still seats takes the free-standing branch, whose resolve carries the
// mounted latch its seat attach's Flags 0x40 set; a free-standing live body's carries none.
void test_dead_seated_body_resolves_with_the_latch() {
	Rig rig(nw::EntityClass::Infantry);
	const int16_t death = as::kDeathBulletBase + 6;
	rig.view->apply(nw::s2c::ENTITY_DEATH,
			{static_cast<uint8_t>(kRider & 0xFF), static_cast<uint8_t>(kRider >> 8),
			 static_cast<uint8_t>(death & 0xFF), static_cast<uint8_t>(death >> 8)});
	rig.tick();
	expect(rig.rider().carrier_handle == kHull && rig.rider_queries() >= 1 &&
					rig.queries.back().mounted_latch,
			"the dead seated body's free-standing resolve sets the mounted latch");
	Rig free_standing(nw::EntityClass::Infantry, false, /*seated=*/false);
	free_standing.tick();
	expect(free_standing.rider_queries() == 1 && !free_standing.queries.back().mounted_latch,
			"a free-standing live body's resolve sets none");
}

// A seated player body's update poses it at its mounted site, so its head's stamp reads the pose
// its last update left: the hull's 2 u move this tick shows between the stamp and the pose, in
// the walk's first half and in its parked half alike.
void test_seated_player_pose_is_its_mounted_sites() {
	for (const bool split : {false, true}) {
		Rig rig(nw::EntityClass::Player, split);
		for (int i = 0; i < 7; ++i) rig.tick();
		const int32_t rider_x = rig.rider().x;
		expect(rider_x == rig.hull().x + fixed_of(1.0), "(the player sits at the hull's x + 1)");
		rig.hull().x += fixed_of(2.0);
		rig.tick();
		expect(rig.rider().net_saved_live_pose[0] == rider_x,
				split ? "a parked player's stamp reads its last seat pose"
				      : "the player's stamp reads its last seat pose");
		expect(rig.rider().x == rig.hull().x + fixed_of(1.0),
				"the mounted site poses the player on this tick's seat");
	}
}

// Both replica resolves read the body's current state: the arbitrated current where the record's
// raw byte differs, and on the death edge's tick the state the edge stored.
void test_resolves_read_the_current_state() {
	Rig rig(nw::EntityClass::Infantry, false, /*seated=*/false);
	rig.tick();
	ns::ClientEntityState &rider = rig.rider();
	rider.net_anim_current = as::kIdle;
	rider.anim_state_id = as::kWalkForward;
	rig.tick();
	expect(rig.rider_queries() == 2 && rig.queries.back().anim_state_id == as::kIdle,
			"the free-standing resolve reads the arbitrated current, not the record's byte");
	const int16_t death = as::kDeathBulletBase + 6;
	rig.view->apply(nw::s2c::ENTITY_DEATH,
			{static_cast<uint8_t>(kRider & 0xFF), static_cast<uint8_t>(kRider >> 8),
			 static_cast<uint8_t>(death & 0xFF), static_cast<uint8_t>(death >> 8)});
	rig.tick();
	expect(rig.rider_queries() == 3 && rig.queries.back().anim_state_id == death,
			"the death edge's tick resolves in the state the edge stored");
	Rig seated(nw::EntityClass::Infantry);
	seated.rider().net_anim_current = as::kIdle;
	seated.rider().anim_state_id = as::kWalkForward;
	for (int i = 0; i < 8; ++i) seated.tick();
	expect(seated.rider_queries() == 1 && seated.queries.back().anim_state_id == as::kIdle,
			"the seated resolve reads the arbitrated current");
}

} // namespace

int main() {
	test_seated_org1_resolves_on_its_eight_tick_key();
	test_seated_org1_stamp_reads_the_last_seat_pose();
	test_seated_org1_seat_block_scrubs_its_flags();
	test_seated_org1_channel_survives_its_records();
	test_seated_player_row_runs_no_resolve();
	test_dead_seated_body_resolves_with_the_latch();
	test_resolves_read_the_current_state();
	test_seated_player_pose_is_its_mounted_sites();
	if (failures != 0) {
		std::fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	std::printf("client_replica_seated_org1_resolve: all checks passed\n");
	return 0;
}
