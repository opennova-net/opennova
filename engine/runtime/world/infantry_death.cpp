// The infantry death edge: once per life a dead body takes its death clip, seeds
// its corpse timer, screams, drops its mount and latches the dead bit; an org1
// (NPC) body on the authority then raises its own death transaction.
// [orig: Entity_UpdateInfantryAI @0x4B9C40..0x4B9D55; the player-body twin in
//  Entity_UpdateInfantryPlayerBody @0x4B4C4A..0x4B4CEA]
#include <runtime/world/infantry_internal.h>
#include <runtime/world/world.h>

namespace opennova::world {

// An org1 body's death transaction is its motor edge's: retail raises
// Entity_CheckAndProcessDeath from the edge [orig: Entity_UpdateInfantryAI
// @0x4B9D44..0x4B9D4D -> Entity_CheckAndProcessDeath @0x51B550], never from the
// damage that dropped the health. Player bodies (org2) and organics without a
// motor keep the damage-time transaction.
bool org1_owns_death_transaction(const World &world, EntityHandle victim) {
    const Entity *ent = world.registry.get(victim);
    const AiEntity *body = world.ai.for_handle(victim);
    return ent != nullptr && body != nullptr && body->inf.active &&
           !body->inf.is_local_player && !body->net_is_remote_peer &&
           ((ent->flags | ent->engine_flags) & kEntityFlagPlayer) == 0;
}

void infantry_death_edge(AiSystem &ai, AiEntity &e, World &world, Entity *ent, bool org1,
                         uint32_t logic_tick) {
    InfantryState &inf = e.inf;
    // A mounted body detaches so the corpse falls with the world, not the seat.
    // [orig: Entity_UpdateInfantryAI @0x4B9910 (the +0x16C test @0x4B9C57, the
    //  Entity_DetachFromVehicleIfServer call @0x4B9C60); the player-body twin
    //  @0x4B4C08..0x4B4C11]
    if (ent != nullptr && ent->mounted)
        world.vehicles.detach(e.handle);
    // [orig: @0x4B9C68] Section bit 0 forces silent, shortened cleanup
    // even for LeaveCorpse items and cancels scripted respawns.
    const bool silent_cleanup = ent != nullptr && (ent->section_mask & 1u) != 0;
    if (ent != nullptr) {
        ent->corpse_timer = static_cast<int32_t>(
                uint32_t(ent->deathtime_ticks) - (silent_cleanup ? 61u : 0u));
        if (silent_cleanup) ent->npc_respawns = 0;
    }
    // The death scream. NPC (org1): profile slot 7 (sounddeath), or 8
    // (SSNightDead) on a night mission — the runtime reads the mission's
    // EnableNVG attribute as the night gate. [orig: @0x4b9ca3-0x4b9cc1
    // Bms_AttribFlags & 0x100000 pick; play at &entity->pos]
    // Player body (org2): the body-model composite set "<prefix>_DEATH"
    // ("_DEATH_K" at night) from the entity's anim-slot byte — NOT the
    // profile slots; a bank without the set is the id-0 silence with no
    // slot fallback. [orig: @0x4b4c4a-0x4b4c6a ->
    // SoundProfile_FindByEntityAndType @0x528180 type 5/0 ->
    // Entity_PlaySound3D_FullVolume]
    if (!silent_cleanup && (ent == nullptr || !ent->dismemberment_piece)) {
        const bool night_death =
            (world.tables.mission_attrib_flags & MissionTables::kMissionAttribEnableNVG) != 0;
        if (inf.is_local_player) {
            SoundSlotEvent scream;
            scream.source_handle = e.handle.packed;
            scream.pos[0] = e.pos[0];
            scream.pos[1] = e.pos[1];
            scream.pos[2] = e.pos[2];
            scream.slot = static_cast<uint8_t>(
                    night_death ? audio::kSlotNightDeath : audio::kSlotDeath);
            audio::compose_entity_sound_set(
                    ent != nullptr ? ent->anim_slot : 0,
                    night_death ? audio::kEntitySoundDeathNight
                                : audio::kEntitySoundDeath,
                    scream.set_name, sizeof(scream.set_name));
            world.out.slot_sounds.push_back(scream);
        } else {
            ai.emit_slot_sound(world, e,
                               night_death ? audio::kSlotNightDeath : audio::kSlotDeath,
                               e.pos);
        }
    }
    // Consume the kill's selection; none staged -> the generic death
    // (cause 4 -> 174 death_pungi) AND the attacker slot (+0x178) is
    // cleared, so a death nothing stamped (script/WAC) reports as
    // unattributed while one that follows a non-lethal hit keeps that
    // hit's clip and shooter [orig: @0x4b9cc9 fallback, @0x4b9ceb
    // lastAttacker = 0; the player-body edge's twin @0x4b4c72..0x4b4c8d;
    // consumed +0x2C0 clears @0x4b9d38].
    if (ent != nullptr && ent->death_anim_state == 0) {
        ent->last_attacker = EntityHandle{};
        // The org1 edge then dispatches its class event callback as a hit
        // (deathCallback(entity, 1, 0)): Entity_HandleDamageTrigger's hit leg
        // raises a non-player body's slot alert byte to 2 and its trigger group
        // to red before anything else. [orig: Entity_UpdateInfantryAI
        // @0x4B9CDB..0x4B9CF1; Entity_HandleDamageTrigger @0x4073C8..0x4073EA]
        if (org1) {
            e.slot.bytes()[AiSlot::kAlertByte] = 2;
            world.script.relations.group(ent->group_id).alert = TriggerRelations::kAlertRed;
        }
    }
    int death = (ent != nullptr && ent->death_anim_state != 0)
                        ? ent->death_anim_state
                        : compute_death_anim_state(0, 0, death_cause::kGeneric);
    // A body that dies afloat (the Flags 0x8000 latch the water blocks set)
    // takes death_drown over any staged selection.
    // [orig: Entity_UpdateInfantryAI @0x4B9CF6..0x4B9D0E; the player-body twin
    //  Entity_UpdateInfantryPlayerBody @0x4B4C93..0x4B4CAB]
    if (ent != nullptr && ((ent->flags | ent->engine_flags) & kEntityFlagDrowning) != 0)
        death = anim_state::kDeathDrown;
    if (ent != nullptr) ent->death_anim_state = 0;
    // Stripped embedder .adm sets may lack the selected clip; keep the pre-P1c
    // stand-in ladder (torso-forward, then death_fire) rather than a T-pose.
    if (ai.root_motion != nullptr && !ai.root_motion->has_clip(inf.adm_id, death)) {
        const int torso = anim_state::kDeathBulletBase + 4;
        death = ai.root_motion->has_clip(inf.adm_id, torso) ? torso : anim_state::kDeathFire;
    }
    // The head already sampled this tick's playing channel; death
    // changes the request for the next update.
    // [orig: @0x4B9D38; death callback tail @0x4B9D55]
    inf.request_body_animation(death);
    inf.move_mode = 0;
    inf.target_dist = 0;
    inf.player_moving = false;
    if (ent == nullptr) return;
    // The edge latches the dead bit, stamps the death tick and drops the
    // carried/climb bits (0xC0) and the attach parent (+0x184). The compact
    // state byte carries the dead bit unmasked, and the 0x0A priority list
    // reads it as the recipient's dead-or-spectator gate; both views of our
    // split flags field take it and the spawn reset clears them.
    // [orig: Entity_UpdateInfantryAI `or eax,2` @0x4B9D18 / store @0x4B9D1B,
    //  death tick @0x4B9D24..0x4B9D2F, `and eax,0FFFFFF3Fh` @0x4B9D2A,
    //  +0x184 = 0 @0x4B9D3E; the player-body twin
    //  Entity_UpdateInfantryPlayerBody @0x4B4CB5..0x4B4CDB]
    ent->flags |= kEntityFlagDead;
    ent->engine_flags |= kEntityFlagDead;
    ent->attach_parent = {};
    ent->flags &= ~(kEntityFlagMounted | kEntityFlagAiClimb);
    ent->engine_flags &= ~(kEntityFlagMounted | kEntityFlagAiClimb);
    ent->death_tick = logic_tick;
    if (!org1) return;
    // The authority raises the body's death transaction from here: the host's
    // death routing sends the 0x13 and scores it against the victim's
    // lastAttacker (+0x178). A damage-time record for this body only fed the
    // SP tally. [orig: @0x4B9D44..0x4B9D4D -> Entity_CheckAndProcessDeath
    //  @0x51B550 (0x13 @0x51B58F, GameEvent_ProcessScoring(+0x178) @0x51B5B3)]
    if (ai.is_authority) {
        RoundDeath d;
        d.victim = e.handle;
        d.killer = ent->last_attacker;
        d.victim_handle = e.handle.packed;
        d.killer_handle = ent->last_attacker.valid() ? ent->last_attacker.packed : 0xFFFFu;
        d.event_flags = ent->cause_flags & 0xF00u;
        d.motor_edge = true;
        world.round_sim.deaths.push_back(d);
    }
}

} // namespace opennova::world
