#pragma once

#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/variant/string.hpp>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include <world/infantry.h>

namespace godot {

class NovaResourceRoot;

// The embedder's IRootMotionSource: a registry of per-model .adm clip sets, each reduced to
// per-state root-motion tracks. Each in-mission soldier grounds + locomotes off its OWN
// model's clip (its adm_id), not one shared set, matching the original which evaluates the
// entity's own anim map per frame [orig: AnimMap_UpdateEntity @0x40b5f0 per entity;
// docs/world/world-wac-ai-re.md D-INF-6].
//
// Resolution chain per set: anim state id -> kInfantryAnimNames[id] -> "anim_<name>" .adm
// entry -> .bad (NovaResourceRoot), keeping only the .bad "events" records — the engine's
// per-frame root data [orig: AnimMap_UpdateEntity @0x40b5f0 out-transform; semantics
// pinned in world/infantry.h and grilled in tests/anim/root_motion_test.cpp]:
//   forward/tick = lerp(vel[2]) * 32768, lateral = lerp(vel[0]) * 32768,
//   vertical     = lerp(vel[1]) * 32768 (normally replaced by the motor's
//                  blended capsule-bottom history delta), events = trigger,
//   capsule_bottom = lerp(bottom) * 65536 (the absolute ground-settle floor).
// The playhead is a half-frame counter (one sim tick = half a clip frame — the 2:1
// tick:frame cadence the *32768 scale bakes in), wrapping on looped clips and clamping
// just below the end otherwise [orig: AnimChannel_AdvancePlayback @0x40b140].
class InfantryRootMotion : public opennova::world::IRootMotionSource {
public:
	// Register a model's .adm (e.g. "E_STAND.adm", "US01.adm") and return its adm_id (an
	// index into the registry). Re-registering the same name returns the cached id, so a
	// given .adm is parsed once however many soldiers use it. Returns -1 if the .adm has no
	// usable clip (its soldiers then hold their state and stand — motion comes from clips,
	// as in the original). The first successful registration is id 0 (the default set).
	int register_adm(const Ref<NovaResourceRoot> &p_resource_root, const String &p_adm_name);
	void clear();

	bool has_clip(int adm_id, int state_id) const override;
	bool advance(int adm_id, int state_id, int32_t &phase_ticks,
	             opennova::world::RootMotionFrame &out) override;
	bool advance_blended(int adm_id,
	                     int primary_state, int32_t &primary_phase_ticks,
	                     int target_state, int32_t &target_phase_ticks,
	                     float target_weight,
	                     opennova::world::RootMotionFrame &out) override;
	int32_t clip_length_ticks(int adm_id, int state_id) const override;

	bool empty() const { return sets_.empty(); }
	int set_count() const { return static_cast<int>(sets_.size()); }
	// States with a usable track in a given set (default set 0 unless specified).
	int clip_count(int adm_id = 0) const;
	const String &adm_name(int adm_id = 0) const;

private:
	// Fence-post root records (frame_count + 1 entries per channel; see bad.h BadEvent).
	struct Track {
		std::vector<float> fwd;      // velocity[2]: forward distance per clip frame
		std::vector<float> lat;      // velocity[0]: lateral
		std::vector<float> vert;     // velocity[1]: vertical fallback lane
		std::vector<float> bottom;   // capsule_bottom: origin->feet (the ground-settle floor)
		std::vector<float> top;      // capsule_top: origin->head (capsule extent)
		std::vector<uint32_t> trigger;
		int32_t frame_count = 0;
		bool loop = false;
	};

	// One model's .adm reduced to its per-state root tracks.
	struct ClipSet {
		std::unordered_map<int, Track> tracks;
		String adm_name;
	};

	// Parse p_adm_name into `out`; returns the number of states with a usable track.
	static int parse_adm(const Ref<NovaResourceRoot> &p_resource_root, const String &p_adm_name,
	                     ClipSet &out);
	const Track *resolve_track(int adm_id, int state_id) const;
	static int32_t position_of(const Track &track, int32_t phase_ticks);
	static float sample(const Track &track, const std::vector<float> &channel,
	                    int32_t phase_ticks);
	static uint32_t sample_trigger(const Track &track, int32_t phase_ticks);

	std::vector<ClipSet> sets_;
	std::unordered_map<std::string, int> by_name_; // lowercased .adm name -> adm_id
};

} // namespace godot
