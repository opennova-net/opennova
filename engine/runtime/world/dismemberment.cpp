// The dismemberment cut (dismemberment.h): the corpse piece a lethal bone hit severs.
#include <runtime/world/dismemberment.h>

#include <runtime/world/ai.h>
#include <runtime/world/collision.h>
#include <runtime/world/round_sim.h>
#include <runtime/world/world.h>

namespace opennova::world {

namespace {

Entity make_dismemberment_piece_seed(const Entity &victim, uint32_t cut_mask,
                                     int32_t death_anim_state) {
    // The original clones by memcpy of the first 0x2B4 entity bytes plus the
    // 0xAC AI block (person -> pool 0), which INHERITS NetId/Ssn/Team and even
    // the anim-slot pointer (the halves share one skeletal evaluation), while
    // the tail past +0x2B4 keeps recycled-slot data. It then severs exactly:
    // sectionMask (complement swap @0x407691), Health = 0 (@0x407697), the
    // burn-emitter link +0x1CC (@0x40769e), shadowSlot0 +0x1B6 (@0x4076a4),
    // and DcbId +0x7C (@0x4076ab). [orig: Entity_CloneFromTemplateByType
    // @0x4398a0 person leg @0x4398c1-0x439924; the sever set above]
    // Our registry/AI split cannot memcpy an entity row, so this seed keeps
    // the authored/model traits and clears every runtime relationship a copied
    // struct would otherwise alias (weapon slots, mounts, emplacement poses) —
    // a superset of the original's sever list, same observable corpse.
    // Deliberate divergence (D-AI-9): net_id and the names are CLEARED, where
    // the original's memcpy duplicates the victim's — our forward-scanning
    // slot allocator could put the clone BELOW the victim, and the ascending
    // first-match find_by_net_id/find_by_name would then misroute scripted
    // kill/target refs to the clone. The 0x0C/0x18 wire records carry 0/""
    // for the clone instead of the duplicate.
    Entity piece = victim;
    piece.net_id = 0;
    piece.bms_id = 0;
    piece.handle = EntityHandle{};
    piece.registry_spawn_id = 0;
    piece.owner_connection_id = 0;
    piece.minimap_net_id = 0;
    piece.player_class = 0;
    piece.display_name.clear();
    piece.name.clear();
    piece.group_id = 0;
    piece.waypoint_id = 0;
    piece.wp_number = 0;
    piece.ai_state = 0;
    piece.ai_target_refcount = 0;
    piece.ammo_damage_class.clear();

    piece.health = 0;
    piece.alive = false;
    piece.death_effect_active[0] &= static_cast<uint8_t>(~1u); // +0x1CC [orig: @0x40769E]
    piece.damage_state = -1;
    piece.death_anim_state = death_anim_state;
    piece.corpse_timer = 0;
    piece.flags = 0;
    piece.engine_flags = 0;
    piece.net_move_input = 0;
    piece.net_stance_bits = 0;
    piece.net_analog_x = 0;
    piece.net_analog_y = 0;
    piece.net_analog_z = 0;
	piece.analog_throttle = 0;
	piece.equipped_adm_index = kAdmSlotNone;
    piece.use_gun_slot_swapped = false;
    piece.hidden = false;
    piece.last_attacker = EntityHandle{};
    piece.death_tick = 0;
    // Equivalent to the original's ~newBoneBits, since ~(~old & cut) =
    // old | ~cut. [orig: clone sectionMask store @0x407691]
    piece.section_mask = victim.section_mask | ~cut_mask;
    piece.dismemberment_piece = true;
    piece.spawned_piece_mask = 0;
    piece.death_motion = DeathMotionMode::None;
    piece.spawn_origin = kSpawnOriginNone;

    piece.seats.clear();
    piece.armory_points.clear();
    piece.primary_weapon.clear();
    piece.emplacement_parent = EntityHandle{};
    piece.emplacement_parent_spawn_id = 0;
    piece.emplacement_local = Vec3{};
    piece.emplacement_yaw_offset = 0;
    piece.emplacement_bone = 0;
    piece.emplacement_kind = 0;
    piece.emplacement_slot = 0;
    piece.emplacement_attachment_flags = 0;
    piece.emplacement_angle_count = 0;
    piece.emplacement_down_limit_bam = 0;
    piece.emplacement_up_limit_bam = 0;
    piece.emplacement_right_limit_bam = 0;
    piece.emplacement_left_limit_bam = 0;
    piece.emplacement_pose_metadata_resolved = false;
    piece.primary_weapon_slot = WeaponSlotState{};
    piece.primary_weapon_slot_adm = kAdmSlotNone;
    piece.primary_weapon_owner = EntityHandle{};
    piece.primary_occupant = EntityHandle{};
    piece.mount_target = EntityHandle{};
    piece.mount_target_net_id = 0;
    piece.mount_target_bms_id = 0;
    piece.mount_target_spawn_origin = 0;
    piece.mount_seat = -1;
    piece.mount_type = SeatType::None;
    piece.mounted = false;
    piece.mounted_config_valid = false;
    piece.mounted_config = 0;
    piece.mount_bone = 0;
    piece.ground_target = EntityHandle{};
    piece.mounted_child = EntityHandle{};
    piece.last_fire_target = TargetRef{};
    piece.spawn_position = piece.position;
    return piece;
}

} // namespace

void try_spawn_dismemberment_piece(World &world, Entity &victim,
                                   const FixedVec3 &round_velocity_q16,
                                   int32_t bone, int32_t death_anim_state,
                                   bool was_alive) {
    const uint32_t cut_mask = dismemberment_mask_for_bone(bone);
    if (!was_alive || cut_mask == 0 || victim.handle.pool() != 0 || victim.dismemberment_piece ||
        (victim.flags & kEntityFlagPlayer) != 0 ||
        (victim.engine_flags & kEntityFlagPlayer) != 0 ||
        victim.player_class != 0 ||
        (victim.item_attrib & kItemAttribNoDismember) != 0 ||
        victim.health > 0 || victim.health > (victim.health_max >> 1))
        return;

    const AiEntity *source = world.ai.for_handle(victim.handle);
    if (source == nullptr) return;

    Entity piece = make_dismemberment_piece_seed(
        victim, cut_mask, death_anim_state);
    const EntityHandle piece_handle = world.registry.spawn(0, piece);
    if (!piece_handle.valid()) return;

    // The horizontal kick only: the original adds roundVel X/Y >> 8 to the
    // clone's velocity pair and leaves the vertical component alone.
    // [orig: +152 add @0x4076b7; +156 add @0x4076c9]
    const int32_t impulse_q16[3] = {
        round_velocity_q16.x >> 8,
        round_velocity_q16.y >> 8,
        0,
    };
    world.ai.attach_dismemberment_piece(piece_handle, *source, impulse_q16);
    // The clone's record copy carries the victim's slice words (D-COL-19).
    if (world.ai.collision != nullptr)
        if (const Entity *piece_row = world.registry.get(piece_handle))
            world.ai.collision->inherit_candidate_slice(victim.handle, *piece_row);

    // Deliberate divergence (docs/divergence-ledger.md D-AI-9): the original
    // commits the victim mask BEFORE the clone call (sectionMask |= newBoneBits
    // @0x407684) and never null-checks the allocation (@0x407691 writes through
    // the Entity_CloneFromTemplateByType return, so actor-pool exhaustion would
    // fault). We commit after a successful allocation + AI attachment instead,
    // so a full pool leaves the original body intact. The |= is equivalent to
    // the original's ~old & boneMask two-step. [orig: @0x407675-0x407684]
    victim.section_mask |= cut_mask;
}

} // namespace opennova::world
