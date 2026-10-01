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
	fu.mount_handle = 0xFFFF;

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
// on an already-dead row commits directly; the respawn edge commits directly.
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

	view.apply(0x0A, player_frame(as::kIdle2, 9, 0));
	expect(es->net_anim_current == as::kIdle2,
			"the respawn edge direct-commits [orig: @0x4c110f..0x4c1151]");
	expect(es->net_anim_pending == 0, "...with the pending cleared");
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

int main() {
	test_tapped_roll_locks_and_queues();
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
