#include <runtime/particle/script_effects.h>

namespace opennova::particle {
namespace {
// The event's mission position and direction in the scene's frame (x, z, -y);
// a zero direction keeps the identity basis (forward_pose).
EffectPose pose(const world::ScriptEffectEvent &event) {
    const Vec3 at{float(event.position[0]) / 65536.0f,
                  float(event.position[2]) / 65536.0f,
                 -float(event.position[1]) / 65536.0f};
    return forward_pose(at, {float(event.direction[0]), float(event.direction[2]),
                             -float(event.direction[1])});
}
} // namespace

EffectSpawnReceipt spawn_script_effect(EffectScene &scene, const world::ScriptEffectEvent &event,
        EffectSlotToken slot, EffectOwnerToken owner, uint32_t age_ticks, float water_height,
        const EffectSectionGate &gate) {
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
    request.section_gate = gate;
    return scene.spawn(request);
}

} // namespace opennova::particle
