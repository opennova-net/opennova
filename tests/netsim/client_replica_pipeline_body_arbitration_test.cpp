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
#include <net/npwire/ingame_message_id.h>
#include <net/protocol/protocol.h>
#include <runtime/world/entity.h>
#include <runtime/world/infantry.h>

#include <cstdint>
#include <cstdio>
#include <vector>
#include "../common/test_expect.h"

namespace {

using namespace opennova;
namespace ns = opennova::replication;
namespace as = opennova::world::anim_state;

int &failures = test_expect::failures;

using test_expect::expect;

constexpr uint16_t kPlayerType = 0x14B9;
constexpr uint16_t kPlayerHandle = 0x0067;

EntityClass classify(uint16_t type_id) {
	return type_id == kPlayerType ? EntityClass::Player : EntityClass::Unknown;
}

std::vector<uint8_t> player_frame(uint8_t anim_state, uint8_t ratio,
		uint8_t state_flags = 0, uint16_t carrier = 0xFFFF, uint8_t bone = 0) {
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
	rec.player.carrier_handle = carrier;
	rec.player.vehicle_bone = bone;
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

// A looping clip whose every sampled frame carries the left-foot event word.
struct StepSource final : public opennova::world::IRootMotionSource {
	bool has_clip(int, int) const override { return true; }
	bool advance(int, int, int32_t &phase, opennova::world::RootMotionFrame &out) override {
		++phase;
		out = {};
		out.capsule_bottom = 1 << 16;
		out.capsule_top = 2 << 16;
		out.events = 0x1u; // kAnimEventFootLeft
		return true;
	}
	int32_t clip_length_ticks(int, int, int) const override { return 62; }
	bool clip_loops(int, int, int) const override { return true; }
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
	view.set_protocol(title_protocol("jo"));
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
	view.set_protocol(title_protocol("jo"));
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
	view.set_protocol(title_protocol("jo"));
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
	view.set_protocol(title_protocol("jo"));
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

// Every dead player record queues its anim byte for the effect pass, behind a 0x13 the same
// receive carried ahead of it, so the client's own body takes the record's byte after that
// 0x13's class callback staged the bullet default; an alive record queues nothing.
// [orig: NapiNPClientMsg_EntityDeath @0x42EB50 (the callback @0x42EBF5) ahead of
//  NetPacket_SerializePlayerState @0x4C09C0's dead arms @0x4C100B..0x4C1027 /
//  @0x4C10F1..0x4C10FB in one receive]
void test_dead_records_queue_behind_the_0x13() {
	ns::ClientReplicaPipeline view;
	view.set_protocol(title_protocol("jo"));
	view.set_item_class_resolver(&classify);
	seed_row(view);
	view.apply(0x0A, player_frame(as::kIdle2, 0));
	bool alive_queued = false;
	for (const ns::ClientEffectCommand &c : view.drain_effect_commands())
		alive_queued = alive_queued || std::get_if<ns::PlayerDeadRecord>(&c) != nullptr;
	expect(!alive_queued, "an alive record queues no dead-record byte");

	const uint8_t dead = static_cast<uint8_t>(opennova::world::kEntityFlagDead);
	const std::vector<uint8_t> body13 = {static_cast<uint8_t>(kPlayerHandle & 0xFFu),
			static_cast<uint8_t>(kPlayerHandle >> 8), 0x00, 0x00}; // deathAnimStateId 0
	view.apply(0x13, body13);
	view.apply(0x0A, player_frame(as::kDeathGrenadeBase, 0, dead));
	std::vector<ns::ClientEffectCommand> fx;
	for (ns::ClientEffectCommand &c : view.drain_effect_commands())
		if (std::get_if<ns::EntityDeathEvent>(&c) != nullptr ||
				std::get_if<ns::PlayerDeadRecord>(&c) != nullptr)
			fx.push_back(std::move(c));
	const auto *death = fx.size() == 2 ? std::get_if<ns::EntityDeathEvent>(&fx[0]) : nullptr;
	const auto *record = fx.size() == 2 ? std::get_if<ns::PlayerDeadRecord>(&fx[1]) : nullptr;
	expect(death != nullptr && death->entity_handle == kPlayerHandle &&
					death->death_anim_state_id == 0,
			"the 0x13 queues first, its word 0");
	expect(record != nullptr && record->entity_handle == kPlayerHandle &&
					record->anim_state_id == as::kDeathGrenadeBase,
			"the dead record queues its byte behind it");

	// A dead record on the dead row queues its byte again (the dead leg's +0x2BC store).
	view.apply(0x0A, player_frame(as::kDeathGrenadeBase, 0, dead));
	int queued = 0;
	for (const ns::ClientEffectCommand &c : view.drain_effect_commands())
		if (const auto *next = std::get_if<ns::PlayerDeadRecord>(&c))
			queued += next->anim_state_id == as::kDeathGrenadeBase ? 1 : 100;
	expect(queued == 1, "each dead record queues its byte");
}

// V-ROLL-BL N2 on D-NET-1310: a carrier-local record whose carrier slot holds no def (no
// row, or a typeless one the team-assign leg made ahead of its spawn) drops whole, its dead
// arms included: nothing queues, the row keeps its byte, the carrier takes a C2S 0x0F. A typed
// carrier lets the same record land.
// [orig: NetPacket_SerializePlayerState @0x4C09C0 - `cmp dword ptr [ebp+20h], 0` @0x4C107B, the
//  0x0F @0x4C10AE, the return @0x4C10C7 ahead of the dead test @0x4C0FE1]
void test_defless_carrier_drops_the_dead_record() {
	constexpr uint16_t kCarrier = 0x1010;
	constexpr uint16_t kCarrierType = 0x0400;
	const uint8_t dead = static_cast<uint8_t>(opennova::world::kEntityFlagDead);
	struct Form {
		bool row;
		uint16_t type;
		bool lands;
		const char *what;
	};
	const Form forms[] = {
		{false, 0, false, "a carrier with no row drops the dead record whole"},
		{true, 0, false, "a typeless carrier row drops the dead record whole"},
		{true, kCarrierType, true, "a typed carrier lets the dead record land"},
	};
	for (const Form &f : forms) {
		for (const uint8_t bone : {uint8_t{0}, uint8_t{3}}) {
			ns::ClientReplicaPipeline view;
			view.set_protocol(title_protocol("jo"));
			view.set_item_class_resolver(&classify);
			seed_row(view);
			if (f.row) view.state().upsert(kCarrier).type_id = f.type;
			view.apply(0x0A, player_frame(as::kDeathGrenadeBase, 0, dead, kCarrier, bone));
			int queued = 0;
			for (const ns::ClientEffectCommand &c : view.drain_effect_commands())
				if (const auto *record = std::get_if<ns::PlayerDeadRecord>(&c))
					queued += record->entity_handle == kPlayerHandle ? 1 : 100;
			const std::vector<uint16_t> repairs = view.drain_carrier_repair_requests();
			const ns::ClientEntityState *row = view.state().find(kPlayerHandle);
			expect(queued == (f.lands ? 1 : 0), f.what);
			expect(f.lands ? repairs.empty()
			               : repairs.size() == 1 && repairs[0] == kCarrier,
					f.lands ? "a typed carrier requests nothing"
					        : "the def-less carrier takes one C2S 0x0F");
			expect(row != nullptr &&
							(row->anim_state_id == as::kDeathGrenadeBase) == f.lands &&
							row->net_health_zero == f.lands,
					f.lands ? "the landed record stores its byte and Health 0"
					        : "the dropped record leaves the row as it stood");
		}
	}
}

// V-ROLL-BL N1 on D-NET-1310: a dead record's queued share belongs to its slot's lifetime.
// Retail's dead arms write the slot as the record decodes; a 0x18 for that slot later in the
// same receive destroys and re-inits it (Health in the memset, +0x2BC / +0x2B8 in the class
// init), so the share the effect pass would land on the rebuilt body drops with the slot's
// 0x13s. Another slot's 0x18 leaves it.
// [orig: NetPacket_SerializePlayerState @0x4C09C0 - the dead arms @0x4C100B..0x4C1027 /
//  @0x4C10F1..0x4C10FB; NapiNPClientMsg_FullEntitySpawn @0x433780 - Entity_Destroy called
//  @0x433815, the memset @0x433821, the class init called @0x433E82; PlayerClass_InitEntity
//  @0x4B1060 - +0x2BC @0x4B1161, +0x2B8 @0x4B1176]
void test_slot_restart_drops_a_queued_dead_record() {
	const uint8_t dead = static_cast<uint8_t>(opennova::world::kEntityFlagDead);
	for (const bool same_slot : {true, false}) {
		ns::ClientReplicaPipeline view;
		view.set_protocol(title_protocol("jo"));
		view.set_item_class_resolver(&classify);
		seed_row(view);
		view.apply(0x0A, player_frame(as::kDeathGrenadeBase, 0, dead));
		FullEntitySpawnRecord rec;
		rec.slot_id = same_slot ? kPlayerHandle : uint16_t{0x0003};
		rec.item_type_id = kPlayerType;
		rec.item_type = 3;
		rec.minimap_flags = static_cast<uint16_t>(opennova::world::kEntityFlagPlayer);
		view.apply(s2c::FULL_ENTITY_SPAWN, encode_full_entity_spawn(rec));
		int queued = 0;
		for (const ns::ClientEffectCommand &c : view.drain_effect_commands())
			if (std::get_if<ns::PlayerDeadRecord>(&c) != nullptr) ++queued;
		expect(queued == (same_slot ? 0 : 1),
				same_slot ? "the slot's 0x18 later in the receive drops its queued dead record"
				          : "another slot's 0x18 leaves the dead record queued");
	}
}

// L5-s7 FOUND 4: every player record, dead or alive, lands its flags byte on the
// row's Flags mirror under 0xFD: bits 0 and 2..7 become the record's (a cleared
// bit clears), the dead bit and every upper bit stay the row's.
// [orig: NetPacket_SerializePlayerState @0x4C09C0 - the byte in bl @0x4C0FDD,
//  `xor ebx, [edi+24h]` @0x4C12F4, `and ebx, 0FDh` @0x4C1307, `xor [edi+24h], ebx`
//  @0x4C1311]
void test_record_flags_land_on_the_mirror() {
	constexpr uint32_t kInAir = 0x2000u;
	constexpr uint32_t kDead = opennova::world::kEntityFlagDead;
	constexpr uint32_t kMounted = opennova::world::kEntityFlagMounted;
	{
		ns::ClientReplicaPipeline view;
		view.set_protocol(title_protocol("jo"));
		view.set_item_class_resolver(&classify);
		seed_row(view);
		ns::ClientEntityState &row = *view.state().find(kPlayerHandle);
		row.rm_entity_flags = kInAir | kMounted;
		// A dead record with the chute and the binoculars bits.
		view.apply(0x0A, player_frame(as::kIdle, 0, 0x2Au));
		expect(row.rm_entity_flags == (kInAir | 0x28u),
				"flags merge: the record's chute and binoculars land, its dead bit does not, "
				"the mounted bit it lacks clears, the airborne bit stays");
		view.apply(0x0A, player_frame(as::kIdle, 0, 0x10u));
		expect(row.rm_entity_flags == (kInAir | 0x10u),
				"flags merge: the next record's byte replaces the low bits again");
	}
	{
		ns::ClientReplicaPipeline view;
		view.set_protocol(title_protocol("jo"));
		view.set_item_class_resolver(&classify);
		seed_row(view);
		ns::ClientEntityState &row = *view.state().find(kPlayerHandle);
		row.rm_entity_flags = kDead;
		view.apply(0x0A, player_frame(as::kIdle, 0, 0x20u));
		expect(row.rm_entity_flags == (kDead | 0x20u),
				"flags merge: a live record leaves the row's own dead bit");
	}
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
	view.set_protocol(title_protocol("jo"));
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
	view.set_protocol(title_protocol("jo"));
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
// left and right, and chases savedLivePitch and Roll a quarter of the way to the true
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
	view.set_protocol(title_protocol("jo"));
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
	standing.set_protocol(title_protocol("jo"));
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

// V-L5-s6 (D-NET-330): a remote player's on-foot lean ramp takes retail's gates: a
// ladder contact or a chute (Flags 0x100020, the chute bit the record's, which the
// apply lands on the row's Flags mirror), the prone
// latch with none of Flags 0x10A000, and the dead bit stop both keys; an NPC row
// ramps nothing.
// [orig: Entity_UpdateInfantryPlayerBody @0x4B40E0 - @0x4B7DA2, @0x4B7DA9..0x4B7DAD
//  (prone_local @0x4B416C..0x4B4183), @0x4B7DAF..0x4B7DB4, the ramps @0x4B7DBF /
//  @0x4B7DD6]
void test_remote_lean_ramp_gates() {
	ns::ClientReplicaPipeline view;
	struct Case {
		uint16_t handle;
		EntityClass cls;
		uint32_t flags;
		uint8_t wire_flags;
		uint8_t stance;
		bool ramps;
		const char *what;
	};
	const Case cases[] = {
		{0x0101, EntityClass::Player, 0u, 0u, 0u, true, "lean ramp: a free body ramps"},
		{0x0102, EntityClass::Player, 0x100000u, 0u, 0u, false, "lean ramp: a ladder contact stops it"},
		{0x0103, EntityClass::Player, 0x20u, 0x20u, 0u, false,
				"lean ramp: the record's chute bit, on the row's Flags mirror, stops it"},
		{0x0104, EntityClass::Player, 0u, 0u, 1u, false, "lean ramp: the prone latch stops it"},
		{0x0105, EntityClass::Player, 0x2000u, 0u, 1u, true,
				"lean ramp: a prone latch in the air does not stop it"},
		{0x0106, EntityClass::Player, world::kEntityFlagDead, 0u, 0u, false,
				"lean ramp: the dead bit stops it"},
		{0x0107, EntityClass::Infantry, 0u, 0u, 0u, false, "lean ramp: an NPC row has none"},
	};
	for (const Case &c : cases) {
		ns::ClientEntityState &row = view.state().upsert(c.handle);
		row.cls = c.cls;
		row.rm_entity_flags = c.flags;
		row.state_flags = c.wire_flags;
		row.state_flags_known = true;
		row.net_stance_bits = c.stance;
		row.move_input = static_cast<uint8_t>(world::Entity::kMoveOrderLeanLeft);
		row.lean_angle = 0;
	}
	view.tick_lean_ramp();
	for (const Case &c : cases) {
		const ns::ClientEntityState *row = view.state().find(c.handle);
		expect(row != nullptr && row->lean_angle == (c.ramps ? -0x3000000 : 0), c.what);
	}

	// A seated body's pass ends at the mounted site: no on-foot ramp; a passenger's
	// keys move its lean 0x1400000 a tick (the seated body tick, ahead of its eye),
	// any other seat's not at all; a row standing on a deck (bone 0) is on foot.
	// [orig: Entity_UpdateInfantryPlayerBody @0x4B40E0 - `cmp [esi+168h], 1` @0x4B6684,
	//  @0x4B66B5 / @0x4B66C3]
	static constexpr uint16_t kCarrier = 0x1010;
	static constexpr uint16_t kCarrierType = 0x0400;
	ns::ClientReplicaPipeline seated;
	seated.set_remote_motion_mode(true);
	seated.set_replica_seat_type_resolver([](uint16_t carrier_type, uint8_t bone) -> uint8_t {
		if (carrier_type != kCarrierType) return 0;
		return bone == 3 ? static_cast<uint8_t>(world::SeatType::Passenger)
		     : bone == 5 ? static_cast<uint8_t>(world::SeatType::Controller) : 0;
	});
	seated.state().upsert(kCarrier).type_id = kCarrierType;
	struct Seated {
		uint16_t handle;
		uint8_t bone;
		uint8_t keys;
		int32_t lean;
		const char *what;
	};
	const Seated rows[] = {
		{0x0111, 3, static_cast<uint8_t>(world::Entity::kMoveOrderLeanLeft), -0x1400000,
				"lean ramp: a seated passenger's left key, 0x1400000"},
		{0x0112, 3, static_cast<uint8_t>(world::Entity::kMoveOrderLeanRight), 0x1400000,
				"lean ramp: a seated passenger's right key, 0x1400000"},
		{0x0113, 5, static_cast<uint8_t>(world::Entity::kMoveOrderLeanLeft), 0,
				"lean ramp: a driver's keys move nothing"},
		{0x0114, 0, static_cast<uint8_t>(world::Entity::kMoveOrderLeanLeft), -0x3000000,
				"lean ramp: a row standing on the deck ramps on foot"},
	};
	for (const Seated &r : rows) {
		ns::ClientEntityState &row = seated.state().upsert(r.handle);
		row.cls = EntityClass::Player;
		row.carrier_handle = kCarrier;
		row.mount_bone = r.bone;
		row.move_input = r.keys;
		row.lean_angle = 0;
		row.net_has_compact = true;
	}
	seated.tick_remote_motion(0xFFFF);
	seated.tick_lean_ramp();
	for (const Seated &r : rows) {
		const ns::ClientEntityState *row = seated.state().find(r.handle);
		expect(row != nullptr && row->lean_angle == r.lean, r.what);
	}
}

// D-NET-330: a seated player row's body pass runs its channel at its head and ends
// at the mounted site: the seat carry writes the seat bone's yaw, roll and pitch into
// the body heading, Roll and savedLivePitch, the four leg words take the body heading,
// a passenger's keys ramp its lean, and the eye is the four-turn capsule leg over the
// channel's capsule (the extent capped at 0xD000) and those words, after the ramp.
// The row presents that channel (present_rows test_seated_replica_row_presents_its_channel).
// [orig: Entity_UpdateInfantryPlayerBody @0x4B40E0 - the dual update @0x4B41DF; the
//  seat carry @0x4B6510..0x4B6575; the leg words @0x4B668B..0x4B66A3; the passenger
//  ramp @0x4B6684..0x4B66C3; the eye @0x4B66D9..0x4B68FC]
void test_seated_row_eye_reads_its_channel_capsule() {
	static constexpr uint16_t kCarrier = 0x1010;
	static constexpr uint16_t kCarrierType = 0x0400;
	static constexpr int32_t kSeatYaw = 0x20000000;
	static constexpr int32_t kSeatPitch = 0x02000000;
	static constexpr int32_t kSeatRoll = -0x01000000;
	ns::ClientReplicaPipeline view;
	view.set_item_class_resolver(&classify);
	view.set_remote_motion_mode(true);
	CapsuleSource src;
	src.extent = 0x9000; // under the 0xD000 cap: the eye's height is the clip's
	view.set_root_motion_source(&src);
	view.set_replica_seat_type_resolver([](uint16_t carrier_type, uint8_t bone) -> uint8_t {
		return carrier_type == kCarrierType && bone == 3
				? static_cast<uint8_t>(world::SeatType::Passenger) : 0u;
	});
	int frame_calls = 0;
	view.set_replica_seat_frame_resolver([&frame_calls](uint16_t carrier, uint16_t carrier_type,
			uint8_t bone, ns::ClientReplicaPipeline::ReplicaSeatFrame &out) {
		++frame_calls;
		if (carrier != kCarrier || carrier_type != kCarrierType || bone != 3) return false;
		out.heading = kSeatYaw;
		out.pitch = kSeatPitch;
		out.roll = kSeatRoll;
		return true;
	});
	view.state().upsert(kCarrier).type_id = kCarrierType;
	ns::ClientEntityState &row = view.state().upsert(kPlayerHandle);
	row.type_id = kPlayerType;
	row.cls = EntityClass::Player;
	row.carrier_handle = kCarrier;
	row.mount_bone = 3;
	row.net_has_compact = true;
	row.rm_adm_id = 0;
	row.anim_state_id = as::kIdle;
	row.heading_bam = kSeatYaw + 0x08000000; // the look, a twist off the seat
	// Its record staged that look, so the seated chase holds it (D-NET-1119).
	row.net_smooth_heading = row.heading_bam;
	row.move_input = static_cast<uint8_t>(world::Entity::kMoveOrderLeanLeft);
	view.tick_remote_motion(0xFFFF);
	const ns::ClientEntityState *es = view.state().find(kPlayerHandle);
	expect(frame_calls == 1, "the seated pass carries its seat bone's frame");
	expect(es->rm_state == as::kIdle && es->rm_phase == 1, "the seated row's channel runs");
	expect(es->rm_body_heading == kSeatYaw && es->roll_bam == kSeatRoll &&
					es->rm_body_pitch == kSeatPitch,
			"the seat carry writes the body heading, Roll and savedLivePitch");
	expect(es->rm_leg_yaw[0] == kSeatYaw && es->rm_leg_yaw[1] == kSeatYaw &&
					es->rm_leg_target[0] == kSeatYaw && es->rm_leg_target[1] == kSeatYaw,
			"the mounted site's leg words take the body heading");
	expect(es->lean_angle == -0x1400000, "a passenger's left key ramps 0x1400000");
	int32_t eye[3] = {};
	world::player_body_capsule_eye(0x9000, -0x1400000, kSeatYaw + 0x08000000, kSeatYaw,
	                               kSeatRoll, kSeatPitch, eye);
	expect(es->eye_offset[0] == eye[0] && es->eye_offset[1] == eye[1] &&
					es->eye_offset[2] == eye[2],
			"the eye is the capsule leg over the channel's extent, after the ramp");
	int32_t unramped[3] = {};
	world::player_body_capsule_eye(0x9000, 0, kSeatYaw + 0x08000000, kSeatYaw, kSeatRoll,
	                               kSeatPitch, unramped);
	expect(unramped[0] != eye[0] || unramped[1] != eye[1],
			"(the ramp moves the eye, so the order is pinned)");
	// The cap: a taller capsule reads 0xD000.
	src.extent = 0x10000;
	view.tick_remote_motion(0xFFFF);
	int32_t capped[3] = {};
	world::player_body_capsule_eye(0xD000, -0x2800000, kSeatYaw + 0x08000000, kSeatYaw,
	                               kSeatRoll, kSeatPitch, capped);
	expect(es->eye_offset[2] == capped[2], "the seated extent caps at 0xD000");
	// The seated row presents its channel, so the channel stays armed.
	expect(es->rm_state >= 0, "(the channel stays armed while seated)");
}

// L5-s10 (V-ROLL-D's FOUND on D-NET-330): the mounted site scrubs a seated body on
// every pass: Flags &= 0xFF8F57DF (the 0x20, 0x800, 0x2000, 0x8000, 0x100000,
// 0x200000 and 0x400000 bits), slideDecay = 0 and both canopy words 0. The seat
// bit 0x40 and the Indoors bit 0x800000 stay.
// [orig: Entity_UpdateInfantryPlayerBody @0x4B40E0 - @0x4B6395, @0x4B63AB,
//  @0x4B63B1 / @0x4B63B8]
void test_seated_row_scrubs_its_body_words() {
	static constexpr uint16_t kCarrier = 0x1010;
	static constexpr uint16_t kCarrierType = 0x0400;
	ns::ClientReplicaPipeline view;
	view.set_item_class_resolver(&classify);
	view.set_remote_motion_mode(true);
	view.state().upsert(kCarrier).type_id = kCarrierType;
	ns::ClientEntityState &row = view.state().upsert(kPlayerHandle);
	row.type_id = kPlayerType;
	row.cls = EntityClass::Player;
	row.carrier_handle = kCarrier;
	row.mount_bone = 3;
	row.net_has_compact = true;
	// The stamp pending (-2), so no channel runs: a channel-less body (-1) runs nothing at all
	// (D-NET-1117).
	row.rm_adm_id = -2;
	row.rm_entity_flags = 0x40u | 0x20u | 0x800u | 0x2000u | 0x8000u | 0x100000u | 0x200000u |
	                      0x400000u | 0x800000u;
	row.rm_vel_z = -0x1234;
	row.rm_parachute.inflation = 5;
	row.rm_parachute.flap = 7;
	view.tick_remote_motion(0xFFFF);
	const ns::ClientEntityState *es = view.state().find(kPlayerHandle);
	expect(es->rm_entity_flags == (0x40u | 0x800000u),
			"the seated pass clears Flags & ~0xFF8F57DF and keeps the seat and Indoors bits");
	expect(es->rm_vel_z == 0, "the seated pass zeroes slideDecay");
	expect(es->rm_parachute.inflation == 0 && es->rm_parachute.flap == 0,
			"the seated pass zeroes both canopy words");
}

// D-NET-196: a decoded row's two channels serve the one ring table every body of
// its .adm reads, the client's own World body included: the dual update re-inits
// the secondary ahead of the primary, each re-init plays the state's head and
// advances it, and an unarmed loop wrap serves the head again, fading the next
// entry in from its first frame over eight ticks.
// [orig: AnimMap_LinkEntity @0x40BA10 (slot+0x48 = &entry+0x44 @0x40BA77);
//  AnimMap_UpdateDualChannels @0x40B8C0 (the secondary @0x40B908, the primary
//  @0x40B94E); AnimMap_UpdateEntity @0x40B737..0x40B778; AnimChannel_AdvancePlayback
//  @0x40B140 (the callback @0x40B1C8) -> AnimMap_AdvanceToNextAnim @0x40BDF0 (the
//  serve @0x40BE02..0x40BE07, InitFromParams(.., 8, 0, 0x1000) @0x40BE24)]
struct RingBodySource final : public opennova::world::IRootMotionSource {
	bool has_clip(int, int) const override { return true; }
	// Idle authors three entries, every other state one.
	int variant_count(int, int state) const override { return state == as::kIdle ? 3 : 1; }
	bool advance(int adm, int state, int32_t &phase,
			opennova::world::RootMotionFrame &out) override {
		return advance_variant(adm, state, 0, phase, out);
	}
	bool advance_variant(int, int, int, int32_t &phase,
			opennova::world::RootMotionFrame &out) override {
		++phase;
		out = {};
		out.capsule_bottom = 1 << 16;
		out.capsule_top = 2 << 16;
		return true;
	}
	// Each entry its own loop length: entry v runs 10 + v ticks.
	int32_t clip_length_ticks(int, int, int variant) const override { return 10 + variant; }
	bool clip_loops(int, int, int) const override { return true; }
};

void test_row_channels_serve_the_shared_variant_rings() {
	ns::ClientReplicaPipeline view;
	view.set_protocol(title_protocol("jo"));
	view.set_item_class_resolver(&classify);
	view.set_remote_motion_mode(true);
	RingBodySource src;
	opennova::world::AnimVariantRings rings;
	view.set_root_motion_source(&src);
	view.set_anim_variant_rings(&rings);
	seed_row(view);
	view.apply(0x0A, player_frame(as::kIdle, 0));
	arm_row(view);
	const ns::ClientEntityState *es = view.state().find(kPlayerHandle);
	// Registration put idle's head on the row's last entry; the ring runs back.
	view.tick_remote_motion(0xFFFF);
	expect(es->wpn_playing == as::kIdle && es->wpn_variant == 2,
			"the secondary re-inits first and plays the head, entry 2");
	expect(es->rm_state == as::kIdle && es->rm_variant == 1,
			"the primary's re-init plays the advanced head, entry 1");
	// The client's own World body plays idle from the same heads: entry 0, and
	// the head wraps back to entry 2.
	expect(rings.serve(&src, 0, as::kIdle) == 0, "the World body serves from the shared heads");
	for (int tick = 2; tick <= 10; ++tick) view.tick_remote_motion(0xFFFF);
	expect(es->rm_variant == 1 && es->rm_phase == 10 && es->rm_blend_weight >= 1.0f,
			"entry 1 runs its 11 ticks");
	view.tick_remote_motion(0xFFFF); // tick 11: the primary's loop wraps
	expect(es->rm_variant == 2 && es->rm_prev_variant == 1 && es->rm_prev_state == as::kIdle &&
					es->rm_phase == 0 && es->rm_blend_weight == 0.0f &&
					es->rm_blend_step == 0.125f,
			"the wrap serves entry 2 and fades it in from its first frame over eight ticks");
	expect(es->wpn_variant == 2 && es->wpn_phase == 11,
			"the secondary's 12-tick entry has not wrapped yet");
	view.tick_remote_motion(0xFFFF); // tick 12: the secondary's loop wraps
	expect(es->wpn_variant == 1 && es->wpn_prev_variant == 2 && es->wpn_prev == as::kIdle &&
					es->wpn_phase == 0 && es->wpn_blend_weight == 0.0f,
			"the secondary's wrap serves the next entry back, 1, from the same heads");
	expect(es->rm_blend_weight == 0.125f && es->rm_phase == 1,
			"the primary's wrap fade steps an eighth");
	// A state with one entry always plays it.
	view.apply(0x0A, player_frame(as::kRunForward, 0));
	view.tick_remote_motion(0xFFFF);
	expect(es->rm_state == as::kRunForward && es->rm_variant == 0,
			"a single-entry state plays its one entry");
}

// D-NET-196 (L5-s10 row 1): the walk serves the shared ring heads body by body in
// slot order, each body's secondary ahead of its primary, whatever order the rows
// arrived in. Rows 0x0005 (arrived first) and 0x0003 both re-init idle on one tick
// over the three-entry ring: slot 3 serves 2 (secondary) and 1 (primary), then slot
// 5 serves 0 and 2. Arrival order with every secondary first would give slot 5 2 and
// 0, slot 3 1 and 2.
// [orig: Entity_UpdateAllEntities @0x4C2100 - the pool-0 walk @0x4C2437..0x4C2472;
//  AnimMap_UpdateDualChannels @0x40B8C0, the secondary @0x40B908 before the primary
//  @0x40B94E]
void test_rows_serve_the_rings_in_slot_order() {
	ns::ClientReplicaPipeline view;
	view.set_protocol(title_protocol("jo"));
	view.set_item_class_resolver(&classify);
	view.set_remote_motion_mode(true);
	RingBodySource src;
	opennova::world::AnimVariantRings rings;
	view.set_root_motion_source(&src);
	view.set_anim_variant_rings(&rings);
	static constexpr uint16_t kLate = 0x0005;
	static constexpr uint16_t kEarly = 0x0003;
	view.state().upsert(kLate).type_id = kPlayerType;
	view.state().upsert(kEarly).type_id = kPlayerType;
	FrameUpdate fu;
	fu.anchor_x = 100 << 16;
	fu.anchor_y = 200 << 16;
	fu.anchor_z = 10 << 16;
	fu.flags2 = 0;
	fu.carried_handle = 0xFFFF;
	for (const uint16_t handle : {kLate, kEarly}) {
		FrameUpdateRecord rec;
		rec.handle = handle;
		rec.type_id = kPlayerType;
		rec.cls = EntityClass::Player;
		rec.player.carrier_handle = 0xFFFF;
		rec.player.pos_x_compressed = network_compress_fixedpoint(1 << 16);
		rec.player.pos_y_compressed = network_compress_fixedpoint(2 << 16);
		rec.player.pos_z_compressed = network_compress_fixedpoint(0);
		rec.player.yaw_byte = 0x40;
		rec.player.anim_state_id = as::kIdle;
		rec.player.anim_channel_ratio = 0;
		fu.records.push_back(rec);
	}
	view.apply(0x0A, encode_frame_update(fu));
	view.state().find(kLate)->rm_adm_id = 0;
	view.state().find(kEarly)->rm_adm_id = 0;
	view.tick_remote_motion(0xFFFF);
	const ns::ClientEntityState *early = view.state().find(kEarly);
	const ns::ClientEntityState *late = view.state().find(kLate);
	expect(early->wpn_playing == as::kIdle && early->wpn_variant == 2 &&
					early->rm_state == as::kIdle && early->rm_variant == 1,
			"slot 3 serves first: its secondary entry 2, its primary entry 1");
	expect(late->wpn_playing == as::kIdle && late->wpn_variant == 0 &&
					late->rm_state == as::kIdle && late->rm_variant == 2,
			"slot 5 serves next: its secondary entry 0, its primary entry 2");
}

// D-NET-196 (L5-s11 row 3, L5-s13 row 2): the own body serves the shared ring heads at
// its own pool-0 slot. Retail's pool-0 walk updates every used slot in slot order with no
// local-player case, after the pool-1 walk, the projectiles and the proximity build; ours
// serves the own body in the World's entity pass. So a joiner's walk parks every pool-0
// row: the rows through the own row's slot (3, then the own 4) for the middle leg, ahead
// of the World's pool-0 walk, and slot 5, with its lean ramp, for the after-self leg
// behind the own body. Over the three-entry ring: slot 3 serves 2 and 1, the own body 0,
// slot 5 2 and 1. Slot 5 ahead of the own body would have served 0 and 2.
// [orig: Entity_UpdateAllEntities @0x4C2100 - the proximity build @0x4C240A, the pool-0
//  walk @0x4C2426..0x4C2472, the +0x1C4 call @0x4C2472; AnimMap_UpdateDualChannels
//  @0x40B8C0, the secondary @0x40B908 before the primary @0x40B94E]
void test_own_body_serves_the_rings_at_its_slot() {
	ns::ClientReplicaPipeline view;
	view.set_protocol(title_protocol("jo"));
	view.set_item_class_resolver(&classify);
	view.set_remote_motion_mode(true);
	view.set_self_body_split(true);
	RingBodySource src;
	opennova::world::AnimVariantRings rings;
	view.set_root_motion_source(&src);
	view.set_anim_variant_rings(&rings);
	static constexpr uint16_t kEarly = 0x0003;
	static constexpr uint16_t kSelf = 0x0004;
	static constexpr uint16_t kLate = 0x0005;
	FrameUpdate fu;
	fu.anchor_x = 100 << 16;
	fu.anchor_y = 200 << 16;
	fu.anchor_z = 10 << 16;
	fu.flags2 = 0;
	fu.carried_handle = 0xFFFF;
	for (const uint16_t handle : {kLate, kSelf, kEarly}) {
		view.state().upsert(handle).type_id = kPlayerType;
		FrameUpdateRecord rec;
		rec.handle = handle;
		rec.type_id = kPlayerType;
		rec.cls = EntityClass::Player;
		rec.player.carrier_handle = 0xFFFF;
		rec.player.pos_x_compressed = network_compress_fixedpoint(1 << 16);
		rec.player.pos_y_compressed = network_compress_fixedpoint(2 << 16);
		rec.player.pos_z_compressed = network_compress_fixedpoint(0);
		rec.player.yaw_byte = 0x40;
		rec.player.anim_state_id = as::kIdle;
		rec.player.anim_channel_ratio = 0;
		fu.records.push_back(rec);
	}
	view.apply(0x0A, encode_frame_update(fu));
	for (const uint16_t handle : {kLate, kSelf, kEarly}) view.state().find(handle)->rm_adm_id = 0;
	view.state().find(kLate)->move_input =
			static_cast<uint8_t>(world::Entity::kMoveOrderLeanLeft);
	view.tick_remote_motion(kSelf);
	view.tick_lean_ramp(); // the embedder's ramp after the walk (ClientRuntime)
	const ns::ClientEntityState *early = view.state().find(kEarly);
	const ns::ClientEntityState *late = view.state().find(kLate);
	expect(early->wpn_playing < 0 && early->rm_state < 0,
			"slot 3 waits for the World's entity pass: no visit in the client net frame");
	expect(late->wpn_playing < 0 && late->rm_state < 0 && late->lean_angle == 0,
			"slot 5 waits past the own body's slot: no visit, no lean ramp yet");
	// The World's entity pass: the middle leg ahead of its pool-0 walk, then the own
	// body's channel at its slot.
	view.tick_remote_motion_through_self();
	expect(early->wpn_variant == 2 && early->rm_state == as::kIdle && early->rm_variant == 1,
			"slot 3 serves ahead of the own body: its secondary entry 2, its primary entry 1");
	expect(late->wpn_playing < 0 && late->rm_state < 0,
			"the middle leg stops at the own row's slot");
	expect(rings.serve(&src, 0, as::kIdle) == 0, "the own body serves entry 0 at slot 4");
	view.tick_remote_motion_after_self();
	expect(late->wpn_playing == as::kIdle && late->wpn_variant == 2 &&
					late->rm_state == as::kIdle && late->rm_variant == 1,
			"slot 5 serves behind the own body: its secondary entry 2, its primary entry 1");
	expect(late->lean_angle == -0x3000000, "slot 5's lean ramp closes its own visit");
	view.tick_remote_motion_after_self();
	expect(late->lean_angle == -0x3000000, "the after-self leg runs once per walk");
}

// D-NET-196 (V-ROLL-K, L5-s12 row 0; L5-s13 row 2): a parked pool-0 row keeps its lean,
// its kick, its dip window and its torso roll until its own visit. Retail runs the
// decay, the dip and the roll inside each body's update, so the World legs ahead of the
// pool-0 walk (the projectile pass, the bone raycast's pose) read every pool-0 body as
// its last frame left it. Rows 3, the own 4 and 5 lean, kick and roll alike: none takes
// the legs in the client net frame; slot 3 takes them in the middle leg ahead of its
// visit, slot 5 in the after-self leg ahead of its.
// [orig: Entity_UpdateAllEntities @0x4C2100 - the Weapon_UpdateAllProjectiles call
//  @0x4C223A ahead of the pool-0 walk @0x4C2426..0x4C2472; Entity_UpdateInfantryPlayerBody
//  @0x4B40E0 - the decay @0x4B5C97..0x4B5CA5, the dip @0x4B5CAB..0x4B5CE7, the torso
//  roll @0x4B5CFF..0x4B5D67]
void test_parked_row_keeps_its_body_legs_until_its_visit() {
	ns::ClientReplicaPipeline view;
	view.set_protocol(title_protocol("jo"));
	view.set_item_class_resolver(&classify);
	view.set_remote_motion_mode(true);
	view.set_self_body_split(true);
	BodySource src;
	view.set_root_motion_source(&src);
	static constexpr uint16_t kEarly = 0x0003;
	static constexpr uint16_t kSelf = 0x0004;
	static constexpr uint16_t kLate = 0x0005;
	FrameUpdate fu;
	fu.anchor_x = 100 << 16;
	fu.anchor_y = 200 << 16;
	fu.anchor_z = 10 << 16;
	fu.flags2 = 0;
	fu.carried_handle = 0xFFFF;
	for (const uint16_t handle : {kLate, kSelf, kEarly}) {
		view.state().upsert(handle).type_id = kPlayerType;
		FrameUpdateRecord rec;
		rec.handle = handle;
		rec.type_id = kPlayerType;
		rec.cls = EntityClass::Player;
		rec.player.carrier_handle = 0xFFFF;
		rec.player.pos_x_compressed = network_compress_fixedpoint(1 << 16);
		rec.player.pos_y_compressed = network_compress_fixedpoint(2 << 16);
		rec.player.pos_z_compressed = network_compress_fixedpoint(0);
		rec.player.yaw_byte = 0x40;
		rec.player.anim_state_id = as::kIdle;
		rec.player.anim_channel_ratio = 0;
		fu.records.push_back(rec);
	}
	view.apply(0x0A, encode_frame_update(fu));
	static constexpr int32_t kLean = 0x10000000;
	static constexpr int32_t kKick = -0x8000000;
	static constexpr int32_t kTorso = 0x1000000;
	for (const uint16_t handle : {kLate, kSelf, kEarly}) {
		ns::ClientEntityState &row = *view.state().find(handle);
		row.rm_adm_id = -2; // the stamp pending, no channel: the body legs alone
		row.lean_angle = kLean;
		row.pitch_kick_accum = kKick;
		row.arms_dip_ticks = 6;
		row.torso_roll = kTorso;
	}
	// The roll the torso chases, read ahead of each row's visit.
	const int32_t early_roll = view.state().find(kEarly)->roll_bam;
	// ClientRuntime's order: the decay, the dip, the walk, the ramp.
	view.tick_lean_decay(kSelf);
	view.tick_arms_dip(kSelf);
	view.tick_remote_motion(kSelf);
	view.tick_lean_ramp();
	const ns::ClientEntityState *early = view.state().find(kEarly);
	const ns::ClientEntityState *late = view.state().find(kLate);
	// The decay: lean -= (lean + 8) >> 4. The dip: the open window drops 0x2800000 and
	// steps, the kick eases an eighth, the window steps again. The roll chases the Roll
	// word a sixteenth.
	const int32_t kick_dropped = kKick - 0x2800000;
	const int32_t kick_eased = kick_dropped - ((kick_dropped + 4) >> 3);
	const int32_t torso_chased = kTorso + ((early_roll - kTorso + 8) >> 4);
	for (const ns::ClientEntityState *row : {early, late})
		expect(row->lean_angle == kLean && row->pitch_kick_accum == kKick &&
						row->arms_dip_ticks == 6 && row->torso_roll == kTorso,
				"a parked pool-0 row keeps its lean, kick, window and torso roll in the "
				"client net frame");
	view.tick_remote_motion_through_self();
	expect(early->lean_angle == 0x0F000000 && early->pitch_kick_accum == kick_eased &&
					early->arms_dip_ticks == 4 && early->torso_roll == torso_chased,
			"slot 3 takes its decay, dip and torso roll in the middle leg, ahead of the own body");
	expect(late->lean_angle == kLean && late->pitch_kick_accum == kKick &&
					late->arms_dip_ticks == 6 && late->torso_roll == kTorso,
			"slot 5, parked, keeps its lean, kick, window and torso roll between the legs");
	const int32_t late_roll = late->roll_bam;
	view.tick_remote_motion_after_self();
	expect(late->lean_angle == 0x0F000000 && late->pitch_kick_accum == kick_eased &&
					late->arms_dip_ticks == 4 &&
					late->torso_roll == kTorso + ((late_roll - kTorso + 8) >> 4),
			"slot 5 takes the same decay, dip and torso roll ahead of its own visit");
	view.tick_remote_motion_after_self();
	expect(late->lean_angle == 0x0F000000 && late->pitch_kick_accum == kick_eased &&
					late->arms_dip_ticks == 4,
			"the after-self leg runs the parked row's legs once per walk");
	// No split (no own row in the walk): every row takes the legs ahead of the walk.
	ns::ClientEntityState &late_row = *view.state().find(kLate);
	late_row.lean_angle = kLean;
	view.tick_lean_decay(0x0006);
	expect(late_row.lean_angle == 0x0F000000, "with no own row the decay runs for every row");
}

// D-NET-196 (V-ROLL's edge): a dead player row in the air tumbles where the conform
// selector would decay it: the body pitch and Roll hold on three ticks of four, and
// on the fourth they ease an eighth toward the tick-phase ramps a and b while the
// aim word takes a + b.
// [orig: Entity_UpdateInfantryPlayerBody @0x4B40E0 - the gate @0x4B6CCB..0x4B6CDE,
//  tick & 3 @0x4B6CE8, the ramps @0x4B6CEC..0x4B6D12, the chases @0x4B6D18..0x4B6D41,
//  the aim store @0x4B6D44]
void test_dead_row_in_the_air_tumbles() {
	ns::ClientReplicaPipeline view;
	view.set_item_class_resolver(&classify);
	view.set_remote_motion_mode(true);
	BodySource src;
	view.set_root_motion_source(&src);
	ns::ClientEntityState &row = view.state().upsert(kPlayerHandle);
	row.type_id = kPlayerType;
	row.cls = EntityClass::Player;
	row.net_has_compact = true;
	row.rm_adm_id = -2; // the stamp pending, no channel: the conform leg alone
	row.rm_entity_flags = world::kEntityFlagDead | world::kEntityFlagInAir;
	row.rm_body_pitch = 0x100000;
	row.roll_bam = 0x100000;
	const ns::ClientEntityState *es = view.state().find(kPlayerHandle);
	for (int tick = 1; tick <= 3; ++tick) view.tick_remote_motion(0xFFFF);
	expect(es->rm_body_pitch == 0x100000 && es->roll_bam == 0x100000 && es->rm_aim_word == 0,
			"ticks 1..3: no decay, no ramp");
	view.tick_remote_motion(0xFFFF); // tick 4: a = 28 * 0xFFFFFF, b = -28 * 0xFFFFFF
	const int32_t a = 28 * 0xFFFFFF;
	const int32_t b = -28 * 0xFFFFFF;
	expect(es->rm_body_pitch == 0x100000 + ((a - 0x100000 + 4) >> 3) &&
					es->roll_bam == 0x100000 + ((b - 0x100000 + 4) >> 3),
			"tick 4: the body pitch and Roll ease an eighth toward a and b");
	expect(es->rm_aim_word == a + b, "tick 4: the aim word takes a + b");
}

// A remote body's sound block runs in its own tick: the player body on the replica clock's
// even ticks, the event word its channel update sampled, the frame's capsule bottom, the row's
// position ahead of its integrate; the own row and a starved one play none.
// [orig: Entity_UpdateInfantryPlayerBody - `test current_tick,1; jnz` @0x4B76E6, the block
//  @0x4B76F1..0x4B78A8 ahead of the velocity @0x4B78A8; the starved return @0x4B4669..0x4B4670]
void test_remote_body_sound_block_runs_per_tick() {
	ns::ClientReplicaPipeline view;
	view.set_protocol(title_protocol("jo"));
	view.set_item_class_resolver(&classify);
	view.set_remote_motion_mode(true);
	StepSource src;
	view.set_root_motion_source(&src);
	seed_row(view);
	view.apply(0x0A, player_frame(as::kIdle, 0));
	arm_row(view);
	(void)view.drain_body_sound_words();
	for (int i = 0; i < 4; ++i) view.tick_remote_motion(0xFFFF);
	const auto words = view.drain_body_sound_words();
	// Four ticks, two of them even: one word on each.
	expect(words.size() == 2, "a player body's block runs on the clock's even ticks: two of four");
	const ns::ClientEntityState *es = view.state().find(kPlayerHandle);
	if (!words.empty() && es != nullptr) {
		expect(words[0].handle == kPlayerHandle && words[0].type_id == kPlayerType &&
						words[0].events == 0x1u && words[0].capsule_bottom == (1 << 16),
				"the word carries the row, its event word and the capsule bottom");
		expect(!words[0].on_entity, "a row with no link plays on the ground");
	}
	// The own row: its block is the World body's.
	(void)view.drain_body_sound_words();
	for (int i = 0; i < 4; ++i) view.tick_remote_motion(kPlayerHandle);
	expect(view.drain_body_sound_words().empty(), "the own row plays no replica block");
}

int main() {
	test_dead_row_in_the_air_tumbles();
	test_seated_row_eye_reads_its_channel_capsule();
	test_seated_row_scrubs_its_body_words();
	test_row_channels_serve_the_shared_variant_rings();
	test_rows_serve_the_rings_in_slot_order();
	test_own_body_serves_the_rings_at_its_slot();
	test_parked_row_keeps_its_body_legs_until_its_visit();
	test_record_flags_land_on_the_mirror();
	test_dead_records_queue_behind_the_0x13();
	test_defless_carrier_drops_the_dead_record();
	test_slot_restart_drops_a_queued_dead_record();
	test_remote_lean_ramp_gates();
	test_remote_body_conform_chases_the_slope();
	test_tapped_roll_locks_and_queues();
	test_remote_eye_restamps_from_the_capsule();
	test_pending_promotes_at_clip_end();
	test_gait_transition_insert();
	test_dead_park_and_respawn_edges();
	test_row_weapon_channel_plays_an_emote_then_returns();
	test_remote_body_sound_block_runs_per_tick();
	if (failures) {
		std::fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	std::printf("client_replica_pipeline_body_arbitration: all checks passed\n");
	return 0;
}
