// Portable ambient sound-marker emitter system: the placed-marker eval/registration
// at the witnessed staggered tick cadence, the transient emitter slot table, and the
// per-frame loudest-N candidate ranking — pushed down out of the Godot layer
// (godot/engine/world/nova_mission_audio.gd) so the engine core stays C++ and the
// per-frame cost is the live-slot mix, not an every-marker eval
// (docs/audio/lwf-dbf-sound-re.md §driver cadence, D-SND-16).
//
// Witnessed split (grilled 2026-07-10/11, cadence 2026-07-28):
// - Registration runs in the 62.5 Hz update lane. Placed markers are pool-2 statics,
//   walked starting at tick & 7 with stride 8, so each marker's eval runs every 8th
//   tick, cohort-staggered [orig: Entity_UpdateAllEntities @ 0x4c225a-0x4c228c].
//   The eval picks the time-of-day region set, suppresses the crossfade when the
//   adjacent region resolves the SAME set, and registers the set's layers with a
//   tick-unit keep-alive lifetime [orig: Entity_UpdateEnvSoundEmitter @ 0x4a8080].
// - The mix runs once per RENDER frame, but on a tick clock: lifetimes decrement by
//   elapsed logic ticks, expired slots self-clear, live slots range-cull, occlusion-
//   inflate, run the two-radius member-0 volume, and the loudest eight win channels
//   [orig: SoundEmitter_UpdateAndMixTop8 @ 0x5284a0, called from the Game Loop mode
//   render callback GameLoop_RenderFrame @ 0x521341].
//
// The embedder keeps everything Godot: LWF data access, stream decode, the eight
// persistent AudioStreamPlayer3D channels (D-SND-6), and bus/pan/doppler mapping
// (D-SND-8). Positions are world-space floats (1 unit = 1 game unit); distances enter
// the witnessed curve as Q16.16, exactly like the GDScript form this replaces.
#ifndef OPENNOVA_AUDIO_AMBIENT_MIXER_H
#define OPENNOVA_AUDIO_AMBIENT_MIXER_H

#include <cstdint>
#include <vector>

namespace opennova::audio {

// The witnessed distance volume curve [orig: SoundBank_CalcDistanceVolPan @ 0x75ca20]:
// at or beyond `radius` the voice is HARD SILENT; inside it the volume runs
// vol * (1 - d/r)^2 with the master-fade steady-state (vol * 255) >> 8 fold, ceilinged
// by `clamp_vol`. Distances are Q16.16; both callers pass a consistent scale.
int32_t calc_distance_volume(int64_t dist_q16, int64_t radius_q16, int32_t vol255,
                             int32_t clamp_vol);

// Layer volume for the looping ambient-emitter path: emitter byte pre-scales member
// volume and clamp [orig: @ 0x5286b9], the arms subtract in Q16 FIRST and truncate to
// whole units at the curve call (HIWORD(d - min) = floor(d - m)); with a min_distance
// the falloff REBASES to run min..falloff and inside it the volume RISES as (d/min)^2;
// a bare falloff runs 0..falloff; both radii zero is silent as a looping emitter
// [orig: SoundEmitter_UpdateAndMixTop8 arms @ 0x528667..0x5286df; zero-radius skip
// @ 0x528704].
int32_t emitter_layer_volume(int64_t dist_q16, int32_t falloff_u, int32_t min_u,
                             int32_t vol_byte, int32_t member_vol, int32_t clamp_vol);

// The emitter volume byte for a region crossfade blend: the rounded register word's
// high byte, with 0xFFFF as the full-blend sentinel — net (0xFFFF * blend_q16 +
// 0x8000) >> 24 [orig: Entity_UpdateEnvSoundEmitter @ 0x4a81c6; the mix reads slot
// byte +25 @ 0x52865e].
int32_t crossfade_volume_byte(float blend);

// Time-of-day region + crossfade [orig: Entity_CalcTimeOfDayRegion @ 0x408110]:
// hours cut at 4/10/17/21 into morning/day/evening/night, intervals OPEN at the low
// cut (the exact cut instant reads night at full blend @ 0x408175/0x4081b0); each
// region fades over ~5 game-minutes (margin 5460 Q16 hours @ 0x408203) at its edges;
// night wraps 21h->4h and only its 21h edge blends (@ 0x40820f); a zero blend
// distance reads full volume (@ 0x408251). `adjacent` is the neighbouring region at
// the active edge.
struct TimeOfDayRegion {
    int32_t region;   // 0 morning / 1 day / 2 evening / 3 night
    int32_t adjacent; // neighbour at the active crossfade edge
    float blend;      // 0..1, 1 = full volume
};
TimeOfDayRegion time_of_day_region(float hours);

// Occlusion seam: inflate `dist_q16` between listener and source through the embedder's
// two-ray LOS [orig: Sound_ApplyOcclusionDistance @ 0x529970, called from the mix
// @ 0x528659]. Rays run only for slots already audible at the raw distance — the
// shipped D-SND-7 form (inflation only ever reduces volume, so a raw-silent slot
// stays silent either way).
using OcclusionFn = int64_t (*)(void *ctx, const float listener[3],
                                const float source[3], int64_t dist_q16,
                                int64_t source_id);

// One ranked audible layer, in mix order (volume desc, candidate_id tie-break — the
// reimpl's deterministic-membership form of the original's slot-order-stable top-8
// sort @ 0x5287ab). The embedder binds the first N non-failed entries to its persistent
// channels and resolves streams by candidate_id.
struct AmbientCandidate {
    int32_t candidate_id;
    int32_t vol; // 0..255 through the witnessed curve (crossfade + member + clamp)
    int32_t pitch_q16; // registration pitch; placed markers use unity (0x10000)
    float pos[3];
};

class AmbientMixer {
public:
    // Slot capacity of the transient emitter table [orig: 767 x 48-byte slots
    // @ 0x24D66A8]. Registrations beyond it are dropped, like the original's fixed
    // table.
    static constexpr int kSlotCapacity = 767;
    // Marker keep-alive default, in 62.5 Hz ticks: outlives the 8-tick pool-2
    // revisit by two ticks [orig: the *(def+92) default arm @ 0x4a80bc].
    static constexpr int kMarkerLifetimeTicks = 10;
    // The pool-2 walk stride: each marker is visited every 8th tick
    // [orig: Entity_UpdateAllEntities @ 0x4c225a tick & 7 / stride 8].
    static constexpr int kStaggerPeriod = 8;

