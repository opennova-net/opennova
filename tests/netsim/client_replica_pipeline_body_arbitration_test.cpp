// The per-record receive-side body-state arbitration (D-NET-209): every folded
// player/infantry record applies its anim byte through the retail FSM pair AS
// IT DECODES [orig: player @0x4c1153, infantry @0x4c0600..0x4c0641] — the
// same-state no-op (pending survives), the queue classes (flags[current]&4;
// flags[current]&0x20 with a non-idle arrival), the direct commit with the
// player-only +0x377 phase seed, the wire-dead park, and the respawn-edge
// commit — plus the channel half [orig: AnimMap_UpdateEntity @0x40b5f0]: the
// gait->stance transition-clip insert (@0x40b662..0x40b737) and the clip-end
// deferred promotion (loop wrap / one-shot end via the armed end-notify,
// @0x40b7db/@0x40b7ad -> @0x40b1ae/@0x40b18f -> @0x40b795/@0x40b7c3).

#include <runtime/replication/client_replica_pipeline.h>

#include <net/npwire/ingame_decode.h>
#include <net/npwire/ingame_encode.h>
#include <runtime/world/entity.h>
#include <runtime/world/infantry.h>

#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

using namespace opennova;
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
constexpr uint16_t kPlayerHandle = 0x0067;

EntityClass classify(uint16_t type_id) {
	return type_id == kPlayerType ? EntityClass::Player : EntityClass::Unknown;
}

std::vector<uint8_t> player_frame(uint8_t anim_state, uint8_t ratio,
		uint8_t state_flags = 0) {
	FrameUpdate fu;
	fu.anchor_x = 100 << 16;
	fu.anchor_y = 200 << 16;
	fu.anchor_z = 10 << 16;
	fu.flags2 = 0;
	fu.carried_handle = 0xFFFF;

	FrameUpdateRecord rec;
	rec.handle = kPlayerHandle;
	rec.type_id = kPlayerType;
	rec.cls = EntityClass::Player;
	rec.player.carrier_handle = 0xFFFF;
	rec.player.pos_x_compressed = network_compress_fixedpoint(1 << 16);
	rec.player.pos_y_compressed = network_compress_fixedpoint(2 << 16);
	rec.player.pos_z_compressed = network_compress_fixedpoint(0);
	rec.player.yaw_byte = 0x40;
	rec.player.anim_state_id = anim_state;
	rec.player.anim_channel_ratio = ratio;
	rec.player.state_flags = state_flags;
	fu.records.push_back(rec);
	return encode_frame_update(fu);
}

// Looping gait + one-shot transition clips over one adm, deterministic lengths.
struct BodySource final : public opennova::world::IRootMotionSource {
	bool has_clip(int, int) const override { return true; }
	bool advance(int, int, int32_t &phase,
			opennova::world::RootMotionFrame &out) override {
		++phase;
		out = {};
		out.capsule_bottom = 1 << 16;
		out.capsule_top = 2 << 16;
		return true;
	}
	int32_t clip_length_ticks(int, int state, int /*variant*/) const override {
		if (state == as::kRollLeft) return 4; // a short locked one-shot
		if (state == as::kRun2Crouch || state == as::kRun2Prone) return 6;
		return 62;
	}
	bool clip_loops(int, int state, int /*variant*/) const override {
		// Gaits and idles loop; rolls and the transition inserts are one-shots.
		return state != as::kRollLeft && state != as::kRun2Crouch &&
		       state != as::kRun2Prone;
	}
};

// emote_1..emote_10 are 20-tick one-shots, emote_4 unauthored; the rest
// 62-tick loops.
struct EmoteSource final : public opennova::world::IRootMotionSource {
	bool has_clip(int, int state) const override { return state != 118; }
	bool advance(int, int, int32_t &phase, opennova::world::RootMotionFrame &out) override {
		++phase;
		out = {};
		out.capsule_bottom = 1 << 16;
		out.capsule_top = 2 << 16;
		return true;
	}
	int32_t clip_length_ticks(int, int state, int) const override {
		return state >= 115 && state <= 124 ? 20 : 62;
	}
	bool clip_loops(int, int state, int) const override { return state < 115 || state > 124; }
};

