#include <runtime/world/script_effects.h>
#include <runtime/world/world.h>
#include <runtime/world/angle.h>
#include <base/io/bam.h>
#include <base/io/fixed.h>

#include <cmath>
#include <utility>

namespace opennova::world {
namespace {

ScriptEffectEvent descriptor(const World &world, const Entity &entity, const std::string &name) {
    ScriptEffectEvent event;
    event.owner = entity.handle;
    event.name = name;
    event.source_tick = world.logic_tick;
    if (const AiEntity *body = world.ai.for_handle(entity.handle))
        event.position = {body->pos[0], body->pos[1], body->pos[2]};
    else
        event.position = {to_fixed(entity.position.x), to_fixed(entity.position.y), to_fixed(entity.position.z)};
    return event;
}

// [orig: WacScript_SpawnEffectAtSsnEntity @0x4F2421..0x4F2488]
// The two 1024-entry Q22 tables are indexed by rounded BAM32 yaw/pitch.
// These are entity angles; no terrain normal or target-facing query participates.
std::array<int32_t, 3> direction(const World &world, const Entity &entity) {
    const AiEntity *body = world.ai.for_handle(entity.handle);
    const uint32_t yaw = uint32_t(body ? body->heading : bam_heading_from_mission_yaw_deg(entity.yaw));
    const uint32_t pitch = uint32_t(body ? body->body_pitch : bam_from_degrees_wrapped(entity.pitch));
    const double yaw_phase = double(((yaw + 0x200000u) >> 22) & 1023u) * (2.0 * io::kPi / 1024.0);
    const double pitch_phase = double(((pitch + 0x200000u) >> 22) & 1023u) * (2.0 * io::kPi / 1024.0);
    const int32_t cy = int32_t(std::cos(yaw_phase) * io::kQ22One);
    const int32_t sy = int32_t(std::sin(yaw_phase) * io::kQ22One);
    const int32_t cp = int32_t(std::cos(pitch_phase) * io::kQ22One);
    const int32_t sp = int32_t(std::sin(pitch_phase) * io::kQ22One);
    return {int32_t((int64_t(cp) * cy) >> 28), int32_t((int64_t(cp) * sy) >> 28), sp >> 6};
}

void emit(World &world, ScriptEffectEvent event) {
    event.source_order = world.out.next_script_effect_order++;
    world.out.script_effects.push_back(std::move(event));
}
} // namespace

// [orig: WacScript_SpawnEffectAtSsnEntity (fx2ssn) @0x4F23A0]
int EntityCommands::effect_at_ssn(int32_t effect, const std::string &name, EntityTarget ssn) {
    const Entity *entity = world_.registry.get(resolve_target(ssn));
    if (!entity || !entity->has_item_def || effect == 0) return 0;
    auto event = descriptor(world_, *entity, name);
    event.direction = direction(world_, *entity);
    event.release_previous = true;
    emit(world_, std::move(event));
    return 1;
}

// [orig: WacScript_SpawnEffectAtTargetMarker (fx2tgt) @0x4F7FD0]
int EntityCommands::effect_at_target(int32_t effect, const std::string &name, int32_t target) {
    const Entity *entity = script_target(target);
    if (!entity || effect == 0) return 1;
    auto event = descriptor(world_, *entity, name);
    event.direction = direction(world_, *entity);
    emit(world_, std::move(event)); // overwrite the slot, keep the older group alive
    return 0;
}

// [orig: WacCmd_FxRain @0x4EE3E0] One WAC PRNG word, drawn by the VM only
// after its nonzero effect gate. The original leaves the entity slot alone.
int EntityCommands::rain_effect(int32_t effect, const std::string &name, uint32_t random) {
    const Entity *player = world_.registry.get(world_.cached.local_player);
    if (!player || effect == 0) return 1; // guard retail's missing-player dereference
    auto event = descriptor(world_, *player, name);
    event.position[0] = io::bam_add(event.position[0], int32_t(int16_t(random >> 16)) * 6);
    event.position[1] = io::bam_add(event.position[1], int32_t(int16_t(random)) * 6);
    event.position[2] = io::bam_add(event.position[2], 8 * 65536);
    event.direction = {0, 0, -io::kFp16OneInt / 2};
    event.store_slot = false;
    emit(world_, std::move(event));
    return 0;
}

// [orig: BMS action 27 / WAC targetfx -> EventAction_SpawnParticleEffect @0x4540E0]
// ALL matching pool-3 markers, strict entity+692 name, zero direction.
// The group's death callback clears its slot only if it still owns it (@0x453580).
int EntityCommands::spawn_marker_particle_effects(int32_t number) {
    int fired = 0;
    for (size_t slot = 0; slot < world_.registry.pool_capacity(3); ++slot) {
        const Entity *entity = world_.registry.get(EntityHandle::make(3, int(slot)));
        if (!entity || !entity->has_item_def || entity->item_id != kParticleEffectMarkerTypeId ||
                entity->wp_number != number) continue;
        auto event = descriptor(world_, *entity, entity->script_effect_name);
        event.lookup_by_name = true;
        emit(world_, std::move(event));
        ++fired;
    }
    return fired;
}

} // namespace opennova::world