    // One layer of one registered set (member 0 — the emitter path never runs the
    // member-selection machine [orig: @ 0x528649]).
    struct LayerDesc {
        int32_t candidate_id = 0;
        int32_t falloff_u = 0; // whole units (playlist u16 @4)
        int32_t min_u = 0;     // whole units (playlist u16 @6)
        int32_t member_vol = 255;
        int32_t clamp_vol = 255;
    };

    // Reset the marker registry AND the live slot table (mission teardown/reload).
    void clear();

    // Register one placed marker. `slot_keys` maps region 0..3 to an index into
    // `sets` (-1 = null slot: silent in that region, no registration [orig:
    // @ 0x4a81a3]); equal keys model the resolved-set-pointer identity the same-set
    // crossfade suppress compares (null == null included) [orig: @ 0x4a819d].
    // `stagger_slot` seeds both the walk cohort (slot & 7) and the per-marker clock
    // stagger nibble ((slot & 0xF) << 11 Q16 hours) [orig: @ 0x408158].
    // Returns the marker index.
    int add_marker(const float pos[3], int64_t source_bms_id, int32_t stagger_slot,
                   int32_t lifetime_ticks, const int32_t slot_keys[4],
                   std::vector<std::vector<LayerDesc>> sets);

    // Register one entity-attached emitter into the SAME transient slot table used
    // by placed markers. Slots are keyed by (source_spawn_id, lane, layer), so a
    // per-tick registration refreshes the existing lifetime and presentation values
    // instead of allocating a second voice. Pitch or volume zero clears every layer
    // for exactly that source+lane [orig: SoundEmitter_RegisterSetLayers @ 0x528340;
    // SoundEmitter_ClearByEntityAndSlot @ 0x527a50]. `volume_q8_8` is the original
    // registration word; its high byte feeds the member-volume curve.
    // A source pose belongs to the entity rather than to one lane. Updating it
    // moves every still-live lane without extending any lane's keep-alive.
    void update_emitter_source(uint64_t source_spawn_id, const float pos[3],
                               int64_t source_bms_id);
    void register_emitter(uint64_t source_spawn_id, int32_t lane,
                          const float pos[3], int64_t source_bms_id,
                          int32_t lifetime_ticks, int32_t pitch_q16,
                          int32_t volume_q8_8, std::vector<LayerDesc> layers);

    int marker_count() const { return static_cast<int>(markers_.size()); }

    // Embedder clock pump (HHMM already converted to hours by the embedder).
    void set_time_of_day_hours(float hours) { tod_hours_ = hours; }

