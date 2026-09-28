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

// The channel's arm for this tick's advance: none, a pending request's, or
// released by this tick's promotion off the end the arm parked it on.
enum class ChannelArm { kUnarmed, kArmed, kReleased };
} // namespace

// Reconcile the requested id before checking the OLD channel's end flag.
// Promoting a pending request does not re-enter initialization in this call.
// A pending request arms the channel for this tick's advance. The promotion
// resets the channel's flag word to the clip's own, so the old channel
// advances unarmed: a loop parked on its end by the arm wraps and serves.
// [orig: AnimMap_UpdateEntity @0x40B630..0x40B7C9, the arm @0x40B7B3, the
//  promotion's flag reset @0x40B7A8]
static ChannelArm prepare_primary_channel(InfantryState &inf, const IRootMotionSource *source,
                                          AnimVariantRings *rings) {
    if (inf.anim_playing_state < 0) inf.anim_playing_state = inf.anim_state;
    if (inf.anim_state != inf.body_clip_state())
        begin_body_transition_with_insert(inf, inf.anim_state, source, rings);
    if (inf.anim_pending != 0 && source != nullptr) {
        const int32_t length = source->clip_length_ticks(
                inf.adm_id, inf.body_clip_state(), inf.anim_variant);
        if (length >= 0 && inf.clip_phase >= length) {
            inf.anim_state = inf.anim_pending;
            inf.anim_pending = 0;
            return inf.clip_phase == length ? ChannelArm::kReleased : ChannelArm::kUnarmed;
        }
    }
    return inf.anim_pending != 0 ? ChannelArm::kArmed : ChannelArm::kUnarmed;
}

bool reset_capsule_bottom_state(int state) {
    return (state >= 32 && state <= 35) || (state >= 176 && state <= 179);
}

bool advance_primary_channel(InfantryState &inf, IRootMotionSource &source,
                             AnimVariantRings &rings, RootMotionFrame &out) {
    const ChannelArm arm = prepare_primary_channel(inf, &source, &rings);
    out = RootMotionFrame{};

    if (!primary_blend_active(inf)) {
        // A queued state arms the channel's end-notify: the promotion tick (the
        // clip's first end, clip_length_ticks -- the step-3b clock) samples the
        // parked clip end, not the wrapped start
        // [orig: AnimChannel_AdvancePlayback @0x40B193..0x40B1B1].
        const int state = inf.body_clip_state();
        const int32_t variant = inf.anim_variant;
        const int32_t armed_boundary = inf.anim_pending != 0
                ? source.clip_length_ticks(inf.adm_id, state, variant)
                : -1;
        const bool have = source.advance_armed(inf.adm_id, state, variant,
                                               inf.clip_phase, armed_boundary, out);
        // An unarmed loop that wraps serves its playing state's ring: another
        // entry fades in from its first frame over eight ticks, the same entry
        // plays on. An armed channel parks instead and never serves; the
        // promotion that releases it leaves the loop parked on its end, so it
        // wraps on the promotion tick.
        // [orig: AnimChannel_AdvancePlayback @0x40B140 — the park @0x40B19E, the
        //  gate @0x40B1B5..0x40B1C1, the callback @0x40B1C8 ->
        //  AnimMap_AdvanceToNextAnim @0x40BDF0: the serve @0x40BE02..0x40BE07,
        //  the latch compare @0x40BE09..0x40BE0C, the fade @0x40BE24]
        const bool wrapped = arm == ChannelArm::kReleased
                ? source.clip_loops(inf.adm_id, state)
                : arm == ChannelArm::kUnarmed &&
                          source.clip_wraps_at(inf.adm_id, state, variant, inf.clip_phase);
        if (have && wrapped) {
            const int32_t served = rings.serve(&source, inf.adm_id, state);
            if (served != variant) {
                inf.begin_body_wrap_fade(served, inf.clip_phase);
                // The wrap tick already reads both halves at weight 0: the
                // wrapped entry's motion and extents, the incoming entry's
                // trigger word at t = 0. Re-read them from copies of the two
                // playheads one step back. [orig: AnimMap_UpdateEntity
                //  @0x40B812..0x40B81D -> AnimChannel_BlendKeyframes @0x40B340,
                //  the trigger from the incoming half @0x40B38D]
                int32_t outgoing_phase = inf.anim_prev_clip_phase - 1;
                int32_t incoming_phase = -1;
                source.advance_blended(inf.adm_id, state, variant, outgoing_phase,
                                       state, served, incoming_phase, inf.anim_blend_weight, out);
            }
        }
        return have;
    }

    inf.anim_blend_weight += inf.anim_blend_step;
    if (inf.anim_blend_weight >= 1.0f) {
        inf.anim_blend_weight = 1.0f;
        inf.anim_blend_step = 0.0f;
    }
    return source.advance_blended(inf.adm_id,
                                  inf.anim_prev, inf.anim_prev_variant, inf.anim_prev_clip_phase,
                                  inf.body_clip_state(), inf.anim_variant, inf.clip_phase,
                                  inf.anim_blend_weight, out);
}

void advance_primary_channel_fallback(InfantryState &inf) {
    prepare_primary_channel(inf, nullptr, nullptr);
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

// The re-init serves the played state's ring entry: the insert's when one
// replaces the request. [orig: AnimMap_UpdateEntity @0x40B737..0x40B778]
void begin_body_transition_with_insert(InfantryState &inf, int resolved,
                                       const IRootMotionSource *root_motion,
                                       AnimVariantRings *rings) {
    const auto serve = [&](int state) {
        return rings != nullptr ? rings->serve(root_motion, inf.adm_id, state) : 0;
    };
    if (inf.anim_pending == 0) {
        const int trans = gait_stance_transition_clip(inf.body_clip_state(), resolved);
        if (trans >= 0 && root_motion != nullptr &&
            root_motion->has_clip(inf.adm_id, trans)) {
            // The blend duration comes from the REQUESTED state's flags (15
            // ticks for the crouch/prone walks), not the insert clip's
            // [orig: the +0x2BC flags test @0x40b64b precedes the insert].
            inf.begin_body_transition(trans, resolved, serve(trans));
            inf.anim_pending = resolved; // deferred to the clip end [orig: @0x40b737]
            return;
        }
    }
    const int pending = inf.anim_pending;
    inf.begin_body_transition(resolved, -1, serve(resolved));
    inf.anim_pending = pending;
}


bool IRootMotionSource::advance_blended(int adm_id,
                                        int primary_state, int primary_variant,
                                        int32_t &primary_phase_ticks,
                                        int target_state, int target_variant,
                                        int32_t &target_phase_ticks,
                                        float target_weight, RootMotionFrame &out) {
    RootMotionFrame primary;
    RootMotionFrame target;
    const bool have_target =
            advance_variant(adm_id, target_state, target_variant, target_phase_ticks, target);
    const bool have_primary =
            advance_variant(adm_id, primary_state, primary_variant, primary_phase_ticks, primary);

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
