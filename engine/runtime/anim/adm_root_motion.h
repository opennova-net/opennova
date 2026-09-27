#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include <runtime/world/infantry.h>
#include <runtime/anim/clip_timeline.h>

namespace opennova {
namespace assets { class AssetStore; }
}

namespace opennova::anim {

// The engine's IRootMotionSource (ADR 0028; moved from the shell binding's
// InfantryRootMotion): a registry of per-model .adm clip sets, each reduced to
// per-state root-motion tracks. Each in-mission soldier grounds + locomotes off
// its OWN model's clip (its adm_id), not one shared set, matching the original
// which evaluates the entity's own anim map per frame
// [orig: AnimMap_UpdateEntity @0x40b5f0 per entity;
// docs/world/world-wac-ai-re.md D-INF-6].
//
// Resolution chain per set: anim state id -> kInfantryAnimNames[id] ->
// "anim_<name>" .adm entry -> .bad (through the mounted resource index),
// keeping only the .bad "events" records — the engine's per-frame root data
// [orig: AnimMap_UpdateEntity @0x40b5f0 out-transform; semantics pinned in
// world/infantry.h and grilled in tests/anim/root_motion_test.cpp]:
//   forward/tick = lerp(vel[2]) * 32768, lateral = lerp(vel[0]) * 32768,
//   vertical     = lerp(vel[1]) * 32768 (normally replaced by the motor's
//                  blended capsule-bottom history delta), events = trigger,
//   capsule_bottom = lerp(bottom) * 65536 (the absolute ground-settle floor).
// The playhead counts simulation ticks. The normalized channel advances by
// fps/(62*frame_count); the root-motion *32768 scale is independent of that
// clock. Loops wrap; stopped one-shots retain extents but zero XYZ/trigger; a
// loop holding a deferred state parks its armed boundary tick at the clip end
// (advance_armed). [orig: AnimChannel_AdvancePlayback @0x40b140].
class AdmRootMotion : public opennova::world::IRootMotionSource {
public:
	// Register a model's .adm (e.g. "E_STAND.adm", "US01.adm") through `assets`
	// and return its adm_id (an index into the registry). Re-registering the
	// same name returns the cached id, so a given .adm is parsed once however
	// many soldiers use it. Returns -1 if the .adm has no usable clip (its
	// soldiers then hold their state and stand — motion comes from clips, as
	// in the original). The first successful registration is id 0; choosing
	// a default fallback is the caller's policy.
	int register_adm(const opennova::assets::AssetStore *assets, const std::string &adm_name);
	void clear();

	bool has_clip(int adm_id, int state_id) const override;
	bool advance(int adm_id, int state_id, int32_t &phase_ticks,
	             opennova::world::RootMotionFrame &out) override;
	// The per-state variant ring: every quoted token on the .adm row is its own
	// clip with its own root track [orig: AnimMap_ParseConfigLine @0x40cb60].
	int variant_count(int adm_id, int state_id) const override;
	bool advance_variant(int adm_id, int state_id, int variant, int32_t &phase_ticks,
	                     opennova::world::RootMotionFrame &out) override;
	// advance_variant with the armed-wrap park: when the incremented playhead
	// lands on `armed_boundary` (the tick clip_boundary_after reported for a
	// looping clip holding a deferred state) the sample is the parked clip end
	// (rec[frames-1]..rec[frames] at 0.99999, trigger[frames-1], XYZ live), not
	// the wrapped start; -1 = unarmed. AnimMap checks the latched end flag BEFORE
	// the advance and the sample, so the promoted retarget lands on the next
	// tick [orig: AnimChannel_AdvancePlayback @0x40B193..0x40B1B1;
	// AnimMap_UpdateEntity pending check @0x40B77B..0x40B7E1, advance @0x40B7FE,
	// sample @0x40B82A].
	bool advance_armed(int adm_id, int state_id, int variant, int32_t &phase_ticks,
	                   int32_t armed_boundary, opennova::world::RootMotionFrame &out) override;
	bool advance_blended(int adm_id,
	                     int primary_state, int primary_variant,
	                     int32_t &primary_phase_ticks,
	                     int target_state, int target_variant,
	                     int32_t &target_phase_ticks,
	                     float target_weight,
	                     opennova::world::RootMotionFrame &out) override;
	// The served ring entry's own length — the promotion clock for ringed rows
	// [orig: the ring rotate @0x40b740-0x40b749 re-inits the channel from the
	// served entry; frame_count read @0x40b25d runs on that clip].
	int32_t clip_length_ticks(int adm_id, int state_id, int variant) const override;
	bool clip_loops(int adm_id, int state_id) const override;
	int32_t clip_boundary_after(int adm_id, int state_id, int32_t phase_ticks,
	                            int variant = 0) const override;
	// The served entry's own clock and loop bit decide the wrap.
	bool clip_wraps_at(int adm_id, int state_id, int variant,
	                   int32_t phase_ticks) const override;