    // Advance the eval clock to logic tick `tick`, running the staggered cohort walk
    // for each elapsed tick — every marker whose (stagger_slot & 7) == (tick & 7)
    // re-evaluates and re-registers [orig: the pool-2 walk @ 0x4c225a]. Gaps longer
    // than one stagger period collapse to the last kStaggerPeriod ticks (each cohort
    // is visited at most once per call, same net registrations as walking every
    // tick). Also drives the mix's lifetime clock.
    void advance_to_tick(int64_t tick);

    // Autonomous clock for embedders with no logic tick source (editor idle): bank wall
    // seconds and derive 62.5 Hz ticks internally.
    void advance_seconds(float dt);

    // The per-frame mix: decrement lifetimes by elapsed clock ticks (expired slots
    // self-clear), range-cull live slots axis-wise then euclidean against the lazily
    // cached falloff range [orig: @ 0x52856a/@ 0x5285da], inflate the distance
    // through `occl` (once per marker per mix, only when raw-audible), compute the
    // witnessed layer volume, and return every audible candidate sorted loudest
    // first. The embedder cuts to its channel budget after filtering decode failures —
    // the original's bad-wave slots drop out pre-sort the same way [orig: @ 0x52870e].
    const std::vector<AmbientCandidate> &mix(const float listener[3],
                                             OcclusionFn occl = nullptr,
                                             void *occl_ctx = nullptr);

    int64_t clock_tick() const { return clock_tick_; }
    int live_slot_count() const;

private:
    struct Marker {
        float pos[3] = {0.0f, 0.0f, 0.0f};
        int64_t source_bms_id = 0;
        int32_t stagger_slot = 0;
        int32_t lifetime_ticks = kMarkerLifetimeTicks;
        int32_t slot_keys[4] = {-1, -1, -1, -1};
        std::vector<std::vector<LayerDesc>> sets;
        // Per-mix occlusion cache (one LOS per marker per mix, shared across its
        // layers — the shipped D-SND-7 lazy form).
        int64_t occl_stamp = -1;
        int64_t occl_dist_q16 = -1;
    };

    // One live entity-attached registration, shared by all of its layer slots.
    // Records remain index-stable while live because Slot stores this vector index;
    // cleared records are reused only after all of their slots are released.
    struct DynamicEmitter {
        bool active = false;
        uint64_t source_spawn_id = 0;
        int32_t lane = 0;
        float pos[3] = {0.0f, 0.0f, 0.0f};
        int64_t source_bms_id = 0;
        std::vector<LayerDesc> layers;
        int64_t occl_stamp = -1;
        int64_t occl_dist_q16 = -1;
    };

    // One transient emitter slot [orig: 48-byte slot @ 0x24D66A8, keyed
    // (entity, slot-type byte = region/lane, layer index)].
    struct Slot {
        bool used = false;
        bool dynamic = false;
        int32_t owner = 0;  // marker index or DynamicEmitter index
        int32_t lane = 0;   // marker region or entity-attached lane
        int32_t layer = 0;
        int32_t lifetime = 0; // remaining ticks; 0 at mix entry clears the slot
        int32_t vol_byte = 0; // emitter volume byte (the crossfade blend)
        int32_t pitch_q16 = 0x10000;
        int64_t range_q16 = -1; // lazily cached falloff << 16 [orig: @ 0x52856a]
        int64_t refreshed_tick = -1; // registrations at the current tick do not age retroactively
    };

    void eval_marker_tick(int32_t marker_index);
    void register_set(int32_t marker_index, int32_t region, int32_t vol_byte);
    void clear_marker_region(int32_t marker_index, int32_t region);
    Slot *find_or_alloc_marker_slot(int32_t marker_index, int32_t region,
                                    int32_t layer);
    int32_t find_or_alloc_dynamic_emitter(uint64_t source_spawn_id, int32_t lane);
    Slot *find_or_alloc_dynamic_slot(int32_t emitter_index, int32_t layer);
    void clear_dynamic_emitter(uint64_t source_spawn_id, int32_t lane);

    std::vector<Marker> markers_;
    std::vector<DynamicEmitter> dynamic_emitters_;
    Slot slots_[kSlotCapacity];
    std::vector<AmbientCandidate> mix_out_;
    float tod_hours_ = 12.0f;
    int64_t clock_tick_ = 0;
    int64_t last_eval_tick_ = -1;
    int64_t last_mix_tick_ = -1;
    int64_t mix_counter_ = 0; // occlusion-cache stamp
    float auto_accum_ = 0.0f;
};

} // namespace opennova::audio

#endif // OPENNOVA_AUDIO_AMBIENT_MIXER_H
