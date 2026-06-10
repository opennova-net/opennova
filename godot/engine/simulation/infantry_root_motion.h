#pragma once

#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/variant/string.hpp>

#include <cstdint>
#include <unordered_map>
#include <vector>

#include <world/infantry.h>

namespace godot {

class NovaResourceRoot;

// The host's IRootMotionSource: a model's .adm clip set reduced to per-state root-motion
// tracks. Resolution chain: anim state id -> kInfantryAnimNames[id] -> "anim_<name>" .adm
// entry -> .bad (NovaResourceRoot), keeping only the .bad "events" records — the engine's
// per-frame root data [orig: AnimMap_UpdateEntity @0x40b5f0 out-transform; semantics
// pinned in world/infantry.h and grilled in tests/anim/root_motion_test.cpp]:
//   forward/tick = lerp(vel[2]) * 32768, lateral = lerp(vel[0]) * 32768,
//   vertical     = delta(lerp(capsule_bottom) * 65536), events = trigger.
// The playhead is a half-frame counter (one sim tick = half a clip frame — the 2:1
// tick:frame cadence the *32768 scale bakes in), wrapping on looped clips and clamping
// just below the end otherwise [orig: AnimChannel_AdvancePlayback @0x40b140].
class InfantryRootMotion : public opennova::world::IRootMotionSource {
public:
	// Build the state -> track map from a model's .adm (e.g. "E_STAND.adm"). Returns the
	// number of states with a usable track; 0 = nothing loaded (AI soldiers then hold
	// their state and stand — motion comes from clips, as in the original engine).
	int load(const Ref<NovaResourceRoot> &p_resource_root, const String &p_adm_name);
	void clear();

	bool has_clip(int state_id) const override;
	bool advance(int state_id, int32_t &phase_ticks,
	             opennova::world::RootMotionFrame &out) override;

	const String &adm_name() const { return adm_name_; }
	int clip_count() const { return static_cast<int>(tracks_.size()); }

private:
	// Fence-post root records (frame_count + 1 entries per channel; see bad.h BadEvent).
	struct Track {
		std::vector<float> fwd;      // velocity[2]: forward distance per clip frame
		std::vector<float> lat;      // velocity[0]: lateral
		std::vector<float> bottom;   // capsule_bottom: vertical root reference
		std::vector<uint32_t> trigger;
		int32_t frame_count = 0;
		bool loop = false;
	};

	std::unordered_map<int, Track> tracks_;
	String adm_name_;
};

} // namespace godot
