#include "audio/ambient_mixer.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace opennova::audio {

namespace {

// Region cuts, hours: [4,10) morning / [10,17) day / [17,21) evening / else night
// [orig: Entity_CalcTimeOfDayRegion @ 0x408110, boundaries 0x40000/0xA0000/0x110000/
// 0x150000 Q16].
constexpr float kRegionCuts[4] = {4.0f, 10.0f, 17.0f, 21.0f};
// Crossfade margin at a region edge: 5460/65536 h (~5 game-minutes) [orig: @ 0x408203].
constexpr float kRegionBlendH = 5460.0f / 65536.0f;

} // namespace

int32_t calc_distance_volume(int64_t dist_q16, int64_t radius_q16, int32_t vol255,
                             int32_t clamp_vol) {
    // [orig: SoundBank_CalcDistanceVolPan @ 0x75ca20] — d >= radius is HARD silent.
    if (radius_q16 <= 0 || dist_q16 >= radius_q16) {
        return 0;
    }
    if (dist_q16 < 0) {
        dist_q16 = 0;
    }
    const int64_t ratio = (dist_q16 << 16) / radius_q16; // d/r as Q0.16
    const int64_t inv = ratio ^ 0xFFFF;                  // (1 - d/r) as Q0.16
    int64_t v = (((static_cast<int64_t>(vol255) * 255) >> 8) * (inv * inv)) >> 32;
    if (v >= clamp_vol) {
        v = clamp_vol;
    }
    return static_cast<int32_t>(v);
}

int32_t emitter_layer_volume(int64_t dist_q16, int32_t falloff_u, int32_t min_u,
                             int32_t vol_byte, int32_t member_vol, int32_t clamp_vol) {
    // Emitter byte pre-scales member volume and clamp [orig: @ 0x5286b9].
    const int32_t vol_in = (vol_byte * member_vol) >> 8;
    const int32_t clamp_in = (vol_byte * clamp_vol) >> 8;
    if (min_u > 0) {
        if (dist_q16 >= static_cast<int64_t>(min_u) << 16) {
            // Rebased falloff, whole-unit args [orig: @ 0x528693-0x5286b9
            // CalcDistanceVolPan(HIWORD(d - min), HIWORD(falloff - min), ...)].
            return calc_distance_volume((dist_q16 - (static_cast<int64_t>(min_u) << 16)) >> 16,
                                        falloff_u - min_u, vol_in, clamp_in);
        }
        // Proximity fade [orig: @ 0x528691 ((min<<16) - d) >> 16, HIWORD(min<<16)].
        return calc_distance_volume(((static_cast<int64_t>(min_u) << 16) - dist_q16) >> 16,
                                    min_u, vol_in, clamp_in);
    }
    if (falloff_u > 0) {
        // Plain falloff [orig: @ 0x5286df HIWORD(d), HIWORD(falloff<<16)].
        return calc_distance_volume(dist_q16 >> 16, falloff_u, vol_in, clamp_in);
    }
    // Both radii zero: silent as a looping emitter [orig: @ 0x528704].
    return 0;
}

int32_t crossfade_volume_byte(float blend) {
    // Rounded register word, 0xFFFF full-blend sentinel; the mix reads the word's
    // HIGH byte [orig: @ 0x4a81c6; slot byte +25 read @ 0x52865e].
    int32_t blend_q16;
    if (blend >= 1.0f) {
        blend_q16 = 0xFFFF;
    } else {
        const float clamped = blend < 0.0f ? 0.0f : blend;
        blend_q16 = static_cast<int32_t>(clamped * 65536.0f);
    }
    return static_cast<int32_t>((0xFFFFLL * blend_q16 + 0x8000) >> 24);
}

