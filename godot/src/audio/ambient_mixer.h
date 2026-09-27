#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/callable.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_float32_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>

#include <vector>

#include <runtime/audio/ambient_mixer.h>

namespace godot {

class Simulation;

// Thin Godot binding over opennova::audio::AmbientMixer: the placed-marker ambient
// emitter system at the witnessed split cadence — staggered tick&7 eval/registration
// on the logic-tick clock, per-frame live-slot mix (docs/audio/lwf-dbf-sound-re.md
// §driver cadence, D-SND-16). The mission audio (audio/mission_audio)
// feeds resolved marker/layer data at setup, pumps the clocks, and binds the ranked
// result to its persistent AudioStreamPlayer3D channels; occlusion routes to the
// Simulation natively (tests inject a Callable override — the typed seam).
//
// The curve family is exposed as statics (the sound bank's pinned seams
// calc_distance_volume / emitter_layer_volume read the same engine functions);
// the time-of-day region and the crossfade byte are consumed natively (ctest
// ambient_mixer pins them) and are not bound.
class AmbientMixer : public RefCounted {
	GDCLASS(AmbientMixer, RefCounted)

	opennova::audio::AmbientMixer mixer_;
	ObjectID provider_id_; // the occlusion provider; resolved fresh each mix
	// Per-mix transient view of the resolved provider (never cached across mixes,
	// so a freed provider degrades to the unoccluded mix instead of dangling).
	Simulation *sim_ = nullptr; // native fast path
	// Test-injection seam: a Callable(listener, source, dist_q16, source_id) -> int
	// override consulted only when no Simulation provider is set.
	Callable occlusion_override_;

	static int64_t occlusion_trampoline(void *ctx, const float listener[3],
			const float source[3], int64_t dist_q16, int64_t source_id);

protected:
	static void _bind_methods();

public:
	void clear();
	void set_occlusion_provider(const Ref<Simulation> &provider);
	void set_occlusion_override(const Callable &override);

	// One placed marker: `slot_keys` maps region 0..3 to an index into `sets`
	// (-1 = silent region), `sets` is an Array of per-set PackedInt32Array layer
	// blocks, stride 5: [candidate_id, falloff_u, min_u, member_vol, clamp_vol].
	// `stagger_slot` seeds the walk cohort and the clock-stagger nibble;
	// lifetime_ticks <= 0 takes the placed-marker default (10 ticks).
	int add_marker(const Vector3 &pos, int64_t source_bms_id, int stagger_slot,
			int lifetime_ticks, const PackedInt32Array &slot_keys, const Array &sets);

	// One entity-attached registration. `layers` uses the same stride-5 rows as
	// add_marker's set entries. The key is (source_spawn_id, lane, layer);
	// pitch_q16 or volume_q8_8 zero clears that source+lane.
	void register_emitter(int64_t source_spawn_id, int lane, const Vector3 &pos,
			int64_t source_bms_id, int lifetime_ticks, int pitch_q16,
			int volume_q8_8, const PackedInt32Array &layers);
	void update_emitter_source(int64_t source_spawn_id, const Vector3 &pos,
			int64_t source_bms_id);

	void set_time_of_day_hours(float hours);
	void advance_to_tick(int64_t tick);
	void advance_seconds(float dt);

	// Ranked candidates: [candidate_id, vol, pitch_q16, x, y, z], stride 6.
	PackedFloat32Array mix(const Vector3 &listener);
	// C++-side seam (not bound): the same ranked mix as the engine's own rows
	// (audio/mission_audio binds them to its channels without the float
	// transport).
	const std::vector<opennova::audio::AmbientCandidate> &mix_rows(const Vector3 &listener);

	int64_t clock_tick() const;

	// The witnessed curve family (see engine/runtime/audio/ambient_mixer.h for the [orig]
	// map); statics so the GUT pins over the curve keep a bound home.
	static int calc_distance_volume(int64_t dist_q16, int64_t radius_q16, int vol255,
			int clamp_vol);
	static int emitter_layer_volume(int64_t dist_q16, int falloff_u, int min_u,
			int vol_byte, int member_vol, int clamp_vol);
	// A 16.16 word (the mix rows' pitch_q16, the emitter registrations) as a
	// float factor (io/fixed.h kFp16One).
	static float q16_to_float(int q16);
};

} // namespace godot
