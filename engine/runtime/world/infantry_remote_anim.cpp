// The remote-player body animation leg of the infantry motor, split out of
// infantry.cpp to keep that TU under the 2500-line size ratchet. Behaviour is
// unchanged: the body moves verbatim and the five helpers it shares with
// infantry.cpp are declared in infantry_internal.h.
// Witness record: docs/world/world-wac-ai-re.md.

#include <algorithm>
#include <cmath>

#include <base/io/bam.h>
#include <runtime/terrain_query/height_field.h>

#include <runtime/world/ai.h>
#include <runtime/world/angle.h>
#include <runtime/world/infantry_internal.h>
#include <runtime/world/infantry_ladder.h>
#include <runtime/audio/sound_profile.h>
#include <runtime/world/vehicle_attach.h>
#include <runtime/world/world.h>

namespace opennova::world {

void AiSystem::remote_player_body_anim(AiEntity &e, World &world, uint32_t logic_tick) {
    InfantryState &inf = e.inf;
    if (!inf.active) return;
    Entity *ent = world.registry.get(e.handle);
    if (ent == nullptr) return;

    // A hard-snap/frozen entity skips the whole body motor — the retail head
    // bails on Flags bit0 before any anim work, which is why a deploy-pending
    // (hidden) player's channel is
    // FROZEN on the wire (golden pre-deploy ratio constant at 40; ours swept to 255 in
    // v32 until this gate). [orig: Entity_UpdateInfantryPlayerBody @0x4b411b-0x4b4127
    // `mov edx,[esi+24h]; test dl,1; jnz return`]
    if ((ent->flags & 1u) != 0) return;

    // The motor's registry hydration is skipped for wire-snapped peers (tick_infantry
    // returns before it); sync the health copy the selection/lean gates read.
    e.health = ent->health;
    RootMotionFrame collision_frame;
    bool have_collision_frame = false;

    // Same motor-head dual update as local org2; only translation is wire-owned.
    // [orig: Entity_UpdateInfantryPlayerBody @0x4B41DF]
    infantry_weapon_channel_advance(e);
    if (root_motion != nullptr) {
        if (reset_capsule_bottom_state(inf.anim_state)) inf.prev_capsule_bottom = 0;
        have_collision_frame =
                advance_primary_channel(inf, *root_motion, anim_rings, collision_frame);
    } else {
        advance_primary_channel_fallback(inf);
    }

    if (ent->health <= 0) {
        // Death edge — one-shot to the death pose, the org2 twin of the motor's
        // death edge (infantry.cpp): consume the damage-time +0x2C0 selection;
        // nothing staged plays the generic 174 death_pungi AND clears the
        // attacker slot (+0x178), so a death nothing stamped reports as
        // unattributed while one that follows a non-lethal hit keeps that hit's
        // clip and shooter. A body that dies afloat takes death_drown 175 over
        // the selection; the edge stamps the death tick and drops Flags 0xC0.
        // [orig: Entity_UpdateInfantryPlayerBody @0x4b4c72 test, @0x4b4c7f
        //  compute(0, 0, 4) into +0x2C0, @0x4b4c8d lastAttacker = 0; the
        //  Flags&0x8000 pick @0x4B4C93..0x4B4CAB; death tick
        //  @0x4B4CC1..0x4B4CCC, `and eax,0FFFFFF3Fh` @0x4B4CC7; consumed
        //  +0x2C0 clears @0x4b4cd5]
        // Relationship teardown is independent of animation state. A peer can
        // already be in a death-class clip when a late/replayed state restores a
        // mount, and that must not leave the seat claim or compact carrier alive.
        // [orig: infantry death detach @0x4b9c57..0x4b9c60]
        if (ent->mounted) world.vehicles.detach(e.handle);
        if (infantry_anim_flags(inf.anim_state) != 0x82u) {
            // The edge's scream: the org2 pass runs for this peer's body on the
            // authority as well, so its player composite plays here too
            // [orig: @0x4b4c4a..0x4b4c6a] (D-SND-22).
            infantry_death_scream(*this, e, world, ent);
            if (ent->death_anim_state == 0) ent->last_attacker = EntityHandle{};
            int death = ent->death_anim_state != 0
                                ? ent->death_anim_state
                                : compute_death_anim_state(0, 0, death_cause::kGeneric);
            if (((ent->flags | ent->engine_flags) & kEntityFlagDrowning) != 0)
                death = anim_state::kDeathDrown;
            ent->death_anim_state = 0;
            ent->flags &= ~(kEntityFlagMounted | kEntityFlagAiClimb);
            ent->engine_flags &= ~(kEntityFlagMounted | kEntityFlagAiClimb);
            ent->death_tick = logic_tick;
            // An unauthored selection plays the slot's registration fill.
            // [orig: AnimMap_RegisterEntity @0x40BB60; the edge stores the
            //  selection unchecked @0x4B4CA3]
            inf.request_body_animation(death);
        }
    } else {
        // The replicated JUMP key (MoveOrder bit 5): the retail host derives the
        // jump launch + anims for a remote player inside the same authority-run
        // jump block the local body uses — cooldown clamp [0,32], >1 counts
        // down, parked at 1 while the key is held, release -> 0, launch only
        // from 0 [orig: maintenance @0x4b7de0-0x4b7e15, park @0x4b7e7a-0x4b7e82,
        // gate @0x4b7e8c-0x4b7eb5, anim 30 + pending 31 + reload 32
        // @0x4b7ef2-0x4b7f06]. A wire-snapped peer's MOTION is uplink-owned, so
        // only the anim/latch leg runs here. The C2S pose apply reconstructs the
        // terrain-backed airborne/landing state that this gate consumes; where
        // terrain is unavailable, the countdown window remains the conservative
        // animation-only fallback (tracked in the D-NET-159/196 record).
        const bool jump_key =
            (ent->net_move_input & Entity::kMoveOrderJump) != 0;
        // Stance-change (0x1D) and the extended movement uplink (0x0C) can arrive in
        // the same network pump.  The jump block is per-tick and reads the CURRENT
        // MoveOrder prone bit; the fourth-tick locomotion selector below is not an
        // eligibility cache. [orig: MoveOrder&0x100 -> the prone local @0x4b4165-0x4b4181;
        // prone gate @0x4b7e99]
        const bool replicated_prone = (ent->net_stance_bits & 0x1u) != 0;
        const bool jump_state_blocked = player_jump_world_state_blocked(inf, ent);
        if (inf.jump_cooldown < 0) inf.jump_cooldown = 0;
        if (inf.jump_cooldown > 32) inf.jump_cooldown = 32;
        if (inf.jump_cooldown > 1) {
            --inf.jump_cooldown;
        } else if (inf.jump_cooldown == 1 && !jump_key) {
            inf.jump_cooldown = 0;
        }
        if (inf.jump_cooldown == 0 && jump_key && !replicated_prone &&
            !jump_state_blocked) {
            inf.jump_cooldown = 32;
            // Only latch the synthesized vertical state when the authority has
            // terrain and can therefore observe its landing on a later uplink.
            // Terrain-free harnesses retain the documented cooldown-only residual.
            if (world.tables.terrain != nullptr && world.tables.terrain->valid()) {
                inf.airborne = true;
                ent->flags |= kEntityFlagInAir;
                ent->engine_flags |= kEntityFlagInAir;
            }
            // STRAIGHT stamps, same as the local block — no availability
            // check in the witnessed org2 jump stamps [orig: @0x4b7ef2/@0x4b7efc].
            inf.request_body_animation(anim_state::kJumpStart);
            inf.anim_pending = anim_state::kJumpLoop;
        }
        const bool jump_episode =
            (inf.anim_state == anim_state::kJumpStart ||
             inf.anim_state == anim_state::kJumpLoop) &&
            inf.jump_cooldown > 1;
        if ((logic_tick & 3u) == 0 && !jump_episode) {
            // Every 4th tick [orig: `test tickCounter, 3` @0x4b70ce]: decode the
            // REPLICATED MoveOrder byte (bits 0-2 = 8-way dir, bit 3 = moving,
            // bits 6-7 = lean [orig: @0x4b4153/@0x4b415c]) + the stance bits
            // (MoveOrder bits 8-9, fed by C2S 0x1D [orig: @0x4b4165-0x4b4181;
            // prone suppressed by Flags & 0x10A000 — swim/parachute unmodeled]),
            // then run the SAME witnessed selection the local player runs (one
            // function in the original; it skips while airborne @0x4b70b8 — the
            // jump-episode window above is the wire-snapped analog).
            inf.player_moving = (ent->net_move_input & 0x08u) != 0;
            inf.player_move_dir_index = ent->net_move_input & 0x07u;
            inf.lean_left = (ent->net_move_input & 0x40u) != 0;
            inf.lean_right = (ent->net_move_input & 0x80u) != 0;
            inf.stance = (ent->net_stance_bits & 0x1u) != 0
                             ? InfantryState::Stance::kProne
                             : ((ent->net_stance_bits & 0x2u) != 0
                                    ? InfantryState::Stance::kCrouch
                                    : InfantryState::Stance::kStand);
            player_body_select(e, world, ent->flags | ent->engine_flags, logic_tick);
        }
    }
    // The lean angle runs on the authority for every player body (the wire echoes the
    // lean BITS, each end integrates the angle), and the torso roll rides the same
    // body pass. [orig: @0x4b5c97 / @0x4b7dbf / @0x4b5cff]
    infantry_lean_tick(e, ent != nullptr ? (ent->flags | ent->engine_flags) : 0u);
    infantry_torso_roll_tick(e);

    // The upper-body weapon channel runs for a WIRE PEER exactly as it does for the
    // local player. The original has no ownership test on it: the only locality check
    // in the whole region guards the refresh of Flags bits 2-4 from the local
    // g_WeaponScopeActive / g_BinocularsRaised / NVG globals, and a non-local entity
    // jumps straight past it into the hold-kind ladder [orig: @0x4b5d77
    // `cmp g_LocalPlayerEntity, esi ; jnz short loc_4B5DAD`]. For a peer those same
    // three bits arrive over the wire instead — the host has already replaced them
    // from the sender's C2S 0x0C state byte (mask 0x1C) — so the selection reads the
    // peer's OWN entity for both of its inputs: bit 0x10 scoped [orig: test @0x4b5deb]
    // and the equipped ADM index at +0x2B0 [orig: read @0x4b5dba]. Without this a
    // remote player holds a rifle pose whatever it carries, and never adopts the
    // scoped stance the wire is already reporting.
    inf.scope_raised = (ent->flags & kEntityFlagScopeRaised) != 0;
    inf.binoculars_raised = (ent->flags & kEntityFlagBinoculars) != 0;
    infantry_weapon_channel(e, world, logic_tick);

    if (have_collision_frame) {
        inf.prev_capsule_bottom = collision_frame.capsule_bottom;
        // The remote-player eye-offset restamp: retail runs the same body
        // updater for net-snapped peers, and the +0x74 store persists while
        // the pose work is inert — the org2 non-local formula, lean at rest.
        // [orig: Entity_UpdateInfantryPlayerBody @0x4b6984..0x4b68f5]
        const int32_t extent = collision_frame.capsule_top -
                               collision_frame.capsule_bottom;
        int32_t eye_z = std::min(extent, 0xD000);
        if (eye_z < 0x2000) eye_z = 0x2000;
        inf.eye_offset_z = eye_z;
    }

    // Vertical velocity persists independently of the uplink-owned position.
    // The authority owns chute admission [orig: org2 @0x4B7AD9..0x4B7C8D].
    uint32_t chute_flags = ent->flags | ent->engine_flags;
    if ((chute_flags & kEntityFlagInAir) == 0 || ent->mounted) inf.vel[2] = 0;
    else if ((chute_flags & (kEntityFlagDrowning | kEntityFlagLadderContact)) == 0)
        inf.vel[2] -= 208;
    const ParachuteEvents chute = parachute_tick(inf.parachute, chute_flags,
            ent->carry_flags, inf.vel[2], is_authority, logic_tick);
    ent->flags = (ent->flags & ~kEntityFlagParachute) | (chute_flags & kEntityFlagParachute);
    ent->engine_flags = (ent->engine_flags & ~kEntityFlagParachute) | (chute_flags & kEntityFlagParachute);
    if ((chute_flags & kEntityFlagParachute) != 0 && inf.anim_state != anim_state::kParachute)
        inf.request_body_animation(anim_state::kParachute);
    if (chute.opened) emit_slot_sound(world, e, audio::kSlotChuteOpen, e.pos);
    if (chute.closed) emit_slot_sound(world, e, audio::kSlotChuteClose, e.pos);
    if (chute.flap) emit_slot_sound(world, e, audio::kSlotChuteFlap, e.pos);
    if (chute.free_fall) emit_slot_sound(world, e, audio::kSlotFreeFall, e.pos);

    const bool carried = infantry_follow_carrier(e, world, collision_frame.capsule_bottom, true);

    // Snapshot ownership suppresses locomotion, not the retail collision tail.
    // Resolve the current pose with the anim capsule and zero movement channels;
    // mounted bodies retain the resolver's ordinary force-suppression rule.
    // [orig: org2 resolver call @0x4B7CE0..0x4B7CF4; CT callback gate
    // @0x4B31DD..0x4B3238]
    const bool resolve_contacts = collision != nullptr && collision->instance_count() != 0;
    if (resolve_contacts) {
        int32_t contact_vel[2] = {0, 0}, contact_vel_z = 0;
        const int32_t tick_start_z = e.pos[2];
        const LadderResolveIO lio = make_ladder_resolve_io(e, tick_start_z);
        collision->resolve_entity(
                world, e.handle, e.collide_state, e.pos, contact_vel,
                contact_vel_z,
                have_collision_frame ? collision_frame.capsule_bottom : 0,
                have_collision_frame ? collision_frame.capsule_top : 0,
                e.heading, e.pitch, ((ent->flags | ent->engine_flags) & kEntityFlagPlayer) != 0,
                is_authority, logic_tick, inf.anim_state,
                infantry_anim_flags(inf.anim_state), e.health, nullptr, &lio);
        ent->health = e.health;
    }
    // Only motor-owned movement writes position back; otherwise retain the
    // wire-owned registry pose, including in data-less authority fixtures.
    if (carried || resolve_contacts) {
        ent->position.x = static_cast<float>(from_fixed(e.pos[0]));
        ent->position.y = static_cast<float>(from_fixed(e.pos[1]));
        ent->position.z = static_cast<float>(from_fixed(e.pos[2]));
    }

    // Present-pass clip for the host's own third-person view of this peer.
    if (ent->alive && ent->health > 0)
        ent->body_anim_slot = body_anim_slot_from_state(inf.body_clip_state());

    mirror_wire_anim(e, world);
}

} // namespace opennova::world
