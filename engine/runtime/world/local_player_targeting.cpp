#include <runtime/world/local_player.h>

#include <algorithm>
#include <cmath>
#include <base/io/bam.h>
#include <runtime/world/ai.h>
#include <runtime/world/angle.h>
#include <runtime/world/collision.h>
#include <runtime/world/world.h>

namespace opennova::world {
// The heat-seeker arm of the shared target query, contexts 6 and 11: the
// pool-0 and pool-1 legs. LOS follows the bounded, stable score sort; the
// query disables it during scan. NOT ported: the ctx-flag 0x200 projectile-
// list leg (the 128-entry {entity, ttl} ring sub_4E70F0 keeps, fed by the
// shell-bounce update and released at round death, walked through the same
// validator/score with the MP case-11 Entity_IsShellProjectile deflection
// roll) -- its feeders, the shell-bounce ring, are absent from the port, so
// live rounds such as flares never enter the candidate list here.
// [orig: Entity_SpawnWeaponEffect @0x545840 (ctx flags 0x662 @0x54591b);
// Entity_FindTargets @0x53a610 -- the pool legs ported, the 0x200 ring leg
// @0x53acf7..0x53ade0 not; ring sub_4E70F0 @0x4e70f0, feeders
// Entity_UpdateShellBounce @0x443e69 / Projectile_ReleaseEffects @0x4e82ea,
// case-11 deflection arm @0x539385..0x539415; Entity_ValidateWeaponTarget
// @0x53a400; Weapon_CalcDamageByType @0x539140]
EntityHandle guided_heat_target(World &world, const Entity &owner, const AiEntity &body,
                         const AmmoTableEntry &ammo) {
    const Entity *occupant = world.registry.get(owner.primary_occupant);
    const int team = occupant ? occupant->team : owner.team;
    if (ammo.heat_det_range == 0 || (team == 0 && (body.slot.f[1] & 0x200) == 0)) return {};
    int32_t pose[6] = {0, 0, 0, body.heading, body.pitch, body.roll};
    world.ai.weapon_aim_origin(world, body, pose);
    struct Candidate { EntityHandle handle; int32_t score; };
    std::vector<Candidate> candidates;
    candidates.reserve(128);
    const auto descending = [](const Candidate &a, const Candidate &b) { return a.score > b.score; };
    for (int pool = 0; pool <= 1; ++pool) {
        for (size_t index = 0; index < world.registry.pool_capacity(pool); ++index) {
            const EntityHandle h = EntityHandle::make(pool, static_cast<int>(index));
            const Entity *target = world.registry.get(h);
            if (!target || !target->item_id || h == owner.handle || h == owner.primary_occupant) continue;
            const uint32_t flags = target->flags | target->engine_flags;
            if ((flags & 0x8000001u) != 0) continue;
            if (((flags & 2u) != 0 || target->health <= 0) &&
                int32_t(world.logic_tick - target->death_tick) > 16) continue;
            if ((flags & kEntityFlagPlayer) != 0 && world.match.outcome().ended) continue;
            if (!owner.target_selectors.admits_team(target->net_id, target->group_id) &&
                (target->team == 0 || target->team == team)) continue;
            if (!owner.target_selectors.allows(target->net_id, target->group_id)) continue;
            if (target->armor_impact == -1 && target->armor_kz == -1) continue;
            int32_t aim[3], metrics[6];
            world.ai.weapon_aim_origin(world, *target, aim);
            const uint32_t arc = AiSystem::weapon_relative_metrics(pose, aim, metrics);
            if (arc > uint32_t(ammo.boresight_maxang) ||
                metrics[2] > int32_t(uint32_t(ammo.heat_det_range) << 16) ||
                metrics[2] >= int32_t(uint32_t(uint16_t(target->heat_sig)) << 16)) continue;
            if (target->item_type == 3 && (flags & 0x8000u) != 0) continue;
            int32_t score = io::bam_sub(0, static_cast<int32_t>(arc));
            if (world.rules.mp_session) score = io::bam_add(uint16_t(target->heat_sig), score >> 12);
            score = owner.target_selectors.adjust_score(score, target->net_id, target->group_id);
            if (score == INT32_MAX) continue;
            candidates.push_back({h, score});
            if (candidates.size() == 128) {
                std::stable_sort(candidates.begin(), candidates.end(), descending);
                candidates.pop_back();
            }
        }
    }
    std::stable_sort(candidates.begin(), candidates.end(), descending);
    for (const Candidate &candidate : candidates) {
        const Entity *target = world.registry.get(candidate.handle);
        int32_t aim[3];
        world.ai.weapon_aim_origin(world, *target, aim);
        if (world.ai.line_of_sight_clear(world, pose, aim, owner.handle, candidate.handle)) return candidate.handle;
        if (((owner.flags | owner.engine_flags) & kEntityFlagIndoors) != 0) {
            const int32_t zero[3] = {}, offset[3] = {24576, 0, 0};
            int32_t shifted[3];
            collision_matrix_from_euler(body.heading, 0, 0, zero).rotate_point(offset, shifted);
            for (int axis = 0; axis < 3; ++axis) shifted[axis] = io::bam_add(pose[axis], shifted[axis]);
            if (world.ai.line_of_sight_clear(world, shifted, aim, owner.handle, candidate.handle)) return candidate.handle;
        }
    }
    return {};
}

// Aim/range acquisition runs every sixteen ticks. The locked tone is refreshed
// every tick, from the held target flag, including ticks between acquisitions.
// [orig: Entity_UpdateInfantryPlayerBody @0x4b4e9b..0x4b5215;
// Entity_BuildCameraFromWeaponView @0x4b0f30]
void LocalPlayer::update_aim_target() {
    Entity *e = player();
    AiEntity *body = player_ai();
    if (!e || !body || (e->flags & 2u) != 0) return;
    if ((world_.logic_tick & 15u) == 0 && world_.collision) {
        int32_t fire[6];
        local_weapon_fire_pose(world_, weapon, active_local_weapon_slot(world_, weapon)->clip, fire);
        int32_t origin[3] = {fire[0], fire[1], fire[2]};
        int32_t yaw = fire[3], pitch = fire[4], roll = fire[5];
        // The weapon-view offsets: the scope-zero elevation comes back OUT of
        // the ray while the weapon can fire. A binocular view substitutes
        // its wander. The shared fire pose supplies mounted and mortar seeds.
        // [orig: Entity_UpdateInfantryPlayerBody @0x4B4EA7..0x4B4EE0;
        // binocular offsets @0x4B4EC8..0x4B4ED6; Entity_BuildCameraFromWeaponView
        // yaw @0x4B0F52 and pitch @0x4B0F56]
        const bool can_fire = local_player_can_fire();
        if (view.binoculars_view_active) {
            yaw = io::bam_sub(yaw, bam_from_degrees_wrapped(view_tracker.binocular_yaw_offset_deg));
            pitch = io::bam_add(pitch, bam_from_degrees_wrapped(view_tracker.binocular_pitch_offset_deg));
        } else if (can_fire) {
            pitch = io::bam_sub(pitch, active_local_weapon_slot(world_, weapon)->zero_pitch);
        }
        if (view.camera_mode != 0 || !weapon.active) {
            LocalPlayerViewFrame frame;
            local_player_view_frame(&world_, weapon, view, view_tracker, frame);
            if (frame.camera_pose_valid) {
                for (int i = 0; i < 3; ++i) origin[i] = static_cast<int32_t>(frame.camera.eye[i] * 65536.0f);
                yaw = bam_heading_from_mission_yaw_deg(frame.camera.yaw_deg);
                pitch = bam_from_degrees_wrapped(frame.camera.pitch_deg);
                roll = bam_from_degrees_wrapped(frame.camera.roll_deg);
            }
        }
        const int32_t forward[3] = {1000 << 16, 0, 0};
        int32_t endpoint[3];
        collision_matrix_from_euler(yaw, pitch, roll, origin).transform_point(forward, endpoint);
        ProjectileTrace trace;
        trace.owner = e->handle;
        trace.start = {origin[0], origin[1], origin[2]};
        trace.end = {endpoint[0], endpoint[1], endpoint[2]};
        trace.walk_terrain = ((e->flags | e->engine_flags) & kEntityFlagIndoors) == 0;
        trace.include_wire_proxies = world_.rules.mp_session && !world_.rules.projectile_authority;
        const auto hit = world_.collision->trace_aim(world_, trace);
        body->inf.head_look_target = hit.geometry_entity;
        for (int i = 0; i < 3; ++i) body->inf.aim_point[i] = hit.position_q16[i];
        const double dx = double(hit.position_q16.x) - body->pos[0];
        const double dy = double(hit.position_q16.y) - body->pos[1];
        const double dz = double(hit.position_q16.z) - body->pos[2];
        weapon.aim_range_q16 = static_cast<int32_t>(std::min(std::sqrt(dx*dx+dy*dy+dz*dz), 2147418112.0));
        const bool allowed = !weapon.active || (weapon.def.flags2 & 0x20) == 0 || local_player_can_fire();
        if (allowed && weapon.active) {
            EntityHandle target;
            if ((weapon.def.flags2 & 0x10) != 0) {
                const Entity *candidate = world_.registry.get(hit.geometry_entity);
                if (candidate && ((candidate->flags | candidate->engine_flags) & 4u) == 0 &&
                    int16_t(active_local_weapon_slot(world_, weapon)->clip) != 0 && candidate->team != e->team) {
                    const Vec3 d{candidate->position.x - e->position.x,
                        candidate->position.y - e->position.y, candidate->position.z - e->position.z};
                    const float distance = std::sqrt(d.x*d.x+d.y*d.y+d.z*d.z);
                    if (distance >= 75.0f && distance <= 1500.0f) {
                        // A mountable gun (Entity_IsMountableGun: itemdef && type
                        // != 1 && attrib & 0x20) redirects to its groundEntity,
                        // and the lock stores the REDIRECTED entity.
                        // [orig: Entity_IsMountableGun @0x434240 -> groundEntity
                        //  @0x4b51bd..0x4b51c9; the type switch @0x4b51d0, the
                        //  cases 1,2,6-8,10 store @0x4b51e6..0x4b51e9]
                        const Entity *typed = candidate;
                        if (candidate->has_item_def && candidate->item_type != 1 &&
                                (candidate->item_attrib & kItemAttribEweap) != 0) {
                            if (const Entity *parent = world_.registry.get(candidate->ground_target)) typed = parent;
                        }
                        switch (typed->item_type) {
                            case 1: case 2: case 6: case 7: case 8: case 10: target = typed->handle; break;
                        }
                    }
                }
            } else {
                const int index = world_.tables.weapons.index_of(weapon.def_name.c_str());
                const WeaponTableEntry *def = index < 0 ? nullptr : world_.tables.weapons.by_index(uint8_t(index));
                const AmmoTableEntry *ammo = def ? world_.tables.ammo.by_index(def->ammo_index) : nullptr;
                if (ammo) target = guided_heat_target(world_, *e, *body, *ammo);
            }
            body->inf.combat_target = target;
            body->slot.f[3] = target.valid() ? static_cast<int32_t>(target.packed) + 1 : 0;
            e->last_fire_target = target;
        }
        if (allowed && body->inf.combat_target.valid()) body->slot.f[2] |= 1;
        else body->slot.f[2] = -2;
    }
    // The tone registers only on a frame's last logic tick: the catch-up gate
    // precedes the death-screen and local-player tests.
    // [orig: dword_24E0E80 @0x4b5229; Game_MainLoop @0x52ba32..0x52ba3a]
    if (world_.rules.last_tick_of_batch &&
        !view_session_inputs.death_screen_active && weapon.active &&
        (body->slot.f[2] & 1) != 0 && weapon.def.soundlockedtone[0] &&
        world_.out.fire_sounds.listener_valid()) {
        SoundEmitterEvent event;
        event.source_spawn_id = e->registry_spawn_id;
        event.source_handle = e->handle.packed;
        event.pos = e->position;
        event.source_bms_id = e->bms_id;
        event.emitted_tick = world_.logic_tick + 1;
        event.lane = 100;
        event.lifetime_ticks = 20;
        event.pitch_q16 = 65536;
        event.volume_q8_8 = 65535;
        event.set_name = weapon.def.soundlockedtone;
        world_.out.sound_emitters.publish(std::move(event));
    }
}
}
