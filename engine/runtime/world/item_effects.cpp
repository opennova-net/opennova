#include <runtime/world/item_effects.h>

#include <cstdio>
#include <cmath>
#include <base/io/bam.h>
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

// The wire handle domain: 16-bit, 0xffff = none (a negative value = none).
constexpr int32_t kWireHandleNone = 0xffff;

void push_alias(std::vector<std::string> &out, const char *prefix, long long value) {
	char buffer[64];
	std::snprintf(buffer, sizeof(buffer), "%s:%lld", prefix, value);
	out.emplace_back(buffer);
}

} // namespace

// [orig: resolve_item_materials_and_spawn_bone_trails @ 0x522ee0 — pool 1
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

void item_effect_identity_aliases(int32_t net_id, int32_t bms_id,
		int64_t spawn_origin, int32_t wire_handle,
		std::vector<std::string> &out) {
	out.clear();
	const bool has_wire_identity = wire_handle >= 0 && wire_handle != kWireHandleNone;
	if (has_wire_identity) {
		push_alias(out, "wire", wire_handle);
	}
	// Synthetic items.def attachments all carry the same sentinel origin and
	// no authored BMS/net identity. Their packed runtime handle is therefore
	// the only alias that distinguishes siblings on the same carrier.
	if (has_wire_identity && bms_id == 0 &&
			(spawn_origin == -1 ||
					spawn_origin == static_cast<int64_t>(kSpawnOriginNone))) {
		return;
	}
	if (net_id > 0) {
		push_alias(out, "net", net_id);
	}
	if (bms_id > 0) {
		push_alias(out, "bms", bms_id);
	}
	if (spawn_origin > 0) {
		push_alias(out, "origin", spawn_origin);
	}
}

bool item_effect_aliases_intersect(const std::vector<std::string> &left,
		const std::vector<std::string> &right) {
	for (const std::string &alias : left) {
		for (const std::string &other : right) {
			if (alias == other) {
				return true;
			}
		}
	}
	return false;
}

// The first-16 case-insensitive scan is ONE impl in engine/formats/threedi
// [orig: ItemDef_GetBoneMaskByName @ 0x49ea40; duplicate names all set their
// bit]; a name matching nothing (or no authored name) still spawns ONE
// emitter at the entity origin — the spawn_count==0 leg
// [orig: Entity_SpawnBoneTrailEffect @ 0x43c097 -> submit_effect_descriptor
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
            bam_heading_from_mission_yaw_deg(entity.yaw),
            bam_from_degrees_wrapped(entity.pitch), bam_from_degrees_wrapped(entity.roll), origin);
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
}

ItemEmitterSystem &ItemEmitterSystem::operator=(ItemEmitterSystem &&other) noexcept {
    if (this == &other) return *this;
    reset();
    scene_ = std::move(other.scene_);
    bindings_ = std::move(other.bindings_);
    next_owner_ = other.next_owner_;
    owns_clock_ = other.owns_clock_;
    return *this;
}

void ItemEmitterSystem::bind_scene(std::shared_ptr<particle::EffectScene> scene, bool owns_clock) {
    owns_clock_ = owns_clock;
    if (scene_ == scene) return;
    reset();
    scene_ = std::move(scene);
}

void ItemEmitterSystem::reset() {
    if (scene_) {
        std::vector<particle::EffectOwnerPoseUpdate> absent;
        for (const auto &binding : bindings_) absent.push_back({binding.owner, {}, false});
        scene_->apply_owner_poses(absent);
    }
    bindings_.clear();
}

void ItemEmitterSystem::event(World &world, Entity &entity,
        const ItemDeathTraits &traits, int phase) {
    if (phase != 0 || !traits.model_loaded || !entity.has_item_def) return;
    for (auto it = bindings_.begin(); it != bindings_.end(); ++it) {
        if (it->handle != entity.handle.packed || it->lifetime != entity.registry_spawn_id) continue;
        const bool live = scene_ && scene_->contains_group(it->group);
        if (live) scene_->apply_owner_poses({{it->owner, {}, false}});
        bindings_.erase(it);
        if (live) {
            entity.class_think_ticks = 15;
            return;
        }
        break;
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
    request.binding = particle::EffectBinding::FollowOwner;
    // Native world owners use the upper half of the token domain. Device owner
    // keys allocate upward from one; no reverse Godot key is needed here.
    request.owner.value = 0x8000000000000000ull | next_owner_++;
    request.owner_relative_pose.position = effect_axes(traits.particlefx_point_q16);
    request.owner_relative_pose.forward = effect_axes(traits.particlefx_direction_q16);
    request.source_tick = world.logic_tick;
    scene_->apply_owner_poses({{request.owner, emitter_owner_pose(entity), true}});
    const auto receipt = scene_->spawn(request);
    if (receipt.spawned()) bindings_.push_back({entity.handle.packed,
            entity.registry_spawn_id, receipt.group, request.owner});
    else scene_->apply_owner_poses({{request.owner, {}, false}});
}

void ItemEmitterSystem::sync_owners(World &world) {
    if (!scene_) return;
    std::vector<particle::EffectOwnerPoseUpdate> poses;
    for (auto it = bindings_.begin(); it != bindings_.end();) {
        const Entity *entity = world.registry.get(EntityHandle{it->handle});
        const bool live = entity != nullptr && entity->registry_spawn_id == it->lifetime &&
                scene_->contains_group(it->group);
        poses.push_back({it->owner, live ? emitter_owner_pose(*entity) : particle::EffectPose{}, live});
        if (!live) it = bindings_.erase(it);
        else ++it;
    }
    scene_->apply_owner_poses(poses);
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
    world.out.destruction.effects.push_back({"Effect_AirExp",pos,{}});
    const int index=world.tables.ammo.index_of("kz_M406HE");
    if (world.rules.logic_authority) if (const auto *ammo=world.tables.ammo.by_index(index)) {
        ExplosionEntry blast;
        blast.pos=pos; blast.ammo_index=index; blast.type=ammo->kztype; blast.hit_word=1;
        if (source) blast.owner=source->handle;
        world.explosions.queue_explosion(world,blast);
    }
    // AmmoDef_GetExplosionRadius is a misleading IDB name: the tag-5 row's
    // sound, not a radius. It seeds the result from dword_A2EB80 — ammo def
    // 0's static effect bank, row 5 dword +8 (AmmoTable::default_explosion_sound,
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
        const std::string &sound = row.authored ? row.sound : world.tables.ammo.default_explosion_sound;
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
            // dword_A2ECF4 is zero-filled PE data with no direct/address-table
            // writer in this retail image. Its caller still consumes the draws.
        }
    }
}

} // namespace opennova::world
