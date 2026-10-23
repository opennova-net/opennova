// D-NET-NEW-L5-25: both organic body updates exit ahead of their per-tick legs. A hidden body
// (Flags bit 0) and a channel-less one (+0x188 null, rm_adm_id -1) return ahead of the dual
// update; a starved one (the chase's progress held at 512, tested ahead of the tick's count)
// returns after its channel and the count. None takes the lean decay, the dip, the torso roll,
// the selection, the mounted site, the lean ramp or the recoil draw. A seated body runs the
// chase's count as a body on foot does: its record resets it, and with no record for 512 ticks
// it starves and leaves its seat pose alone. The authority's view counts nothing, so its rows
// never starve.
// [orig: Entity_UpdateInfantryPlayerBody @0x4B40E0 - the bit-0 test @0x4B411E, the +0x188 test
//  @0x4B412D..0x4B4135, the dual update @0x4B41DF, the one chase skip @0x4B4279..0x4B428F, the
//  count @0x4B4431..0x4B443E, the starved exit @0x4B4669..0x4B4670, the recoil draw @0x4B581B,
//  the decay @0x4B5C97, the dip @0x4B5CAB, the selection gate @0x4B5D71, the site selector
//  @0x4B637C..0x4B638F, the ramp @0x4B7DBF; Entity_UpdateInfantryAI @0x4B9910 - the +0x188
//  test @0x4B9931..0x4B993B, the bit-0 test @0x4B9A03..0x4B9A07, the starved exit
//  @0x4B9C09..0x4B9C39, the recoil draw @0x4BE82D; NetPacket_SerializePlayerState @0x4C09C0 -
//  the vehicle compose @0x4C10D4, the reset @0x4C0FFC]

#include <runtime/replication/client_replica_pipeline.h>
#include <runtime/anim/aim_overlay.h>
#include <runtime/inmatch/client_replica_emplaced.h>
#include <runtime/inmatch/client_replica_present.h>
#include <runtime/mission/promote.h>
#include <runtime/world/world.h>

#include <net/npwire/entity_class.h>
#include <net/npwire/ingame_decode.h>
#include <net/npwire/ingame_encode.h>
#include <net/npwire/ingame_message_id.h>
#include <runtime/world/entity.h>
#include <runtime/world/infantry.h>
#include <runtime/terrain_query/height_field.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <vector>