void arm_row(ns::ClientReplicaPipeline &view) {
	view.state().find(kPlayerHandle)->rm_adm_id = 0;
}

// The spawn stream's row: a compact record never creates one.
void seed_row(ns::ClientReplicaPipeline &view) {
	view.state().upsert(kPlayerHandle).type_id = kPlayerType;
}

// The tapped prone roll, now arbitrated per record: a fold of [41, 48] locks
// the roll (48 queues behind flags[41]&4) instead of coalescing to 48.
void test_tapped_roll_locks_and_queues() {
	ns::ClientReplicaPipeline view;
	view.set_item_class_resolver(&classify);
	seed_row(view);
	view.apply(0x0A, player_frame(as::kIdleProne, 0));
	const ns::ClientEntityState *es = view.state().find(kPlayerHandle);
	expect(es != nullptr, "player row decoded");
	if (es == nullptr) return;
	expect(es->net_anim_current == as::kIdleProne, "first record seeds current");
	expect(es->net_anim_pending == 0, "...with no pending");
	expect(es->net_stance_bits == 1, "prone receive maps animation flag 0x200 to MoveOrder 0x100");

	view.apply(0x0A, player_frame(as::kRollLeft, 6));
	view.apply(0x0A, player_frame(as::kIdleProne, 60));
	expect(es->net_anim_current == as::kRollLeft,
			"the one-sample roll commits and LOCKS (flags[41] bit2)");
	expect(es->net_anim_pending == as::kIdleProne,
			"the follow-up state queues behind it [orig: @0x4c1174]");
	expect(es->net_anim_ratio == 6,
			"the +0x377 seed is the DIRECT commit's ratio, not the queued one");

	// Same-as-current arrivals are pure no-ops: the pending SURVIVES
	// [orig: @0x4c115f jz done — no pending write].
	view.apply(0x0A, player_frame(as::kRollLeft, 20));
	expect(es->net_anim_pending == as::kIdleProne,
			"a same-state record never cancels an armed pending");

	// A replaced pending swaps the deferred target only.
	view.apply(0x0A, player_frame(as::kIdle, 0));
	expect(es->net_anim_current == as::kRollLeft, "the lock still stands");
	expect(es->net_anim_pending == as::kIdle, "the pending retargets");
	expect(es->net_stance_bits == 1, "queued standing byte retains prone MoveOrder");
}

// The channel promotes the pending at the locked one-shot's end and the
// retarget lands the tick after (retail's read-then-write order).
void test_pending_promotes_at_clip_end() {
	ns::ClientReplicaPipeline view;
	view.set_item_class_resolver(&classify);
	view.set_remote_motion_mode(true);
	BodySource src;
	view.set_root_motion_source(&src);
	seed_row(view);
	view.apply(0x0A, player_frame(as::kIdleProne, 0));
	arm_row(view);
	const ns::ClientEntityState *es = view.state().find(kPlayerHandle);
	view.tick_remote_motion(0xFFFF); // channel arms on idle_prone
	expect(es->rm_state == as::kIdleProne, "channel armed on the current");

	view.apply(0x0A, player_frame(as::kRollLeft, 0));
	view.apply(0x0A, player_frame(as::kIdleProne, 0));
	view.tick_remote_motion(0xFFFF); // retarget to the locked roll
	expect(es->rm_state == as::kRollLeft, "the roll plays");
	// The 4-tick one-shot: the boundary arms at the roll's end.
	for (int i = 0; i < 6; ++i) view.tick_remote_motion(0xFFFF);
	expect(es->net_anim_current == as::kIdleProne,
			"the queued state promoted at the one-shot's end");
	expect(es->net_anim_pending == 0, "...consuming the pending");
	view.tick_remote_motion(0xFFFF);
	expect(es->rm_state == as::kIdleProne,
			"the promoted retarget lands on the following tick");
}

