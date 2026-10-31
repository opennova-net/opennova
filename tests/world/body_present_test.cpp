/* engine/runtime/world body_present + player_present's avatar row — a person body's present inputs,
   moved from the binding's present walks (ADR 0077 R2), over hand-built present rows and a recording
   channel target (no retail data, no model):

     1. placed_body_clip_request: the state at its tick, blended from its source below weight 1, the
        source alone when the state names no clip, the coarse slot with neither; equal rows make equal
        requests;
     2. local_body_clip_request: the avatar's primary channel, the slot when its state has no clip;
     3. present_wire_body: a host-loopback row poses the authority's state (blend / clip / free /
        source / slot); a remote-request row forwards the pulse then the state to the arbitration,
        ticks an accepted transition once per simulation tick while the target reports one in flight,
        and dispatches nothing on a steady state [orig: NetPacket_SerializePlayerState @0x4C1153];
     4. row_weapon_channel: the PF_WPN_* fields as a WeaponChannel;
     5. the aim overlay deltas: the body class is identity, a class differing by a yaw is that yaw
        about the up axis, and a row without PF_AIM_OVERLAY_VALID writes none. */

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "common/test_expect.h"

#include <runtime/anim/aim_overlay.h>
#include <runtime/anim/body_channel.h>
#include <runtime/world/body_present.h>
#include <runtime/world/infantry.h>
#include <runtime/world/player_present.h>
#include <runtime/world/present_rows.h>

using namespace opennova;
using world::BodyClipRequest;

namespace {

// Records every verb as one line; remote verbs answer `in_flight`.
class Recorder final : public anim::BodyChannelTarget {
public:
	std::vector<std::string> calls;
	bool in_flight = false;
	int ticks_left = 0; // remote_tick answers true this many times

	void play_free(const std::string &key) override { calls.push_back("free " + key); }
	void play_at(const std::string &key, int32_t phase, int32_t variant, bool parked) override {
		calls.push_back("at " + key + " " + std::to_string(phase) + " " + std::to_string(variant) +
				(parked ? " parked" : ""));
	}
	void play_blend_at(const std::string &source, int32_t source_phase, const std::string &key,
			int32_t phase, float weight, int32_t source_variant, int32_t variant) override {
		char w[32];
		std::snprintf(w, sizeof(w), "%.2f", weight);
		calls.push_back("blend " + source + " " + std::to_string(source_phase) + " " + key + " " +
				std::to_string(phase) + " " + w + " " + std::to_string(source_variant) + " " +
				std::to_string(variant));
	}
	void play_slot(int32_t slot) override { calls.push_back("slot " + std::to_string(slot)); }
	void play_slot_at(int32_t slot, int32_t phase, bool parked) override {
		calls.push_back("slot_at " + std::to_string(slot) + " " + std::to_string(phase) +
				(parked ? " parked" : ""));
	}
	bool remote_request(int32_t state, const std::string &key, uint32_t, int32_t phase) override {
		calls.push_back("remote " + std::to_string(state) + " " + key + " " + std::to_string(phase));
		return in_flight;
	}
	bool remote_tick(int32_t state) override {
		calls.push_back("tick " + std::to_string(state));
		if (ticks_left <= 0) return false;
		--ticks_left;
		return ticks_left > 0;
	}
};

// One present row with no state, no source, no slot and no phase.
std::vector<float> blank_row() {
	std::vector<float> row(world::PF_STRIDE, 0.0f);
	row[world::PF_ANIM_STATE] = -1.0f;
	row[world::PF_ANIM_SOURCE_STATE] = -1.0f;
	row[world::PF_ANIM_BLEND_WEIGHT] = 1.0f;
	row[world::PF_ANIM_PHASE_TICKS] = -1.0f;
	row[world::PF_BODY_ANIM_SLOT] = -1.0f;
	row[world::PF_ANIM_STATE_PULSE] = -1.0f;
	row[world::PF_WPN_ANIM_STATE] = -1.0f;
	row[world::PF_WPN_SOURCE_STATE] = -1.0f;
	row[world::PF_WPN_BLEND_WEIGHT] = 1.0f;
	return row;
}

bool quat_near(const anim::Quat &a, const anim::Quat &b, float eps = 1e-5f) {
	// q and -q are one rotation.
	const float d = a.w * b.w + a.x * b.x + a.y * b.y + a.z * b.z;
	return std::fabs(std::fabs(d) - 1.0f) <= eps;
}

} // namespace