namespace {

namespace nw = opennova;
namespace ns = opennova::replication;
namespace as = opennova::world::anim_state;

int failures = 0;
bool expect(bool cond, const char *msg) {
	if (cond) return true;
	std::fprintf(stderr, "FAIL: %s\n", msg);
	++failures;
	return false;
}

constexpr uint16_t kPlayerType = 0x14B9;
constexpr uint16_t kInfantryType = 0x0042;
constexpr uint16_t kHullType = 0x0248;
constexpr uint16_t kHull = 0x1006;

int32_t fixed_of(double v) { return static_cast<int32_t>(v * 65536.0); }

// Looping clips over one adm, every state authored.
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

struct Row {
	uint16_t handle = 0;
	nw::EntityClass cls = nw::EntityClass::Player;
	uint8_t flags = 0;
	uint16_t carrier = 0xFFFF;
	uint8_t pitch = 0;
};

// One 0x0A from the anchor (100, 200, 10): each row free-standing at the anchor, or seated on
// the hull (bone 1) at the local x +1, the hull free-standing at hull_x.
nw::FrameUpdate hull_update(double hull_x) {
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
	hull.vehicle.pos_x_compressed = nw::network_compress_fixedpoint(fixed_of(hull_x - 100.0));
	hull.vehicle.health_word = 100;
	fu.records.push_back(hull);
	return fu;
}

std::vector<uint8_t> frame(const std::vector<Row> &rows, double hull_x = 50.0) {
	nw::FrameUpdate fu = hull_update(hull_x);
	for (const Row &row : rows) {
		nw::FrameUpdateRecord rec;
		rec.handle = row.handle;
		rec.cls = row.cls;
		if (row.cls == nw::EntityClass::Player) {
			rec.type_id = kPlayerType;
			rec.player.carrier_handle = row.carrier;
			rec.player.vehicle_bone = row.carrier != 0xFFFF ? 1 : 0;
			rec.player.pos_x_compressed = nw::network_compress_fixedpoint(
					row.carrier != 0xFFFF ? fixed_of(1.0) : 0);
			rec.player.yaw_byte = 0x40;
			rec.player.pitch_byte = row.pitch;
			rec.player.state_flags = row.flags;
			rec.player.anim_state_id = as::kIdle;
		} else {
			rec.type_id = kInfantryType;
			rec.infantry.vehicle_slot_handle = 0xFFFF;
			rec.infantry.yaw_byte = 0x40;
			rec.infantry.flags_byte = row.flags;
			rec.infantry.anim_byte = as::kIdle;
		}
		fu.records.push_back(rec);
	}
	return nw::encode_frame_update(fu);
}

std::unique_ptr<ns::ClientReplicaPipeline> make_view(opennova::world::IRootMotionSource &src,
		bool remote_motion = true) {
	auto view = std::make_unique<ns::ClientReplicaPipeline>();
	view->set_remote_motion_mode(remote_motion);
	view->set_root_motion_source(&src);
	view->set_item_class_resolver([](uint16_t type) -> ns::ClientReplicaPipeline::ItemClassResolution {
		if (type == kHullType) return nw::EntityClass::Vehicle;
		if (type == kPlayerType) return nw::EntityClass::Player;
		if (type == kInfantryType) return nw::EntityClass::Infantry;
		return std::nullopt;
	});
	view->state().upsert(kHull).type_id = kHullType;
	return view;
}

// The spawn stream's rows (a compact record never creates one), one record landed, then the
// legs' words every test reads.
void seed(ns::ClientReplicaPipeline &view, const std::vector<Row> &rows) {
	for (const Row &row : rows)
		view.state().upsert(row.handle).type_id =
				row.cls == nw::EntityClass::Player ? kPlayerType : kInfantryType;
	view.apply(0x0A, frame(rows));
	for (const Row &row : rows) {
		ns::ClientEntityState &es = *view.state().find(row.handle);
		es.rm_adm_id = 0;
		es.lean_angle = 0x10000000;
		es.pitch_kick_accum = -0x8000000;
		es.arms_dip_ticks = 6;
		es.torso_roll = 0x1000000;
		es.recoil_pitch = 1 << 18;
		es.move_input = static_cast<uint8_t>(nw::world::Entity::kMoveOrderLeanLeft);
	}
}

// ClientRuntime's order with no own-body split: the decay, the dip, the walk (and its
// selection), the ramp, then the joiner's recoil leg and its seated looks (JoinerRole's order:
// no gun channel here).
void tick(ns::ClientReplicaPipeline &view) {
	view.tick_lean_decay(0xFFFF);
	view.tick_arms_dip(0xFFFF);
	view.tick_remote_motion(0xFFFF);
	view.tick_lean_ramp();
	view.tick_recoil();
	view.tick_org1_looks();
}

struct Words {
	int32_t lean, kick, torso, recoil, heading, pitch;
	int dip;
};
Words words_of(const ns::ClientEntityState &es) {
	return Words{es.lean_angle, es.pitch_kick_accum, es.torso_roll, es.recoil_pitch,
	             es.heading_bam, es.pitch_bam, es.arms_dip_ticks};
}
bool same(const Words &a, const Words &b) {
	return a.lean == b.lean && a.kick == b.kick && a.torso == b.torso && a.recoil == b.recoil &&
	       a.heading == b.heading && a.pitch == b.pitch && a.dip == b.dip;
}

// A hidden player and a hidden infantry take no leg and draw nothing: the visible row behind
// them takes the stream's first draw, as it does alone, where two visible rows ahead of it
// leave it the third (the first two draws are even, the third odd, so the yaw half-step's sign
// tells them apart).
void test_hidden_rows_run_no_legs() {
	BodySource src;
	const std::vector<Row> hidden = {{0x0003, nw::EntityClass::Player, 0x01},
	                                 {0x0004, nw::EntityClass::Infantry, 0x01},
	                                 {0x0005, nw::EntityClass::Player, 0x00}};
	auto view = make_view(src);
	seed(*view, hidden);
	const Words a0 = words_of(*view->state().find(0x0003));
	const Words b0 = words_of(*view->state().find(0x0004));
	const int32_t a_x = view->state().find(0x0003)->x;
	for (int i = 0; i < 16; ++i) {
		tick(*view);
		if (i == 0) {
			const ns::ClientEntityState &a = *view->state().find(0x0003);
			const ns::ClientEntityState &b = *view->state().find(0x0004);
			expect(same(words_of(a), a0), "a hidden player keeps its lean, kick, window, torso "
			                               "roll and recoil words, and draws nothing");
			expect(same(words_of(b), b0), "a hidden infantry keeps its words and draws nothing");
		}
	}
	const ns::ClientEntityState &a = *view->state().find(0x0003);
	expect(a.rm_state == -1 && a.wpn_playing == -1 && a.wpn_state == -1,
			"a hidden player's dual update and selection never run");
	expect(view->state().find(0x0004)->rm_state == -1, "a hidden infantry's channel never runs");
	expect(a.x == a_x, "a hidden player holds its pose");
	const ns::ClientEntityState &c = *view->state().find(0x0005);
	expect(c.rm_state >= 0 && c.rm_phase > 0 && c.wpn_playing >= 0 &&
					c.lean_angle != 0x10000000,
			"(the visible row takes its legs)");

	// The recoil draw: alone, behind two hidden rows, behind two visible rows.
	auto alone = make_view(src);
	seed(*alone, {{0x0005, nw::EntityClass::Player, 0x00}});
	tick(*alone);
	auto behind_hidden = make_view(src);
	seed(*behind_hidden, hidden);
	tick(*behind_hidden);
	auto behind_shown = make_view(src);
	seed(*behind_shown, {{0x0003, nw::EntityClass::Player, 0x00},
	                     {0x0004, nw::EntityClass::Infantry, 0x00},
	                     {0x0005, nw::EntityClass::Player, 0x00}});
	tick(*behind_shown);
	const int32_t h_alone = alone->state().find(0x0005)->heading_bam;
	expect(behind_hidden->state().find(0x0005)->heading_bam == h_alone,
			"the hidden rows ahead consume no PRNG_Next16 draw");
	expect(behind_shown->state().find(0x0005)->heading_bam != h_alone,
			"(two visible rows ahead do: the third draw's sign differs)");
}

// A channel-less row (the type registered no channel) runs nothing, its chase included; a row
// whose stamp is pending (-2) chases.
void test_channel_less_row_runs_nothing() {
	BodySource src;
	auto view = make_view(src);
	seed(*view, {{0x0003, nw::EntityClass::Player, 0x00}, {0x0004, nw::EntityClass::Player, 0x00}});
	ns::ClientEntityState &none = *view->state().find(0x0003);
	ns::ClientEntityState &pending = *view->state().find(0x0004);
	none.rm_adm_id = -1;
	pending.rm_adm_id = -2;
	// The record staged each row's target at the anchor, away from its spawn pose.
	const Words n0 = words_of(none);
	const int32_t none_x = none.x;
	const int32_t pending_x = pending.x;
	tick(*view);
	expect(same(words_of(none), n0) && none.x == none_x && none.net_interp_progress == 0,
			"a channel-less body takes no leg, no chase and no count");
	expect(pending.x != pending_x && pending.net_interp_progress == 1 &&
					pending.lean_angle != 0x10000000,
			"a pending stamp is not channel-less: the row chases and takes its legs");
}

// A starved row (no record for 512 ticks) still advances its channel, then takes nothing: the
// test reads the progress ahead of the count, so the tick that counts 511 -> 512 still runs the
// legs. Its next record restarts the count.
void test_starved_row_stops() {
	BodySource src;
	const std::vector<Row> rows = {{0x0003, nw::EntityClass::Player, 0x00},
	                               {0x0004, nw::EntityClass::Infantry, 0x00},
	                               {0x0005, nw::EntityClass::Player, 0x00}};
	auto view = make_view(src);
	seed(*view, rows);
	ns::ClientEntityState &a = *view->state().find(0x0003);
	ns::ClientEntityState &b = *view->state().find(0x0004);
	a.net_interp_progress = 511;
	b.net_interp_progress = 511;
	Words a0 = words_of(a);
	tick(*view);
	expect(a.net_interp_progress == 512 && !a.rm_starved && !same(words_of(a), a0),
			"the tick that counts 511 -> 512 runs the legs");
	a0 = words_of(a);
	const Words b0 = words_of(b);
	const int32_t phase = a.rm_phase;
	tick(*view);
	expect(a.rm_starved && a.net_interp_progress == 512, "the next tick starves");
	expect(same(words_of(a), a0), "a starved player takes no leg and draws nothing");
	expect(same(words_of(b), b0), "a starved infantry takes no leg and draws nothing");
	expect(a.rm_phase == phase + 1, "a starved row still advances its channel");

	// Two starved rows ahead consume no draw: the visible row's sign is the alone one's.
	auto alone = make_view(src);
	seed(*alone, {{0x0005, nw::EntityClass::Player, 0x00}});
	tick(*alone);
	auto behind = make_view(src);
	seed(*behind, rows);
	behind->state().find(0x0003)->net_interp_progress = 512;
	behind->state().find(0x0004)->net_interp_progress = 512;
	tick(*behind);
	expect(behind->state().find(0x0005)->heading_bam == alone->state().find(0x0005)->heading_bam,
			"the starved rows ahead consume no PRNG_Next16 draw");

	// The next record restarts the count, and the legs resume.
	view->apply(0x0A, frame(rows));
	expect(a.net_interp_progress == 0, "a record resets the count");
	a0 = words_of(a);
	tick(*view);
	expect(!a.rm_starved && a.lean_angle != a0.lean, "the legs resume");
}

// A seated rider counts its chase: its record resets it, each tick counts it, and at the cap
// it starves, leaving its seat pose and its mounted site alone while the hull moves on; the
// next seated record brings it back onto the seat.
void test_seated_row_counts_and_starves() {
	BodySource src;
	auto view = make_view(src);
	int frame_calls = 0;
	view->set_replica_seat_frame_resolver([&frame_calls](uint16_t, uint16_t, uint8_t,
			ns::ClientReplicaPipeline::ReplicaSeatFrame &out) {
		++frame_calls;
		out = {};
		return true;
	});
	const std::vector<Row> rows = {{0x0003, nw::EntityClass::Player, 0x00, kHull}};
	seed(*view, rows);
	ns::ClientEntityState &rider = *view->state().find(0x0003);
	expect(rider.carrier_handle == kHull && rider.net_seat_valid && rider.net_interp_progress == 0,
			"(the rider is seated on the hull)");
	tick(*view);
	expect(rider.net_interp_progress == 1 && frame_calls == 1, "a seated tick counts the chase");
	view->apply(0x0A, frame(rows));
	expect(rider.net_interp_progress == 0, "the seated record resets the count");
	for (int i = 0; i < 512; ++i) tick(*view);
	expect(rider.net_interp_progress == 512 && !rider.rm_starved,
			"512 seated ticks reach the cap");
	const int calls = frame_calls;
	ns::ClientEntityState &hull = *view->state().find(kHull);
	hull.x += fixed_of(10.0); // the hull moves on (its own chase is starved too)
	const int32_t rider_x = rider.x;
	const int32_t lean = rider.lean_angle;
	tick(*view);
	expect(rider.rm_starved, "the next seated tick starves");
	expect(frame_calls == calls, "a starved rider runs no mounted site");
	expect(rider.x == rider_x, "a starved rider keeps its pose off the moving hull");
	expect(rider.lean_angle == lean, "a starved rider takes no leg");
	view->apply(0x0A, frame(rows, 60.0));
	tick(*view);
	expect(!rider.rm_starved && frame_calls == calls + 1 && rider.x != rider_x,
			"the next seated record puts the rider back on its seat");
}

// D-NET-NEW-L5-27: a seated player's record stages its composed Yaw and its Pitch (no live
// write), and the chase steps both a twelfth per tick (the client's divisor, (d + 6) / 12) for
// twelve ticks, then holds them while the hull turns between records: the mounted site writes
// the body heading, never the Yaw word.
// [orig: NetPacket_SerializePlayerState @0x4C09C0 - Entity_TransformLocalToWorld @0x4C10D4, the
//  stores @0x4C0FD7..0x4C0FF6; Entity_UpdateInfantryPlayerBody @0x4B40E0 - the angle restage
//  @0x4B444A..0x4B445E, the step @0x4B459D, the apply @0x4B43E6..0x4B440A, the seat carry
//  @0x4B654E..0x4B6575]
void test_seated_rider_yaw_chases_its_record() {
	BodySource src;
	auto view = make_view(src);
	std::vector<Row> rows = {{0x0003, nw::EntityClass::Player, 0x00, kHull, 0x10}};
	seed(*view, rows);
	ns::ClientEntityState &rider = *view->state().find(0x0003);
	rider.recoil_pitch = 0; // no recoil step on the Yaw / Pitch words
	// The hull's Euler is 0, the record's local yaw 0x40 and pitch 0x10.
	expect(rider.heading_bam == 0 && rider.pitch_bam == 0 &&
					rider.net_smooth_heading == 0x40000000 && rider.net_smooth_pitch == 0x10000000,
			"the seated record stages the composed Yaw and the Pitch, writing neither live");
	const int32_t yaw_step = (0x40000000 + 6) / 12;
	const int32_t pitch_step = (0x10000000 + 6) / 12;
	tick(*view);
	expect(rider.heading_bam == yaw_step && rider.pitch_bam == pitch_step,
			"the first seated tick steps a twelfth of each");
	for (int i = 1; i < 12; ++i) tick(*view);
	expect(rider.heading_bam == 12 * yaw_step && rider.pitch_bam == 12 * pitch_step,
			"twelve steps, then the chase stops");
	ns::ClientEntityState &hull = *view->state().find(kHull);
	hull.heading_bam += 0x10000000;
	for (int i = 0; i < 8; ++i) tick(*view);
	expect(rider.heading_bam == 12 * yaw_step && rider.pitch_bam == 12 * pitch_step,
			"the Yaw holds while the hull turns between records");
}

constexpr uint16_t kSeatedInfantry = 0x0004;

// One 0x0A: the hull, and an org1 row seated on it (bone 2, the local x +1, the local target
// heading byte 0x10) carrying its two aim bytes, the aim heading's (+0x2EC) and the aim pitch's
// (+0x2D0).
std::vector<uint8_t> seated_infantry_frame(uint8_t aim_heading, uint8_t aim_pitch, uint8_t flags) {
	nw::FrameUpdate fu = hull_update(50.0);
	nw::FrameUpdateRecord rec;
	rec.handle = kSeatedInfantry;
	rec.type_id = kInfantryType;
	rec.cls = nw::EntityClass::Infantry;
	rec.infantry.seat_bone_idx = 2;
	rec.infantry.vehicle_slot_handle = kHull;
	rec.infantry.pos_x_compressed = nw::network_compress_fixedpoint(fixed_of(1.0));
	rec.infantry.yaw_byte = 0x10;
	rec.infantry.flags_byte = flags;
	rec.infantry.aim_heading_byte = aim_heading;
	rec.infantry.aim_pitch_byte = aim_pitch;
	rec.infantry.anim_byte = as::kIdle;
	fu.records.push_back(rec);
	return nw::encode_frame_update(fu);
}

// A view with the seat frame's body heading 0 and the carrier's mount config (3 when wide).
std::unique_ptr<ns::ClientReplicaPipeline> seated_infantry_view(BodySource &src, bool wide,
		uint8_t aim_heading, uint8_t aim_pitch, uint8_t flags = 0) {
	auto view = make_view(src);
	view->set_replica_seat_frame_resolver([wide](uint16_t, uint16_t, uint8_t,
			ns::ClientReplicaPipeline::ReplicaSeatFrame &out) {
		out = {};
		out.mount_config_valid = wide;
		out.mount_config = wide ? 3 : 0;
		return true;
	});
	view->state().upsert(kSeatedInfantry).type_id = kInfantryType;
	view->apply(0x0A, seated_infantry_frame(aim_heading, aim_pitch, flags));
	view->state().find(kSeatedInfantry)->rm_adm_id = 0;
	return view;
}

// D-NET-NEW-L5-29: a live seated org1 row's record stages its aim words and writes neither the
// live Yaw nor the Pitch; each tick its seat branch steps the Yaw a quarter of the way to the
// aim heading, the step clamped to +-0x2000000, and the Pitch an eighth of the way to the aim
// pitch. The hull's turn is not carried into the Yaw.
// [orig: NetPacket_SerializeInfantryEntityState @0x4C0320 - the stores @0x4C066C / @0x4C0672;
//  Entity_UpdateInfantryAI @0x4B9910 - the seat test @0x4B9960..0x4B9985, the look
//  @0x4BEF57..0x4BEF97]
void test_seated_infantry_look_chases_its_aim_words() {
	BodySource src;
	auto view = seated_infantry_view(src, false, 0x20, 0xF0);
	ns::ClientEntityState &rider = *view->state().find(kSeatedInfantry);
	expect(rider.carrier_handle == kHull && rider.mount_bone == 2 && rider.net_seat_valid,
			"(the org1 row is seated on the hull)");
	expect(rider.heading_bam == 0 && rider.pitch_bam == 0,
			"the seated record writes neither the live Yaw nor the Pitch");
	tick(*view);
	// (0x20000000 + 2) >> 2 clamps to 0x2000000; (-0x10000000 + 4) >> 3 = -0x2000000.
	expect(rider.heading_bam == 0x02000000 && rider.pitch_bam == -0x02000000,
			"the first tick steps the Yaw a clamped quarter and the Pitch an eighth");
	view->state().find(kHull)->heading_bam += 0x10000000;
	tick(*view);
	// (-0x0E000000 + 4) >> 3 = -0x01C00000.
	expect(rider.heading_bam == 0x04000000 && rider.pitch_bam == -0x03C00000,
			"the second tick steps from the live words (no carrier pose: no ride)");
}

// D-NET-NEW-L5-38: a live seated org1 body rides its parent, its ground: the hull's turn this tick
// (0x1000000) goes into the Yaw and the aim heading (+0x2EC) unless the parent is IsTurret, and
// into the target (+0x1A8) either way, ahead of the seat branch, whose bone pose then overwrites
// the body heading and keeps the seat's position (the ride's 1 u translation does not show).
// [orig: Entity_UpdateInfantryAI @0x4B9910 - the ground store @0x4B9A0F..0x4B9A11, the ride
//  @0x4BA45D..0x4BA88E (the gate @0x4BA812..0x4BA840), the seat pose @0x4BED96..0x4BEDDA, the
//  seated look @0x4BEF57..0x4BEF84]
void test_seated_org1_rider_rides_its_parent() {
	for (const bool turret : {false, true}) {
		BodySource src;
		auto view = seated_infantry_view(src, false, 0x20, 0x00);
		view->set_carrier_pose_provider(
				[](uint16_t handle, ns::ClientReplicaPipeline::CarrierPose &out) {
					if (handle != kHull) return false;
					out = ns::ClientReplicaPipeline::CarrierPose{};
					out.pos[0] = 1 << 16;
					out.yaw = 0x01000000;
					out.bound_radius = 0x7FFFFFFF;
					return true;
				});
		view->set_replica_def_attrib2_resolver([turret](uint16_t type) {
			return type == kHullType && turret ? 0x1000u : 0u;
		});
		ns::ClientEntityState &rider = *view->state().find(kSeatedInfantry);
		tick(*view);
		expect(rider.x == fixed_of(51.0) && rider.rm_body_heading == 0 &&
						rider.net_target_heading_bam == 0x01000000 && rider.resolved_ground == kHull,
				"the seated ride turns the target; the seat keeps the position and the body heading");
		// The seated look: (0x20000000 + 2) >> 2 clamps to 0x2000000 either way.
		expect(rider.rm_aim_heading_word == (turret ? 0x20000000 : 0x21000000) &&
						rider.heading_bam == (turret ? 0x02000000 : 0x03000000),
				turret ? "an IsTurret parent's ride leaves the seated Yaw and aim heading"
				       : "the seated ride turns the Yaw and the aim heading with the hull");
	}
}

// D-NET-NEW-L5-34: an org1 body is in its vehicle only while its Health is above 0. The 0x13
// zeroes a seated body's Health ahead of the record that detaches it (a dead record names no
// seat: the host's death edge detaches the body first), so for those ticks the body takes the
// free-standing branch with its parent kept: no seat pose (the hull's 2 u move leaves it), its
// parent as its ground (the ride's 1 u), the death edge, and the on-foot look: an eighth of the
// way to the aim heading, not the seat's clamped quarter, held within 90 degrees of the body
// heading unless the parent's config is 3, 4, 5 or 7.
// [orig: Entity_UpdateInfantryAI @0x4B9910 - the seat test @0x4B9960..0x4B9985, the ground store
//  @0x4B9A0F..0x4B9A11, the death edge @0x4B9C51, the eighth @0x4BEB8C..0x4BEBE7, the parent's
//  config gate @0x4BEF9A..0x4BEFC3, the clamp @0x4BEFC5..0x4BEFED; Entity_KillBySlotId
//  @0x42BCE0 - Health = 0 @0x42BD33]
void test_dead_seated_infantry_takes_the_free_standing_branch() {
	for (const bool wide : {false, true}) {
		BodySource src;
		auto view = seated_infantry_view(src, wide, 0x20, 0xF0);
		view->set_carrier_pose_provider(
				[](uint16_t handle, ns::ClientReplicaPipeline::CarrierPose &out) {
					if (handle != kHull) return false;
					out = ns::ClientReplicaPipeline::CarrierPose{};
					out.pos[0] = 1 << 16; // the parent moved 1 u this tick
					out.bound_radius = 0x7FFFFFFF;
					return true;
				});
		ns::ClientEntityState &rider = *view->state().find(kSeatedInfantry);
		tick(*view); // alive: the seat's clamped quarter and eighth
		expect(rider.heading_bam == 0x02000000 && rider.pitch_bam == -0x02000000 &&
						rider.x == fixed_of(51.0),
				"(the live seated tick looks from its seat at the hull's x + 1)");
		const int16_t death = as::kDeathBulletBase + 6;
		view->apply(nw::s2c::ENTITY_DEATH,
				{static_cast<uint8_t>(kSeatedInfantry & 0xFF),
				 static_cast<uint8_t>(kSeatedInfantry >> 8), static_cast<uint8_t>(death & 0xFF),
				 static_cast<uint8_t>(death >> 8)});
		view->state().find(kHull)->x += fixed_of(2.0);
		tick(*view);
		expect(rider.carrier_handle == kHull && rider.resolved_ground == kHull &&
						(rider.rm_entity_flags & nw::world::kEntityFlagDead) != 0,
				"the dead seated body keeps its parent as its ground and takes its death edge");
		expect(rider.x == fixed_of(52.0),
				"no seat pose: the body rides its parent's 1 u, not the hull's 2 u seat move");
		// (0x1E000000 + 4) >> 3 = 0x3C00000; (-0xE000000 + 4) >> 3 clamps to -0xC00000.
		expect(rider.heading_bam == 0x05C00000 && rider.pitch_bam == -0x02C00000,
				"the on-foot look steps an eighth of the way to the aim words");
		// Half a turn off the body heading: the eighth (0x40800000 + 4) >> 3 = 0x8100000 takes
		// the Yaw past 90 degrees, held there unless the parent's config is wide.
		rider.heading_bam = 0x3F800000;
		rider.rm_aim_heading_word = static_cast<int32_t>(0x80000000u);
		tick(*view);
		expect(rider.heading_bam == (wide ? 0x47900000 : 0x40000000),
				wide ? "a wide parent config lets the dead body's Yaw past 90 degrees"
				     : "the Yaw holds 90 degrees off the body heading");
	}
}

// D-NET-NEW-L5-30 (found on the way): a seated org1 gunner's look reads its aim heading WORD,
// which the record writes and the gun channel cannot reach. The joiner's gun channel writes the
// gunner's Pitch high byte back into the row's class-specific byte (an infantry row's aim heading
// byte); a look that read that byte chased the gunner's own Pitch (0x30) instead of its aim
// heading (0x01): a clamped 0x2000000 where retail steps (0x1000000 + 2) >> 2 = 0x400000.
// [orig: NetPacket_SerializeInfantryEntityState @0x4C0320 - the store @0x4C066C;
//  Entity_UpdateChildAttachment @0x4409A0 - the immediate path @0x440B39..0x440B58 (the gun's
//  words, never the occupant's aim words); Entity_UpdateInfantryAI @0x4B9910 - the seated look
//  @0x4BEF57..0x4BEF84]
void test_gun_channel_leaves_the_gunner_aim_heading() {
	BodySource src;
	auto view = seated_infantry_view(src, false, 0x01, 0x00);
	ns::ClientEntityState &rider = *view->state().find(kSeatedInfantry);
	rider.pitch_bam = 0x30000000;
	auto world = std::make_unique<opennova::world::World>();
	world->registry.configure_pool(1, 8);
	std::vector<opennova::mission::ItemSeatSpec> specs(1);
	specs[0].type_id = kHullType;
	opennova::world::Seat gunner_seat;
	gunner_seat.type = opennova::world::SeatType::Gunner;
	gunner_seat.bone_index = 2;
	specs[0].seats.push_back(gunner_seat);
	view->tick_remote_motion(0xFFFF); // the walk arms the seated look
	opennova::inmatch::tick_replica_emplaced_channels(view->state(), specs, *world, 0xFFFF,
			nullptr, false, opennova::inmatch::ReplicaGunLeg::Pool1Walk);
	expect(rider.pitch_byte == 0x30 && rider.heading_bam == 0 && rider.pitch_bam == 0x30000000,
			"(the gun channel left the look and wrote the Pitch byte back)");
	view->tick_org1_looks();
	expect(rider.heading_bam == 0x00400000,
			"the seated gunner's look chases its aim heading word, not the written-back byte");
}

// D-NET-NEW-L5-29: the seated look holds the Yaw within 90 degrees of the seat's body heading
// (+0x8C, the seat bone's yaw) unless the carrier's config is 3, 4, 5 or 7.
// [orig: Entity_UpdateInfantryAI @0x4B9910 - the config gate @0x4BEF9A..0x4BEFC3, the clamp
//  @0x4BEFC5..0x4BEFED]
void test_seated_infantry_look_holds_within_ninety_degrees() {
	for (const bool wide : {false, true}) {
		BodySource src;
		auto view = seated_infantry_view(src, wide, 0x80, 0x00);
		ns::ClientEntityState &rider = *view->state().find(kSeatedInfantry);
		for (int i = 0; i < 32; ++i) tick(*view);
		expect(rider.heading_bam == -0x40000000, "32 clamped steps turn the Yaw 90 degrees");
		tick(*view);
		expect(rider.heading_bam == (wide ? -0x42000000 : -0x40000000),
				wide ? "a wide config lets the Yaw past 90 degrees"
				     : "the Yaw holds 90 degrees off the seat's body heading");
	}
}

constexpr uint16_t kFreeInfantry = 0x0007;

// One 0x0A: the hull, and a free-standing org1 row at the anchor (the yaw byte 0x10) carrying
// its two aim bytes, the aim heading's (+0x2EC) and the aim pitch's (+0x2D0).
std::vector<uint8_t> free_infantry_frame(uint8_t aim_heading, uint8_t aim_pitch,
		uint8_t flags = 0, uint8_t yaw = 0x10) {
	nw::FrameUpdate fu = hull_update(50.0);
	nw::FrameUpdateRecord rec;
	rec.handle = kFreeInfantry;
	rec.type_id = kInfantryType;
	rec.cls = nw::EntityClass::Infantry;
	rec.infantry.vehicle_slot_handle = 0xFFFF;
	rec.infantry.yaw_byte = yaw;
	rec.infantry.flags_byte = flags;
	rec.infantry.aim_heading_byte = aim_heading;
	rec.infantry.aim_pitch_byte = aim_pitch;
	rec.infantry.anim_byte = as::kIdle;
	fu.records.push_back(rec);
	return nw::encode_frame_update(fu);
}

std::unique_ptr<ns::ClientReplicaPipeline> free_infantry_view(BodySource &src,
		uint8_t aim_heading, uint8_t aim_pitch) {
	auto view = make_view(src);
	view->state().upsert(kFreeInfantry).type_id = kInfantryType;
	view->apply(0x0A, free_infantry_frame(aim_heading, aim_pitch));
	view->state().find(kFreeInfantry)->rm_adm_id = 0;
	return view;
}

// D-NET-NEW-L5-30: a free-standing org1 row's body heading (+0x8C) and its look Yaw (+0x10) are
// two words. The body chase steps the body heading a quarter of the way to the promoted target
// (+0x1A8) and adds the same step to the Yaw; then the look (aim flag clear on a client) steps
// the Yaw an eighth of the way to the aim heading (+0x2EC) and the Pitch to the aim pitch
// (+0x2D0), each step's positive clamp testing 16 times the stored one (0xE000000 / 0xC000000
// against 0xE00000 / 0xC00000). The record writes the aim words, never the live ones; a respawn
// record's reset stamps the body heading with the Yaw.
// [orig: NetPacket_SerializeInfantryEntityState @0x4C0320 - the promote @0x4C0653, the stores
//  @0x4C066C / @0x4C0672, the respawn Yaw @0x4C05D5; Entity_UpdateInfantryAI @0x4B9910 - the
//  body chase @0x4BE8FD..0x4BE931, the eighth @0x4BEB8C..0x4BEBE7; Entity_ResetToSpawnState
//  @0x4B9610 - +0x8C @0x4B968C]
void test_free_standing_infantry_body_and_look_split() {
	BodySource src;
	auto view = free_infantry_view(src, 0x20, 0xF0);
	view->apply(0x0A, free_infantry_frame(0x20, 0xF0)); // promotes the first record's 0x10
	ns::ClientEntityState &es = *view->state().find(kFreeInfantry);
	expect(es.net_target_heading_bam == 0x10000000 && es.rm_body_heading == 0 &&
					es.heading_bam == 0 && es.pitch_bam == 0 &&
					es.rm_aim_heading_word == 0x20000000 &&
					es.rm_aim_word == static_cast<int32_t>(0xF0000000u),
			"the records stage the target and write the aim words, never the live Yaw or Pitch");
	tick(*view);
	// (0x10000000 + 2) >> 2 = 0x4000000; (0x1C000000 + 4) >> 3 = 0x3800000 (under the positive
	// test 0xE000000); (-0x10000000 + 4) >> 3 = -0x2000000, clamped to -0xC00000.
	expect(es.rm_body_heading == 0x04000000, "the body chase steps the body heading a quarter");
	expect(es.heading_bam == 0x07800000,
			"the Yaw takes the body's step, then an eighth of the way to the aim heading");
	expect(es.pitch_bam == -0x00C00000, "the Pitch steps an eighth, clamped to -0xC00000");
	tick(*view);
	// 0x3000000 to the body; (0x15800000 + 4) >> 3 = 0x2B00000; the Pitch clamps again.
	expect(es.rm_body_heading == 0x07000000 && es.heading_bam == 0x0D300000 &&
					es.pitch_bam == -0x01800000,
			"the second tick steps from the live words");

	// A respawn record writes the Yaw live and the reset stamps the body heading with it.
	view->apply(0x0A, free_infantry_frame(0x20, 0xF0,
			static_cast<uint8_t>(nw::world::kEntityFlagDead)));
	view->apply(0x0A, free_infantry_frame(0x20, 0xF0, 0, 0x30));
	expect(es.heading_bam == 0x30000000 && es.rm_body_heading == 0x30000000,
			"the respawn record's reset stamps the body heading with the new Yaw");
	// D-NET-NEW-L5-33: its promote still runs after the reset: the target is the dead
	// record's stage (0x10), the new yaw only the stage.
	// [orig: NetPacket_SerializeInfantryEntityState @0x4C0320 - the reset call @0x4C05EC,
	//  the promote @0x4C0646..0x4C0653, the stage @0x4C0693]
	expect(es.net_target_heading_bam == 0x10000000 && es.net_smooth_heading == 0x30000000,
			"the respawn record promotes the previous stage into the target");
}

// D-NET-NEW-L5-30: the free-standing look holds the Yaw within 90 degrees of the body heading.
// The aim heading 0x80 is half a turn off; each eighth clamps to -0xE00000, 73 of them stay
// within 0x3FE00000, and the 74th is held at -0x40000000.
// [orig: Entity_UpdateInfantryAI @0x4B9910 - the eighth @0x4BEB8C..0x4BEBB4, the clamp with no
//  parent @0x4BEF9A..0x4BEFA2 -> @0x4BEFC5..0x4BEFED]
void test_free_standing_infantry_look_holds_within_ninety_degrees() {
	BodySource src;
	auto view = free_infantry_view(src, 0x80, 0x00); // one record: the target is 0
	ns::ClientEntityState &es = *view->state().find(kFreeInfantry);
	for (int i = 0; i < 73; ++i) tick(*view);
	expect(es.rm_body_heading == 0 && es.heading_bam == -73 * 0xE00000,
			"73 clamped eighths turn the Yaw, the body holds its target");
	tick(*view);
	expect(es.heading_bam == -0x40000000, "the 74th is held 90 degrees off the body heading");
	tick(*view);
	expect(es.heading_bam == -0x40000000, "and holds there");
}

// D-NET-NEW-L5-30: a deck ride carries the carrier's yaw delta into the aim heading word too,
// so the look keeps its aim on the turning deck.
// [orig: Entity_UpdateInfantryAI @0x4B9910 - the ride's adds: Yaw @0x4BA849, +0x2EC @0x4BA84C,
//  +0x2D0 @0x4BA852, +0x8C @0x4BA861, +0x1A8 @0x4BA867]
void test_deck_ride_turns_the_aim_words() {
	BodySource src;
	auto view = free_infantry_view(src, 0x20, 0x00);
	static constexpr uint16_t kDeck = 0x1009; // static: MSVC needs no capture for it
	view->set_carrier_pose_provider(
			[](uint16_t handle, ns::ClientReplicaPipeline::CarrierPose &out) {
				if (handle != kDeck) return false;
				out = ns::ClientReplicaPipeline::CarrierPose{};
				out.yaw = 0x01000000; // the deck turns 0x1000000 this tick
				out.bound_radius = 0x7FFFFFFF;
				return true;
			});
	ns::ClientEntityState &es = *view->state().find(kFreeInfantry);
	es.resolved_ground = kDeck;
	view->tick_remote_motion(0xFFFF);
	expect(es.rm_aim_heading_word == 0x21000000 && es.rm_aim_word == 0 &&
					es.rm_body_heading == 0x01000000 &&
					es.net_target_heading_bam == 0x01000000,
			"the deck's yaw delta turns the aim heading word with the body and the target");
	// D-NET-NEW-L5-35: and the four leg words [orig: @0x4BA86D..0x4BA87F].
	expect(es.rm_leg_yaw[0] == 0x01000000 && es.rm_leg_yaw[1] == 0x01000000 &&
					es.rm_leg_target[0] == 0x01000000 && es.rm_leg_target[1] == 0x01000000,
			"the deck's yaw delta turns the four leg words");
}

// D-NET-NEW-L5-35: a free-standing org1 row's legs. rm_leg_yaw[0] / [1] are the right / left leg
// yaws (+0x2D4 / +0x2D8), rm_leg_target[0] / [1] their re-plant targets (+0x2E4 / +0x2E8). An
// idle leg re-plants on the midpoint of the target and the body heading when it drifts past
// 0x38E38E0 and past 0x15555540 or on its 64-tick window, the left's at key 32, the right's at
// key 64 (a client's stagger key is the bare tick: its DcbId is 0); each leg chases a quarter,
// clamped to +-0x5000000, held within 0x20000000 of the body heading. A walking body (the def's
// attrib 0x200 here) half-snaps the right leg to the target, pulls the left a quarter of the
// rest, re-plants both on the target and chases a sixteenth.
// [orig: Entity_UpdateInfantryAI @0x4B9910 - the key @0x4B9948..0x4B9959, the selector
//  @0x4BE944..0x4BE967, the idle re-plant @0x4BE969..0x4BE9CC, the walk path
//  @0x4BE9D4..0x4BEA0B, the chases @0x4BEA11..0x4BEB04]
void test_free_standing_infantry_legs() {
	{
		BodySource src;
		auto view = free_infantry_view(src, 0x00, 0x00);
		ns::ClientEntityState &es = *view->state().find(kFreeInfantry);
		es.rm_body_heading = es.net_target_heading_bam = 0x10000000;
		for (int key = 1; key <= 31; ++key) tick(*view);
		expect(es.rm_leg_yaw[0] == 0 && es.rm_leg_yaw[1] == 0 && es.rm_leg_target[0] == 0 &&
						es.rm_leg_target[1] == 0,
				"an idle leg 0x10000000 off its re-plant point waits for its window");
		tick(*view); // key 32
		expect(es.rm_leg_target[1] == 0x10000000 && es.rm_leg_yaw[1] == 0x04000000 &&
						es.rm_leg_target[0] == 0 && es.rm_leg_yaw[0] == 0,
				"the left leg re-plants at key 32 and chases a quarter");
		for (int key = 33; key <= 64; ++key) tick(*view);
		expect(es.rm_leg_target[0] == 0x10000000 && es.rm_leg_yaw[0] == 0x04000000,
				"the right leg re-plants 32 ticks later, at key 64");
	}
	{
		BodySource src;
		auto view = free_infantry_view(src, 0x00, 0x00);
		view->set_replica_def_attrib_resolver([](uint16_t) { return 0x200u; });
		ns::ClientEntityState &es = *view->state().find(kFreeInfantry);
		es.rm_body_heading = 0;
		es.net_target_heading_bam = 0x08000000;
		tick(*view);
		// The body steps 0x2000000; the right leg half-snaps to 0x4000000, the left pulls
		// (0x8000000 - 0x4000000) >> 2; then sixteenths: (0x4000000 + 8) >> 4 = 0x400000 and
		// (0x7000000 + 8) >> 4 = 0x700000.
		expect(es.rm_body_heading == 0x02000000 && es.rm_leg_target[0] == 0x08000000 &&
						es.rm_leg_target[1] == 0x08000000 && es.rm_leg_yaw[0] == 0x04400000 &&
						es.rm_leg_yaw[1] == 0x01700000,
				"a walking body's legs shuffle toward the target");
	}
	{
		BodySource src;
		auto view = free_infantry_view(src, 0x00, 0x00);
		ns::ClientEntityState &es = *view->state().find(kFreeInfantry);
		es.rm_body_heading = es.net_target_heading_bam = 0x30000000;
		tick(*view);
		// Past 0x15555540 both re-plant at once; the quarter 0xC000000 clamps to 0x5000000, and
		// 0x5000000 is held 0x20000000 off the body heading.
		expect(es.rm_leg_target[0] == 0x30000000 && es.rm_leg_target[1] == 0x30000000 &&
						es.rm_leg_yaw[0] == 0x10000000 && es.rm_leg_yaw[1] == 0x10000000,
				"a far leg re-plants at once and is held within 0x20000000 of the body");
	}
}

// D-NET-NEW-L5-37: an org1 body's deck ride adds the yaw delta to its Yaw and its aim heading
// (+0x2EC) unless its parent's def is IsTurret (attrib2 0x1000); the body heading, the target and
// the legs take it either way. A dead body in the seat its record names (the 0x13 window) rides
// its parent: the hull turns 0x1000000 this tick.
// [orig: Entity_UpdateInfantryAI @0x4B9910 - the gate @0x4BA812..0x4BA840, the adds
//  @0x4BA842..0x4BA852, the rest @0x4BA85A..0x4BA88E]
void test_org1_ride_skips_the_aim_adds_on_a_turret() {
	for (const bool turret : {false, true}) {
		BodySource src;
		auto view = seated_infantry_view(src, false, 0x20, 0x00);
		view->set_carrier_pose_provider(
				[](uint16_t handle, ns::ClientReplicaPipeline::CarrierPose &out) {
					if (handle != kHull) return false;
					out = ns::ClientReplicaPipeline::CarrierPose{};
					out.yaw = 0x01000000;
					out.bound_radius = 0x7FFFFFFF;
					return true;
				});
		view->set_replica_def_attrib2_resolver([turret](uint16_t type) {
			return type == kHullType && turret ? 0x1000u : 0u;
		});
		ns::ClientEntityState &rider = *view->state().find(kSeatedInfantry);
		// Alive and seated, the body rides its parent too (D-NET-NEW-L5-38): the target turns
		// 0x1000000; the Yaw (0x3000000, or 0x2000000 under IsTurret) and the aim heading
		// (0x21000000, or 0x20000000) as test_seated_org1_rider_rides_its_parent pins; the
		// seat pose leaves the body heading and the legs at 0.
		tick(*view);
		view->apply(nw::s2c::ENTITY_DEATH, {static_cast<uint8_t>(kSeatedInfantry & 0xFF),
				static_cast<uint8_t>(kSeatedInfantry >> 8), 0, 0});
		tick(*view);
		// The body chase (0x1000000 + 2) >> 2 = 0x400000, then the ride's 0x1000000.
		expect(rider.rm_body_heading == 0x01400000 && rider.net_target_heading_bam == 0x02000000 &&
						rider.rm_leg_yaw[0] == 0x01000000 && rider.rm_leg_target[1] == 0x01000000,
				"the ride turns the body heading, the target and the legs either way");
		// The Yaw took the chase's 0x400000 (and the ride's 0x1000000 without IsTurret); the look
		// then steps an eighth of the 0x1DC00000 left: 0x3B80000.
		expect(rider.rm_aim_heading_word == (turret ? 0x20000000 : 0x22000000) &&
						rider.heading_bam == (turret ? 0x05F80000 : 0x07F80000),
				turret ? "an IsTurret parent's ride leaves the Yaw and the aim heading"
				       : "any other parent's ride turns the Yaw and the aim heading");
	}
}

// A source whose channel yields no frame: the row's update ends before its velocity integrate,
// so a test reads the velocity its slope pass left.
struct NoFrameSource final : public opennova::world::IRootMotionSource {
	bool has_clip(int, int) const override { return true; }
	bool advance(int, int, int32_t &, opennova::world::RootMotionFrame &) override {
		return false;
	}
	int32_t clip_length_ticks(int, int, int) const override { return 62; }
	bool clip_loops(int, int, int) const override { return true; }
};

// D-NET-NEW-L5-36: a free-standing org1 row's slope pass, on every eighth key tick. A body that
// does not conform decays savedLivePitch (+0x90) and Roll a sixteenth toward level. A
// conforming one (the def's attrib 0x200 here) probes four columns (0x5800 fore and aft, 0x1600
// to the sides at Yaw 0); on a ground rising 2 per unit of x and 1 per unit of y the slopes
// (0x16000 << 14, 0x2C00 << 16) clamp to 0x271C71A0, past the slide threshold 0x22222200 on
// both, so the velocity takes 0x800 against the fore-aft slope and 0x800 against the side one,
// and savedLivePitch and Roll chase an eighth: (0x271C71A0 + 4) >> 3 = 0x4E38E34. A corpse aims
// its +0x2D0 at the fore-aft slope and its +0x2EC at the target (+0x1A8).
// [orig: Entity_UpdateInfantryAI @0x4B9910 - the key test @0x4BA066..0x4BA079, the selector
//  @0x4BA10F..0x4BA131, the decay @0x4BA133..0x4BA152, the probes @0x4BA15A..0x4BA221, the
//  clamps @0x4BA1D1..0x4BA244, the slides @0x4BA249..0x4BA2FB, the corpse's aim words
//  @0x4BA301..0x4BA319, the chases @0x4BA320..0x4BA343]
void test_org1_slope_pass() {
	{
		BodySource src;
		auto view = free_infantry_view(src, 0x00, 0x00);
		ns::ClientEntityState &es = *view->state().find(kFreeInfantry);
		es.rm_body_pitch = 0x01000000;
		es.roll_bam = -0x01000000;
		for (int key = 1; key <= 7; ++key) tick(*view);
		expect(es.rm_body_pitch == 0x01000000 && es.roll_bam == -0x01000000,
				"the slope pass waits for an eighth key tick");
		tick(*view); // key 8
		expect(es.rm_body_pitch == 0x00F00000 && es.roll_bam == -0x00F00000,
				"a body that does not conform decays savedLivePitch and Roll a sixteenth");
	}
	for (const bool dead : {false, true}) {
		NoFrameSource src;
		auto view = make_view(src);
		view->state().upsert(kFreeInfantry).type_id = kInfantryType;
		view->apply(0x0A, free_infantry_frame(0x00, 0x00, 0, 0x00));
		ns::ClientEntityState &es = *view->state().find(kFreeInfantry);
		es.rm_adm_id = 0;
		view->set_replica_def_attrib_resolver([](uint16_t) { return 0x200u; });
		view->set_replica_ground_probe(
				[](const ns::ClientReplicaPipeline::ReplicaGroundColumn &q) {
					return q.dx * 2 + q.dy;
				});
		if (dead) {
			view->apply(nw::s2c::ENTITY_DEATH, {static_cast<uint8_t>(kFreeInfantry & 0xFF),
					static_cast<uint8_t>(kFreeInfantry >> 8), 0, 0});
			es.net_target_heading_bam = es.rm_body_heading = 0x01000000;
		}
		for (int key = 1; key <= 8; ++key) tick(*view);
		expect(es.rm_vel_xy[0] == -0x800 && es.rm_vel_xy[1] == -0x800,
				"the steep slopes slide the body 0x800 down each");
		expect(es.rm_body_pitch == 0x04E38E34 && es.roll_bam == 0x04E38E34,
				"savedLivePitch and Roll chase an eighth of the way to the clamped slopes");
		if (dead)
			expect(es.rm_aim_word == 0x271C71A0 && es.rm_aim_heading_word == 0x01000000,
					"a corpse aims +0x2D0 at the fore-aft slope and +0x2EC at the target");
	}
}

// D-NET-NEW-L5-36: a dead org1 body in the air tumbles on every eighth key tick: at key 8 the
// ramps are a = 24 * 0xFFFFFF and b = -24 * 0xFFFFFF (((8 >> 6) - 8) & 0x3F = 56), so +0x2D0 is
// a + b = 0 and +0x2EC the target as it stood; savedLivePitch takes (a + 4) >> 3, Roll
// (b + 4) >> 3, and the target turns by b >> 2.
// [orig: Entity_UpdateInfantryAI @0x4B9910 - the gate @0x4BA07F..0x4BA092, the ramps
//  @0x4BA094..0x4BA0BD, the aim words @0x4BA0C0..0x4BA0CC, the chases @0x4BA0D2..0x4BA0F5, the
//  target @0x4BA0F7..0x4BA0FA]
void test_org1_dead_body_tumbles_in_the_air() {
	NoFrameSource src;
	auto view = make_view(src);
	view->state().upsert(kFreeInfantry).type_id = kInfantryType;
	view->apply(0x0A, free_infantry_frame(0x00, 0x7F, 0, 0x00));
	ns::ClientEntityState &es = *view->state().find(kFreeInfantry);
	es.rm_adm_id = 0;
	view->apply(nw::s2c::ENTITY_DEATH, {static_cast<uint8_t>(kFreeInfantry & 0xFF),
			static_cast<uint8_t>(kFreeInfantry >> 8), 0, 0});
	tick(*view); // key 1: the death edge
	es.rm_entity_flags |= nw::world::kEntityFlagInAir;
	es.net_target_heading_bam = es.rm_body_heading = 0x10000000;
	for (int key = 2; key <= 7; ++key) tick(*view);
	expect(es.rm_aim_heading_word == 0 && es.rm_body_pitch == 0 && es.roll_bam == 0,
			"(no tumble before key 8)");
	tick(*view); // key 8
	expect(es.rm_aim_word == 0 && es.rm_aim_heading_word == 0x10000000,
			"the tumble aims +0x2D0 at a + b and +0x2EC at the target");
	expect(es.rm_body_pitch == 0x02FFFFFD && es.roll_bam == -0x02FFFFFD,
			"savedLivePitch and Roll chase an eighth of the way to the ramps");
	// The target turned by b >> 2 = -0x5FFFFFA; the body chase then stepped its quarter.
	expect(es.net_target_heading_bam == 0x0A000006,
			"the tumble turns the target by b >> 2");
}

// D-NET-NEW-L5-35: the class init stamps a spawned org1 row's aim heading and its four leg words
// with the Yaw (Entity_InitAllFromModels after the 0x0C stream), and the respawn record's reset
// stamps the legs again.
// [orig: Entity_InitOrganicAI @0x4BFCC0 - @0x4BFD53..0x4BFDA5; Entity_ResetToSpawnState
//  @0x4B9610 - @0x4B969E..0x4B96B0]
void test_org1_init_and_reset_stamp_the_legs() {
	BodySource src;
	auto view = make_view(src);
	nw::OrganicSpawnRecord rec;
	rec.slot_id = kFreeInfantry;
	rec.def_type = 3;
	rec.item_type_id = kInfantryType;
	rec.orientation = 0x12345678;
	nw::OrganicSpawnBatch batch;
	batch.records.push_back(rec);
	batch.entity_count = 1;
	view->apply(nw::s2c::ENTITY_SPAWN_BATCH, nw::encode_organic_spawn_batch(batch));
	ns::ClientEntityState &es = *view->state().find(kFreeInfantry);
	expect(es.rm_aim_heading_word == 0x12345678 && es.rm_leg_yaw[0] == 0x12345678 &&
					es.rm_leg_yaw[1] == 0x12345678 && es.rm_leg_target[0] == 0x12345678 &&
					es.rm_leg_target[1] == 0x12345678 && es.rm_body_heading == 0x12345678 &&
					es.pitch_bam == 0,
			"the 0x0C row's class init stamps the aim heading and the legs with the Yaw");
	view->apply(0x0A, free_infantry_frame(0x20, 0x00));
	view->apply(0x0A, free_infantry_frame(0x20, 0x00,
			static_cast<uint8_t>(nw::world::kEntityFlagDead)));
	view->apply(0x0A, free_infantry_frame(0x20, 0x00, 0, 0x30));
	expect(es.heading_bam == 0x30000000 && es.rm_leg_yaw[0] == 0x30000000 &&
					es.rm_leg_yaw[1] == 0x30000000 && es.rm_leg_target[0] == 0x30000000 &&
					es.rm_leg_target[1] == 0x30000000 && es.rm_aim_heading_word == 0x20000000,
			"the respawn reset stamps the legs; the record's aim heading lands over its +0x2EC");
}

// D-NET-NEW-L5-32: an org1 body's deck ride tests no radius: 223 u off a carrier whose bound
// radius is a quarter unit, it still rides the carrier's 1 u step and keeps its ground link (a
// player body there drops both: netsim_client_replica_pipeline_contact_resolver pins that).
// [orig: Entity_UpdateInfantryAI @0x4B9910 - the ride's head @0x4BA45D..0x4BA479, the
//  translation @0x4BA47F..0x4BA4C9; Entity_UpdateInfantryPlayerBody @0x4B40E0 - the org2 drop
//  @0x4B529F..0x4B52FF]
void test_org1_deck_ride_has_no_radius_drop() {
	BodySource src;
	auto view = free_infantry_view(src, 0x00, 0x00);
	static constexpr uint16_t kDeck = 0x1009; // static: MSVC needs no capture for it
	view->set_carrier_pose_provider(
			[](uint16_t handle, ns::ClientReplicaPipeline::CarrierPose &out) {
				if (handle != kDeck) return false;
				out = ns::ClientReplicaPipeline::CarrierPose{};
				out.pos[0] = 1 << 16; // the deck moved 1 u this tick
				out.bound_radius = 1 << 14;
				return true;
			});
	ns::ClientEntityState &es = *view->state().find(kFreeInfantry);
	es.resolved_ground = kDeck;
	view->tick_remote_motion(0xFFFF);
	// The visit snaps the row onto its record (the anchor, 100 u on x), then the ride adds 1 u.
	expect(es.resolved_ground == kDeck && es.x == fixed_of(101.0),
			"an org1 body rides its deck at any distance, keeping the ground link");
}

// Row 2 (D-NET-NEW-L5-29 / D-NET-NEW-L5-27 / D-NET-NEW-L5-30): the replica aim overlay's pitch is
// the body's live Pitch word for every row; an org1 row's body chain turns by its body heading.
// [orig: Entity_BuildBoneTransformMatrices @0x4B1290 - `mov edx, [ebp+14h]` @0x4B17AD;
//  Entity_UpdateInfantryAI @0x4B9910 - the body chase @0x4BE8FD..0x4BE931]
void test_overlay_pitch_is_the_live_pitch_word() {
	ns::ClientState state;
	opennova::anim::AimOverlayInputs in;
	ns::ClientEntityState player;
	player.handle = 0x0003;
	player.cls = nw::EntityClass::Player;
	player.carrier_handle = kHull;
	player.mount_bone = 1;
	player.pitch_bam = 0x01234567;
	player.pitch_byte = 0x40;
	expect(opennova::inmatch::aim_overlay_inputs_for_client(player, state, {}, in) &&
					in.aim_pitch == 0x01234567,
			"a seated player's overlay reads its live Pitch, not the record's byte");
	ns::ClientEntityState npc;
	npc.handle = kSeatedInfantry;
	npc.cls = nw::EntityClass::Infantry;
	npc.carrier_handle = kHull;
	npc.mount_bone = 2;
	npc.pitch_bam = 0x00765432;
	npc.aim_pitch_byte = 0xF0;
	npc.rm_aim_word = static_cast<int32_t>(0xF0000000u);
	expect(opennova::inmatch::aim_overlay_inputs_for_client(npc, state, {}, in) &&
					in.aim_pitch == 0x00765432,
			"a live seated org1 row's overlay reads its live Pitch");
	npc.carrier_handle = 0xFFFF;
	npc.mount_bone = 0;
	npc.heading_bam = 0x12000000;
	npc.rm_body_heading = 0x10000000;
	npc.rm_leg_yaw[0] = 0x0E000000;
	npc.rm_leg_yaw[1] = 0x0F000000;
	expect(opennova::inmatch::aim_overlay_inputs_for_client(npc, state, {}, in) &&
					in.aim_pitch == 0x00765432 && in.aim_yaw == 0x12000000 &&
					in.body_yaw == 0x10000000 && in.leg_yaw_r == 0x0E000000 &&
					in.leg_yaw_l == 0x0F000000,
			"a free-standing org1 row presents its live Pitch, its look Yaw, its body heading and "
			"its leg words");
}

// D-NET-NEW-L5-41 (V-ROLL-AV F1): a free-standing org1 row rides its deck after its slope pass
// and ahead of its body chase, so the ride turns the deck's pitch delta by the deck's Yaw less
// the rider's as the last look left it (0 here): the whole delta lands in the Pitch, the aim
// pitch word and savedLivePitch, none in the Roll, though the body chase then steps the Yaw
// 0x4000000 toward its target. Riding after the chase would split the delta by that step.
// [orig: Entity_UpdateInfantryAI @0x4B9910 - the ride @0x4BA45D..0x4BA88E (the relative yaw
//  @0x4BA7A5..0x4BA7A8, Pitch @0x4BA88E, +0x2D0 @0x4BA852, +0x90 @0x4BA885, Roll @0x4BA88B),
//  the body chase @0x4BE8FD..0x4BE931]
void test_org1_deck_ride_precedes_the_body_chase() {
	BodySource src;
	auto view = free_infantry_view(src, 0x00, 0x00);
	static constexpr uint16_t kDeck = 0x1009; // static: MSVC needs no capture for it
	static constexpr int32_t kPitchDelta = 0x01000000;
	view->set_carrier_pose_provider(
			[](uint16_t handle, ns::ClientReplicaPipeline::CarrierPose &out) {
				if (handle != kDeck) return false;
				out = ns::ClientReplicaPipeline::CarrierPose{};
				out.pos[0] = out.saved_pos[0] = fixed_of(100.0);
				out.pos[1] = out.saved_pos[1] = fixed_of(200.0);
				out.pos[2] = out.saved_pos[2] = fixed_of(10.0);
				out.pitch = kPitchDelta; // the deck pitches 0x1000000 this tick
				out.bound_radius = 0x7FFFFFFF;
				return true;
			});
	ns::ClientEntityState &es = *view->state().find(kFreeInfantry);
	es.resolved_ground = kDeck;
	es.net_target_heading_bam = 0x10000000;
	view->tick_remote_motion(0xFFFF);
	expect(es.rm_body_heading == 0x04000000 && es.heading_bam == 0x04000000,
			"(the body chase stepped the body heading and the Yaw a quarter)");
	expect(es.pitch_bam == kPitchDelta && es.rm_aim_word == kPitchDelta &&
					es.rm_body_pitch == kPitchDelta && es.roll_bam == 0,
			"the ride turns the deck's pitch by the Yaw ahead of the body chase");
}

// A terrain rising toward +x: each texel (one unit, its column the position's whole units) is
// `per_texel` raw above the one before it over x 64..191, so the gradient under x = 100 is
// (raw(101) - raw(99), 0) = (2 * per_texel, 0).
struct SlopeTerrain {
	static constexpr int kDim = 512;
	std::vector<uint16_t> heightmap;
	std::vector<int> sector_grid;
	opennova::terrain::TerrainHeightField field;