TimeOfDayRegion time_of_day_region(float hours) {
    float t = std::fmod(hours, 24.0f);
    if (t < 0.0f) {
        t += 24.0f;
    }
    int32_t region = 3;
    float low = kRegionCuts[3];
    float high = 0.0f;
    for (int i = 0; i < 3; ++i) {
        // Intervals are OPEN at the low cut (the original's unsigned range-check
        // idiom starts each interval one tick past the cut): the exact cut instant
        // falls through to night at full blend [orig: @ 0x408175/0x4081b0].
        if (t > kRegionCuts[i] && t < kRegionCuts[i + 1]) {
            region = i;
            low = kRegionCuts[i];
            high = kRegionCuts[i + 1];
            break;
        }
    }
    float blend = 1.0f;
    int32_t direction = 1;
    float blend_dist = 0.0f;
    bool in_blend = false;
    if (region == 3) {
        // Night wraps 21h -> 4h; only its 21h edge fades [orig: the wrapped
        // high-edge test can't fire @ 0x40820f].
        if (t > low && t - kRegionBlendH < low) {
            blend_dist = t - low;
            in_blend = true;
        }
    } else {
        if (t - kRegionBlendH < low) {
            blend_dist = t - low;
            in_blend = true;
        } else if (t + kRegionBlendH > high) {
            direction = -1;
            blend_dist = high - t;
            in_blend = true;
        }
    }
    // A zero blend distance falls through to full volume [orig: @ 0x408251].
    if (in_blend && blend_dist > 0.0f) {
        blend = blend_dist / kRegionBlendH;
    }
    int32_t adjacent = region - direction;
    if (adjacent > 3) {
        adjacent = 0;
    } else if (adjacent < 0) {
        adjacent = 3;
    }
    if (blend < 0.0f) {
        blend = 0.0f;
    } else if (blend > 1.0f) {
        blend = 1.0f;
    }
    return TimeOfDayRegion{region, adjacent, blend};
}

void AmbientMixer::clear() {
    markers_.clear();
    dynamic_emitters_.clear();
    for (Slot &s : slots_) {
        s = Slot{};
    }
    mix_out_.clear();
    clock_tick_ = 0;
    last_eval_tick_ = -1;
    last_mix_tick_ = -1;
    mix_counter_ = 0;
    auto_accum_ = 0.0f;
}

int AmbientMixer::add_marker(const float pos[3], int64_t source_bms_id,
                             int32_t stagger_slot, int32_t lifetime_ticks,
                             const int32_t slot_keys[4],
                             std::vector<std::vector<LayerDesc>> sets) {
    Marker m;
    m.pos[0] = pos[0];
    m.pos[1] = pos[1];
    m.pos[2] = pos[2];
    m.source_bms_id = source_bms_id;
    m.stagger_slot = stagger_slot;
    m.lifetime_ticks = lifetime_ticks > 0 ? lifetime_ticks : kMarkerLifetimeTicks;
    const int32_t set_count = static_cast<int32_t>(sets.size());
    for (int i = 0; i < 4; ++i) {
        const int32_t key = slot_keys[i];
        m.slot_keys[i] = (key >= 0 && key < set_count) ? key : -1;
    }
    m.sets = std::move(sets);
    markers_.push_back(std::move(m));
    return static_cast<int>(markers_.size()) - 1;
}

void AmbientMixer::advance_to_tick(int64_t tick) {
    if (tick <= last_eval_tick_) {
        if (tick > clock_tick_) {
            clock_tick_ = tick;
        }
        return;
    }
    // A gap longer than one stagger period collapses to the last kStaggerPeriod
    // ticks: each cohort is visited at most once, the same net registrations as
    // walking every elapsed tick [orig: the pool-2 walk visits cohort tick & 7
    // @ 0x4c225a].
    int64_t from = last_eval_tick_ + 1;
    if (last_eval_tick_ < 0 || tick - from >= kStaggerPeriod) {
        from = tick - (kStaggerPeriod - 1);
    }
    if (from < 0) {
        from = 0;
    }
    for (int64_t tk = from; tk <= tick; ++tk) {
        // Registrations carry the tick at which they were refreshed. This also
        // prevents a newly registered dynamic source from aging through the
        // entire interval since the preceding render-frame mix.
        clock_tick_ = tk;
        const int32_t cohort = static_cast<int32_t>(tk & (kStaggerPeriod - 1));
        for (int32_t i = 0; i < static_cast<int32_t>(markers_.size()); ++i) {
            if ((markers_[i].stagger_slot & (kStaggerPeriod - 1)) == cohort) {
                eval_marker_tick(i);
            }
        }
    }
    last_eval_tick_ = tick;
    clock_tick_ = tick;
}