// The gait->stance transition insert: run committing to the crouch walk plays
// run2crouch first and defers the crouch walk [orig: @0x40b662..0x40b737].
void test_gait_transition_insert() {
	ns::ClientReplicaPipeline view;
	view.set_item_class_resolver(&classify);
	view.set_remote_motion_mode(true);
	BodySource src;
	view.set_root_motion_source(&src);
	seed_row(view);
	view.apply(0x0A, player_frame(as::kRunForward, 0));
	arm_row(view);
	const ns::ClientEntityState *es = view.state().find(kPlayerHandle);
	view.tick_remote_motion(0xFFFF);
	expect(es->rm_state == as::kRunForward, "running");

	view.apply(0x0A, player_frame(as::kWalkCrouchForward, 0));
	expect(es->net_anim_current == as::kWalkCrouchForward,
			"the record itself commits directly");
	view.tick_remote_motion(0xFFFF);
	expect(es->rm_state == as::kRun2Crouch,
			"the tick inserts the run2crouch transition clip");
	expect(es->net_anim_pending == as::kWalkCrouchForward,
			"...and defers the real target");
	// The 6-tick transition one-shot runs out; the crouch walk promotes.
	for (int i = 0; i < 8; ++i) view.tick_remote_motion(0xFFFF);
	expect(es->rm_state == as::kWalkCrouchForward,
			"the deferred crouch walk takes the channel at the clip end");
}

// Wire-dead records on a live row PARK the byte (no FSM write); a dead record
// on an already-dead row commits directly; a player's respawn edge commits
// nothing (the next record does).
void test_dead_park_and_respawn_edges() {
	ns::ClientReplicaPipeline view;
	view.set_item_class_resolver(&classify);
	seed_row(view);
	view.apply(0x0A, player_frame(as::kIdle, 0));
	const ns::ClientEntityState *es = view.state().find(kPlayerHandle);

	const uint8_t dead = static_cast<uint8_t>(opennova::world::kEntityFlagDead);
	view.apply(0x0A, player_frame(180, 0, dead));
	expect(es->net_anim_current == as::kIdle,
			"a dead record on a live row parks the byte [orig: @0x4c10f1]");
	expect(es->anim_state_id == 180, "...which the raw wire fallback carries");

	view.apply(0x0A, player_frame(181, 0, dead));
	expect(es->net_anim_current == 181,
			"dead-on-dead commits directly [orig: @0x4c1109 -> @0x4c1153]");

	// A player's respawn leg commits nothing: it snaps the pose and resets
	// the body (pending = 0) while the dead bit is still set, then jumps past
	// the arbitration [orig: @0x4c110f..0x4c114a -> @0x4c11b4;
	// Entity_ResetToSpawnState @0x4b96ed..0x4b96f7]. The next record
	// arbitrates the death state away.
	view.apply(0x0A, player_frame(as::kIdle2, 9, 0));
	expect(es->net_anim_current == 181,
			"the respawn record keeps the death state current");
	expect(es->net_anim_pending == 0, "...with the pending cleared");
	view.apply(0x0A, player_frame(as::kIdle2, 9, 0));
	expect(es->net_anim_current == as::kIdle2,
			"the next record direct-commits over the death state");
}

} // namespace