	explicit SlopeTerrain(int per_texel) : heightmap(kDim * kDim), sector_grid(16 * 16, 1) {
		for (int z = 0; z < kDim; ++z)
			for (int x = 0; x < kDim; ++x)
				heightmap[static_cast<size_t>(z) * kDim + x] =
						static_cast<uint16_t>(std::clamp(x - 64, 0, 127) * per_texel);
		field.heightmap = heightmap.data();
		field.dim = kDim;
		field.layout.sector_grid = sector_grid.data();
	}
};

// Every clip walks 0x2000 forward a tick, the bodies' capsule as BodySource's.
struct WalkSource final : public opennova::world::IRootMotionSource {
	bool has_clip(int, int) const override { return true; }
	bool advance(int, int, int32_t &phase, opennova::world::RootMotionFrame &out) override {
		++phase;
		out = {};
		out.dx = 0x2000;
		out.capsule_bottom = 1 << 16;
		out.capsule_top = 2 << 16;
		return true;
	}
	int32_t clip_length_ticks(int, int, int) const override { return 62; }
	bool clip_loops(int, int, int) const override { return true; }
};

// The slide pair under a gradient of (1000, 0): x = -((419392 * 1000 + 0x8000) >> 16), y = 0.
constexpr int32_t kGradientSlideX = -6399;
constexpr uint32_t kFlagSlide = 0x10000u;

// D-NET-NEW-L5-40: a player row with no ground entity on terrain whose gradient magnitude
// reaches 768 takes the downhill push in place of its rotated root pair (the clip's 0x2000 step
// is gone, whatever the body heading), holds its vertical velocity at -167 and sets Flags
// 0x10000. A gentler slope (600), an indoors row or one with a ground entity keep the root and
// clear the bit.
// [orig: Entity_UpdateInfantryPlayerBody @0x4B40E0 - the rotation @0x4B41E4..0x4B4255, the Flags
//  test @0x4B79DC, the ground test @0x4B79E8, Terrain_GetHeightGradient called @0x4B7A13, the
//  test @0x4B7A47, the pair @0x4B7A4E..0x4B7A7C, the velocity @0x4B7A80..0x4B7A8D, the set
//  @0x4B7A93, the clears @0x4B7A9C / @0x4B7AA5]
void test_player_gradient_slide_replaces_the_world_pair() {
	SlopeTerrain steep(500);
	SlopeTerrain gentle(300);
	enum class Case { Steep, Gentle, Indoors, Grounded };
	for (const Case c : {Case::Steep, Case::Gentle, Case::Indoors, Case::Grounded}) {
		WalkSource src;
		auto view = make_view(src);
		view->set_remote_motion_terrain(c == Case::Gentle ? &gentle.field : &steep.field);
		view->state().upsert(0x0003).type_id = kPlayerType;
		view->apply(0x0A, frame({{0x0003, nw::EntityClass::Player, 0x00}}));
		ns::ClientEntityState &es = *view->state().find(0x0003);
		es.rm_adm_id = 0;
		es.rm_entity_flags = kFlagSlide;
		if (c == Case::Indoors) es.rm_entity_flags |= nw::world::kEntityFlagIndoors;
		if (c == Case::Grounded) es.resolved_ground = kHull;
		view->tick_remote_motion(0xFFFF);
		if (c == Case::Steep) {
			expect(es.x == fixed_of(100.0) + kGradientSlideX && es.y == fixed_of(200.0),
					"the slide replaces the rotated root pair");
			expect((es.rm_entity_flags & kFlagSlide) != 0 && es.rm_vel_z == -167,
					"the slide sets Flags 0x10000 and holds the vertical velocity at -167");
		} else {
			expect(es.x != fixed_of(100.0) + kGradientSlideX &&
							(es.x != fixed_of(100.0) || es.y != fixed_of(200.0)),
					"a row the slide skips walks its root");
			expect((es.rm_entity_flags & kFlagSlide) == 0 && es.rm_vel_z == 0,
					"a row the slide skips clears Flags 0x10000 and keeps its velocity");
		}
	}
}

// D-NET-NEW-L5-40: a free-standing org1 row with no ground entity takes the push in place of its
// clip's unrotated pair, which its body heading (+0x8C) then turns: at 0x40000000 the push
// (-6399, 0) leaves along the heading's rotation, not along -x as a player's does. A row with a
// ground entity rides it instead and leaves Flags 0x10000 as it was.
// [orig: Entity_UpdateInfantryAI @0x4B9910 - the ground test @0x4BA45D..0x4BA479, the Flags test
//  @0x4BA896, Terrain_GetHeightGradient called @0x4BA8CC, the pair's stores @0x4BA91D /
//  @0x4BA93B, the velocity @0x4BA935..0x4BA941, the set @0x4BA947, the rotation @0x4BF001]
void test_infantry_gradient_slide_replaces_the_clip_pair() {
	SlopeTerrain steep(500);
	for (const bool grounded : {false, true}) {
		WalkSource src;
		auto view = make_view(src);
		view->set_remote_motion_terrain(&steep.field);
		view->state().upsert(kFreeInfantry).type_id = kInfantryType;
		view->apply(0x0A, free_infantry_frame(0x00, 0x00));
		ns::ClientEntityState &es = *view->state().find(kFreeInfantry);
		es.rm_adm_id = 0;
		es.rm_body_heading = es.net_target_heading_bam = 0x40000000;
		es.rm_entity_flags = kFlagSlide;
		if (grounded) es.resolved_ground = kHull;
		view->tick_remote_motion(0xFFFF);
		const opennova::world::RootRotationQ22 rot =
				opennova::world::root_rotation_q22(0x40000000, false);
		const auto q22 = [](int32_t v, int32_t k) {
			return static_cast<int32_t>((static_cast<int64_t>(v) * k) >> 22);
		};
		if (!grounded) {
			expect(es.x == fixed_of(100.0) + q22(kGradientSlideX, rot.c) &&
							es.y == fixed_of(200.0) + q22(kGradientSlideX, rot.s),
					"the body heading turns the slide that replaced the clip's pair");
			expect((es.rm_entity_flags & kFlagSlide) != 0 && es.rm_vel_z == -167,
					"the slide sets Flags 0x10000 and holds the vertical velocity at -167");
		} else {
			expect(es.x == fixed_of(100.0) + q22(0x2000, rot.c) &&
							es.y == fixed_of(200.0) + q22(0x2000, rot.s),
					"a grounded row walks its clip's pair");
			expect((es.rm_entity_flags & kFlagSlide) != 0 && es.rm_vel_z == 0,
					"a grounded row leaves Flags 0x10000 and its velocity alone");
		}
	}
}

// The authority's view runs no chase, so its rows count nothing and never starve: with no
// record for 600 ticks a row still takes its legs.
void test_authority_rows_never_starve() {
	BodySource src;
	auto view = make_view(src, /*remote_motion=*/false);
	seed(*view, {{0x0003, nw::EntityClass::Player, 0x00}});
	ns::ClientEntityState &row = *view->state().find(0x0003);
	for (int i = 0; i < 600; ++i) tick(*view);
	row.lean_angle = 0x10000000;
	tick(*view);
	expect(row.net_interp_progress == 0 && !row.rm_starved && row.lean_angle != 0x10000000,
			"an authority's row counts nothing and keeps its legs");
}

} // namespace

