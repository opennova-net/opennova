#include <runtime/world/item_sections.h>
#include <base/io/fixed.h>
#include <runtime/world/world.h>
#include <runtime/world/collision.h>
#include <runtime/world/angle.h>
#include <runtime/terrain_query/height_field.h>
#include <base/io/bam.h>
#include <cmath>
#include <climits>

namespace opennova::world {
namespace {
int32_t section_ground(const World &world, const Vec3 &position) {
    const auto *terrain = world.tables.terrain;
    return io::bam_add(terrain && terrain->valid() ? int32_t(
            terrain::height_field_height_world_bilinear(*terrain,
                position.x, -position.y) * 65536) : 0, 16384);
}
void section_effect(World &world, const std::string &name, const Vec3 &position) {
    if (!name.empty()) world.out.destruction.effects.push_back({name, position, {}});
}
void tower_husk(World &world, Entity &entity) {
    if ((entity.engine_flags & kEntityFlagHusk) != 0) return;
    entity.engine_flags |= kEntityFlagHusk;
    world.out.destruction.husk_swaps.push_back({entity.net_id, entity.handle.packed,
            entity.bms_id, entity.spawn_origin, entity.item_id, entity.spawned_piece_mask, entity.position});
    ++world.out.destruction.items_destroyed;
}
void spawn_tower_section(World &world, Entity &entity, const ItemDeathTraits &traits,
        int section, ItemHitContext hit) {
    // [orig: Entity_SpawnSectionEntity @ 0x4402D0]
    if (!traits.model_loaded || section < 0 ||
            size_t(section) >= traits.model_section_origins_q16.size()) return;
    Entity seed;
    seed.item_id = entity.item_id; seed.kind = entity.kind; seed.item_type = entity.item_type;
    seed.uniform_scale_q16 = entity.uniform_scale_q16;
    seed.health = 20; seed.engine_flags = 6; seed.alive = false;
    seed.item_section_piece = true;
    seed.death_motion = DeathMotionMode::SectionFalling;
    seed.yaw = entity.yaw; seed.pitch = entity.pitch; seed.roll = entity.roll;
    int32_t pos[3];
    entity_placement_matrix(entity).transform_point(
            traits.model_section_origins_q16[section].data(), pos);
    seed.position = {pos[0] / 65536.0f, pos[1] / 65536.0f, pos[2] / 65536.0f};
    seed.spawn_position = seed.position;
    section_effect(world, "Effect_FlashPop", seed.position);
    const auto handle = world.item_piece_spawner
            ? world.item_piece_spawner->spawn_item_piece(seed) : EntityHandle{};
    Entity *piece = world.registry.get(handle);
    if (!piece) return;
    piece->spawned_piece_mask = entity.spawned_piece_mask;
    piece->death_anim_state = (section & 255) << 8;
    for (int i = 0; i < section; ++i) piece->spawned_piece_mask |= 1u << (i & 31);
    const int count = static_cast<int8_t>(traits.husk_sub_part_count);
    int remaining = count;
    for (int i = 0; i < count; ++i)
        if ((piece->spawned_piece_mask & (1u << (i & 31))) && remaining) --remaining;
    piece->section_pitch_rate = remaining == 1 ? 190887424 :
            remaining == 2 ? 0x16C16C0 : 0x1000000;
    const int32_t speed = remaining == 1 ? 4 * 65536 : remaining == 2 ? 2 * 65536 : 65536;
    hit.heading = io::bam_add(hit.heading, 11930464 * (int(world.crt_rand.next() % 20) - 10));
    const int32_t zero[3] = {}, local[3] = {speed, 0, 0};
    collision_matrix_from_euler(hit.heading, hit.pitch, hit.roll, zero).rotate_point(local, pos);
    piece->veh.vel_x = pos[0]; piece->veh.vel_y = pos[1]; piece->veh.slide_z = pos[2];
    if (world.crt_rand.next() & 1) piece->death_anim_state |= 0xF0;
    for (int i = section; i < count; ++i) entity.spawned_piece_mask |= 1u << (i & 31);
    // Retail drops baked terrain tiles over the old bounds. Our model masks
    // are consumed directly by the live collision/present walkers.
}
void fit_section_to_ground(Entity &entity, const int32_t end[3], int32_t ground) {
    // Per-product Q16 rounding, signed sum, x87 nearest sqrt, then <<8.
    // The angle conversions truncate after multiplying the witnessed double.
    // [orig: Entity_InitFloatingPhysics @ 0x4A8581, 0x4A8735]
    const int32_t dx = io::bam_sub(end[0], int32_t(entity.position.x * 65536));
    const int32_t dy = io::bam_sub(end[1], int32_t(entity.position.y * 65536));
    const int32_t dz = io::bam_sub(ground, int32_t(entity.position.z * 65536));
    const int32_t squared = io::bam_add(int32_t((int64_t(dx) * dx + (io::kFp16OneInt / 2)) >> 16),
            int32_t((int64_t(dy) * dy + (io::kFp16OneInt / 2)) >> 16));
    const int32_t root = squared >= 0 ? int32_t(std::nearbyint(std::sqrt(double(squared)))) : INT32_MIN;
    const int32_t planar = int32_t(uint32_t(root) << 8);
    const int32_t heading = int32_t(int64_t(std::atan2(double(dy), double(dx)) * io::kBamPerRadian));
    const int32_t slope = int32_t(int64_t(std::atan2(double(dz), double(planar)) * io::kBamPerRadian));
    entity.yaw = float(mission_yaw_deg_from_bam_heading(heading));
    entity.pitch = float(io::bam_sub(0x40000000, slope) * kDegreesPerBam);
}
bool tick_tower_section(World &world, Entity &entity) {
    // [orig: Entity_InitFloatingPhysics @ 0x4A8340;
    // Entity_UpdateFloatingPhysics @ 0x4A8220]
    const auto *traits = world.tables.item_death_traits.get(entity.item_id);
    if (!traits) return true;
    if (entity.death_motion == DeathMotionMode::SectionSettled) {
        // Retail computes a point along the section, but the emitter call
        // actually passes the placement matrix's translation every time.
        // sub_5F6FE0 tags the group with this entity but its return is discarded:
        // no +0x1CC store, transform following, or owner-release path is armed.
        section_effect(world, traits->particledeath, entity.position);
        if (uint32_t(entity.section_pitch_rate) >= uint32_t(entity.section_pitch_accel))
            entity.death_motion = DeathMotionMode::None;
        else
            entity.section_pitch_rate = io::bam_add(entity.section_pitch_rate, 0x40000);
        return true;
    }
    int32_t height = 0;
    const int count = static_cast<int8_t>(traits->husk_sub_part_count);
    for (int i = (uint32_t(entity.death_anim_state) >> 8) & 255; i < count; ++i) {
        if ((entity.spawned_piece_mask & (1u << (i & 31))) == 0 &&
                size_t(i) < traits->model_section_heights_q16.size())
            height = io::bam_add(height, traits->model_section_heights_q16[i]);
    }
    const uint8_t old_flags = uint8_t(entity.death_anim_state);
    const int32_t yaw = bam_heading_from_mission_yaw_deg(entity.yaw);
    entity.yaw = float(mission_yaw_deg_from_bam_heading(
            io::bam_add(yaw, (old_flags & 0xF0) ? -0x1000000 : 0x1000000)));
    if ((old_flags & 2) == 0) {
        const int32_t pitch = bam_from_degrees_wrapped(entity.pitch);
        entity.section_pitch_accel = io::bam_add(entity.section_pitch_accel,
                pitch <= 0x40000000 ? 0x1000000 : -0x1000000);
        entity.pitch = float(io::bam_add(io::bam_add(pitch, entity.section_pitch_accel),
                entity.section_pitch_rate) * kDegreesPerBam);
    }
    if (old_flags & 3) {
        entity.veh.vel_x >>= 1; entity.veh.vel_y >>= 1;
        if (entity.veh.slide_z < 0) entity.veh.slide_z = 0;
    }
    int32_t pos[3] = {int32_t(entity.position.x * 65536),
            int32_t(entity.position.y * 65536), int32_t(entity.position.z * 65536)};
    const int32_t ground = section_ground(world, entity.position);
    const int32_t z = io::bam_add(pos[2], entity.veh.slide_z);
    bool bottom_contact = false;
    if (z > ground && entity.veh.slide_z != 0) {
        pos[0] = io::bam_add(pos[0], entity.veh.vel_x);
        pos[1] = io::bam_add(pos[1], entity.veh.vel_y);
        pos[2] = z;
        entity.veh.slide_z = io::bam_sub(entity.veh.slide_z, (io::kFp16OneInt / 2));
    } else {
        if (!entity.section_bounced) {
            entity.veh.vel_x >>= 2; entity.veh.vel_y >>= 2;
            entity.veh.slide_z = io::bam_sub(0, entity.veh.slide_z) >> 3;
            entity.section_bounced = true;
        }
        bottom_contact = true; pos[2] = ground;
    }
    entity.position = {pos[0] / 65536.0f, pos[1] / 65536.0f, pos[2] / 65536.0f};
    // Every InitFloatingPhysics contact/settle effect is the interned def+0x412
    // handle: particledeath, the word the SectionSettled leg above reads too.
    // [orig: submit_effect_descriptor(.., word def+0x412) @0x4A84B8 (bottom),
    //  @0x4A8565 (top), @0x4A8880 (the settle loop)]
    if (bottom_contact && !(old_flags & 1))
        section_effect(world, traits->particledeath, entity.position);
    const int32_t local[3] = {0, 0, height};
    int32_t end[3];
    entity_placement_matrix(entity).transform_point(local, end);
    const Vec3 end_position{end[0] / 65536.0f, end[1] / 65536.0f, end[2] / 65536.0f};
    const int32_t end_ground = section_ground(world, end_position);
    bool top_contact = false;
    if (end[2] < end_ground) {
        end[2] = end_ground;
        section_effect(world, traits->particledeath,
                {end[0] / 65536.0f, end[1] / 65536.0f, end[2] / 65536.0f});
        if (bottom_contact) {
            fit_section_to_ground(entity, end, end_ground);
            entity_placement_matrix(entity).transform_point(local, end);
        }
        entity.section_pitch_accel = 0;
        top_contact = true;
        if (!entity.section_bounced && !bottom_contact) {
            entity.veh.vel_x >>= 1; entity.veh.vel_y >>= 1;
            entity.veh.slide_z = io::bam_sub(0, entity.veh.slide_z) >> 3;
            entity.section_bounced = true;
            entity.section_pitch_rate = io::bam_sub(0, int32_t(
                    (13107LL * entity.section_pitch_rate + (io::kFp16OneInt / 2)) >> 16));
            top_contact = false;
        } else if (bottom_contact) {
            fit_section_to_ground(entity, end, end_ground);
            entity.section_pitch_accel = height;
            entity.section_pitch_rate = height >> 1;
            const auto matrix = entity_placement_matrix(entity);
            for (int32_t step = height >> 1; step > 0; step -= 0x40000) {
                const int32_t point[3] = {0, 0, step};
                matrix.transform_point(point, end);
                section_effect(world, traits->particledeath,
                        {end[0] / 65536.0f, end[1] / 65536.0f, end[2] / 65536.0f});
            }
            entity.death_motion = DeathMotionMode::SectionSettled;
            return true;
        }
    }
    entity.death_anim_state = (entity.death_anim_state & ~3) |
            (int(bottom_contact) + 2 * int(top_contact));
    return true;
}
CollisionMatrix palm_matrix(const Entity &entity) {
    const int32_t pos[3] = {int32_t(entity.position.x * 65536),
            int32_t(entity.position.y * 65536), int32_t(entity.position.z * 65536)};
    // This callback explicitly uses the unscaled Euler builder.
    // [orig: TerrainTile_TransformPointFromSector @ 0x53BDB0]
    return collision_matrix_from_euler(bam_heading_from_mission_yaw_deg(entity.yaw),
            bam_from_degrees_wrapped(entity.pitch), bam_from_degrees_wrapped(entity.roll), pos);
}
void palm_transition(World &world, Entity &entity, int section) {
    // Read the PUSH operands, not the decompiler's phantom ECX argument:
    // 0->1 spawns 16, 2->1 spawns 32, 0->2 spawns 17.
    // [orig: sub_53C3E0 @ 0x53C3E0]
    int type = 0;
    if (section == 0 || section == 1 || section == 3 || section == 4) {
        if (entity.palm_state == 0) type = 16;
        else if (entity.palm_state == 2) type = 32;
        if (type) entity.palm_state = 1;
    } else if ((section == 2 || section == 5) && entity.palm_state == 0) {
        entity.palm_state = 2;
        type = 17;
    }
    if (!type) return;
    // [orig: spawn_projectile_from_tile @ 0x53C1C0]
    const auto *traits = world.tables.item_death_traits.get(entity.item_id);
    const size_t pivot = type == 17 ? 1 : 0;
    if (traits && pivot < traits->model_pivots_q16.size()) {
        Entity seed;
        seed.item_id = 900;
        seed.kind = EntityKind::Building;
        seed.position = entity.position;
        int32_t pos[3];
        palm_matrix(entity).transform_point(traits->model_pivots_q16[pivot].data(), pos);
        seed.position = {pos[0] / 65536.0f, pos[1] / 65536.0f, pos[2] / 65536.0f};
        seed.spawn_position = seed.position;
        seed.yaw = entity.yaw; // pitch/roll in the zeroed fragment template stay zero.
        seed.uniform_scale_q16 = 65536;
        seed.engine_flags = kEntityFlagDead;
        seed.alive = false;
        seed.health = 20;
        seed.item_section_piece = true;
        seed.palm_sections = true;
        seed.palm_state = type;
        seed.death_motion = DeathMotionMode::PalmPiece;
        const int32_t x = int32_t(entity.position.x * 65536);
        const int32_t y = int32_t(entity.position.y * 65536);
        world.prng16_b_state = uint32_t(entity.handle.slot() + 8 * type) +
                uint32_t((y >> 10) & ~63) + uint32_t((x >> 10) & ~63);
        seed.veh.vel_x = (world.next_prng16_b() & 8191) - 4096;
        seed.veh.vel_y = (world.next_prng16_b() & 8191) - 4096;
        seed.veh.slide_z = (world.next_prng16_b() & 8191) + 2048;
        if (world.item_piece_spawner) world.item_piece_spawner->spawn_item_piece(seed);
    }
    if (world.rules.mp_session && traits && !traits->sound_death.empty())
        world.out.destruction.sounds.push_back({traits->sound_death, entity.position});
}
void palm_dead(World &world, Entity &entity) {
    entity.alive = false;
    entity.engine_flags |= kEntityFlagDead;
    if ((entity.engine_flags & kEntityFlagHusk) == 0) {
        entity.engine_flags |= kEntityFlagHusk;
        world.out.destruction.husk_swaps.push_back({entity.net_id, entity.handle.packed,
                entity.bms_id, entity.spawn_origin, entity.item_id,
                entity.spawned_piece_mask, entity.position});
        ++world.out.destruction.items_destroyed;
    }
}
} // namespace

// [orig: Entity_UpdateSectionDamage @ 0x4406A0]
bool tower_item_event(World &world, Entity &entity, int phase, ItemHitContext hit) {
    const auto *traits = world.tables.item_death_traits.get(entity.item_id);
    if (!traits) return false;
    const int count = static_cast<int8_t>(traits->husk_sub_part_count);
    const auto add_damage = [&] {
        // Retail's client leg bounds only section > 0 (@0x4406C2) and its
        // unchecked +0x2BA byte index can overwrite adjacent entity fields;
        // both legs take the authority bound `section > (char)huskSubPartCount`
        // (@0x440722) so a malformed section stays inert in this owned bank
        // (D-ITEM-7).
        if (hit.section <= 0 || hit.section > count) return;
        uint8_t &damage = entity.item_section_damage[hit.section];
        damage = uint8_t(damage + uint8_t(hit.damage));
        if (damage > 14) {
            spawn_tower_section(world, entity, *traits, hit.section, hit);
            tower_husk(world, entity);
        }
    };
    if (!world.rules.logic_authority && phase == 1) {
        add_damage(); entity.class_think_ticks = 32; return false;
    }
    if (entity.engine_flags & kEntityFlagDead) {
        entity.class_think_ticks = 1024; return false;
    }
    if (phase == 1) {
        if (hit.section > count) { entity.class_think_ticks = 32; return false; }
        add_damage();
    }
    int section = count - 1;
    bool complete = count == 1;
    if (section > 0) {
        while (section > 0 && entity.item_section_damage.read(section) >= 14) --section;
        complete = section == 0;
    }
    if (complete) {
        emit_item_state(world, entity, -1);
        entity.engine_flags |= kEntityFlagDead; entity.alive = false;
        entity.health = 0; entity.class_think_ticks = 1024;
    } else {
        entity.engine_flags &= ~kEntityFlagDead; entity.alive = true;
        entity.health = 100; entity.class_think_ticks = 32;
    }
    return true;
}

// [orig: WeaponOverlay_HandleDamage @ 0x53C4C0]
void palm_item_event(World &world, Entity &entity, int phase, ItemHitContext hit) {
    entity.palm_sections = true;
    if (!world.rules.logic_authority) {
        if (phase != 4) { entity.class_think_ticks = 1024; return; }
        world.out.scars.clear_entity(entity.handle);
        palm_transition(world, entity, hit.section);
        if (hit.section == -1) palm_dead(world, entity);
        entity.class_think_ticks = 32;
        return;
    }
    if (entity.engine_flags & kEntityFlagDead) {
        entity.class_think_ticks = 1024;
        return;
    }
    if (phase == 1) {
        const int group = hit.section == 0 || hit.section == 1 ||
                hit.section == 3 || hit.section == 4 ? 0 :
                hit.section == 2 || hit.section == 5 ? 1 : -1;
        if (group >= 0) {
            entity.palm_damage[group] = io::bam_add(entity.palm_damage[group], hit.damage);
            if (entity.palm_damage[group] > 100) {
                world.out.scars.clear_entity(entity.handle);
                palm_transition(world, entity, hit.section);
                emit_item_state(world, entity, hit.section);
            }
        }
        entity.class_think_ticks = 32;
        if (entity.health > 0) return;
    } else if (entity.health > 0) {
        entity.class_think_ticks = ((world.next_prng16() >> 14) + 5) << 6;
        return;
    }
    world.out.scars.clear_entity(entity.handle);
    emit_item_state(world, entity, -1);
    palm_dead(world, entity);
    entity.class_think_ticks = 32;
}

// [orig: CTerrainMap_BuildSectorTransformMatrices @ 0x53BF10]
uint32_t item_hidden_sections(const Entity &entity) {
    uint32_t mask = entity.section_mask;
    if (entity.engine_flags & kEntityFlagHusk)
        mask |= entity.spawned_piece_mask;
    if (!entity.palm_sections) return mask;
    switch (entity.palm_state) {
    case 0: return mask | 0x38; // 3,4,5
    case 1: return mask | 0x36; // 1,2,4,5
    case 2: return mask | 0x1C; // 2,3,4
    case 16: return mask | 0x29; // 0,3,5
    case 17: return mask | 0x3B; // 0,1,3,4,5
    case 32: return mask | 0x0D; // 0,2,3
    default: return mask;
    }
}
Vec3 item_section_render_position(const World &world, const Entity &entity) {
    if (entity.item_section_piece && !entity.palm_sections && entity.spawned_piece_mask) {
        // The death render callback pivots around the first surviving HUSK
        // section, not the intact-model pivot used by the spawn.
        // [orig: Entity_BuildDeathSectionTransforms @ 0x492AF0]
        const auto *traits = world.tables.item_death_traits.get(entity.item_id);
        if (traits) {
            size_t first = 0;
            while (first < traits->husk_section_origins_q16.size() &&
                    (entity.spawned_piece_mask & (1u << (first & 31)))) ++first;
            if (first == traits->husk_section_origins_q16.size()) first = 0;
            if (first < traits->husk_section_origins_q16.size()) {
                int32_t offset[3], position[3];
                for (int i = 0; i < 3; ++i)
                    offset[i] = io::bam_sub(0, traits->husk_section_origins_q16[first][i]);
                entity_placement_matrix(entity).transform_point(offset, position);
                return {position[0] / 65536.0f, position[1] / 65536.0f, position[2] / 65536.0f};
            }
        }
    }
    if (!entity.palm_sections ||
            (entity.palm_state != 16 && entity.palm_state != 17 && entity.palm_state != 32))
        return entity.position;
    const auto *traits = world.tables.item_death_traits.get(entity.item_id);
    const size_t pivot = entity.palm_state == 17 ? 1 : 0;
    if (!traits || pivot >= traits->model_pivots_q16.size()) return entity.position;
    int32_t local[3], pos[3];
    for (int i = 0; i < 3; ++i)
        local[i] = io::bam_sub(0, traits->model_pivots_q16[pivot][i]);
    palm_matrix(entity).transform_point(local, pos);
    return {pos[0] / 65536.0f, pos[1] / 65536.0f, pos[2] / 65536.0f};
}

// [orig: update_entity_physics_step @ 0x53BE10]
bool tick_item_section_motion(World &world, Entity &entity) {
    if (entity.death_motion == DeathMotionMode::SectionFalling ||
            entity.death_motion == DeathMotionMode::SectionSettled)
        return tick_tower_section(world, entity);
    if (entity.death_motion != DeathMotionMode::PalmPiece) return false;
    int32_t pos[3] = {int32_t(entity.position.x * 65536),
            int32_t(entity.position.y * 65536), int32_t(entity.position.z * 65536)};
    for (int i = 0; i < 3; ++i) entity.saved_live_pos[i] = pos[i];
    entity.saved_live_yaw = bam_heading_from_mission_yaw_deg(entity.yaw);
    entity.saved_live_pitch = bam_from_degrees_wrapped(entity.pitch);
    entity.saved_live_roll = bam_from_degrees_wrapped(entity.roll);
    entity.saved_live_valid = true;
    const auto *terrain = world.tables.terrain;
    const int32_t ground = io::bam_add(terrain && terrain->valid() ? int32_t(
            terrain::height_field_height_world_bilinear(*terrain,
                entity.position.x, -entity.position.y) * 65536) : 0, 16384);
    const int32_t z = io::bam_add(pos[2], entity.veh.slide_z);
    if (z <= ground) {
        entity.veh.vel_x >>= 2;
        entity.veh.vel_y >>= 2;
        entity.veh.slide_z = io::bam_sub(0, entity.veh.slide_z) >> 2;
        if (entity.veh.slide_z < 167) entity.veh.slide_z = 0;
        pos[2] = ground;
    } else {
        pos[0] = io::bam_add(pos[0], entity.veh.vel_x);
        pos[1] = io::bam_add(pos[1], entity.veh.vel_y);
        pos[2] = z;
        entity.yaw = float(mission_yaw_deg_from_bam_heading(io::bam_add(
                entity.saved_live_yaw, entity.palm_state == 17 ? 0x2000000 : 0x800000)));
        if (entity.palm_state != 17 && uint32_t(entity.saved_live_pitch) < 0x40000000u)
            entity.pitch = float(io::bam_add(entity.saved_live_pitch, 0x1000000) * kDegreesPerBam);
        entity.veh.slide_z = io::bam_sub(entity.veh.slide_z, 167);
    }
    entity.position = {pos[0] / 65536.0f, pos[1] / 65536.0f, pos[2] / 65536.0f};
    entity.engine_flags &= ~0x20000u;
    return true;
}
} // namespace opennova::world