// A decoded player row runs its own secondary channel: armed on the hold
// ladder (the rifle default mirrors the primary), an S2C 0x2D stamp plays
// emote_N, the next 16-tick selection defers the hold to the emote's end, and
// the hold returns after it. An unauthored emote does not stamp.
// [orig: NapiNPClientMsg_HandleEmote @0x427efb..0x427f18;
//  Entity_UpdateInfantryPlayerBody @0x4b5d71 / @0x4b5e72..0x4b5ea3;
//  AnimMap_UpdateDualChannels @0x40b8c0]
void test_row_weapon_channel_plays_an_emote_then_returns() {
	ns::ClientReplicaPipeline view;
	view.set_item_class_resolver(&classify);
	view.set_remote_motion_mode(true);
	EmoteSource src;
	view.set_root_motion_source(&src);
	seed_row(view);
	view.apply(0x0A, player_frame(as::kIdle, 0));
	arm_row(view);
	const ns::ClientEntityState *es = view.state().find(kPlayerHandle);
	view.tick_remote_motion(0xFFFF);
	expect(es->wpn_playing == as::kIdle, "the channel arms on the ladder's mirror of the primary");
	expect(!view.stamp_row_emote(kPlayerHandle, 4), "an unauthored emote does not stamp");
	expect(view.stamp_row_emote(kPlayerHandle, 3) && es->wpn_state == 117,
			"emote 3 targets emote_3");
	view.tick_remote_motion(0xFFFF);
	expect(es->wpn_playing == 117 && es->wpn_blend_weight < 1.0f && es->wpn_prev == as::kIdle,
			"the channel re-inits onto the emote, blending from the hold");
	int ticks = 2;
	while (es->wpn_deferred == 0 && ticks < 40) {
		view.tick_remote_motion(0xFFFF);
		++ticks;
	}
	expect(ticks == 16 && es->wpn_deferred == as::kIdle && es->wpn_playing == 117,
			"the 16-tick selection defers the hold behind the emote (flag 0x20)");
	while (es->wpn_playing == 117 && ticks < 60) {
		view.tick_remote_motion(0xFFFF);
		++ticks;
	}
	expect(es->wpn_playing == as::kIdle && es->wpn_deferred == 0 && ticks > 20,
			"the hold returns once the emote clip ends");
	// The row's radio-request latch ages on the 64-tick window, a spent count
	// clearing it [orig: Entity_UpdateInfantryPlayerBody @0x4b467a..0x4b469d].
	ns::ClientEntityState *row = view.state().find(kPlayerHandle);
	row->radio_request = 1;
	row->radio_request_seconds = 1;
	for (int i = 0; i < 64; ++i) view.tick_remote_motion(0xFFFF);
	expect(row->radio_request == 1 && row->radio_request_seconds == 0,
			"one window takes the last second");
	for (int i = 0; i < 64; ++i) view.tick_remote_motion(0xFFFF);
	expect(row->radio_request == 0, "the next window clears the latch");
}

// D-NET-330: a remote player's eye (CameraOffset +0x6C..+0x74) is its body update's
// non-local arm over the clip's capsule, every tick: the extent capped at 0xD000 on the
// lean's arc, turned by the twist, Roll, the body pitch and the body heading
// (world::player_body_capsule_eye), the vertical floored at 0x2000; it reads the lean
// decayed but not yet ramped; the local player's own row is its local arm's and keeps
// none.
// [orig: Entity_UpdateInfantryPlayerBody @0x4B40E0 - the decay @0x4B5C97, the cap
//  @0x4B698C..0x4B6997, the turns @0x4B699F..0x4B6B96, the floor @0x4B6B98..0x4B6BA0, the
//  stores @0x4B6BA5..0x4B6BAB, the ramp @0x4B7DBF / @0x4B7DD6]
struct CapsuleSource final : public opennova::world::IRootMotionSource {
	int32_t extent = 0;
	bool has_clip(int, int) const override { return true; }
	bool advance(int, int, int32_t &phase, opennova::world::RootMotionFrame &out) override {
		++phase;
		out = {};
		out.capsule_bottom = 0x4000;
		out.capsule_top = 0x4000 + extent;
		return true;
	}
	int32_t clip_length_ticks(int, int, int) const override { return 62; }
	bool clip_loops(int, int, int) const override { return true; }
};