int main() {
	const int kIdle = world::anim_state::kIdle;
	const int kWalk = world::anim_state::kWalkForward;
	const std::string idle = world::infantry_anim_key_ref(kIdle);
	const std::string walk = world::infantry_anim_key_ref(kWalk);

	// ---- 1. the placed request ----
	{
		std::vector<float> row = blank_row();
		TEST_EXPECT(world::placed_body_clip_request(row.data()).kind == BodyClipRequest::Kind::kNone);
		row[world::PF_BODY_ANIM_SLOT] = 3.0f;
		row[world::PF_ANIM_PHASE_TICKS] = 12.0f;
		row[world::PF_ANIM_PHASE_PARKED] = 1.0f;
		BodyClipRequest r = world::placed_body_clip_request(row.data());
		TEST_EXPECT(r.kind == BodyClipRequest::Kind::kSlotAt && r.selector == 3 && r.phase == 12 && r.parked);
		row[world::PF_ANIM_SOURCE_STATE] = static_cast<float>(kIdle);
		row[world::PF_ANIM_SOURCE_PHASE_TICKS] = 7.0f;
		row[world::PF_ANIM_SOURCE_VARIANT] = 2.0f;
		r = world::placed_body_clip_request(row.data());
		TEST_EXPECT(r.kind == BodyClipRequest::Kind::kClipAt && r.selector == kIdle && r.phase == 7 &&
				r.variant == 2 && !r.parked);
		row[world::PF_ANIM_STATE] = static_cast<float>(kWalk);
		row[world::PF_ANIM_VARIANT] = 1.0f;
		row[world::PF_ANIM_BLEND_WEIGHT] = 0.5f;
		r = world::placed_body_clip_request(row.data());
		TEST_EXPECT(r.kind == BodyClipRequest::Kind::kBlendAt && r.selector == kWalk && r.phase == 12 &&
				r.variant == 1 && r.source_state == kIdle && r.source_phase == 7 && r.source_variant == 2 &&
				r.weight == 0.5f && !r.parked);
		TEST_EXPECT(r == world::placed_body_clip_request(row.data()));
		row[world::PF_ANIM_BLEND_WEIGHT] = 1.0f;
		const BodyClipRequest clip = world::placed_body_clip_request(row.data());
		TEST_EXPECT(clip.kind == BodyClipRequest::Kind::kClipAt && clip.selector == kWalk && clip.parked);
		TEST_EXPECT(clip != r);
		Recorder rec;
		world::play_body_clip_request(rec, r);
		world::play_body_clip_request(rec, clip);
		TEST_EXPECT(rec.calls.size() == 2);
		TEST_EXPECT(rec.calls[0] == "blend " + idle + " 7 " + walk + " 12 0.50 2 1");
		TEST_EXPECT(rec.calls[1] == "at " + walk + " 12 1 parked");
	}

	// ---- 2. the avatar's request ----
	{
		world::InfantryBodyPose pose;
		pose.state = kWalk;
		pose.phase = 9;
		pose.variant = 1;
		pose.parked = true;
		BodyClipRequest r = world::local_body_clip_request(pose, 5);
		TEST_EXPECT(r.kind == BodyClipRequest::Kind::kClipAt && r.selector == kWalk && r.phase == 9 && r.parked);
		pose.blending = true;
		pose.source_state = kIdle;
		pose.source_phase = 3;
		pose.weight = 0.25f;
		r = world::local_body_clip_request(pose, 5);
		TEST_EXPECT(r.kind == BodyClipRequest::Kind::kBlendAt && r.source_state == kIdle && r.weight == 0.25f);
		pose.state = -1;
		r = world::local_body_clip_request(pose, 5);
		TEST_EXPECT(r.kind == BodyClipRequest::Kind::kSlotAt && r.selector == 5 && r.phase == 9 && r.parked);
		// No slot either: nothing plays.
		Recorder rec;
		world::play_body_clip_request(rec, world::local_body_clip_request(pose, -1));
		TEST_EXPECT(rec.calls.empty());
	}

	// ---- 3. the wire walk ----
	{
		// Host loopback: the accepted state at its tick.
		std::vector<float> row = blank_row();
		row[world::PF_ANIM_STATE] = static_cast<float>(kWalk);
		row[world::PF_ANIM_PHASE_TICKS] = 4.0f;
		world::WireBodyLatch latch;
		Recorder rec;
		TEST_EXPECT(world::present_wire_body(rec, row.data(), 1, latch));
		TEST_EXPECT(rec.calls.size() == 1 && rec.calls[0] == "at " + walk + " 4 0");
		TEST_EXPECT(latch.anim_state == kWalk && latch.anim_request == 0 && latch.remote_tick == 0);
		// ... blended from its source while the weight is below 1.
		row[world::PF_ANIM_SOURCE_STATE] = static_cast<float>(kIdle);
		row[world::PF_ANIM_SOURCE_PHASE_TICKS] = 2.0f;
		row[world::PF_ANIM_BLEND_WEIGHT] = 0.75f;
		rec.calls.clear();
		TEST_EXPECT(world::present_wire_body(rec, row.data(), 1, latch));
		TEST_EXPECT(rec.calls.size() == 1 && rec.calls[0] == "blend " + idle + " 2 " + walk + " 4 0.75 0 0");
		// No phase: the clip runs free.
		row[world::PF_ANIM_PHASE_TICKS] = -1.0f;
		rec.calls.clear();
		TEST_EXPECT(world::present_wire_body(rec, row.data(), 1, latch));
		TEST_EXPECT(rec.calls.size() == 1 && rec.calls[0] == "free " + walk);
		// No state: the source alone; then the slot.
		row[world::PF_ANIM_STATE] = -1.0f;
		rec.calls.clear();
		TEST_EXPECT(world::present_wire_body(rec, row.data(), 1, latch));
		TEST_EXPECT(rec.calls.size() == 1 && rec.calls[0] == "at " + idle + " 2 0");
		row[world::PF_ANIM_SOURCE_STATE] = -1.0f;
		row[world::PF_BODY_ANIM_SLOT] = 6.0f;
		rec.calls.clear();
		TEST_EXPECT(world::present_wire_body(rec, row.data(), 1, latch));
		TEST_EXPECT(rec.calls.size() == 1 && rec.calls[0] == "slot 6");
		row[world::PF_ANIM_PHASE_TICKS] = 3.0f;
		row[world::PF_ANIM_PHASE_PARKED] = 1.0f;
		rec.calls.clear();
		TEST_EXPECT(world::present_wire_body(rec, row.data(), 1, latch));
		TEST_EXPECT(rec.calls.size() == 1 && rec.calls[0] == "slot_at 6 3 parked");
	}
	{
		// A remote-request row: the pulse first, then the state; a transition in flight ticks once
		// per simulation tick; a steady state dispatches nothing.
		std::vector<float> row = blank_row();
		row[world::PF_ANIM_REMOTE_REQUEST] = 1.0f;
		row[world::PF_ANIM_STATE] = static_cast<float>(kWalk);
		row[world::PF_ANIM_PHASE_TICKS] = 5.0f;
		row[world::PF_ANIM_STATE_PULSE] = static_cast<float>(kIdle);
		row[world::PF_ANIM_PULSE_TICKS] = 8.0f;
		world::WireBodyLatch latch;
		Recorder rec;
		rec.in_flight = true;
		TEST_EXPECT(world::present_wire_body(rec, row.data(), 1, latch));
		TEST_EXPECT(rec.calls.size() == 2);
		TEST_EXPECT(rec.calls[0] == "remote " + std::to_string(kIdle) + " " + idle + " 8");
		TEST_EXPECT(rec.calls[1] == "remote " + std::to_string(kWalk) + " " + walk + " 5");
		TEST_EXPECT(latch.remote_tick == 1 && latch.anim_request == 1 && latch.anim_state == kWalk);
		// The next frames: no pulse, the same state. Three ticks run while the target reports the
		// transition in flight (it ends on the second).
		row[world::PF_ANIM_STATE_PULSE] = -1.0f;
		rec.calls.clear();
		rec.ticks_left = 2;
		TEST_EXPECT(world::present_wire_body(rec, row.data(), 3, latch));
		TEST_EXPECT(rec.calls.size() == 2 && rec.calls[0] == "tick " + std::to_string(kWalk));
		TEST_EXPECT(latch.remote_tick == 0);
		rec.calls.clear();
		TEST_EXPECT(!world::present_wire_body(rec, row.data(), 2, latch));
		TEST_EXPECT(rec.calls.empty());
		// A new state dispatches again; a remote row never takes the source leg.
		row[world::PF_ANIM_STATE] = -1.0f;
		row[world::PF_ANIM_SOURCE_STATE] = static_cast<float>(kIdle);
		rec.calls.clear();
		rec.in_flight = false;
		TEST_EXPECT(world::present_wire_body(rec, row.data(), 1, latch));
		TEST_EXPECT(rec.calls.empty() && latch.anim_state == -1);
	}

	// ---- 4. the weapon channel ----
	{
		std::vector<float> row = blank_row();
		TEST_EXPECT(world::row_weapon_channel(row.data()).key.empty());
		row[world::PF_WPN_ANIM_STATE] = static_cast<float>(kWalk);
		row[world::PF_WPN_PHASE_TICKS] = 14.0f;
		row[world::PF_WPN_SOURCE_STATE] = static_cast<float>(kIdle);
		row[world::PF_WPN_SOURCE_PHASE_TICKS] = 3.0f;
		row[world::PF_WPN_BLEND_WEIGHT] = 0.5f;
		row[world::PF_WPN_VARIANT] = 1.0f;
		row[world::PF_WPN_SOURCE_VARIANT] = 2.0f;
		row[world::PF_WPN_PHASE_PARKED] = 1.0f;
		const anim::WeaponChannel w = world::row_weapon_channel(row.data());
		TEST_EXPECT(w.key == walk && w.phase_ticks == 14 && w.prev_key == idle && w.prev_phase_ticks == 3 &&
				w.blend_weight == 0.5f && w.variant == 1 && w.prev_variant == 2 && w.parked);
	}

	// ---- 5. the overlay deltas ----
	{
		const float body[3] = {0.0f, 30.0f, 0.0f};
		float classes[anim::kOverlayClassCount][3] = {};
		for (auto &c : classes) {
			c[0] = 0.0f;
			c[1] = 30.0f;
			c[2] = 0.0f;
		}
		classes[anim::kOverlayHead][1] = 50.0f; // the head 20 degrees further round
		anim::Quat deltas[anim::kOverlayClassCount];
		world::aim_overlay_deltas(body, classes, deltas);
		TEST_EXPECT(quat_near(deltas[anim::kOverlayBody], anim::Quat{}));
		TEST_EXPECT(quat_near(deltas[anim::kOverlaySpine], anim::Quat{}));
		// A pure yaw difference is a turn about the up axis alone, by 20 degrees.
		const anim::Quat head = deltas[anim::kOverlayHead];
		TEST_EXPECT(std::fabs(head.x) < 1e-5f && std::fabs(head.z) < 1e-5f);
		const double angle = 2.0 * std::acos(std::fmin(1.0, std::fabs(static_cast<double>(head.w))));
		TEST_EXPECT(std::fabs(angle - 20.0 * 3.14159265358979 / 180.0) < 1e-4);

		std::vector<float> row = blank_row();
		anim::Quat out[anim::kOverlayClassCount];
		TEST_EXPECT(!world::row_aim_overlay_deltas(row.data(), out));
		row[world::PF_AIM_OVERLAY_VALID] = 1.0f;
		row[world::PF_AIM_BODY_YAW_DEG] = 30.0f;
		for (size_t k = 0; k < anim::kOverlayClassCount; ++k)
			row[world::PF_AIM_ANGLES + static_cast<int>(k) * world::PF_AIM_CLASS_STRIDE + 1] = classes[k][1];
		TEST_EXPECT(world::row_aim_overlay_deltas(row.data(), out));
		for (size_t k = 0; k < anim::kOverlayClassCount; ++k) TEST_EXPECT(quat_near(out[k], deltas[k], 1e-6f));
	}
	return 0;
}