void AmbientMixer::advance_seconds(float dt) {
    if (dt <= 0.0f) {
        return;
    }
    auto_accum_ += dt * 62.5f;
    const int64_t n = static_cast<int64_t>(auto_accum_);
    if (n > 0) {
        auto_accum_ -= static_cast<float>(n);
        advance_to_tick(clock_tick_ + n);
    }
}

// [orig: Entity_UpdateEnvSoundEmitter @ 0x4a8080] — no internal rate gate; the
// caller (the staggered walk) defines the cadence.
void AmbientMixer::eval_marker_tick(int32_t marker_index) {
    Marker &m = markers_[static_cast<size_t>(marker_index)];
    // Per-marker clock stagger nibble de-syncs region flips
    // [orig: @ 0x408158 (poolHandle & 0xF) << 11 Q16 hours].
    const float stagger_h =
            static_cast<float>((m.stagger_slot & 0xF) << 11) / 65536.0f;
    const TimeOfDayRegion tod = time_of_day_region(tod_hours_ + stagger_h);
    float blend = tod.blend;
    // Neighbouring regions resolving the SAME set keep full volume through the
    // crossfade (null == null included) [orig: @ 0x4a819d resolved-pointer compare].
    if (m.slot_keys[tod.region] == m.slot_keys[tod.adjacent]) {
        blend = 1.0f;
    }
    // A null region slot registers nothing — the marker is silent there and its
    // other-region slots expire through their lifetimes [orig: @ 0x4a81a3].
    if (m.slot_keys[tod.region] < 0) {
        return;
    }
    register_set(marker_index, tod.region, crossfade_volume_byte(blend));
}

void AmbientMixer::register_set(int32_t marker_index, int32_t region, int32_t vol_byte) {
    // Registering with volume 0 CLEARS the (entity, type) slots — the witnessed
    // unregister [orig: SoundEmitter_RegisterSetLayers @ 0x528340 pitch-0/vol-0 arm;
    // SoundEmitter_ClearByEntityAndSlot @ 0x527a50].
    if (vol_byte <= 0) {
        clear_marker_region(marker_index, region);
        return;
    }
    const Marker &m = markers_[static_cast<size_t>(marker_index)];
    const std::vector<LayerDesc> &layers =
            m.sets[static_cast<size_t>(m.slot_keys[region])];
    for (int32_t li = 0; li < static_cast<int32_t>(layers.size()); ++li) {
        Slot *s = find_or_alloc_marker_slot(marker_index, region, li);
        if (s == nullptr) {
            return; // table full: the registration drops, like the fixed table
        }
        s->lifetime = m.lifetime_ticks;
        s->vol_byte = vol_byte;
        s->pitch_q16 = 0x10000;
        s->refreshed_tick = clock_tick_;
    }
}

void AmbientMixer::clear_marker_region(int32_t marker_index, int32_t region) {
    for (Slot &s : slots_) {
        if (s.used && !s.dynamic && s.owner == marker_index && s.lane == region) {
            s = Slot{};
        }
    }
}

AmbientMixer::Slot *AmbientMixer::find_or_alloc_marker_slot(
        int32_t marker_index, int32_t region, int32_t layer) {
    Slot *free_slot = nullptr;
    for (Slot &s : slots_) {
        if (s.used) {
            if (!s.dynamic && s.owner == marker_index && s.lane == region &&
                s.layer == layer) {
                return &s;
            }
        } else if (free_slot == nullptr) {
            free_slot = &s;
        }
    }
    if (free_slot != nullptr) {
        *free_slot = Slot{};
        free_slot->used = true;
        free_slot->owner = marker_index;
        free_slot->lane = region;
        free_slot->layer = layer;
    }
    return free_slot;
}

