#include <runtime/anim/adm_root_motion.h>

#include <utility>
#include <algorithm>

#include <formats/adm/adm.h>
#include <formats/bad/bad.h>
#include <base/io/strutil.h>
#include <runtime/assets/asset_store.h>

using namespace opennova::adm;
using namespace opennova::bad;

namespace opennova::anim {

void AdmRootMotion::clear() {
	sets_.clear();
	by_name_.clear();
}

int AdmRootMotion::parse_adm(const opennova::assets::AssetStore *assets,
                             const std::string &adm_name, ClipSet &out) {
	out.tracks.clear();
	out.adm_name.clear();
	if (assets == nullptr) {
		return 0;
	}
	const auto map = assets->animation_map(adm_name);
	if (!map) return 0;
	const AdmFile &adm = *map;
	out.adm_name = adm_name;

	// .bad basename resolution, as in SkeletalAnim::load_from_resource_root.
	auto resolve_bad = [](const std::string &value) -> std::string {
		if (!strutil::ends_with_icase(value, ".bad")) {
			return value + ".bad";
		}
		return value;
	};

	// Several states usually share one clip (walk_* dirs, idles): cache parsed tracks by
	// resolved .bad name so each file is read + parsed once per .adm.
	std::unordered_map<std::string, Track> by_file;
	auto track_for = [&](const std::string &value) -> const Track * {
		const std::string bad_name = resolve_bad(value);
		const std::string cache_key = strutil::to_lower(bad_name);
		auto cached = by_file.find(cache_key);
		if (cached != by_file.end()) {
			return cached->second.frame_count > 0 ? &cached->second : nullptr;
		}
		Track t;
		if (const auto file = assets->bone_animation(bad_name)) {
			const BadFile &bf = *file;
			// Fence-post: frame_count+1 root records [orig: 0x40b230 lerps rec[i]..rec[i+1]].
			// No fps gate: the channel delta is fps/62/frames with no fps test, so an
			// fps of 0 is a channel frozen at frame 0 with live capsule extents, not a
			// missing clip [orig: AnimChannel_InitFromData @0x41058E..0x4105BA].
			if (bf.frame_count > 0 && bf.num_events == static_cast<size_t>(bf.frame_count) + 1) {
				t.frame_count = static_cast<int32_t>(bf.frame_count);
				t.loop = (bf.flags & 0x1u) != 0;
				t.clock = anim::ClipTimeline(bf.fps, bf.frame_count, t.loop);
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
		}
		auto [it, inserted] = by_file.emplace(cache_key, std::move(t));
		return it->second.frame_count > 0 ? &it->second : nullptr;
	};

	// adm key lookup, case-insensitive (keys are authored "anim_<name>", same names as
	// the state table off_8135F0). EVERY quoted token on a row registers on that one
	// slot, in authored order — the variant ring [orig: AnimMap_ParseConfigLine
	// @0x40cb60 -> AnimMap_RegisterBoneNode @0x40c2d0 links each into the slot's
	// circular list; duplication is the rotation weighting].
	std::unordered_map<std::string, std::vector<std::string>> values;
	for (size_t i = 0; i < adm.count; ++i) {
		const std::string key = strutil::to_lower(adm.entries[i].key);
		std::vector<std::string> ring;
		for (size_t v = 0; v < adm.entries[i].variant_count; ++v) {
			const std::string value = adm.entries[i].variants[v];
			if (!value.empty()) ring.push_back(value);
		}
		if (!ring.empty()) {
			values.emplace(key, std::move(ring));
		}
	}

	using opennova::world::kInfantryAnimStateCount;
	for (int state = 0; state < kInfantryAnimStateCount; ++state) {
		const std::string key = opennova::world::infantry_anim_key(state);
		if (key.empty()) continue;
		auto it = values.find(key);
		if (it == values.end()) {
			continue;
		}
		std::vector<Track> ring;
		for (const std::string &value : it->second) {
			if (const Track *t = track_for(value)) {
				ring.push_back(*t);
			}
		}
		if (!ring.empty()) {
			out.tracks.emplace(state, std::move(ring));
		}
	}
	return static_cast<int>(out.tracks.size());
}

int AdmRootMotion::register_adm(const opennova::assets::AssetStore *assets,
                                const std::string &adm_name) {
	const std::string key = strutil::to_lower(adm_name);
	auto cached = by_name_.find(key);
	if (cached != by_name_.end()) {
		return cached->second;
	}
	ClipSet set;
	if (parse_adm(assets, adm_name, set) <= 0) {
		return -1; // no usable clips: caller chooses a configured fallback or none
	}
	const int adm_id = static_cast<int>(sets_.size());
	sets_.push_back(std::move(set));
	by_name_.emplace(key, adm_id);
	return adm_id;
}

// The retail availability test is `animMap[id] != animMap[0]`: at
// AnimMap_RegisterEntity @0x40bb60 every slot the .adm left NULL is filled
// with slot 0's node, and AnimMap_RegisterBoneNode @0x40c2d0 stores a fresh
// per-registration wrapper (AnimMap_FindOrLoadBoneFile @0x40c030 allocates one
// even on a name hit), so the pointer compare is exactly "was this slot
// authored", independent of which .bad it names. Playback of an unauthored
// state still binds RESET's ring (resolve_track) -- that is the same fill rule
// seen from the play side. Reporting resolvability here instead made every
// `animMap[x] != animMap[0]` port in the think read true: Eindo_R.adm has no
// anim_stop, retail forces its 147 to 43 (@0x4b9910 post-pass), we kept 147.
bool AdmRootMotion::has_clip(int adm_id, int state_id) const {
	if (adm_id < 0 || adm_id >= static_cast<int>(sets_.size())) return false;
	const auto &tracks = sets_[adm_id].tracks;
	auto it = tracks.find(state_id);
	return it != tracks.end() && !it->second.empty();
}

const AdmRootMotion::Track *AdmRootMotion::resolve_track(int adm_id,
                                                         int state_id,
                                                         int variant) const {
	if (adm_id < 0 || adm_id >= static_cast<int>(sets_.size())) {
		return nullptr;
	}
	const auto &tracks = sets_[adm_id].tracks;
	auto it = tracks.find(state_id);
	if (it == tracks.end()) {
		// Missing semantic keys are bound to state-0 RESET's channel during retail
		// AnimMap registration; the requested semantic state id itself is retained.
		it = tracks.find(opennova::world::anim_state::kReset);
		if (it == tracks.end()) return nullptr;
	}
	const std::vector<Track> &ring = it->second;
	if (ring.empty()) return nullptr;
	const int n = static_cast<int>(ring.size());
	int v = variant % n;
	if (v < 0) v += n;
	return &ring[static_cast<size_t>(v)];
}

int AdmRootMotion::variant_count(int adm_id, int state_id) const {
	if (adm_id < 0 || adm_id >= static_cast<int>(sets_.size())) {
		return 1;
	}
	const auto &tracks = sets_[adm_id].tracks;
	auto it = tracks.find(state_id);
	if (it == tracks.end()) {
		it = tracks.find(opennova::world::anim_state::kReset);
		if (it == tracks.end()) return 1;
	}
	return it->second.empty() ? 1 : static_cast<int>(it->second.size());
}

double AdmRootMotion::position_of(const Track &track, int32_t phase_ticks,
                                  int32_t armed_boundary) {
	const double frame = double(track.clock.normalized_at(phase_ticks, armed_boundary)) *
	                     track.frame_count;
	return std::clamp(frame, 0.0, double(track.frame_count));
}

float AdmRootMotion::sample(const Track &track, const std::vector<float> &channel,
                            int32_t phase_ticks, int32_t armed_boundary) {
	const double position = position_of(track, phase_ticks, armed_boundary);
	const size_t frame = std::min(static_cast<size_t>(position),
	                             static_cast<size_t>(track.frame_count - 1));
	const double fraction = position - frame;
	// x87 interpolates in extended precision and spills one float result.
	return static_cast<float>(channel[frame] * (1.0 - fraction) +
	                          channel[frame + 1] * fraction);
}

uint32_t AdmRootMotion::sample_trigger(const Track &track, int32_t phase_ticks,
                                       int32_t armed_boundary) {
	if (track.clock.stopped_at(phase_ticks)) return 0;
	return track.trigger[static_cast<size_t>(position_of(track, phase_ticks, armed_boundary))];
}

int AdmRootMotion::scan_triggers(int adm_id, int state_id, int32_t from_phase,
                                 int32_t to_phase, uint32_t *out,
                                 int max_out, int variant) const {
	if (out == nullptr || max_out <= 0) return 0;
	const Track *track = resolve_track(adm_id, state_id, variant);
	if (track == nullptr || track->trigger.empty()) return 0;
	int written = 0;
	// Walk the simulation playhead one tick at a time and emit the authored
	// word each time the FRAME index changes (or on the first step, which is
	// the frame the playhead just entered). A non-looping clip clamps just
	// below its end, so the walk terminates there.
	int32_t prev_frame = from_phase < 0
			? -1
			: static_cast<int32_t>(position_of(*track, from_phase));
	for (int32_t phase = from_phase + 1; phase <= to_phase; ++phase) {
		if (track->clock.stopped_at(phase)) break;
		const int32_t frame = static_cast<int32_t>(position_of(*track, phase));
		if (frame == prev_frame) continue;
		prev_frame = frame;
		out[written++] = track->trigger[static_cast<size_t>(frame)];
		if (written >= max_out) break;
	}
	return written;
}

int32_t AdmRootMotion::capsule_bottom_at(int adm_id, int state_id,
                                         int32_t phase_ticks, int variant) const {
	const Track *track = resolve_track(adm_id, state_id, variant);
	if (track == nullptr || track->bottom.empty()) return 0;
	return static_cast<int32_t>(sample(*track, track->bottom, phase_ticks) * 65536.0f);
}

bool AdmRootMotion::advance(int adm_id, int state_id, int32_t &phase_ticks,
                            opennova::world::RootMotionFrame &out) {
	return advance_variant(adm_id, state_id, 0, phase_ticks, out);
}

bool AdmRootMotion::advance_variant(int adm_id, int state_id, int variant,
                                    int32_t &phase_ticks,
                                    opennova::world::RootMotionFrame &out) {
	return advance_armed(adm_id, state_id, variant, phase_ticks, -1, out);
}

bool AdmRootMotion::advance_armed(int adm_id, int state_id, int variant,
                                  int32_t &phase_ticks, int32_t armed_boundary,
                                  opennova::world::RootMotionFrame &out) {
	const Track *track = resolve_track(adm_id, state_id, variant);
	if (track == nullptr) {
		return false;
	}
	++phase_ticks;

	// Retail normalized playhead, wrapped (loop) or parked just below the end
	// [orig: AnimChannel_AdvancePlayback parks t at 0.99999 on one-shot clip end
	// @0x40B17C..0x40B185, and on an ARMED loop wrap @0x40B1A2..0x40B1B1 -- the
	// latter without the 0x10000 stop, so its XYZ and trigger still sample].
	out = opennova::world::RootMotionFrame{};
	if (!track->clock.stopped_at(phase_ticks)) {
		out.dx = static_cast<int32_t>(sample(*track, track->fwd, phase_ticks, armed_boundary) * 32768.0f);
		out.dy = static_cast<int32_t>(sample(*track, track->lat, phase_ticks, armed_boundary) * 32768.0f);
		// Raw vertical fallback. The motor overwrites this with blended-bottom history
		// whenever anim_slot[19] is live; reset-state families clear that history first.
		out.dz = static_cast<int32_t>(sample(*track, track->vert, phase_ticks, armed_boundary) * 32768.0f);
	}
	// Absolute capsule extents for THIS frame — the on-foot ground settle floors pos[2] to
	// ground + capsule_bottom (origin->feet) [orig: AnimMap_UpdateEntity @0x40b82f
	// out_transform[3]=bottom*65536, out_transform[4]=top*65536+0x2000; consumed by
	// tick_infantry's ground clamp — docs/world/world-wac-ai-re.md D-INF-6].
	out.capsule_bottom =
			static_cast<int32_t>(sample(*track, track->bottom, phase_ticks, armed_boundary) * 65536.0f);
	out.capsule_top =
			static_cast<int32_t>(sample(*track, track->top, phase_ticks, armed_boundary) * 65536.0f) + 0x2000;
	// Event bits from the lower keyframe of the current position [orig: trigger unlerped;
	// consumers sample on alternating ticks, so the per-frame repeat is faithful].
	out.events = sample_trigger(*track, phase_ticks, armed_boundary);
	return true;
}

bool AdmRootMotion::advance_blended(
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
	                 const std::vector<float> &target_channel, bool motion = false) {
		const float primary_value = motion && primary->clock.stopped_at(primary_phase_ticks)
				? 0.0f : sample(*primary, primary_channel, primary_phase_ticks);
		const float target_value = motion && target->clock.stopped_at(target_phase_ticks)
				? 0.0f : sample(*target, target_channel, target_phase_ticks);
		// The original x87 path keeps both products and the sum live, then spills one
		// float32 result. Products of float32 inputs are exact in double, so this
		// reproduces that single-rounding boundary on modern SSE builds.
		return static_cast<float>(
				static_cast<double>(primary_value) * static_cast<double>(primary_weight) +
				static_cast<double>(target_value) * static_cast<double>(target_weight));
	};

	out = opennova::world::RootMotionFrame{};
	out.dx = static_cast<int32_t>(blend(primary->fwd, target->fwd, true) * 32768.0f);
	out.dy = static_cast<int32_t>(blend(primary->lat, target->lat, true) * 32768.0f);
	out.dz = static_cast<int32_t>(blend(primary->vert, target->vert, true) * 32768.0f);
	out.capsule_bottom =
			static_cast<int32_t>(blend(primary->bottom, target->bottom) * 65536.0f);
	out.capsule_top =
			static_cast<int32_t>(blend(primary->top, target->top) * 65536.0f) + 0x2000;
	out.events = sample_trigger(*target, target_phase_ticks);
	return true;
}

int32_t AdmRootMotion::clip_length_ticks(int adm_id, int state_id,
                                         int variant) const {
	// Simulation ticks through the first normalized-time end boundary. -1 when the
	// state has no track — the weapon channel's deferred promotion then never length-fires.
	const Track *track = resolve_track(adm_id, state_id, variant);
	return track != nullptr ? track->clock.length_ticks() : -1;
}

int32_t AdmRootMotion::clip_boundary_after(int adm_id, int state_id,
                                             int32_t phase_ticks, int variant) const {
	const Track *track = resolve_track(adm_id, state_id, variant);
	return track ? track->clock.boundary_after(phase_ticks) : -1;
}

bool AdmRootMotion::clip_loops(int adm_id, int state_id) const {
	// The clip data's own loop bit, the same flag word the retail channel wraps
	// on [orig: AnimChannel_InitFromData @0x410577 -> AnimChannel_AdvancePlayback
	// @0x40b16a].
	const Track *track = resolve_track(adm_id, state_id);
	return track != nullptr && track->loop;
}

int AdmRootMotion::clip_count(int adm_id) const {
	if (adm_id < 0 || adm_id >= static_cast<int>(sets_.size())) {
		return 0;
	}
	return static_cast<int>(sets_[adm_id].tracks.size());
}

const std::string &AdmRootMotion::adm_name(int adm_id) const {
	static const std::string empty;
	if (adm_id < 0 || adm_id >= static_cast<int>(sets_.size())) {
		return empty;
	}
	return sets_[adm_id].adm_name;
}

} // namespace opennova::anim
