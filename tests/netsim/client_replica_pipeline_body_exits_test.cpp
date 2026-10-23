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

#include <net/npwire/entity_class.h>
#include <net/npwire/ingame_decode.h>
#include <net/npwire/ingame_encode.h>
#include <runtime/world/entity.h>
#include <runtime/world/infantry.h>

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
};

// One 0x0A from the anchor (100, 200, 10): each row free-standing at the anchor, or seated on
// the hull (bone 1) at the local x +1, the hull free-standing at hull_x.
std::vector<uint8_t> frame(const std::vector<Row> &rows, double hull_x = 50.0) {
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

std::unique_ptr<ns::ClientReplicaPipeline> make_view(BodySource &src, bool remote_motion = true) {
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
// selection), the ramp, then the joiner's recoil leg.
void tick(ns::ClientReplicaPipeline &view) {
	view.tick_lean_decay(0xFFFF);
	view.tick_arms_dip(0xFFFF);
	view.tick_remote_motion(0xFFFF);
	view.tick_lean_ramp();
	view.tick_recoil();
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
	test_authority_rows_never_starve();
	if (failures != 0) {
		std::fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	std::printf("client_replica_pipeline_body_exits: all checks passed\n");
	return 0;
}