int32_t AmbientMixer::find_or_alloc_dynamic_emitter(uint64_t source_spawn_id,
                                                     int32_t lane) {
    int32_t reusable = -1;
    for (int32_t i = 0; i < static_cast<int32_t>(dynamic_emitters_.size()); ++i) {
        DynamicEmitter &emitter = dynamic_emitters_[static_cast<size_t>(i)];
        if (emitter.source_spawn_id == source_spawn_id && emitter.lane == lane) {
            return i;
        }
        if (!emitter.active && reusable < 0) {
            reusable = i;
        }
    }
    if (reusable >= 0) {
        dynamic_emitters_[static_cast<size_t>(reusable)] = DynamicEmitter{};
        return reusable;
    }
    dynamic_emitters_.push_back(DynamicEmitter{});
    return static_cast<int32_t>(dynamic_emitters_.size()) - 1;
}

AmbientMixer::Slot *AmbientMixer::find_or_alloc_dynamic_slot(
        int32_t emitter_index, int32_t layer) {
    Slot *free_slot = nullptr;
    for (Slot &s : slots_) {
        if (s.used) {
            if (s.dynamic && s.owner == emitter_index && s.layer == layer) {
                return &s;
            }
        } else if (free_slot == nullptr) {
            free_slot = &s;
        }
    }
    if (free_slot != nullptr) {
        *free_slot = Slot{};
        free_slot->used = true;
        free_slot->dynamic = true;
        free_slot->owner = emitter_index;
        free_slot->lane =
                dynamic_emitters_[static_cast<size_t>(emitter_index)].lane;
        free_slot->layer = layer;
    }
    return free_slot;
}

void AmbientMixer::update_emitter_source(uint64_t source_spawn_id,
                                         const float pos[3],
                                         int64_t source_bms_id) {
    for (DynamicEmitter &emitter : dynamic_emitters_) {
        if (!emitter.active || emitter.source_spawn_id != source_spawn_id) {
            continue;
        }
        emitter.pos[0] = pos[0];
        emitter.pos[1] = pos[1];
        emitter.pos[2] = pos[2];
        emitter.source_bms_id = source_bms_id;
        emitter.occl_stamp = -1;
        emitter.occl_dist_q16 = -1;
    }
}

void AmbientMixer::clear_dynamic_emitter(uint64_t source_spawn_id, int32_t lane) {
    for (int32_t i = 0; i < static_cast<int32_t>(dynamic_emitters_.size()); ++i) {
        DynamicEmitter &emitter = dynamic_emitters_[static_cast<size_t>(i)];
        if (emitter.source_spawn_id != source_spawn_id || emitter.lane != lane) {
            continue;
        }
        for (Slot &s : slots_) {
            if (s.used && s.dynamic && s.owner == i) {
                s = Slot{};
            }
        }
        emitter.active = false;
        emitter.layers.clear();
        return;
    }
}

void AmbientMixer::register_emitter(uint64_t source_spawn_id, int32_t lane,
                                    const float pos[3], int64_t source_bms_id,
                                    int32_t lifetime_ticks, int32_t pitch_q16,
                                    int32_t volume_q8_8,
                                    std::vector<LayerDesc> layers) {
    // Position is entity-owned, not lane-owned. An unrefreshed lane remains
    // spatially attached while its keep-alive naturally expires; this update
    // deliberately leaves lifetime/refreshed_tick untouched.
    update_emitter_source(source_spawn_id, pos, source_bms_id);

    // [orig: SoundEmitter_RegisterSetLayers @ 0x528340] — either zero field
    // routes to SoundEmitter_ClearByEntityAndSlot for this source/lane only.
    if (pitch_q16 == 0 || volume_q8_8 == 0) {
        clear_dynamic_emitter(source_spawn_id, lane);
        return;
    }

    const int32_t emitter_index =
            find_or_alloc_dynamic_emitter(source_spawn_id, lane);
    DynamicEmitter &emitter =
            dynamic_emitters_[static_cast<size_t>(emitter_index)];
    emitter.active = true;
    emitter.source_spawn_id = source_spawn_id;
    emitter.lane = lane;
    emitter.pos[0] = pos[0];
    emitter.pos[1] = pos[1];
    emitter.pos[2] = pos[2];
    emitter.source_bms_id = source_bms_id;
    emitter.layers = std::move(layers);
    emitter.occl_stamp = -1;
    emitter.occl_dist_q16 = -1;

    // A keyed refresh may replace a set with fewer layers. The retail key includes
    // layer ordinal, so release any old ordinal no longer present.
    for (Slot &s : slots_) {
        if (s.used && s.dynamic && s.owner == emitter_index &&
            s.layer >= static_cast<int32_t>(emitter.layers.size())) {
            s = Slot{};
        }
    }

    const int32_t vol_byte =
            static_cast<int32_t>((static_cast<uint32_t>(volume_q8_8) >> 8) & 0xFFu);
    for (int32_t li = 0; li < static_cast<int32_t>(emitter.layers.size()); ++li) {
        Slot *s = find_or_alloc_dynamic_slot(emitter_index, li);
        if (s == nullptr) {
            return; // fixed table full: later layers drop like retail
        }
        s->lane = lane;
        s->lifetime = lifetime_ticks;
        s->vol_byte = vol_byte;
        s->pitch_q16 = pitch_q16;
        s->range_q16 = -1;
        s->refreshed_tick = clock_tick_;
    }
}

