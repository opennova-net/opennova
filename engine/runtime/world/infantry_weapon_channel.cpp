// The upper-body weapon channel producer — the entity's SECONDARY AnimMap
// channel, split out of infantry.cpp (W3-7 size gate): the per-tick arms-dip /
// reload windows + playhead advance, the 16-tick selection/commit, the pure
// hold-pose ladder both the motor path and the decode-only joiner path share,
// and the fire/switch stamps. Witness: world-wac-ai-re.md §14.8.

#include <base/io/bam.h>

#include <runtime/world/ai.h>
#include <runtime/world/world.h>

namespace opennova::world {

// ----------------------------------------------------------------------------
// The upper-body weapon channel — the entity's SECONDARY AnimMap channel.
// [orig: Entity_UpdateInfantryPlayerBody @0x4b5cab..0x4b5ea9 (selection + commit)
//  + AnimMap_UpdateDualChannels @0x40b8c0 (advance; deferred promotion at clip end
//  via AnimMap_UpdateEntity @0x40b77b); witness world-wac-ai-re.md §14.8]
// ----------------------------------------------------------------------------
void AiSystem::infantry_weapon_channel(AiEntity &e, World &world, uint32_t logic_tick) {
    InfantryState &inf = e.inf;

    // The arms-dip / pitch-kick block [orig: @0x4b5cab..0x4b5ce7]: while the dip
    // window runs, the decay term drops 0x2800000 per tick BEFORE the eighth-step ease;
    // the window byte decrements in BOTH branches — twice per tick — so the 20-tick
    // weapon-switch stamp dips for 10 ticks (the 0x49 remote-reload 80 for 40).
    if (inf.arms_dip_ticks > 0) {
        --inf.arms_dip_ticks;             // [orig: @0x4b5cb5]
        inf.pitch_kick_accum -= 0x2800000; // [orig: @0x4b5cb7 += 0xFD800000]
    }
    inf.pitch_kick_accum -=
        io::bam_sar(io::bam_add(inf.pitch_kick_accum, 4), 3); // [orig: @0x4b5cc7..0x4b5cd5]
    if (inf.arms_dip_ticks > 0) --inf.arms_dip_ticks;        // [orig: @0x4b5cdb..0x4b5ce7]

    // The 3P reload-anim window counts down once per tick [orig: @0x4b5cf9].
    if (inf.reload_anim_ticks > 0) --inf.reload_anim_ticks;

    // The SELECTION + COMMIT run only on retail's 16-tick slow pass, not every tick
    // [orig: gate @0x4b5d6d/@0x4b5d71, key `current_tick & 0xF` stored @0x4b4e79]. The
    // key is the RAW tick — unstaggered, unlike the org1 `logic_tick + 36*net_id`
    // idiom a few lines up — so every body selects on the same phase. Consequences are
    // witnessed behavior, not approximation: a hold-pose change lands 0-15 ticks late,
    // and the 80-tick reload window is sampled by five passes rather than eighty.
    // Deferred promotion and playback already ran at the motor head through
    // AnimMap_UpdateDualChannels @0x40b8c0, ahead of this gate. Retail's slow pass carries much more than the weapon channel (the
    // slot timer, threat scan, damage and the music gamescript block, @0x4b5d77..
    // @0x4b637b); this ports the weapon-channel tenant and the ride link that
    // follows its commit.
    if ((logic_tick & 0xFu) == 0u) {
        // The hold kind is re-read from the ADM table EVERY selection pass, keyed by
        // this entity's OWN equipped index — the original keeps no per-player copy
        // (`dword_24E8084[280 * entityData->equippedAdmIndex]`). That is precisely what
        // lets any observer derive a REMOTE player's hold pose from the single wire
        // byte at entity+0x2B0, so resolving it here rather than from a local-player
        // scalar is what makes the non-local case work at all.
        // [orig: @0x4b5dba]
        inf.wpn_hold_kind = 0;
        if (const Entity *owner = world.registry.get(e.handle)) {
            if (const WeaponTableEntry *held =
                        world.tables.weapons.by_index(owner->equipped_adm_index))
                inf.wpn_hold_kind = held->special_hold;
        }
        infantry_weapon_channel_select(e);
        // The org2 twin of the org1 ride link, right after the hold-state commit.
        // [orig: Entity_UpdateInfantryPlayerBody @0x4B5EA9..0x4B5F2C]
        if (Entity *body = world.registry.get(e.handle)) infantry_ride_link(world, *body);
    }

}

void AiSystem::infantry_weapon_channel_advance(AiEntity &e) {
    InfantryState &inf = e.inf;

    if (inf.wpn_playing_state < 0) inf.wpn_playing_state = inf.wpn_state;
    if (inf.wpn_state != inf.weapon_clip_state()) {
        const int requested = inf.wpn_state;
        const int inserted = inf.wpn_deferred == 0
                ? gait_stance_transition_clip(inf.weapon_clip_state(), requested) : -1;
        const bool use_insert = inserted >= 0 && root_motion != nullptr &&
                root_motion->has_clip(inf.adm_id, inserted);
        const int played = use_insert ? inserted : requested;
        // The secondary re-init serves from the heads it shares with the primary
        // and every body of its .adm, ahead of the primary's own re-init.
        // [orig: AnimMap_UpdateDualChannels @0x40B908 (secondary) before
        //  @0x40B94E (primary)]
        inf.begin_weapon_transition(played, anim_rings.serve(root_motion, inf.adm_id, played));
        if (use_insert) {
            inf.wpn_deferred = requested;
            inf.wpn_blend_step = (infantry_anim_flags(requested) & 0x400u) != 0
                    ? 1.0f / 15.0f : 0.1f;
        }
    }
    // End notification promotes the REQUEST only. The old channel advances
    // once more here; the next update initializes the promoted channel.
    // [orig: shared AnimMap_UpdateEntity @0x40B77B..0x40B7FE]
    // A deferred state arms the channel, and the arm outlives its promotion
    // until the next re-init clears the channel's flags [orig: the arm
    // @0x40B780..0x40B7E1; AnimChannel_InitFromData @0x410580].
    bool armed = inf.wpn_deferred != 0;
    if (inf.wpn_deferred != 0 && root_motion != nullptr) {
        const int32_t length = root_motion->clip_length_ticks(
                inf.adm_id, inf.weapon_clip_state(), inf.wpn_variant);
        if (length >= 0 && inf.wpn_clip_phase >= length) {
            inf.wpn_state = inf.wpn_deferred;
            inf.wpn_deferred = 0;
        }
    }

    // Advance the secondary playhead every tick; root motion is DISCARDED — the weapon
    // layer never feeds the parent transform [orig: parentEntity=0 @0x40b8f3].
    if (root_motion != nullptr) {
        RootMotionFrame discard;
        if (inf.weapon_blend_active()) {
            // Both playheads stay alive while the weight ramps, exactly as the primary
            // channel does [orig: AnimMap_UpdateEntity @0x40b5f0 keeps the outgoing
            // channel advancing; AnimChannel_BlendTwoChannels @0x410740].
            inf.wpn_blend_weight += inf.wpn_blend_step;
            if (inf.wpn_blend_weight >= 1.0f) {
                inf.wpn_blend_weight = 1.0f;
                inf.wpn_blend_step = 0.0f;
            }
            // Both playheads step under their own served variants; the blended root
            // output is discarded either way (the weapon layer never feeds motion), so
            // stepping the two variant tracks independently is exact.
            RootMotionFrame discard_prev;
            root_motion->advance_variant(inf.adm_id, inf.weapon_clip_state(), inf.wpn_variant,
                                         inf.wpn_clip_phase, discard);
            root_motion->advance_variant(inf.adm_id, inf.wpn_prev, inf.wpn_prev_variant,
                                         inf.wpn_prev_clip_phase, discard_prev);
        } else {
            const int state = inf.weapon_clip_state();
            const int32_t variant = inf.wpn_variant;
            const bool have = root_motion->advance_variant(inf.adm_id, state, variant,
                                                           inf.wpn_clip_phase, discard);
            // The secondary's unarmed loop wrap serves its playing state's ring
            // from the heads it shares with the primary, exactly as the primary
            // does; only its pose changes. [orig: AnimMap_UpdateEntity(0, S, entity)
            //  @0x40B908 -> AnimChannel_AdvancePlayback @0x40B7FE -> the callback
            //  @0x40B1C8, AnimMap_AdvanceToNextAnim @0x40BDF0]
            if (have && !armed &&
                root_motion->clip_wraps_at(inf.adm_id, state, variant, inf.wpn_clip_phase)) {
                const int32_t served = anim_rings.serve(root_motion, inf.adm_id, state);
                if (served != variant) inf.begin_weapon_wrap_fade(served, inf.wpn_clip_phase);
            }
        }
    }
}

// The pose ladder itself — see the infantry.h contract. Pure so both the motor-driven
// path (local player, and wire peers on the authority) and the decode-only path (a
// joiner's view of its peers, which has no motor entity to run) resolve the SAME
// selection from the same four inputs, instead of two ladders drifting apart.
// [orig: Entity_UpdateInfantryPlayerBody @0x4b5dad..0x4b5e6f]
int infantry_weapon_hold_state(int hold_kind, int primary_anim_state, bool scope_raised,
                               bool binoculars_raised, bool reloading) {
    // Desired state [orig: @0x4b5dad..0x4b5e6f]: the held weapon's hold kind (the
    // g_AdmDefs dword @0x24E8084 + 0x460*idx = the def's special_hold key) selects the
    // pose ladder; the default (rifles, kind 0) MIRRORS the primary state.
    int desired;
    switch (hold_kind) {
        case 1: // knife family -> 50 [orig: @0x4b5dc0 lea eax,[ecx+31h]]
            desired = anim_state::kHoldKnife;
            break;
        case 2: // pistol -> 51 [orig: @0x4b5dcd]
            desired = anim_state::kHoldPistol;
            break;
        case 3: // grenade -> 52 [orig: @0x4b5dd7]
            desired = anim_state::kHoldGrenade;
            break;
        case 4: // stinger/AT4/RPG -> 53 [orig: @0x4b5de1]
            desired = anim_state::kHoldStinger;
            break;
        case 5: // designator -> 54, scoped 55 [orig: @0x4b5deb test Flags&0x10]
            desired = scope_raised ? anim_state::kHoldDesignatorScoped
                                   : anim_state::kHoldDesignator;
            break;
        case 6: // P90 -> 56, scoped 57 [orig: @0x4b5dfe]
            desired = scope_raised ? anim_state::kHoldP90Scoped : anim_state::kHoldP90;
            break;
        case 7: // MP7 -> 58, scoped 59 [orig: @0x4b5e11]
            desired = scope_raised ? anim_state::kHoldMP7Scoped : anim_state::kHoldMP7;
            break;
        case 8: // javelin -> 60, scoped 61 [orig: @0x4b5e24]
            desired = scope_raised ? anim_state::kHoldJavelinScoped
                                   : anim_state::kHoldJavelin;
            break;
        default:
            // MIRROR the primary state — 43 idle when the primary is locked (flag 4);
            // 49 idle_3 when scoped [orig: @0x4b5e37..0x4b5e4e].
            desired = (infantry_anim_flags(primary_anim_state) & 0x4u) != 0
                              ? anim_state::kIdle
                              : primary_anim_state;
            if (scope_raised) desired = anim_state::kIdle3;
            break;
    }
    // Overrides, strongest last [orig: @0x4b5e53..0x4b5e6f]: binoculars 64, then the
    // reload window — 66 reload2 when the hold kind is 2 (pistol), else 65 reload.
    if (binoculars_raised) desired = anim_state::kBinoculars; // [orig: @0x4b5e53]
    if (reloading)
        desired = hold_kind == 2 ? anim_state::kReload2
                                 : anim_state::kReload; // [orig: @0x4b5e5e..0x4b5e6f]
    return desired;
}

// The selection + commit half of the weapon channel, behind the 16-tick gate above.
// [orig: Entity_UpdateInfantryPlayerBody @0x4b5dad..0x4b5ea3]
void AiSystem::infantry_weapon_channel_select(AiEntity &e) {
    InfantryState &inf = e.inf;
    const int desired = infantry_weapon_hold_state(
            inf.wpn_hold_kind, inf.anim_state, inf.scope_raised, inf.binoculars_raised,
            inf.reload_anim_ticks > 0);

    // Commit [orig: @0x4b5e72]: same -> skip; a locked (flag 4: attacks 62/63, reloads
    // 65/66) or emote (0x20) current defers the change to clip end; else stamp now.
    if (desired != inf.wpn_state) {
        const uint32_t curf = infantry_anim_flags(inf.wpn_state);
        if ((curf & 0x4u) != 0 || (curf & 0x20u) != 0) {
            inf.wpn_deferred = desired; // [orig: @0x4b5e88/@0x4b5e95]
        } else {
            // The next motor-head update consumes this request.
            // [orig: @0x4B5E9D; re-init @0x40B5F0 on the next @0x4B41DF]
            inf.request_weapon_animation(desired);
            inf.wpn_deferred = 0;       // [orig: @0x4b5ea3]
        }
    }
}

// The fire-path attack stamp — see the infantry.h declaration. Unlike the per-tick
// selection this writes the target immediately, whatever the current state's flags.
// [orig: WeaponAction_Fire @0x542bbc..0x542bea; ebx = 0 from @0x542b22]
void infantry_weapon_attack_stamp(InfantryState &inf, int attack_kind) {
    int state;
    if (attack_kind == 1)
        state = anim_state::kKnifeAttack;   // 62 [orig: @0x542bcb]
    else if (attack_kind == 2)
        state = anim_state::kGrenadeAttack; // 63 [orig: @0x542be0]
    else
        return; // rifle fire stamps NO body state [orig: only the 1/2 compares]
    // The channel re-inits only on a target CHANGE [orig: AnimMap_UpdateEntity @0x40b5f0
    // pulls a new clip only when target differs] — a repeat stamp of the same attack
    // state mid-clip does not restart the playing clip.
    // Only the next channel update serves a variant and reinitializes playback.
    inf.request_weapon_animation(state);
    inf.wpn_deferred = 0;
}

void infantry_weapon_switch_stamp(InfantryState &inf, uint64_t anim_map_serial) {
    if (anim_map_serial == 0 || inf.wpn_anim_map_serial == anim_map_serial) return;
    inf.wpn_anim_map_serial = anim_map_serial;
    inf.arms_dip_ticks = 20;
}

bool infantry_weapon_channel_visible(const InfantryState &inf, bool weapon_in_hands,
                                     bool mount_blocks_channel) {
    return inf.active && weapon_in_hands && !mount_blocks_channel &&
           (infantry_anim_flags(inf.anim_state) & 0x40u) != 0;
}


// The physical recoil accumulator's per-body decay and orientation drift.
// [orig: Entity_UpdateInfantryPlayerBody @0x4B40E0]
void infantry_recoil_tick(InfantryState &inf, int32_t &heading,
                          int32_t &pitch, int32_t random16) {
    // The accumulator yields an eighth-step, then loses half of that step.
    // Pitch receives one eighth of the pre-halved step and yaw receives the
    // half-step with PRNG-selected sign. The caller draws PRNG_Next16 even when
    // recoil is zero. [orig: the entity+0x380 body-update block]
    const int32_t step = io::bam_sar(io::bam_add(inf.recoil_pitch, 4), 3);
    const int32_t half = io::bam_sar(step, 1);
    inf.recoil_pitch = io::bam_sub(inf.recoil_pitch, half);
    if (inf.recoil_pitch <= 0x300) inf.recoil_pitch = 0;
    pitch = io::bam_add(pitch, io::bam_sar(step, 3));
    heading = (random16 & 1) == 0 ? io::bam_add(heading, half)
                                  : io::bam_sub(heading, half);
}

void infantry_weapon_weight_spread_tick(
        InfantryState &inf, const InfantryWeightSpreadInputs &inputs) {
    if (inputs.produce) {
        const int32_t weight = io::bam_add(inputs.weaponweight_fp16,
                                           inputs.clipweight_fp16);
        int32_t increment = 0;
        if (inputs.aimed_shot_available ||
            (inputs.prone && !inputs.drowning)) {
            increment = weight / 3;
        } else if (inputs.crouched && !inputs.drowning) {
            increment = static_cast<int32_t>(
                    static_cast<double>(weight) * 2.0 / 3.0);
        } else {
            increment = static_cast<int32_t>(static_cast<double>(weight) * 1.5);
        }
        inf.weapon_weight_spread =
                io::bam_add(inf.weapon_weight_spread, increment);
        if (inputs.airborne_rising) {
            inf.weapon_weight_spread =
                    io::bam_add(inf.weapon_weight_spread, 0x01000000);
        }
    }

    // Shared decay is after the local producer; remote players and AI jump
    // directly here. There is no upper clamp.
    // [orig: Entity_UpdateInfantryPlayerBody @0x4B5945]
    inf.weapon_weight_spread = io::bam_sub(
            inf.weapon_weight_spread,
            io::bam_sar(io::bam_add(inf.weapon_weight_spread, 4), 4));
    if (inf.weapon_weight_spread <= 0x300) inf.weapon_weight_spread = 0;
}

} // namespace opennova::world
