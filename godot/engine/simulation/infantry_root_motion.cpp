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
				t.bottom.resize(n);
				t.top.resize(n);
				t.trigger.resize(n);
				for (size_t i = 0; i < n; ++i) {
					t.fwd[i] = bf.events[i].velocity[2];
					t.lat[i] = bf.events[i].velocity[0];
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
	if (adm_id < 0 || adm_id >= static_cast<int>(sets_.size())) {
		return false;
	}
	const auto &tracks = sets_[adm_id].tracks;
	return tracks.find(state_id) != tracks.end();
}

bool InfantryRootMotion::advance(int adm_id, int state_id, int32_t &phase_ticks,
                                 opennova::world::RootMotionFrame &out) {
	if (adm_id < 0 || adm_id >= static_cast<int>(sets_.size())) {
		return false;
	}
	const auto &tracks = sets_[adm_id].tracks;
	auto it = tracks.find(state_id);
	if (it == tracks.end()) {
		return false;
	}
	const Track &t = it->second;
	const int32_t prev = phase_ticks;
	++phase_ticks;

	// Half-frame playhead positions, wrapped (loop) or clamped just below the end
	// [orig: AnimChannel_AdvancePlayback parks t at 0.99999 on one-shot clip end].
	const int32_t total_half = t.frame_count * 2;
	auto pos_of = [&](int32_t ph) -> int32_t {
		if (t.loop) {
			ph %= total_half;
			return ph < 0 ? ph + total_half : ph;
		}
		return ph >= total_half ? total_half - 1 : (ph < 0 ? 0 : ph);
	};
	auto sample = [&](const std::vector<float> &chan, int32_t ph) -> float {
		const int32_t p = pos_of(ph);
		const size_t frame = static_cast<size_t>(p >> 1);
		return (p & 1) ? (chan[frame] + chan[frame + 1]) * 0.5f : chan[frame];
	};

	out = opennova::world::RootMotionFrame{};
	out.dx = static_cast<int32_t>(sample(t.fwd, phase_ticks) * 32768.0f);
	out.dy = static_cast<int32_t>(sample(t.lat, phase_ticks) * 32768.0f);
	// Vertical = delta of the scaled capsule-bottom track between this tick's sample and
	// the previous one — derived from the caller-owned phase, so a clip switch (phase
	// reset) starts with no cross-clip delta [orig: anim_slot[19] prev, reset semantics].
	out.dz = static_cast<int32_t>(sample(t.bottom, phase_ticks) * 65536.0f) -
	         static_cast<int32_t>(sample(t.bottom, prev) * 65536.0f);
	// Absolute capsule extents for THIS frame — the on-foot ground settle floors pos[2] to
	// ground + capsule_bottom (origin->feet) [orig: AnimMap_UpdateEntity @0x40b82f
	// out_transform[3]=bottom*65536, out_transform[4]=top*65536+0x2000; consumed by
	// tick_infantry's ground clamp — docs/world/world-wac-ai-re.md D-INF-6].
	out.capsule_bottom = static_cast<int32_t>(sample(t.bottom, phase_ticks) * 65536.0f);
	out.capsule_top = static_cast<int32_t>(sample(t.top, phase_ticks) * 65536.0f) + 0x2000;
	// Event bits from the lower keyframe of the current position [orig: trigger unlerped;
	// consumers sample on alternating ticks, so the per-frame repeat is faithful].
	out.events = t.trigger[static_cast<size_t>(pos_of(phase_ticks) >> 1)];
	return true;
}

int32_t InfantryRootMotion::clip_length_ticks(int adm_id, int state_id) const {
	// Half-frame ticks, the advance() playhead convention (frame_count * 2). -1 when the
	// state has no track — the weapon channel's deferred promotion then never length-fires.
	if (adm_id < 0 || adm_id >= static_cast<int>(sets_.size())) {
		return -1;
	}
	const auto &tracks = sets_[adm_id].tracks;
	auto it = tracks.find(state_id);
	if (it == tracks.end()) {
		return -1;
	}
	return it->second.frame_count * 2;
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
