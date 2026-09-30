#include <runtime/audio/oneshot_play.h>

#include <base/io/fixed.h>
#include <base/io/strutil.h>

#include <cmath>

namespace opennova::audio {

int OneshotChannelPool::acquire(uint64_t wave, uint8_t volume, uint32_t sound_id) {
	if (!wave || !volume) return -1;
	int best = -1;
	int minimum = 12 * volume;
	for (size_t i = kFirstOneshotChannel; i < channels_.size(); ++i) {
		const Channel &channel = channels_[i];
		const int score = sound_id && channel.wave == wave && channel.sound_id == sound_id
				? 0 : 6 * channel.volume;
		if (score < minimum) { minimum = score; best = static_cast<int>(i); }
	}
	if (best >= 0) channels_[static_cast<size_t>(best)] = {wave, sound_id, volume};
	return best;
}

void OneshotChannelPool::release(size_t channel) {
	if (channel < channels_.size()) channels_[channel] = {};
}

void SoundSetIndex::add_bank(int32_t bank_index, const lwf::File &bank) {
	for (size_t si = 0; si < bank.multis.size(); ++si) {
		const std::string key = strutil::to_lower(bank.multis[si].name);
		if (key.empty()) {
			continue;
		}
		if (index_.find(key) != index_.end()) {
			continue;
		}
		index_.emplace(key, SetLocation{ bank_index, static_cast<int32_t>(si),
				static_cast<int32_t>(bank.multis[si].target_id) });
		names_.push_back(key);
	}
}

SetLocation SoundSetIndex::find(const std::string &name) const {
	const auto it = index_.find(strutil::to_lower(name));
	return it == index_.end() ? SetLocation{} : it->second;
}

void SoundSetIndex::clear() {
	index_.clear();
	names_.clear();
}

int64_t oneshot_cull_range_q16(const lwf::Multi &set) {
	const int64_t range = static_cast<int64_t>(set.target_id);
	return (range > 0 ? range : 0) << 16;
}

std::vector<uint32_t> set_layers(const lwf::File &bank, const lwf::Multi &set) {
	std::vector<uint32_t> out;
	out.reserve(set.playlist_indices.size());
	for (uint32_t pidx : set.playlist_indices) {
		if (pidx < bank.playlists.size()) {
			out.push_back(pidx);
		}
	}
	return out;
}

std::vector<uint32_t> layer_members(const lwf::File &bank, const lwf::Playlist &layer) {
	std::vector<uint32_t> out;
	out.reserve(layer.sndparm_indices.size());
	for (uint32_t sidx : layer.sndparm_indices) {
		if (sidx < bank.sndparms.size()) {
			out.push_back(sidx);
		}
	}
	return out;
}

int32_t pick_layer_member(const lwf::File &bank, const SetLocation &loc,
		int32_t layer_index, uint32_t playlist_index, SoundSelector &selector) {
	if (playlist_index >= bank.playlists.size()) {
		return -1;
	}
	const lwf::Playlist &layer = bank.playlists[playlist_index];
	const std::vector<uint32_t> members = layer_members(bank, layer);
	if (members.empty()) {
		return -1;
	}
	const int mode = static_cast<int>(selection_mode_for_flags(layer.flags));
	const int idx = selector.select(SoundSelector::make_key(loc.bank, loc.set, layer_index),
			static_cast<int>(members.size()), mode);
	if (idx < 0 || idx >= static_cast<int>(members.size())) {
		return -1;
	}
	return idx;
}

namespace {
// Admission precedes member selection and both pitch draws, including for
// interface/distance-flat plays. [orig: SoundBank_PlayTriggerEntries @0x75CD54]
bool layer_matches_listener_view(const lwf::Multi &set, const lwf::Playlist &layer,
        uint8_t listener_view_flags) {
    return (listener_view_flags & layer.flags & 6u) != 0 &&
            (!(set.set_flags & 1u) || ((listener_view_flags ^ layer.flags) & 0x20u) == 0);
}
}

std::optional<RadioVoice> select_radio_voice(const lwf::File &bank,
        const SetLocation &loc, SoundSelector &selector, uint8_t listener_view_flags) {
    if (!loc.valid() || loc.set < 0 || static_cast<size_t>(loc.set) >= bank.multis.size())
        return std::nullopt;
	const auto &set = bank.multis[static_cast<size_t>(loc.set)];
	const auto layers = set_layers(bank, set);
	for (size_t li = 0; li < layers.size(); ++li) {
		if (!layer_matches_listener_view(set, bank.playlists[layers[li]], listener_view_flags)) continue;
		const int ordinal = pick_layer_member(bank, loc,
				static_cast<int32_t>(li), layers[li], selector);
		if (ordinal < 0) continue;
		const auto members = layer_members(bank, bank.playlists[layers[li]]);
		const auto &member = bank.sndparms[members[static_cast<size_t>(ordinal)]];
		if (member.single_index >= bank.singles.size()) continue;
		RadioVoice result;
		result.filename = bank.singles[member.single_index].path;
		const auto dot = result.filename.find_last_of('.');
		if (dot != std::string::npos) result.filename.resize(dot);
		result.filename += ".wav";
		// Unlike the trigger-fire selector, this function never writes the
		// caller's pitch or consumes pitch jitter. [orig: @0x75C130]
		result.volume = static_cast<int32_t>(member.volume);
		result.max_distance = static_cast<int32_t>(bank.playlists[layers[li]].falloff_radius << 16);
		return result;
	}
	return std::nullopt;
}

std::optional<RadioVoice> select_entity_voice(const lwf::File &bank,
        const SetLocation &loc, SoundSelector &selector, uint8_t listener_view_flags) {
    if (!loc.valid() || loc.set < 0 || static_cast<size_t>(loc.set) >= bank.multis.size())
        return std::nullopt;
    const auto &set = bank.multis[static_cast<size_t>(loc.set)];
    const auto layers = set_layers(bank, set);
    for (size_t li = 0; li < layers.size(); ++li) {
        if (!layer_matches_listener_view(set, bank.playlists[layers[li]], listener_view_flags)) continue;
        const int ordinal = pick_layer_member(bank, loc,
                static_cast<int32_t>(li), layers[li], selector);
        if (ordinal < 0) continue;
        const auto members = layer_members(bank, bank.playlists[layers[li]]);
        const auto &member = bank.sndparms[members[static_cast<size_t>(ordinal)]];
        if (member.single_index >= bank.singles.size()) continue;
        RadioVoice result;
        result.filename = bank.singles[member.single_index].path;
        // [orig: @0x75c089..0x75c109]
        result.pitch_q16 = selector.compose_pitch(set.pitch_base, set.pitch_random_range,
                member.pitch_scaled, member.random_pitch_scaled);
        result.volume = static_cast<int32_t>(member.volume);
        result.max_distance = static_cast<int32_t>(bank.playlists[layers[li]].falloff_radius << 16);
        return result;
    }
    return std::nullopt;
}

int64_t listener_distance_q16(const float world_pos[3], const float listener_pos[3]) {
	const float dx = world_pos[0] - listener_pos[0];
	const float dy = world_pos[1] - listener_pos[1];
	const float dz = world_pos[2] - listener_pos[2];
	const float length = std::sqrt(dx * dx + dy * dy + dz * dz);
	return static_cast<int64_t>(static_cast<double>(length) * io::kFp16OneD);
}

OneshotPlan plan_oneshot_3d(const lwf::File &bank, const SetLocation &loc,
		const float world_pos[3], const float listener_pos[3], bool has_listener,
		int64_t source_bms_id, uint32_t sound_id, OcclusionFn occl, void *occl_ctx,
		SoundSelector &selector, uint8_t listener_view_flags) {
	OneshotPlan plan;
	if (!loc.valid() || loc.set < 0 || static_cast<size_t>(loc.set) >= bank.multis.size()) {
		return plan;
	}
	const lwf::Multi &set = bank.multis[static_cast<size_t>(loc.set)];
	int64_t dist_q16 = 0;
	if (has_listener) {
		dist_q16 = listener_distance_q16(world_pos, listener_pos);
		// The set-level 3D cull: beyond the set's range (Multi dword 18,
		// in-memory set+72) the one-shot does not fire at all -- axis checks,
		// then euclidean, all <= range<<16 (equality passes); the euclidean
		// test subsumes the axis ones [orig: Sound_Play3DPositional
		// @ 0x527cd1-0x527d83].
		const int64_t cull_q16 = oneshot_cull_range_q16(set);
		if (dist_q16 > cull_q16) {
			return plan;
		}
		// Occlusion inflates the fire distance between the euclidean cull and
		// the recheck, and the INFLATED distance feeds the once-at-fire volume
		// snapshot [orig: the Sound_ApplyOcclusionDistance call @ 0x527d95 and
		// the <= range recheck @ 0x527da1].
		if (occl != nullptr) {
			dist_q16 = occl(occl_ctx, listener_pos, world_pos, dist_q16, source_bms_id);
			if (dist_q16 > cull_q16) {
				return plan;
			}
		}
	}
    plan = plan_oneshot_at_distance(bank, loc, dist_q16, selector, listener_view_flags, has_listener);
    plan.sound_id = sound_id;
    return plan;
}

OneshotPlan plan_oneshot_at_distance(const lwf::File &bank, const SetLocation &loc,
        int64_t dist_q16, SoundSelector &selector, uint8_t listener_view_flags, bool attenuate) {
    OneshotPlan plan;
    if (!loc.valid() || loc.set < 0 || static_cast<size_t>(loc.set) >= bank.multis.size()) {
        return plan;
    }
    const lwf::Multi &set = bank.multis[static_cast<size_t>(loc.set)];
    plan.in_range = true;
    plan.dist_q16 = dist_q16;
	const std::vector<uint32_t> layers = set_layers(bank, set);
	for (size_t li = 0; li < layers.size(); ++li) {
		const lwf::Playlist &layer = bank.playlists[layers[li]];
		if (!layer_matches_listener_view(set, layer, listener_view_flags)) continue;
		const int32_t member = pick_layer_member(bank, loc, static_cast<int32_t>(li),
				layers[li], selector);
		if (member < 0) {
			continue;
		}
		const std::vector<uint32_t> members = layer_members(bank, layer);
		const uint32_t sidx = members[static_cast<size_t>(member)];
		const lwf::Sndparm &sndparm = bank.sndparms[sidx];
		const uint32_t pitch = selector.compose_pitch(set.pitch_base, set.pitch_random_range,
				sndparm.pitch_scaled, sndparm.random_pitch_scaled);
		int32_t vol255 = static_cast<int32_t>(sndparm.volume);
		if (attenuate) {
			vol255 = oneshot_layer_volume(dist_q16,
					static_cast<int64_t>(layer.min_distance) << 16,
					static_cast<int64_t>(layer.falloff_radius) << 16,
					static_cast<int32_t>(sndparm.volume),
					static_cast<int32_t>(sndparm.clamp_volume));
			if (vol255 <= 0) {
				continue;
			}
		}
		OneshotVoice voice;
		voice.layer = static_cast<int32_t>(li);
		voice.playlist = layers[li];
		voice.sndparm = sidx;
		voice.vol255 = vol255;
		voice.pitch_q16 = pitch;
		plan.voices.push_back(voice);
	}
	return plan;
}

} // namespace opennova::audio
