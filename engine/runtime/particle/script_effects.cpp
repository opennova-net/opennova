#include <runtime/particle/script_effects.h>

#include <cmath>

namespace opennova::particle {
namespace {
Vec3 cross(Vec3 a, Vec3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
Vec3 normalized(Vec3 v) {
    const float scale = 1.0f / std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    return {v.x * scale, v.y * scale, v.z * scale};
}
EffectPose pose(const world::ScriptEffectEvent &event) {
    EffectPose result;
    result.position = {float(event.position[0]) / 65536.0f,
                       float(event.position[2]) / 65536.0f,
                      -float(event.position[1]) / 65536.0f};
    if (event.direction[0] || event.direction[1] || event.direction[2]) {
        result.forward = normalized({float(event.direction[0]), float(event.direction[2]),
                                    -float(event.direction[1])});
        const Vec3 hint = std::abs(result.forward.y) > 0.999f ? Vec3{1, 0, 0} : Vec3{0, 1, 0};
        result.right = normalized(cross(hint, result.forward));
        result.up = normalized(cross(result.forward, result.right));
    }
    return result;
}
} // namespace

EffectSpawnReceipt spawn_script_effect(EffectScene &scene, const world::ScriptEffectEvent &event,
        EffectSlotToken slot, EffectOwnerToken owner, uint32_t age_ticks, float water_height) {
    // SSN releases even when the new effect cannot allocate. Target effects
    // merely overwrite +460, so older groups may continue to emit.
    // [orig: @0x4F2406 -> @0x5F75D0; @0x4F80D8; @0x45418C]
    if (event.release_previous) scene.detach_slot(slot);
    EffectSpawnRequest request;
    request.effect = event.name.empty() ? EffectHandle{} :
            (event.lookup_by_name ? scene.find(event.name) : scene.intern(event.name));
    request.pose = pose(event);
    request.admission = event.store_slot ? EffectAdmission::StoreOwned : EffectAdmission::Always;
    request.binding = EffectBinding::World;
    request.slot = slot;
    request.owner = owner;
    request.initial_age_ticks = age_ticks;
    request.source_tick = event.source_tick;
    request.source_order = event.source_order;
    request.kill_plane_y = water_height;
    return scene.spawn(request);
}

} // namespace opennova::particle