void test_remote_eye_restamps_from_the_capsule() {
	ns::ClientReplicaPipeline view;
	view.set_item_class_resolver(&classify);
	view.set_remote_motion_mode(true);
	CapsuleSource src;
	src.extent = 0x18000; // a 1.5 u standing capsule: the cap
	view.set_root_motion_source(&src);
	seed_row(view);
	view.apply(0x0A, player_frame(as::kIdle, 0));
	arm_row(view);
	ns::ClientEntityState *es = view.state().find(kPlayerHandle);
	if (es == nullptr) {
		expect(false, "eye: the player row decoded");
		return;
	}
	view.tick_remote_motion(0xFFFF);
	expect(es->eye_offset[2] == 0xD000 && es->eye_offset[0] == 0 && es->eye_offset[1] == 0,
			"eye: the extent caps at 0xD000; a level, unleaned row has no lateral words");
	src.extent = 0x8000;
	view.tick_remote_motion(0xFFFF);
	expect(es->eye_offset[2] == 0x8000, "eye: a crouched capsule's extent");
	src.extent = 0x1000;
	view.tick_remote_motion(0xFFFF);
	expect(es->eye_offset[2] == 0x2000, "eye: the vertical floors at 0x2000");
	// A 0x20000000 lean takes the extent times its Q22 cosine through retail's
	// radians-per-BAM double (dbl_7C3608, io::kRadiansPerBamX87, slightly over 2 pi /
	// 2^32): cos(0.785422) * 2^22 = 2965749.71 -> 2965749, and 0x8000 * 2965749 >> 22 =
	// 23169 (23169.91), each step clear of a rounding edge at 53 or 64 bits.
	src.extent = 0x8000;
	es->lean_angle = 0x20000000;
	es->move_input = 0;
	view.tick_remote_motion(0xFFFF);
	expect(es->eye_offset[2] == 23169, "eye: the extent times the lean's cosine");
	// The row's own words drive the turns: the eye is the shared leg over the row's
	// lean, chased heading, body heading, Roll and body pitch (a level standing row's
	// slope conform leaves both at 0).
	{
		int32_t want[3] = {};
		opennova::world::player_body_capsule_eye(0x8000, es->lean_angle, es->heading_bam,
				es->rm_body_heading, es->roll_bam, es->rm_body_pitch, want);
		expect(es->eye_offset[0] == want[0] && es->eye_offset[1] == want[1] &&
						es->eye_offset[1] != 0,
				"eye: the lean's sine turns into the lateral words");
	}
	// The order (V-L5-s5 0b): the body pass's eye reads the decayed lean, the held key's
	// ramp lands after it. A held left lean from 0x20000000: the decay takes
	// (0x20000000 + 8) >> 4 = 0x2000000, the eye reads 0x1E000000, then the ramp adds
	// -0x3000000 for 0x1B000000.
	es->lean_angle = 0x20000000;
	es->move_input = opennova::world::Entity::kMoveOrderLeanLeft;
	view.tick_lean_decay();
	view.tick_remote_motion(0xFFFF);
	view.tick_lean_ramp();
	{
		int32_t want[3] = {};
		opennova::world::player_body_capsule_eye(0x8000, 0x1E000000, es->heading_bam,
				es->rm_body_heading, es->roll_bam, es->rm_body_pitch, want);
		expect(es->eye_offset[2] == want[2] && es->eye_offset[1] == want[1],
				"eye: the pass reads the decayed, unramped lean");
		expect(es->lean_angle == 0x1B000000, "eye: the held key's ramp follows the eye");
	}
	es->move_input = 0;
	// The self row is the local arm's: the replica restamps none.
	es->eye_offset[2] = 0;
	view.tick_remote_motion(kPlayerHandle);
	expect(es->eye_offset[2] == 0, "eye: the local player's own row keeps its local arm's");
}

