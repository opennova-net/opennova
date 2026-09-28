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

// The channel's arm for this tick's advance: none, the end-notify's (a pending
// state armed it, and it stays on the channel until a promotion or a re-init),
// or released by this tick's promotion off the end the arm parked it on.
enum class ChannelArm { kUnarmed, kArmed, kReleased };
} // namespace

int32_t arm_end_notify(const IRootMotionSource &source, int adm_id, int state, int variant,
                       int32_t phase) {
    const int32_t length = source.clip_length_ticks(adm_id, state, variant);
    if (length < 0) return -1;
    // A one-shot past its end carries 0x10000: the arm lands, the advance body
    // is skipped whole, the end flag never latches [orig:
    // AnimChannel_AdvancePlayback @0x40B14D].
    if (!source.clip_loops(adm_id, state) && phase >= length) return kEndNotifyNeverLatches;
    return source.clip_boundary_after(adm_id, state, phase, variant);
}

// Reconcile the requested id before checking the OLD channel's end flag.
// Promoting a pending request does not re-enter initialization in this call.
// A pending request arms the channel for this tick's advance: the arm lands
// once, on the tick the pending state is first seen, and the channel parks on
// its clip's next wrap after that tick (a one-shot on its end), which is when
// the end flag latches; the promotion follows on the next tick and resets the
// flag word to the clip's own, so the old channel advances unarmed and a loop
// parked on its end wraps and serves. A loop past its first cycle therefore
// waits for its next wrap rather than promoting at once. The arm lives on the
// channel, not on the request: a pending cleared while armed leaves the loop
// parked, and a pending set again promotes at once off the latched end.
// [orig: AnimMap_UpdateEntity @0x40B630..0x40B7C9, the arm @0x40B7B3, the
//  promotion on 0x20000 @0x40B793, its flag reset @0x40B7A8;
//  AnimChannel_AdvancePlayback latches 0x20000 on the armed wrap
//  @0x40B19E..0x40B1B1 and the armed one-shot end @0x40B188..0x40B18F]
static ChannelArm prepare_primary_channel(InfantryState &inf, const IRootMotionSource *source,
                                          AnimVariantRings *rings) {
    if (inf.anim_playing_state < 0) inf.anim_playing_state = inf.anim_state;
    if (inf.anim_state != inf.body_clip_state())
        begin_body_transition_with_insert(inf, inf.anim_state, source, rings);
    if (inf.anim_pending != 0 && source != nullptr) {
        const int state = inf.body_clip_state();
        if (inf.anim_pending_boundary < 0)
            inf.anim_pending_boundary =
                    arm_end_notify(*source, inf.adm_id, state, inf.anim_variant, inf.clip_phase);
        const int32_t boundary = inf.anim_pending_boundary;
        if (boundary >= 0 && inf.clip_phase >= boundary) {
            inf.anim_state = inf.anim_pending;
            inf.anim_pending = 0;
            inf.anim_pending_boundary = -1;
            return inf.clip_phase == boundary ? ChannelArm::kReleased : ChannelArm::kUnarmed;
        }
    }
    return inf.anim_pending_boundary >= 0 ? ChannelArm::kArmed : ChannelArm::kUnarmed;
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
        // wrap the arm parks on, anim_pending_boundary) samples the parked clip
        // end, not the wrapped start
        // [orig: AnimChannel_AdvancePlayback @0x40B193..0x40B1B1].
        const int state = inf.body_clip_state();
        const int32_t variant = inf.anim_variant;
        const bool armed = arm == ChannelArm::kArmed;
        const int32_t boundary = inf.anim_pending_boundary;
        // An armed loop already parked on its boundary (its pending cleared
        // while armed) re-parks every tick: retail wraps it and parks it at
        // 0.99999 again while 0x40000 holds, so the playhead never leaves the
        // boundary tick and the ring never serves [orig:
        // AnimChannel_AdvancePlayback @0x40B199..0x40B1B1, re-entered each tick].
        if (armed && boundary != kEndNotifyNeverLatches && inf.clip_phase >= boundary)
            inf.clip_phase = boundary - 1;
        const bool have = source.advance_armed(inf.adm_id, state, variant, inf.clip_phase,
                                               armed ? boundary : -1, out);
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
