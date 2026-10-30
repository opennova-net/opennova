#include <runtime/world/item_effects.h>

#include <algorithm>
#include <cstdio>
#include <base/io/bam.h>
#include <runtime/particle/script_effects.h>
#include <runtime/world/world.h>
#include <runtime/world/collision.h>
#include <runtime/world/angle.h>

#include <formats/def/def.h>
#include <runtime/world/entity.h>

using namespace opennova::def;
using namespace opennova::threedi;

namespace opennova::world {

namespace {

constexpr uint32_t kPowerupBit = DEF_ITEM_ATTRIB_POWERUP;
constexpr uint32_t kPlayerControlBit = DEF_ITEM_ATTRIB_PLAYERCONTROL;

constexpr int kKindMarker = static_cast<int>(EntityKind::Marker);
constexpr int kKindItem = static_cast<int>(EntityKind::Item);
constexpr int kKindBuilding = static_cast<int>(EntityKind::Building);

} // namespace

// [orig: Game_ResolveItemMaterialsAndSpawnBoneTrails @ 0x522ee0 — pool 1
//  skips attrib 0x42, pools 2/3 skip attrib 0x2, pool 0 is never walked]
bool item_effect_pool_allows(int kind, uint32_t attrib) {
	if (kind == kKindItem) {
		return (attrib & (kPowerupBit | kPlayerControlBit)) == 0;
	}
	if (kind == kKindBuilding || kind == kKindMarker) {
		return (attrib & kPowerupBit) == 0;
	}
	return false;
}

bool item_effect_controller_allows(int kind, uint32_t attrib) {
	// The occupied-controller pass bypasses only PlayerControl. The
	// independent powerup exclusion remains intact.
	return kind == kKindItem &&
			(attrib & (kPowerupBit | kPlayerControlBit)) == kPlayerControlBit;
}

// The first-16 case-insensitive scan is ONE impl in engine/formats/threedi
// [orig: ItemDef_GetBoneMaskByName @ 0x49ea40; duplicate names all set their
// bit]; a name matching nothing (or no authored name) still spawns ONE
// emitter at the entity origin — the spawn_count==0 leg
// [orig: Entity_SpawnBoneTrailEffect @ 0x43c097 -> Effect_SubmitDescriptor
//  @ 0x43c0a4 at entity->Position].
ItemEffectAttachPlan item_effect_attach_plan(const Threedi3di3 &model,
		const char *userpoint_name) {
	ItemEffectAttachPlan plan;
	if (userpoint_name != nullptr && userpoint_name[0] != '\0') {
		plan.mask = threedi_3di3_user_point_mask(&model, userpoint_name);
		for (int i = 0; i < THREEDI_USER_POINT_SCAN_LIMIT; ++i) {
			if ((plan.mask & (1u << i)) != 0) {
				plan.user_points.push_back(i);
			}
		}
	}
	plan.origin_fallback = plan.user_points.empty();
	return plan;
}


namespace {
particle::Vec3 effect_axes(const int32_t xyz[3]) {
    return {xyz[0] / 65536.0f, xyz[2] / 65536.0f, -xyz[1] / 65536.0f};
}
particle::EffectPose emitter_owner_pose(const Entity &entity) {
    const int32_t origin[3] = {};
    const auto matrix = collision_matrix_from_euler(
            entity.heading,
            entity.pitch_bam, entity.roll_bam, origin);
    particle::EffectPose pose;
    pose.position = {entity.position.x, entity.position.z, -entity.position.y};
    const int32_t scale = entity.uniform_scale_q16 != 0 ? entity.uniform_scale_q16 : 65536;
    const int32_t local[3][3] = {{scale, 0, 0}, {0, 0, scale}, {0, -scale, 0}};
    particle::Vec3 *basis[] = {&pose.right, &pose.up, &pose.forward};
    for (int i = 0; i < 3; ++i) {
        int32_t rotated[3];
        matrix.rotate_point(local[i], rotated);
        *basis[i] = effect_axes(rotated);
    }
    return pose;
}

// The pool a row lives in, as the slot-A walk's gates read it (EntityKind's ordinal).
int entity_pool_kind(const Entity &entity) {
    return static_cast<int>(entity.kind);
}

// The descriptor spawn every slot-A emitter takes: world-bound, tagged with the entity.
particle::EffectSpawnRequest slot_a_request(World &world, particle::EffectScene &scene,
        const std::string &effect, const particle::EffectPose &pose, const int32_t at[3]) {
    particle::EffectSpawnRequest request;
    request.effect = scene.intern(effect);
    request.pose = pose;
    request.binding = particle::EffectBinding::World;
    request.source_tick = world.logic_tick;
    request.kill_plane_y = effect_water_height(world);
    request.section_gate = effect_section_gate(world, at);
    return request;
}
} // namespace

// [orig: CEffectWorld_SpawnEmitterAtPosition @ 0x5F6EFB (the tag), @ 0x5F6F5A]
particle::EffectSectionGate effect_section_gate(World &world, const int32_t position[3]) {
    particle::EffectSectionGate gate;
    gate.tagged = true;
    if (world.collision != nullptr) {
        BlinkAccum accum;
        world.collision->query_blink_boxes_at_point(world, position, accum);
        for (size_t i = 0; i < gate.blink_hits.size(); ++i) gate.blink_hits[i] = accum.hits[i];
    }
    return gate;
}

particle::EffectSectionGate effect_section_gate(World &world, const Vec3 &position) {
    const int32_t words[3] = {to_fixed(position.x), to_fixed(position.y), to_fixed(position.z)};
    return effect_section_gate(world, words);
}

float effect_water_height(const World &world) {
    return float(world.env.water_z) / 65536.0f;
}

particle::Vec3 effect_matrix_point(const CollisionMatrix &matrix, const int32_t local[3]) {
    int32_t out[3];
    matrix.transform_point(local, out);
    return effect_axes(out);
}

particle::Vec3 effect_matrix_direction(const CollisionMatrix &matrix, const int32_t local[3]) {
    int32_t out[3];
    matrix.rotate_point(local, out);
    return effect_axes(out);
}

bool emitter_update_runs(const World &world, const Entity &entity) {
    const std::vector<PlayerTemplate> &rows = world.tables.item_rows;
    if (rows.empty()) return true;
    const int32_t index = entity.item_type_index;
    if (index <= 0 || static_cast<size_t>(index) >= rows.size()) return false;
    return rows[static_cast<size_t>(index)].move_row != PhysicsClass::Null;
}

bool emitter_transform(const World &world, int32_t item_id, bool has_model,
        const CollisionMatrix &matrix, const EntityPosition &position, particle::Vec3 &at,
        particle::Vec3 &forward) {
    const ItemDeathTraits *traits = world.tables.item_death_traits.get(item_id);
    bool aimed = false;
    if (has_model && traits != nullptr && traits->has_particlefx_point) {
        at = effect_matrix_point(matrix, traits->particlefx_point_q16);
        forward = effect_matrix_direction(matrix, traits->particlefx_direction_q16);
        aimed = true;
    } else {
        at = effect_point(position);
    }
    if (traits != nullptr && traits->unit_type >= 5 && traits->unit_type <= 8) {
        const float water = effect_water_height(world);
        if (at.y < water) at.y = water;
    }
    return aimed;
}

ItemEmitterSystem &ItemEmitterSystem::operator=(ItemEmitterSystem &&other) noexcept {
    if (this == &other) return *this;
    reset();
    scene_ = std::move(other.scene_);
    held_ = std::move(other.held_);
    attached_ = std::move(other.attached_);
    pending_hidden_ = std::move(other.pending_hidden_);
    controlled_ = std::move(other.controlled_);
    owns_clock_ = other.owns_clock_;
    producers_open_ = other.producers_open_;
    return *this;
}

bool ItemEmitterSystem::holds(const std::vector<Lifetime> &rows, const Entity &entity) {
    for (const Lifetime &row : rows)
        if (row.handle == entity.handle.packed && row.lifetime == entity.registry_spawn_id)
            return true;
    return false;
}

void ItemEmitterSystem::put(std::vector<Lifetime> &rows, const Entity &entity) {
    for (Lifetime &row : rows)
        if (row.handle == entity.handle.packed) {
            row.lifetime = entity.registry_spawn_id;
            return;
        }
    rows.push_back({entity.handle.packed, entity.registry_spawn_id});
}

void ItemEmitterSystem::drop(std::vector<Lifetime> &rows, uint16_t handle) {
    rows.erase(std::remove_if(rows.begin(), rows.end(),
            [handle](const Lifetime &row) { return row.handle == handle; }), rows.end());
}

void ItemEmitterSystem::open_producers(World &world, const std::vector<std::string> &effect_names) {
    if (!scene_) return;
    for (const std::string &name : effect_names) scene_->intern(name);
    producers_open_ = true;
    attach_mission_effects(world);
    spawn_queued_script_effects(world);
}

void ItemEmitterSystem::submit_script_effect(World &world, const ScriptEffectEvent &event) {
    // The slot the group is stored in: its row's destroy releases it.
    if (event.store_slot)
        if (const Entity *entity = world.registry.get(event.owner)) hold_slot(*entity);
    if (producers_open()) {
        spawn_script_effect(world, event);
        return;
    }
    world.out.script_effects.push_back(event);
}

void ItemEmitterSystem::spawn_script_effect(World &world, const ScriptEffectEvent &event) {
    const particle::EffectSlotToken slot{event.store_slot ? event.slot : 0};
    int32_t position[3] = {event.position[0], event.position[1], event.position[2]};
    // A World-bound group follows no owner: none is named.
    particle::spawn_script_effect(*scene_, event, slot, {}, 0, effect_water_height(world),
            effect_section_gate(world, position));
}

void ItemEmitterSystem::spawn_queued_script_effects(World &world) {
    if (!producers_open() || world.out.script_effects.empty()) return;
    std::vector<ScriptEffectEvent> queued;
    queued.swap(world.out.script_effects);
    for (const ScriptEffectEvent &event : queued) spawn_script_effect(world, event);
}

void ItemEmitterSystem::store_positioned_emitter(World &world, const Entity &entity,
        const std::string &effect) {
    release_slot(entity);
    if (!producers_open() || effect.empty()) return;
    const Vec3 at = entity.position;
    particle::EffectSpawnRequest request;
    request.effect = scene_->intern(effect);
    // The descriptor's orientation word 0xFFFF8000 on z: (0, 0, -0.5), in the scene's frame
    // (x, z, -y) as every mission point and vector.
    request.pose = particle::descriptor_pose(effect_point(at), {0.0f, -0.5f, 0.0f});
    request.admission = particle::EffectAdmission::StoreOwned;
    request.binding = particle::EffectBinding::World;
    request.slot = entity_effect_slot(entity.handle.packed);
    request.source_tick = world.logic_tick;
    request.kill_plane_y = effect_water_height(world);
    const auto words = entity.position.words();
    request.section_gate = effect_section_gate(world, words.data());
    if (scene_->spawn(request).spawned()) hold_slot(entity);
}

void ItemEmitterSystem::store_damage_smoke(World &world, const Entity &entity,
        const std::string &effect, const Vec3 &position) {
    if (!producers_open() || effect.empty()) return;
    const particle::EffectSlotToken slot = entity_effect_slot(entity.handle.packed);
    if (scene_->slot_group(slot)) return;
    const int32_t at[3] = {to_fixed(position.x), to_fixed(position.y), to_fixed(position.z)};
    particle::EffectSpawnRequest request = slot_a_request(world, *scene_, effect,
            particle::descriptor_pose(effect_point(position), {}), at);
    request.admission = particle::EffectAdmission::StoreOwned;
    request.slot = slot;
    if (scene_->spawn(request).spawned()) hold_slot(entity);
}

void ItemEmitterSystem::bind_scene(std::shared_ptr<particle::EffectScene> scene, bool owns_clock) {
    owns_clock_ = owns_clock;
    if (scene_ == scene) return;
    reset();
    scene_ = std::move(scene);
}

void ItemEmitterSystem::reset() {
    held_.clear();
    attached_.clear();
    pending_hidden_.clear();
    controlled_.clear();
}

void ItemEmitterSystem::hold_slot(const Entity &entity) {
    put(held_, entity);
}

void ItemEmitterSystem::release_slot(const Entity &entity) {
    if (scene_) scene_->detach_slot(entity_effect_slot(entity.handle.packed));
    forget_slot(entity);
}

void ItemEmitterSystem::destroy_slot(const Entity &entity) {
    if (scene_) scene_->destroy_slot(entity_effect_slot(entity.handle.packed));
    forget_slot(entity);
}

void ItemEmitterSystem::forget_slot(const Entity &entity) {
    drop(held_, entity.handle.packed);
}

int ItemEmitterSystem::spawn_bone_trail(World &world, const Entity &entity) {
    // What the slot holds goes first, whoever stored it [orig: @ 0x43BF0B..0x43BF22].
    release_slot(entity);
    const ItemDeathTraits *traits = world.tables.item_death_traits.get(entity.item_id);
    if (traits == nullptr || traits->particlefx.empty() || !producers_open()) return 0;
    // The retail master particle switch refuses every spawn: the attach waits for its lift
    // (retry_hidden), the port's leg for the dev tools' switch.
    if (!scene_->spawn_enabled()) {
        put(pending_hidden_, entity);
        return 0;
    }
    drop(pending_hidden_, entity.handle.packed);
    const CollisionMatrix matrix = entity_placement_matrix(entity);
    const particle::EffectSlotToken slot = entity_effect_slot(entity.handle.packed);
    int spawned = 0;
    if (traits->model_loaded) {
        const size_t count = std::min<size_t>(traits->graphic_userpoints.size(),
                THREEDI_USER_POINT_SCAN_LIMIT);
        for (size_t i = 0; i < count; ++i) {
            if ((traits->particlefx_mask & (1u << i)) == 0) continue;
            const ModelUserPoint &point = traits->graphic_userpoints[i];
            int32_t at[3];
            matrix.transform_point(point.pos_q16, at);
            const particle::EffectPose pose = particle::descriptor_pose(
                    effect_matrix_point(matrix, point.pos_q16),
                    effect_matrix_direction(matrix, point.dir_q16));
            particle::EffectSpawnRequest request = slot_a_request(world, *scene_,
                    traits->particlefx, pose, at);
            request.admission = particle::EffectAdmission::StoreOwned;
            request.slot = slot;
            ++spawned;
            scene_->spawn(request);
        }
    }
    if (spawned == 0) {
        // One emitter at the entity's position, stored nowhere [orig: @ 0x43C097 -> @ 0x43C0A4].
        const auto words = entity.position.words();
        const particle::EffectSpawnRequest request = slot_a_request(world, *scene_,
                traits->particlefx, particle::descriptor_pose(effect_point(entity.position), {}),
                words.data());
        scene_->spawn(request);
        spawned = 1;
    }
    if (scene_->slot_group(slot)) hold_slot(entity);
    return spawned;
}

int ItemEmitterSystem::attach_entity_effects(World &world, const Entity &entity) {
    if (!producers_open() || !entity.has_item_def || holds(attached_, entity))
        return 0;
    const int kind = entity_pool_kind(entity);
    const bool claimed = entity.primary_occupant.valid();
    if (!item_effect_pool_allows(kind, entity.item_attrib) &&
            !(claimed && item_effect_controller_allows(kind, entity.item_attrib)))
        return 0;
    put(attached_, entity);
    if (claimed && item_effect_controller_allows(kind, entity.item_attrib)) put(controlled_, entity);
    return spawn_bone_trail(world, entity);
}

int ItemEmitterSystem::attach_mission_effects(World &world) {
    attached_.clear();
    pending_hidden_.clear();
    controlled_.clear();
    if (!producers_open()) return 0;
    int spawned = 0;
    for (int pool = 1; pool <= 3; ++pool) {
        const size_t used = std::min(world.registry.pool_used(pool), world.registry.pool_capacity(pool));
        for (size_t slot = 0; slot < used; ++slot) {
            Entity *entity = world.registry.get(EntityHandle::make(pool, static_cast<int>(slot)));
            if (entity != nullptr) spawned += attach_entity_effects(world, *entity);
        }
    }
    return spawned;
}

void ItemEmitterSystem::control_started(World &world, const Entity &vehicle) {
    if (!item_effect_controller_allows(entity_pool_kind(vehicle), vehicle.item_attrib)) return;
    put(controlled_, vehicle);
    put(attached_, vehicle);
    spawn_bone_trail(world, vehicle);
}

void ItemEmitterSystem::control_stopped(World &world, const Entity &vehicle) {
    (void)world;
    // The release is the claimant detach's PlayerControl leg [orig: the 0x40 test
    //  @ 0x4356EF..0x4356F4].
    if ((vehicle.item_attrib & kPlayerControlBit) == 0) return;
    drop(controlled_, vehicle.handle.packed);
    drop(pending_hidden_, vehicle.handle.packed);
    release_slot(vehicle);
}

int ItemEmitterSystem::retry_hidden(World &world) {
    if (!producers_open() || !scene_->spawn_enabled()) return 0;
    std::vector<Lifetime> pending;
    pending.swap(pending_hidden_);
    int spawned = 0;
    for (const Lifetime &row : pending) {
        Entity *entity = world.registry.get(EntityHandle{row.handle});
        if (entity == nullptr || entity->registry_spawn_id != row.lifetime) continue;
        // A PlayerControl item's attach waits for its controller still.
        const bool controller_leg = !item_effect_pool_allows(entity_pool_kind(*entity),
                entity->item_attrib);
        if (controller_leg && !holds(controlled_, *entity)) continue;
        spawned += spawn_bone_trail(world, *entity);
    }
    return spawned;
}

void ItemEmitterSystem::update_emitter_transform(World &world, const Entity &entity) {
    if (!scene_) return;
    const particle::EffectSlotToken slot = entity_effect_slot(entity.handle.packed);
    if (!scene_->slot_group(slot)) return;
    particle::Vec3 at;
    particle::Vec3 forward;
    const bool aimed = emitter_transform(world, entity.item_id, entity.has_graphic_model,
            entity_placement_matrix(entity), entity.position, at, forward);
    scene_->move_slot(slot, at, aimed ? &forward : nullptr);
}

ItemEmitterSystem::Counts ItemEmitterSystem::counts() const {
    Counts out;
    out.attached = static_cast<int>(attached_.size());
    out.pending_hidden = static_cast<int>(pending_hidden_.size());
    out.controlled = static_cast<int>(controlled_.size());
    out.held_slots = static_cast<int>(held_.size());
    return out;
}

void ItemEmitterSystem::event(World &world, Entity &entity,
        const ItemDeathTraits &traits, int phase) {
    if (phase != 0 || !traits.model_loaded || !entity.has_item_def) return;
    // A group the entity's slot holds, whichever producer stored it, is destroyed outright, its
    // particles with it, the slot cleared and the class clock set to 15 [orig: @0x43F91A..0x43F949,
    // Effect_DestroyEmitterIfLive called @0x43F935].
    const particle::EffectSlotToken slot = entity_effect_slot(entity.handle.packed);
    if (scene_ && scene_->slot_group(slot)) {
        destroy_slot(entity);
        entity.class_think_ticks = 15;
        return;
    }
    // The draw precedes the point/effect allocation checks, even on a miss.
    // It is one step of the inline dword_31BFBB8 rotate LCG (the owner of the
    // throwable fan stream), not PRNG_Next16 [orig: @0x43F999; the delay
    // store @0x43F9BF].
    const auto &delay = traits.regional_sounds[0];
    const uint64_t product = uint64_t(int64_t(delay.range_ticks)) * world.throwables.fan_prng() + 0x8000u;
    entity.class_think_ticks = io::bam_add(delay.base_ticks, int32_t(uint32_t(product >> 16)));
    if (!scene_ || !traits.has_particlefx_point || traits.particlefx.empty()) return;
    particle::EffectSpawnRequest request;
    request.effect = scene_->intern(traits.particlefx);
    // World-bound at the first masked point through the entity's pose; the pool walks re-pose it
    // with the slot (update_emitter_transform). Stored in the entity's slot, with
    // Entity_ClearOwnerSessionIfMatches as its death callback [orig: @0x43FA4F..0x43FA57, the
    // store @0x43FB14].
    particle::EffectPose relative;
    relative.position = effect_axes(traits.particlefx_point_q16);
    relative.forward = effect_axes(traits.particlefx_direction_q16);
    request.pose = particle::compose_pose(emitter_owner_pose(entity), relative);
    request.binding = particle::EffectBinding::World;
    request.admission = particle::EffectAdmission::StoreOwned;
    request.slot = slot;
    request.source_tick = world.logic_tick;
    if (scene_->spawn(request).spawned()) hold_slot(entity);
}

void ItemEmitterSystem::sync_slots(World &world) {
    if (!scene_) return;
    // A restore queues its baseline's script effects again; open, they spawn at this tick.
    spawn_queued_script_effects(world);
    // A held slot whose row was destroyed, or taken by another entity, is released: the destroy
    // releases the row's +0x1CC group [orig: Entity_Destroy @0x43E8EC..0x43E8F5]. Retail frees a
    // row only through that destroy, so a row our registry freed another way, or gave to another
    // entity, has no witness of its own: its group goes with it.
    for (auto it = held_.begin(); it != held_.end();) {
        const Entity *entity = world.registry.get(EntityHandle{it->handle});
        if (entity != nullptr && entity->registry_spawn_id == it->lifetime) {
            ++it;
            continue;
        }
        scene_->detach_slot(entity_effect_slot(it->handle));
        it = held_.erase(it);
    }
    if (owns_clock_) scene_->advance_simulation({1.0f / float(io::kTickHz)});
}

// [orig: Server_BroadcastExplosionEffect @0x508450;
// Entity_SpawnExplosionEffects @0x4399C0]
void spawn_item_explosion(World &world, const Entity *source, const FixedVec3 &position,
        int32_t heading, int count, bool broadcast) {
    if (broadcast && world.rules.logic_authority && world.rules.mp_session)
        world.out.entity_events.push_back(ItemExplosionEvent{source ? source->handle.packed : uint16_t(0xFFFF),
                uint8_t(count),position,heading});
    const Vec3 pos{position.x/65536.0f,position.y/65536.0f,position.z/65536.0f};
    world.out.destruction.effects.push_back({kItemExplosionEffect,pos,{}});
    const int index=world.tables.ammo.index_of(kItemExplosionAmmo);
    if (const auto *ammo=world.tables.ammo.by_index(index)) {
        ExplosionEntry blast;
        blast.pos.set_q16(position); // the words (D-NET-507)
        blast.ammo_index=index; blast.type=ammo->kztype;
        if (source) blast.owner=source->handle;
        // The gated entry: off the authority nothing queues [orig:
        //  Entity_SpawnExplosionEffects @0x439A06 -> WeaponEffect_QueueExplosion
        //  @0x4E8330 (@0x4E8333..0x4E8339)].
        world.explosions.queue_explosion_on_authority(world,blast);
    }
    // AmmoDef_GetExplosionRadius is a misleading IDB name: the tag-5 row's
    // sound, not a radius. It seeds the result from dword_A2EB80 — ammo def
    // 0's static effect bank, row 5 dword +8 (AmmoTable::null_bank[5].sound,
    // baked at table build) — then overwrites it with every tag-5 row of the
    // requested ammo, so an authored-but-'none' row yields silence while an
    // absent row yields def 0's fallback.
    // [orig: AmmoDef_GetExplosionRadius @0x409770 — `radius = dword_A2EB80`
    //  @0x40978c, the tag-5 overwrite @0x4097ab]
    int sound_ammo = source ? source->squib.damage_ammo_index : 0;
    if (source) {
        if (const auto *body = world.ai.for_handle(source->handle))
            sound_ammo = body->inf.last_advanced_ammo;
        for (const auto &device : world.throwables.devices)
            if (device.active && device.entity == source->handle &&
                    device.entity_spawn_id == source->registry_spawn_id)
                sound_ammo = device.ammo_index;
    }
    if (const auto *ammo=world.tables.ammo.by_index(sound_ammo)) {
        const AmmoImpactEffectRow &row = ammo->impact_effects[5];
        const std::string &sound = row.authored ? row.sound : world.tables.ammo.null_bank[5].sound;
        world.out.fire_sounds.play_with_distance_delay(sound.c_str(),
                pos, source ? source->bms_id : 0, source ? source->handle.packed : uint16_t(0xFFFF));
    }
    if (!world.rules.mp_session) {
        const int half=std::min(count,8)>>1;
        for (int i=0;i<2*half;++i) {
            world.throwables.fan_prng(); // heading
            world.throwables.fan_prng(); // pitch (first half <<15, second <<16)
            world.throwables.fan_prng(); // speed
            // Weapon_SpawnSingleProjectile @0x4EBE80 returns immediately:
            // g_SingleProjectileAmmoIndex is zero-filled PE data with no direct/address-table
            // writer in this retail image. Its caller still consumes the draws.
        }
    }
}

} // namespace opennova::world
