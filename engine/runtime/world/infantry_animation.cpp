// Motor-head AnimMap update shared by org1, org2, and spawn warm-up.
// [orig: AnimMap_UpdateDualChannels @0x40B8C0; AnimMap_UpdateEntity @0x40B5F0]
// See docs/world/world-wac-ai-re.md section 36 (D-INF-26).
#include <runtime/world/infantry_internal.h>

namespace opennova::world {
namespace {
bool primary_blend_active(const InfantryState &inf) {
    return inf.body_blend_active();
}

int32_t blend_root_lane(int32_t previous, int32_t current, float current_weight) {
    // The provider has already quantized each channel to the integer RootMotionFrame
    // seam. Retail mixes the underlying float tracks before that conversion, so this
    // fallback can differ at the final integer by a bounded LSB. Keeping every
    // operation float32 still preserves its accumulated-weight behavior and signed
    // truncation rather than replacing it with a rational tick/tick count.
    const float previous_weight = 1.0f - current_weight;
    const float mixed = static_cast<float>(previous) * previous_weight +
                        static_cast<float>(current) * current_weight;
    return static_cast<int32_t>(mixed);
}

void blend_root_frame(const RootMotionFrame &previous, const RootMotionFrame &current,
                      float current_weight, RootMotionFrame &out) {
    out.dx = blend_root_lane(previous.dx, current.dx, current_weight);
    out.dy = blend_root_lane(previous.dy, current.dy, current_weight);
    out.dz = blend_root_lane(previous.dz, current.dz, current_weight);
    out.capsule_bottom =
            blend_root_lane(previous.capsule_bottom, current.capsule_bottom,
                            current_weight);
    out.capsule_top =
            blend_root_lane(previous.capsule_top, current.capsule_top, current_weight);
    // Event triggers are copied from the secondary/current channel, never blended
    // with or ORed against primary. [orig: AnimMap_UpdateEntity @0x40b5f0]
    out.events = current.events;
}

} // namespace

// Reconcile the requested id before checking the OLD channel's end flag.
// Promoting a pending request does not re-enter initialization in this call.
// [orig: AnimMap_UpdateEntity @0x40B630..0x40B7C9]
static void prepare_primary_channel(InfantryState &inf, const IRootMotionSource *source) {
    if (inf.anim_playing_state < 0) inf.anim_playing_state = inf.anim_state;
    if (inf.anim_state != inf.body_clip_state())
        begin_body_transition_with_insert(inf, inf.anim_state, source);
    if (inf.anim_pending != 0 && source != nullptr) {
        const int32_t length = source->clip_length_ticks(inf.adm_id, inf.body_clip_state(), 0);
        if (length >= 0 && inf.clip_phase >= length) {
            inf.anim_state = inf.anim_pending;
            inf.anim_pending = 0;
        }
    }
}

bool reset_capsule_bottom_state(int state) {
    return (state >= 32 && state <= 35) || (state >= 176 && state <= 179);
}

bool advance_primary_channel(InfantryState &inf, IRootMotionSource &source,
                             RootMotionFrame &out) {
    prepare_primary_channel(inf, &source);
    out = RootMotionFrame{};

    if (!primary_blend_active(inf)) {
        // A queued state arms the channel's end-notify: the promotion tick (the
        // clip's first end, clip_length_ticks -- the step-3b clock) samples the
        // parked clip end, not the wrapped start
        // [orig: AnimChannel_AdvancePlayback @0x40B193..0x40B1B1].
        const int32_t armed_boundary = inf.anim_pending != 0
                ? source.clip_length_ticks(inf.adm_id, inf.body_clip_state(), 0)
                : -1;
        return source.advance_armed(inf.adm_id, inf.body_clip_state(), 0, inf.clip_phase,
                                    armed_boundary, out);
    }

    inf.anim_blend_weight += inf.anim_blend_step;
    if (inf.anim_blend_weight >= 1.0f) {
        inf.anim_blend_weight = 1.0f;
        inf.anim_blend_step = 0.0f;
    }
    return source.advance_blended(inf.adm_id,
                                  inf.anim_prev, inf.anim_prev_clip_phase,
                                  inf.body_clip_state(), inf.clip_phase,
                                  inf.anim_blend_weight, out);
}

void advance_primary_channel_fallback(InfantryState &inf) {
    prepare_primary_channel(inf, nullptr);
    if (primary_blend_active(inf)) {
        inf.anim_prev_clip_phase = (inf.anim_prev_clip_phase + 1) % 62;
        inf.clip_phase = (inf.clip_phase + 1) % 62;
        inf.anim_blend_weight += inf.anim_blend_step;
        if (inf.anim_blend_weight >= 1.0f) {
            inf.anim_blend_weight = 1.0f;
            inf.anim_blend_step = 0.0f;
        }
        return;
    }
    inf.clip_phase = (inf.clip_phase + 1) % 62;
}

void begin_body_transition_with_insert(InfantryState &inf, int resolved,
                                              const IRootMotionSource *root_motion) {
    if (inf.anim_pending == 0) {
        const int trans = gait_stance_transition_clip(inf.body_clip_state(), resolved);
        if (trans >= 0 && root_motion != nullptr &&
            root_motion->has_clip(inf.adm_id, trans)) {
            // The blend duration comes from the REQUESTED state's flags (15
            // ticks for the crouch/prone walks), not the insert clip's
            // [orig: the +0x2BC flags test @0x40b64b precedes the insert].
            inf.begin_body_transition(trans, resolved);
            inf.anim_pending = resolved; // deferred to the clip end [orig: @0x40b737]
            return;
        }
    }
    const int pending = inf.anim_pending;
    inf.begin_body_transition(resolved);
    inf.anim_pending = pending;
}


bool IRootMotionSource::advance_blended(int adm_id,
                                        int primary_state, int32_t &primary_phase_ticks,
                                        int target_state, int32_t &target_phase_ticks,
                                        float target_weight, RootMotionFrame &out) {
    RootMotionFrame primary;
    RootMotionFrame target;
    const bool have_target = advance(adm_id, target_state, target_phase_ticks, target);
    const bool have_primary = advance(adm_id, primary_state, primary_phase_ticks, primary);

    if (have_primary && have_target) {
        if (target_weight < 1.0f)
            blend_root_frame(primary, target, target_weight, out);
        else
            out = target;
        return true;
    }
    if (have_target) {
        out = target;
        return true;
    }
    if (have_primary) {
        out = primary;
        out.events = target.events;
        return true;
    }
    out = RootMotionFrame{};
    return false;
}

} // namespace opennova::world
