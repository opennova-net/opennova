#include "infantry_root_motion.h"

#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <adm/adm.h>
#include <bad/bad.h>

#include "resource_index/nova_resource_root.h"

namespace godot {

void InfantryRootMotion::clear() {
	sets_.clear();
	by_name_.clear();
}

int InfantryRootMotion::parse_adm(const Ref<NovaResourceRoot> &p_resource_root,
                                  const String &p_adm_name, ClipSet &out) {
	out.tracks.clear();
	out.adm_name = String();
	if (p_resource_root.is_null()) {
		return 0;
	}
	const PackedByteArray adm_bytes = p_resource_root->read_file(p_adm_name);
	if (adm_bytes.is_empty()) {
		return 0;
	}
	AdmFile adm;
	if (adm_parse_buffer(reinterpret_cast<const char *>(adm_bytes.ptr()),
	                     static_cast<size_t>(adm_bytes.size()), &adm) != 0) {
		return 0;
	}
	out.adm_name = p_adm_name;

	// .bad basename resolution, as in NovaSkeletalAnim::load_from_resource_root.
	auto resolve_bad = [](const String &value) -> String {
		String bad_name = value;
		if (!bad_name.to_lower().ends_with(".bad")) {
			bad_name += ".bad";
		}
		return bad_name;
	};

	// Several states usually share one clip (walk_* dirs, idles): cache parsed tracks by
	// resolved .bad name so each file is read + parsed once per .adm.
	std::unordered_map<std::string, Track> by_file;
	auto track_for = [&](const String &value) -> const Track * {
		const String bad_name = resolve_bad(value);
		const std::string cache_key{bad_name.to_lower().utf8().get_data()};
		auto cached = by_file.find(cache_key);
		if (cached != by_file.end()) {
			return cached->second.frame_count > 0 ? &cached->second : nullptr;
		}
		Track t;
		const PackedByteArray bytes = p_resource_root->read_file(bad_name);
		BadFile bf;
		if (!bytes.is_empty() &&
		    bad_parse_buffer(bytes.ptr(), static_cast<size_t>(bytes.size()), &bf) == 0) {
			// Fence-post: frame_count+1 root records [orig: 0x40b230 lerps rec[i]..rec[i+1]].
			if (bf.frame_count > 0 && bf.num_events == static_cast<size_t>(bf.frame_count) + 1) {
				t.frame_count = static_cast<int32_t>(bf.frame_count);
				t.loop = (bf.flags & 0x1u) != 0;
				const size_t n = bf.num_events;
				t.fwd.resize(n);
				t.lat.resize(n);
				t.vert.resize(n);
				t.bottom.resize(n);
				t.top.resize(n);
				t.trigger.resize(n);
				for (size_t i = 0; i < n; ++i) {
					t.fwd[i] = bf.events[i].velocity[2];
					t.lat[i] = bf.events[i].velocity[0];
					t.vert[i] = bf.events[i].velocity[1];
					t.bottom[i] = bf.events[i].bottom;
					t.top[i] = bf.events[i].top;
					t.trigger[i] = static_cast<uint32_t>(bf.events[i].trigger);
				}
			}
			bad_free(&bf);
		}
		auto [it, inserted] = by_file.emplace(cache_key, std::move(t));
		return it->second.frame_count > 0 ? &it->second : nullptr;
	};

	// adm key lookup, case-insensitive (keys are authored "anim_<name>", same names as
	// the state table off_8135F0).
	std::unordered_map<std::string, String> values;
	for (size_t i = 0; i < adm.count; ++i) {
		const String key = String(adm.entries[i].key).to_lower();
		const String value = String(adm.entries[i].value);
		if (!value.is_empty()) {
			values.emplace(std::string{key.utf8().get_data()}, value);
		}
	}
	adm_free(&adm);

	using opennova::world::kInfantryAnimNames;
	using opennova::world::kInfantryAnimStateCount;
	for (int state = 0; state < kInfantryAnimStateCount; ++state) {
		const std::string key = std::string("anim_") + kInfantryAnimNames[state];
		auto it = values.find(key);
		if (it == values.end()) {
			continue;
		}
		if (const Track *t = track_for(it->second)) {
			out.tracks.emplace(state, *t);
		}
	}
	return static_cast<int>(out.tracks.size());
}

int InfantryRootMotion::register_adm(const Ref<NovaResourceRoot> &p_resource_root,
                                     const String &p_adm_name) {
	const std::string key{p_adm_name.to_lower().utf8().get_data()};
	auto cached = by_name_.find(key);
	if (cached != by_name_.end()) {
		return cached->second;
	}
	ClipSet set;
	if (parse_adm(p_resource_root, p_adm_name, set) <= 0) {
		return -1; // no usable clips: caller leaves the entity at adm_id 0 / sourceless
	}
	const int adm_id = static_cast<int>(sets_.size());
	sets_.push_back(std::move(set));
	by_name_.emplace(key, adm_id);
	return adm_id;
}

bool InfantryRootMotion::has_clip(int adm_id, int state_id) const {
	return resolve_track(adm_id, state_id) != nullptr;
}

const InfantryRootMotion::Track *InfantryRootMotion::resolve_track(int adm_id,
                                                                   int state_id) const {
	if (adm_id < 0 || adm_id >= static_cast<int>(sets_.size())) {
		return nullptr;
	}
	const auto &tracks = sets_[adm_id].tracks;
	auto it = tracks.find(state_id);
	if (it != tracks.end()) {
		return &it->second;
	}
	// Missing semantic keys are bound to state-0 RESET's channel during retail
	// AnimMap registration; the requested semantic state id itself is retained.
	it = tracks.find(opennova::world::anim_state::kReset);
	return it != tracks.end() ? &it->second : nullptr;
}

int32_t InfantryRootMotion::position_of(const Track &track, int32_t phase_ticks) {
	const int32_t total_half = track.frame_count * 2;
	if (track.loop) {
		phase_ticks %= total_half;
		return phase_ticks < 0 ? phase_ticks + total_half : phase_ticks;
	}
	return phase_ticks >= total_half ? total_half - 1 : (phase_ticks < 0 ? 0 : phase_ticks);
}

float InfantryRootMotion::sample(const Track &track, const std::vector<float> &channel,
                                 int32_t phase_ticks) {
	const int32_t position = position_of(track, phase_ticks);
	const size_t frame = static_cast<size_t>(position >> 1);
	return (position & 1) ? (channel[frame] + channel[frame + 1]) * 0.5f
	                      : channel[frame];
}

uint32_t InfantryRootMotion::sample_trigger(const Track &track, int32_t phase_ticks) {
	return track.trigger[static_cast<size_t>(position_of(track, phase_ticks) >> 1)];
}

bool InfantryRootMotion::advance(int adm_id, int state_id, int32_t &phase_ticks,
                                 opennova::world::RootMotionFrame &out) {
	const Track *track = resolve_track(adm_id, state_id);
	if (track == nullptr) {
		return false;
	}
	++phase_ticks;

	// Half-frame playhead positions, wrapped (loop) or clamped just below the end
	// [orig: AnimChannel_AdvancePlayback parks t at 0.99999 on one-shot clip end].
	out = opennova::world::RootMotionFrame{};
	out.dx = static_cast<int32_t>(sample(*track, track->fwd, phase_ticks) * 32768.0f);
	out.dy = static_cast<int32_t>(sample(*track, track->lat, phase_ticks) * 32768.0f);
	// Raw vertical fallback. The motor overwrites this with blended-bottom history
	// whenever anim_slot[19] is live; reset-state families clear that history first.
	out.dz = static_cast<int32_t>(sample(*track, track->vert, phase_ticks) * 32768.0f);
	// Absolute capsule extents for THIS frame — the on-foot ground settle floors pos[2] to
	// ground + capsule_bottom (origin->feet) [orig: AnimMap_UpdateEntity @0x40b82f
	// out_transform[3]=bottom*65536, out_transform[4]=top*65536+0x2000; consumed by
	// tick_infantry's ground clamp — docs/world/world-wac-ai-re.md D-INF-6].
	out.capsule_bottom =
			static_cast<int32_t>(sample(*track, track->bottom, phase_ticks) * 65536.0f);
	out.capsule_top =
			static_cast<int32_t>(sample(*track, track->top, phase_ticks) * 65536.0f) + 0x2000;
	// Event bits from the lower keyframe of the current position [orig: trigger unlerped;
	// consumers sample on alternating ticks, so the per-frame repeat is faithful].
	out.events = sample_trigger(*track, phase_ticks);
	return true;
}

bool InfantryRootMotion::advance_blended(
		int adm_id,
		int primary_state, int32_t &primary_phase_ticks,
		int target_state, int32_t &target_phase_ticks,
		float target_weight,
		opennova::world::RootMotionFrame &out) {
	const Track *primary = resolve_track(adm_id, primary_state);
	const Track *target = resolve_track(adm_id, target_state);
	if (primary == nullptr || target == nullptr) {
		return opennova::world::IRootMotionSource::advance_blended(
				adm_id, primary_state, primary_phase_ticks,
				target_state, target_phase_ticks, target_weight, out);
	}

	++primary_phase_ticks;
	++target_phase_ticks;
	const float primary_weight = 1.0f - target_weight;
	auto blend = [&](const std::vector<float> &primary_channel,
	                 const std::vector<float> &target_channel) {
		const float primary_value = sample(*primary, primary_channel, primary_phase_ticks);
		const float target_value = sample(*target, target_channel, target_phase_ticks);
		// The original x87 path keeps both products and the sum live, then spills one
		// float32 result. Products of float32 inputs are exact in double, so this
		// reproduces that single-rounding boundary on modern SSE builds.
		return static_cast<float>(
				static_cast<double>(primary_value) * static_cast<double>(primary_weight) +
				static_cast<double>(target_value) * static_cast<double>(target_weight));
	};

	out = opennova::world::RootMotionFrame{};
	out.dx = static_cast<int32_t>(blend(primary->fwd, target->fwd) * 32768.0f);
	out.dy = static_cast<int32_t>(blend(primary->lat, target->lat) * 32768.0f);
	out.dz = static_cast<int32_t>(blend(primary->vert, target->vert) * 32768.0f);
	out.capsule_bottom =
			static_cast<int32_t>(blend(primary->bottom, target->bottom) * 65536.0f);
	out.capsule_top =
			static_cast<int32_t>(blend(primary->top, target->top) * 65536.0f) + 0x2000;
	out.events = sample_trigger(*target, target_phase_ticks);
	return true;
}

int32_t InfantryRootMotion::clip_length_ticks(int adm_id, int state_id) const {
	// Half-frame ticks, the advance() playhead convention (frame_count * 2). -1 when the
	// state has no track — the weapon channel's deferred promotion then never length-fires.
	const Track *track = resolve_track(adm_id, state_id);
	return track != nullptr ? track->frame_count * 2 : -1;
}

int InfantryRootMotion::clip_count(int adm_id) const {
	if (adm_id < 0 || adm_id >= static_cast<int>(sets_.size())) {
		return 0;
	}
	return static_cast<int>(sets_[adm_id].tracks.size());
}

const String &InfantryRootMotion::adm_name(int adm_id) const {
	static const String empty;
	if (adm_id < 0 || adm_id >= static_cast<int>(sets_.size())) {
		return empty;
	}
	return sets_[adm_id].adm_name;
}

} // namespace godot