	// THE CROSSED-FRAME TRIGGER SCAN — the authored event words a body crossed
	// between two playhead positions, in order, one entry per authored clip
	// FRAME [.bad v1 event stride 24, trigger i32 @+20 — sampled unlerped from
	// the FLOOR keyframe, AnimChannel_InterpolateKeyframe @0x40b32f].
	//
	// Retail's own consume is a per-TICK floor-frame sample (out[5] ->
	// g_AnimEventTriggerBits @0x40b8a3) parity-gated to every other tick
	// (org1 odd @0x4bf144 / org2 even @0x4b76e6), which at the authored-30fps
	// vs 62 Hz ratio fires each entered frame once — that per-tick form is
	// the authority path (infantry_anim_sound_pass). This scan serves the
	// WIRE lane, where a consumed row jumps the playhead across several
	// authored frames at once: emitting each entered frame's word in order
	// reproduces what the per-tick consume would have fired. `from_phase` is
	// EXCLUSIVE and `to_phase` INCLUSIVE (the frame just entered fires), both
	// in the same IDA half-frame ticks `advance` uses. A fresh clip start
	// passes from_phase = -1 so frame 0 fires; a mid-clip attach passes the
	// attach phase so nothing back-fires. Writes at most `max_out` words and
	// returns how many were written.
	int scan_triggers(int adm_id, int state_id, int32_t from_phase,
	                  int32_t to_phase, uint32_t *out, int max_out,
	                  int variant) const;

	// The frame's capsule bottom (out[3] = bottom * 65536) at one playhead
	// position — the dip that puts a footstep at FOOT level rather than the
	// body origin [orig: the AnimMap out[3] cell @0x4b77d3].
	int32_t capsule_bottom_at(int adm_id, int state_id, int32_t phase_ticks,
	                          int variant) const;

	bool empty() const { return sets_.empty(); }
	int set_count() const { return static_cast<int>(sets_.size()); }
	// States with a usable track in a given set (first registered set unless specified).
	int clip_count(int adm_id = 0) const;
	const std::string &adm_name(int adm_id = 0) const;

private:
	// Fence-post root records (frame_count + 1 entries per channel; see bad.h BadEvent).
	struct Track {
		std::vector<float> fwd;      // velocity[2]: forward distance per clip frame
		std::vector<float> lat;      // velocity[0]: lateral
		std::vector<float> vert;     // velocity[1]: vertical fallback lane
		std::vector<float> bottom;   // capsule_bottom: the origin's (hips') height above the ground (the settle floor)
		std::vector<float> top;      // capsule_top: the head's height above the ground (top - bottom is origin->head)
		std::vector<uint32_t> trigger;
		int32_t frame_count = 0;
		anim::ClipTimeline clock;
		bool loop = false;
	};

	// One model's .adm reduced to its per-state root tracks. Each state owns the
	// RING of tracks its .adm row authored, in file order (index = variant).
	struct ClipSet {
		std::unordered_map<int, std::vector<Track>> tracks;
		std::string adm_name;
	};

	// Parse adm_name into `out`; returns the number of states with a usable track.
	static int parse_adm(const opennova::assets::AssetStore *assets,
	                     const std::string &adm_name, ClipSet &out);
	// Variant wraps modulo the ring size, so a stale cursor from a shorter row on
	// another rig still resolves; missing states bind RESET's ring.
	const Track *resolve_track(int adm_id, int state_id, int variant = 0) const;
	static double position_of(const Track &track, int32_t phase_ticks,
	                          int32_t armed_boundary = -1);
	static float sample(const Track &track, const std::vector<float> &channel,
	                    int32_t phase_ticks, int32_t armed_boundary = -1);
	static uint32_t sample_trigger(const Track &track, int32_t phase_ticks,
	                               int32_t armed_boundary = -1);

	std::vector<ClipSet> sets_;
	std::unordered_map<std::string, int> by_name_; // lowercased .adm name -> adm_id
};

} // namespace opennova::anim