int main() {
	test_hidden_rows_run_no_legs();
	test_channel_less_row_runs_nothing();
	test_starved_row_stops();
	test_seated_row_counts_and_starves();
	test_seated_rider_yaw_chases_its_record();
	test_seated_infantry_look_chases_its_aim_words();
	test_seated_infantry_look_holds_within_ninety_degrees();
	test_dead_seated_infantry_takes_the_free_standing_branch();
	test_seated_org1_rider_rides_its_parent();
	test_gun_channel_leaves_the_gunner_aim_heading();
	test_free_standing_infantry_body_and_look_split();
	test_free_standing_infantry_look_holds_within_ninety_degrees();
	test_deck_ride_turns_the_aim_words();
	test_org1_deck_ride_has_no_radius_drop();
	test_free_standing_infantry_legs();
	test_org1_init_and_reset_stamp_the_legs();
	test_org1_slope_pass();
	test_org1_dead_body_tumbles_in_the_air();
	test_org1_ride_skips_the_aim_adds_on_a_turret();
	test_overlay_pitch_is_the_live_pitch_word();
	test_org1_deck_ride_precedes_the_body_chase();
	test_player_gradient_slide_replaces_the_world_pair();
	test_infantry_gradient_slide_replaces_the_clip_pair();
	test_authority_rows_never_starve();
	if (failures != 0) {
		std::fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	std::printf("client_replica_pipeline_body_exits: all checks passed\n");
	return 0;
}