// D-NET-196: a remote player row runs the player body's slope conform. A prone body
// (anim flag bit 2) probes four ground columns on every second tick, ahead, behind,
// left and right, and chases bodyPitch and Roll a quarter of the way to the true
// slope angles, holding between; the eye reads the words the previous tick left. A
// standing body (no flag bit 2, alive) decays both a sixteenth toward level every
// tick. The slope angles are retail's arithmetic over the probe drops: a 0x3000 drop
// over the 45056 fore-aft span, atan2 * 2^32 / 2 pi = 182000655.35 (truncated to
// 182000655), and a -0x800 drop over the 11264 lateral span, -122941607.15 (to
// -122941607), each clear of its truncation edge at 53 or 64 bits, both under the
// 60-degree slide threshold.
// [orig: Entity_UpdateInfantryPlayerBody @0x4B40E0 - the selector @0x4B6D95..0x4B6DBB,
//  the decay @0x4B6DBD..0x4B6DDC, the tick & 1 gate @0x4B6DE4, the probes
//  @0x4B6E41 / @0x4B6E5D / @0x4B6E98 / @0x4B6EB3, the slopes @0x4B6E68 / @0x4B6EC8, the
//  chase @0x4B6FC1..0x4B6FF4; the eye @0x4B699F..0x4B6BAB ahead of it]
void test_remote_body_conform_chases_the_slope() {
	ns::ClientReplicaPipeline view;
	view.set_item_class_resolver(&classify);
	view.set_remote_motion_mode(true);
	CapsuleSource src;
	src.extent = 0x8000;
	view.set_root_motion_source(&src);
	int calls = 0;
	view.set_replica_ground_probe(
			[&](const ns::ClientReplicaPipeline::ReplicaGroundColumn &q) -> int32_t {
				const int which = calls++ % 4;
				(void)q;
				// ahead, behind, left, right: the fore-aft drop 0x3000, the lateral -0x800.
				const int32_t base = 10 << 16;
				return which == 0 ? base + 0x1800 : which == 1 ? base - 0x1800
				     : which == 2 ? base - 0x400 : base + 0x400;
			});
	seed_row(view);
	view.apply(0x0A, player_frame(as::kIdleProne, 0));
	arm_row(view);
	ns::ClientEntityState *es = view.state().find(kPlayerHandle);
	if (es == nullptr) {
		expect(false, "conform: the prone row decoded");
		return;
	}
	constexpr int32_t kPitchSlope = 182000655;
	constexpr int32_t kRollSlope = -122941607;
	view.tick_remote_motion(0xFFFF); // tick 1: odd, the conform holds
	expect(calls == 0 && es->rm_body_pitch == 0 && es->roll_bam == 0,
			"conform: an odd tick probes nothing and holds");
	view.tick_remote_motion(0xFFFF); // tick 2: the probes and the quarter step
	const int32_t pitch1 = (kPitchSlope + 2) >> 2;
	const int32_t roll1 = (kRollSlope + 2) >> 2;
	expect(calls == 4 && es->rm_body_pitch == pitch1 && es->roll_bam == roll1,
			"conform: an even tick probes four columns and chases a quarter step");
	view.tick_remote_motion(0xFFFF); // tick 3: the eye reads tick 2's words, then the hold
	{
		int32_t want[3] = {};
		opennova::world::player_body_capsule_eye(0x8000, es->lean_angle, es->heading_bam,
				es->rm_body_heading, roll1, pitch1, want);
		expect(es->eye_offset[0] == want[0] && es->eye_offset[1] == want[1] &&
						es->eye_offset[2] == want[2] && es->rm_body_pitch == pitch1,
				"conform: the eye turns by the body pitch and Roll the conform left");
	}
	view.tick_remote_motion(0xFFFF); // tick 4: the second quarter step
	expect(calls == 8 && es->rm_body_pitch == pitch1 + ((kPitchSlope - pitch1 + 2) >> 2) &&
					es->roll_bam == roll1 + ((kRollSlope - roll1 + 2) >> 2),
			"conform: the next even tick closes another quarter of the gap");

	// A standing, live body does not conform: both words decay a sixteenth a tick.
	ns::ClientReplicaPipeline standing;
	standing.set_item_class_resolver(&classify);
	standing.set_remote_motion_mode(true);
	standing.set_root_motion_source(&src);
	int standing_calls = 0;
	standing.set_replica_ground_probe(
			[&](const ns::ClientReplicaPipeline::ReplicaGroundColumn &) -> int32_t {
				++standing_calls;
				return 0;
			});
	seed_row(standing);
	standing.apply(0x0A, player_frame(as::kIdle, 0));
	arm_row(standing);
	ns::ClientEntityState *row = standing.state().find(kPlayerHandle);
	if (row == nullptr) {
		expect(false, "conform: the standing row decoded");
		return;
	}
	row->rm_body_pitch = 0x1000000;
	row->roll_bam = -0x1000000;
	standing.tick_remote_motion(0xFFFF);
	standing.tick_remote_motion(0xFFFF);
	const int32_t p1 = 0x1000000 - ((0x1000000 + 8) >> 4);
	const int32_t r1 = -0x1000000 - ((-0x1000000 + 8) >> 4);
	expect(standing_calls == 0 && row->rm_body_pitch == p1 - ((p1 + 8) >> 4) &&
					row->roll_bam == r1 - ((r1 + 8) >> 4),
			"conform: a standing body decays both words every tick and probes nothing");
}

int main() {
	test_remote_body_conform_chases_the_slope();
	test_tapped_roll_locks_and_queues();
	test_remote_eye_restamps_from_the_capsule();
	test_pending_promotes_at_clip_end();
	test_gait_transition_insert();
	test_dead_park_and_respawn_edges();
	test_row_weapon_channel_plays_an_emote_then_returns();
	if (failures) {
		std::fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	std::printf("client_replica_pipeline_body_arbitration: all checks passed\n");
	return 0;
}
