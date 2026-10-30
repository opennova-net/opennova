// The mission's ambient emitters: see ambient_emitters.h.

#include <runtime/audio/ambient_emitters.h>

#include <runtime/audio/sound_options.h>
#include <base/io/fixed.h>

#include <runtime/environment/environment_state.h>
#include <runtime/mission/placement_traits.h>

#include <algorithm>
#include <utility>

namespace opennova::audio {

namespace {

constexpr double kMinutesPerHour = 60.0;

// A mission-frame point in the presentation frame the listener and the mix
// share, as every positional presentation drain maps it.
void presentation_point(float x, float y, float z, float out[3]) {
	const mission::PlacementVec3 placed =
			mission::bms_to_presentation_position(mission::PlacementVec3{ x, y, z });
	out[0] = placed.x;
	out[1] = placed.y;
	out[2] = placed.z;
}

// One layer row as the table registers it: member 0's radii, volume and ceiling
// [orig: SoundEmitter_UpdateAndMixTop8 @ 0x528649].
AmbientMixer::LayerDesc layer_desc(int32_t candidate_id, const AmbientLayerRow &row) {
	AmbientMixer::LayerDesc desc;
	desc.candidate_id = candidate_id;
	desc.falloff_u = row.layer.falloff_u;
	desc.min_u = row.layer.min_u;
	desc.member_vol = row.layer.volume;
	desc.clamp_vol = row.layer.clamp;
	return desc;
}

} // namespace

AmbientEmitters::AmbientEmitters(int channel_budget) : pool_(channel_budget) {}

void AmbientEmitters::clear() {
	live_ = false;
	mixer_.clear();
	pool_.reset();
	markers_.clear();
	candidates_.clear();
	validated_.clear();
	lanes_.clear();
	queued_.clear();
	free_ids_.clear();
	retired_ids_.clear();
	next_candidate_id_ = 1;
	world_driven_ = false;
	world_tick_offset_ = 0;
	stats_ = AmbientEmitterStats{};
}

void AmbientEmitters::add_envs_markers(const std::vector<EnvsMarker> &rows) {
	for (const EnvsMarker &row : rows) {
		++stats_.markers_total;
		// Authored slot names -> playable slots: only sets the loaded bank chain
		// carries participate; an empty slot stays SILENT in its region (the
		// original has no such gate: a missing set is simply silent).
		std::array<std::string, 4> playable;
		for (size_t i = 0; i < playable.size(); ++i) {
			const std::string &name = row.slot_sets[i];
			if (!name.empty() && sets_.has_set && sets_.has_set(name)) playable[i] = name;
		}
		// Each set once, in slot order (envs_distinct_sets).
		const std::vector<std::string> distinct = envs_distinct_sets(playable);
		if (distinct.empty()) continue;
		// Keep layer candidates as data. The original registers only the current
		// region's set and has eight physical channels; it does not materialize a
		// player for every marker/time-of-day layer [orig: @ 0x4a81da].
		Marker marker;
		int candidate_count = 0;
		for (const std::string &set_name : distinct) {
			std::vector<AmbientLayerRow> described;
			if (sets_.layers) described = sets_.layers(set_name);
			if (described.empty()) continue;
			MarkerSet set;
			set.name = set_name;
			for (AmbientLayerRow &layer : described)
				set.layers.push_back(MarkerLayer{ next_candidate_id_++, std::move(layer) });
			candidate_count += static_cast<int>(set.layers.size());
			marker.sets.push_back(std::move(set));
		}
		if (marker.sets.empty()) continue;
		presentation_point(row.x, row.y, row.z, marker.pos);
		marker.source_bms_id = row.bms_id;
		marker.slot_sets = playable;
		marker.stagger_slot = envs_stagger_slot(markers_.size());
		markers_.push_back(std::move(marker));
		++stats_.markers_resolved;
		stats_.candidates += candidate_count;
	}
}

void AmbientEmitters::restart() {
	mixer_.clear();
	mixer_.set_time_of_day_hours(tod_hours_);
	candidates_.clear();
	free_ids_.clear();
	retired_ids_.clear();
	queued_.clear();
	lanes_.clear();
	world_driven_ = false;
	world_tick_offset_ = 0;
	for (const Marker &marker : markers_) {
		std::vector<std::string> set_names;
		std::vector<std::vector<AmbientMixer::LayerDesc>> sets;
		for (const MarkerSet &set : marker.sets) {
			std::vector<AmbientMixer::LayerDesc> layers;
			layers.reserve(set.layers.size());
			for (const MarkerLayer &layer : set.layers) {
				candidates_[layer.candidate_id] =
						AmbientCandidateRef{ layer.row.wave, AmbientSource::Marker };
				layers.push_back(layer_desc(layer.candidate_id, layer.row));
			}
			if (layers.empty()) continue;
			set_names.push_back(set.name);
			sets.push_back(std::move(layers));
		}
		// Each region's slot keyed into the sets registered (envs_slot_keys).
		const std::array<int32_t, 4> keys = envs_slot_keys(marker.slot_sets, set_names);
		mixer_.add_marker(marker.pos, marker.source_bms_id, marker.stagger_slot, 0, keys.data(),
				std::move(sets));
	}
	live_ = true;
}

// HHMM -> hours through the environment's conversion (hhmm_to_minute_of_day).
void AmbientEmitters::set_time_of_day_hhmm(double hhmm) {
	tod_hours_ = static_cast<float>(
			env::EnvironmentState::hhmm_to_minute_of_day(hhmm) / kMinutesPerHour);
	mixer_.set_time_of_day_hours(tod_hours_);
}

void AmbientEmitters::queue_emitters(const std::vector<world::SoundEmitterEvent> &events) {
	if (!live_) return;
	queued_.insert(queued_.end(), events.begin(), events.end());
}

void AmbientEmitters::advance_to_logic_tick(int64_t logic_tick) {
	if (!live_) return;
	if (!world_driven_) {
		world_driven_ = true;
		world_tick_offset_ = std::max<int64_t>(0, mixer_.clock_tick() - logic_tick);
	}
	const int64_t target_tick = logic_tick + world_tick_offset_;
	// Apply each intent at its producer tick before advancing to the end of a
	// catch-up batch. A lane last refreshed early in the batch therefore spends
	// the elapsed ticks from its real lifetime.
	flush_emitters(target_tick);
	mixer_.advance_to_tick(target_tick);
}

const AmbientChannelPlan &AmbientEmitters::frame(const float listener[3], float delta_seconds,
		bool can_grow, const Resolve &resolve, OcclusionFn occl, void *occl_ctx) {
	if (!live_) {
		plan_ = AmbientChannelPlan{};
		plan_.channel_count = pool_.channel_count();
		return plan_;
	}
	// A late same-tick delivery lands at the current clock before this frame's
	// elapsed time; an owner the world never ticks free-runs the wall clock.
	flush_emitters(mixer_.clock_tick());
	if (!world_driven_ && delta_seconds > 0.0f) mixer_.advance_seconds(delta_seconds);
	// Ranked loudest-first (candidate-id tie-break) by the table. The rows a live
	// candidate describes go to the pool, which decides which candidate rides
	// which channel (incumbents keep theirs, dropouts release, entrants restart);
	// the device's stream resolve is asked once per entrant.
	const std::vector<AmbientCandidate> &rows = mixer_.mix(listener, occl, occl_ctx);
	describable_.clear();
	for (const AmbientCandidate &row : rows)
		if (candidates_.count(row.candidate_id) != 0) describable_.push_back(row);
	pool_.plan(describable_, [&](int32_t candidate_id) {
		if (validated_.insert(candidate_id).second)
			stats_.candidates_validated = static_cast<int>(validated_.size());
		const auto found = candidates_.find(candidate_id);
		const bool resolved =
				found != candidates_.end() && resolve && resolve(candidate_id, found->second);
		if (!resolved) ++stats_.decode_failures;
		return resolved;
	}, can_grow, plan_);
	prune_lanes();
	release_retired_ids();
	return plan_;
}

void AmbientEmitters::play(Mixer &mixer, const SoundListener &listener, int32_t sound_fx_option,
		const WaveOf &wave, const Centered &centered) {
	// A row's channel bytes: its place back in the mission frame's 16.16 units (presentation
	// (x, y, z) is mission (x, -z, y)), its pan, its volume through the option and its play
	// word through the doppler.
	const auto channel_of = [&](const AmbientCandidate &row, uint8_t &pan, uint8_t &volume,
			uint32_t &play) {
		const int32_t at[3] = { int32_t(row.pos[0] * io::kFp16One), int32_t(-row.pos[2] * io::kFp16One),
			int32_t(row.pos[1] * io::kFp16One) };
		const int32_t dx = int32_t(uint32_t(at[0]) - uint32_t(listener.position()[0]));
		const int32_t dy = int32_t(uint32_t(at[1]) - uint32_t(listener.position()[1]));
		const bool rides = row.source_spawn_id != 0 && centered && centered(row.source_spawn_id);
		pan = positional_pan(dx, dy, listener.yaw(), rides);
		volume = static_cast<uint8_t>(scaled_volume(row.vol, sound_fx_option));
		play = doppler_play_word(emitter_play_word(row.pitch_q16), listener.position(),
				listener.velocity(), at, row.velocity, 0);
	};
	for (const int channel : plan_.released) {
		if (channel < 0 || channel >= kAmbientMixChannels) continue;
		// [orig: AudioChannel_ResetByHandle @0x528A70]
		mixer.reset(handles_[size_t(channel)]);
		handles_[size_t(channel)] = 0;
	}
	for (const AmbientChannelPlan::Bind &update : plan_.updates) {
		if (update.channel < 0 || update.channel >= kAmbientMixChannels) continue;
		uint8_t pan = 0, volume = 0;
		uint32_t play = 0;
		channel_of(update.row, pan, volume, play);
		uint32_t &handle = handles_[size_t(update.channel)];
		// [orig: AudioChannel_SetAndPlay @0x528A5C; a handle it no longer plays reset @0x528A6C]
		if (mixer.set_and_play(handle, pan, volume, play) == 0) {
			mixer.reset(handle);
			handle = 0;
		}
	}
	for (const AmbientChannelPlan::Bind &bind : plan_.binds) {
		if (bind.channel < 0 || bind.channel >= kAmbientMixChannels) continue;
		const AmbientCandidateRef *ref = candidate(bind.row.candidate_id);
		const MixWaveRef buffer = ref != nullptr && wave ? wave(bind.row.candidate_id, *ref) : nullptr;
		uint32_t &handle = handles_[size_t(bind.channel)];
		handle = 0;
		if (!buffer) continue;
		uint8_t pan = 0, volume = 0;
		uint32_t play = 0;
		channel_of(bind.row, pan, volume, play);
		// [orig: AudioChannel_OpenSlotChecked(desc_ptr + 4, ...) @0x528ADB]
		handle = mixer.open_looping(kFirstAmbientChannel + bind.channel,
				voice_params(buffer, pan, volume, play, 0));
	}
}

const AmbientCandidateRef *AmbientEmitters::candidate(int32_t candidate_id) const {
	const auto found = candidates_.find(candidate_id);
	return found != candidates_.end() ? &found->second : nullptr;
}

// Resolve the queued name-keyed registrations into layer rows at their producer
// ticks. Each layer keeps a stable candidate id while its keyed lane is alive.
void AmbientEmitters::flush_emitters(int64_t final_tick) {
	if (!live_ || queued_.empty()) return;
	std::vector<world::SoundEmitterEvent> &pending = flushing_;
	pending.clear();
	pending.swap(queued_);
	std::stable_sort(pending.begin(), pending.end(),
			[](const world::SoundEmitterEvent &a, const world::SoundEmitterEvent &b) {
				return a.emitted_tick < b.emitted_tick;
			});
	for (const world::SoundEmitterEvent &event : pending) {
		int64_t event_tick = static_cast<int64_t>(event.emitted_tick);
		if (world_driven_) event_tick += world_tick_offset_;
		event_tick = std::min(event_tick, final_tick);
		if (event_tick > mixer_.clock_tick()) mixer_.advance_to_tick(event_tick);
		const uint64_t source = event.source_spawn_id;
		const int32_t lane_id = static_cast<int32_t>(event.lane);
		float pos[3];
		presentation_point(event.pos.x, event.pos.y, event.pos.z, pos);
		const int64_t source_bms_id = event.source_bms_id;
		// The retail 16-bit slot word the table keeps (a sign-extended kick byte is
		// a long loop, not a dead one); the lane's expiry counts the same word.
		const int32_t lifetime = emitter_lifetime_word(event.lifetime_ticks);
		const int32_t pitch_q16 = event.pitch_q16;
		const int32_t volume_q8_8 = static_cast<int32_t>(event.volume_q8_8);
		if (event.source_only) {
			mixer_.update_emitter_source(source, pos, source_bms_id);
			continue;
		}
		if (pitch_q16 == 0 || volume_q8_8 == 0) {
			mixer_.register_emitter(source, lane_id, pos, source_bms_id, lifetime, pitch_q16,
					volume_q8_8, {});
			const int known = find_lane(source, lane_id);
			if (known >= 0) forget_lane(known);
			continue;
		}
		if (event.set_name.empty()) continue;
		std::vector<AmbientLayerRow> described;
		if (sets_.layers) described = sets_.layers(event.set_name);
		if (described.empty()) continue;
		int index = find_lane(source, lane_id);
		if (index < 0 || lanes_[static_cast<size_t>(index)].set_name != event.set_name ||
				lanes_[static_cast<size_t>(index)].candidate_ids.size() != described.size()) {
			// A set change retires the lane's ids: its old layers clear first.
			if (index >= 0) {
				mixer_.register_emitter(source, lane_id, pos, source_bms_id, lifetime, 0, 0, {});
				forget_lane(index);
			}
			Lane fresh;
			fresh.source_spawn_id = source;
			fresh.lane = lane_id;
			fresh.set_name = event.set_name;
			for (size_t i = 0; i < described.size(); ++i)
				fresh.candidate_ids.push_back(allocate_candidate_id());
			lanes_.push_back(std::move(fresh));
			index = static_cast<int>(lanes_.size()) - 1;
		}
		Lane &lane = lanes_[static_cast<size_t>(index)];
		lane.expires_tick = mixer_.clock_tick() + lifetime;
		std::vector<AmbientMixer::LayerDesc> layers;
		layers.reserve(described.size());
		for (size_t i = 0; i < described.size(); ++i) {
			const int32_t candidate_id = lane.candidate_ids[i];
			candidates_[candidate_id] =
					AmbientCandidateRef{ described[i].wave, AmbientSource::Emitter };
			layers.push_back(layer_desc(candidate_id, described[i]));
		}
		mixer_.register_emitter(source, lane_id, pos, source_bms_id, lifetime, pitch_q16,
				volume_q8_8, std::move(layers), event.velocity);
	}
	pending.clear();
}

int AmbientEmitters::find_lane(uint64_t source_spawn_id, int32_t lane) const {
	for (size_t i = 0; i < lanes_.size(); ++i)
		if (lanes_[i].source_spawn_id == source_spawn_id && lanes_[i].lane == lane)
			return static_cast<int>(i);
	return -1;
}

// A lane's ids leave the live set: no row describes them, nothing remembers them
// failed or validated, and they wait in the retired list for their channels.
void AmbientEmitters::forget_lane(int index) {
	const Lane &lane = lanes_[static_cast<size_t>(index)];
	for (const int32_t id : lane.candidate_ids) {
		candidates_.erase(id);
		pool_.forget_candidate(id);
		validated_.erase(id);
		retired_ids_.push_back(id);
	}
	lanes_.erase(lanes_.begin() + index);
}

int32_t AmbientEmitters::allocate_candidate_id() {
	if (!free_ids_.empty()) {
		const int32_t id = free_ids_.back();
		free_ids_.pop_back();
		return id;
	}
	return next_candidate_id_++;
}

// A lane its producer stopped refreshing past its keep-alive is forgotten.
void AmbientEmitters::prune_lanes() {
	const int64_t now_tick = mixer_.clock_tick();
	for (size_t i = 0; i < lanes_.size();) {
		if (now_tick > lanes_[i].expires_tick) {
			forget_lane(static_cast<int>(i));
		} else {
			++i;
		}
	}
}

// Retired ids are recycled only after no channel still carries them, keeping a
// channel's row identity exact for long missions.
void AmbientEmitters::release_retired_ids() {
	if (retired_ids_.empty()) return;
	std::vector<int32_t> still_retired;
	for (const int32_t id : retired_ids_) {
		if (pool_.channel_of(id) >= 0) {
			still_retired.push_back(id);
		} else {
			free_ids_.push_back(id);
		}
	}
	retired_ids_.swap(still_retired);
}

} // namespace opennova::audio