const std::vector<AmbientCandidate> &AmbientMixer::mix(const float listener[3],
                                                       OcclusionFn occl,
                                                       void *occl_ctx) {
    mix_out_.clear();
    // The mix runs per render frame but its clock is the logic tick — lifetimes are
    // frame-rate independent [orig: the current_tick argument @ 0x52133a; delta
    // decrement @ 0x5284aa/0x528541].
    const int64_t previous_mix_tick = last_mix_tick_;
    last_mix_tick_ = clock_tick_;
    ++mix_counter_;
    for (Slot &s : slots_) {
        if (!s.used) {
            continue;
        }
        // A slot whose keep-alive already ran out self-clears at entry
        // [orig: the alive-bit gate -> memset @ 0x528571/0x5284f8].
        if (s.lifetime <= 0) {
            s = Slot{};
            continue;
        }
        int64_t decay_from = previous_mix_tick;
        if (s.refreshed_tick > decay_from) {
            decay_from = s.refreshed_tick;
        }
        const int64_t delta =
                decay_from >= 0 && clock_tick_ > decay_from
                        ? clock_tick_ - decay_from
                        : 0;
        s.lifetime = delta >= s.lifetime ? 0
                                         : s.lifetime - static_cast<int32_t>(delta);
        const float *source_pos = nullptr;
        int64_t source_bms_id = 0;
        const LayerDesc *ld = nullptr;
        int64_t *occl_stamp = nullptr;
        int64_t *occl_dist_q16 = nullptr;
        if (s.dynamic) {
            if (s.owner < 0 ||
                s.owner >= static_cast<int32_t>(dynamic_emitters_.size())) {
                s = Slot{};
                continue;
            }
            DynamicEmitter &emitter =
                    dynamic_emitters_[static_cast<size_t>(s.owner)];
            if (!emitter.active || s.layer < 0 ||
                s.layer >= static_cast<int32_t>(emitter.layers.size())) {
                s = Slot{};
                continue;
            }
            source_pos = emitter.pos;
            source_bms_id = emitter.source_bms_id;
            ld = &emitter.layers[static_cast<size_t>(s.layer)];
            occl_stamp = &emitter.occl_stamp;
            occl_dist_q16 = &emitter.occl_dist_q16;
        } else {
            if (s.owner < 0 || s.owner >= static_cast<int32_t>(markers_.size()) ||
                s.lane < 0 || s.lane >= 4) {
                s = Slot{};
                continue;
            }
            Marker &marker = markers_[static_cast<size_t>(s.owner)];
            const int32_t set_key = marker.slot_keys[s.lane];
            if (set_key < 0 ||
                set_key >= static_cast<int32_t>(marker.sets.size()) ||
                s.layer < 0 ||
                s.layer >= static_cast<int32_t>(
                                   marker.sets[static_cast<size_t>(set_key)].size())) {
                s = Slot{};
                continue;
            }
            source_pos = marker.pos;
            source_bms_id = marker.source_bms_id;
            ld = &marker.sets[static_cast<size_t>(set_key)]
                             [static_cast<size_t>(s.layer)];
            occl_stamp = &marker.occl_stamp;
            occl_dist_q16 = &marker.occl_dist_q16;
        }
        if (s.range_q16 < 0) {
            // Lazily cached falloff range [orig: @ 0x52856a falloff << 16].
            s.range_q16 = static_cast<int64_t>(ld->falloff_u) << 16;
        }
        // Axis cull then euclidean against the layer range [orig: @ 0x5285da axis
        // abs checks; fsqrt compare @ 0x5285e0..0x528633]. World-space positions are
        // world floats; the Q16 boundary is the curve call, like the GDScript form
        // this replaces.
        const float range_f = static_cast<float>(s.range_q16) / 65536.0f;
        const float dx = source_pos[0] - listener[0];
        const float dy = source_pos[1] - listener[1];
        const float dz = source_pos[2] - listener[2];
        if (std::fabs(dx) > range_f || std::fabs(dy) > range_f ||
            std::fabs(dz) > range_f) {
            continue;
        }
        const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (dist > range_f) {
            continue;
        }
        const int64_t dist_q16 = static_cast<int64_t>(dist * 65536.0f);
        int32_t vol =
                emitter_layer_volume(dist_q16, ld->falloff_u, ld->min_u,
                                     s.vol_byte, ld->member_vol, ld->clamp_vol);
        if (vol > 0 && occl != nullptr) {
            // One LOS per marker per mix, shared across its layers; rays run only
            // for raw-audible slots (the shipped D-SND-7 lazy form)
            // [orig: Sound_ApplyOcclusionDistance call @ 0x528659].
            if (*occl_stamp != mix_counter_) {
                *occl_stamp = mix_counter_;
                *occl_dist_q16 = occl(occl_ctx, listener, source_pos, dist_q16,
                                      source_bms_id);
            }
            if (*occl_dist_q16 != dist_q16) {
                vol = emitter_layer_volume(*occl_dist_q16, ld->falloff_u,
                                           ld->min_u, s.vol_byte, ld->member_vol,
                                           ld->clamp_vol);
            }
        }
        if (vol <= 0) {
            continue; // [orig: volume_low == 0 skip @ 0x528706]
        }
        AmbientCandidate c;
        c.candidate_id = ld->candidate_id;
        c.vol = vol;
        c.pitch_q16 = s.pitch_q16;
        c.pos[0] = source_pos[0];
        c.pos[1] = source_pos[1];
        c.pos[2] = source_pos[2];
        mix_out_.push_back(c);
        // The original's candidate collect buffer holds 64 entries and the slot
        // walk BREAKS when it fills — slots past the 64th audible candidate never
        // rank that frame [orig: the v77 >= &buffer_end break @ 0x528775].
        if (mix_out_.size() >= 64) {
            break;
        }
    }
    // Natural expiry releases the reusable dynamic source record after its
    // final layer slot has gone. Explicit clears already do this immediately.
    std::vector<bool> dynamic_live(dynamic_emitters_.size(), false);
    for (const Slot &s : slots_) {
        if (s.used && s.dynamic && s.owner >= 0 &&
            s.owner < static_cast<int32_t>(dynamic_live.size())) {
            dynamic_live[static_cast<size_t>(s.owner)] = true;
        }
    }
    for (size_t i = 0; i < dynamic_emitters_.size(); ++i) {
        DynamicEmitter &emitter = dynamic_emitters_[i];
        if (emitter.active && !dynamic_live[i]) {
            emitter.active = false;
            emitter.layers.clear();
        }
    }
    // Loudest first; equal volumes keep deterministic membership by candidate id —
    // the host form of the original's slot-order-stable top-8 sort [orig: @ 0x5287ab].
    std::sort(mix_out_.begin(), mix_out_.end(),
              [](const AmbientCandidate &a, const AmbientCandidate &b) {
                  if (a.vol != b.vol) {
                      return a.vol > b.vol;
                  }
                  return a.candidate_id < b.candidate_id;
              });
    return mix_out_;
}

int AmbientMixer::live_slot_count() const {
    int n = 0;
    for (const Slot &s : slots_) {
        if (s.used) {
            ++n;
        }
    }
    return n;
}

} // namespace opennova::audio
