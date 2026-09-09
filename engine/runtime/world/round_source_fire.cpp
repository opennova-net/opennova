#include <runtime/world/round_sim.h>
#include <runtime/world/world.h>
#include <base/io/fixed.h>

namespace opennova::world {
namespace {
RoundSpawnParams ammo_fire_params(EntityHandle owner, FixedVec3 p,
        int32_t yaw, int32_t pitch, uint8_t ammo_index) {
    RoundSpawnParams params;
    params.owner = owner;
    params.shooter_handle = owner.packed;
    params.origin = {p.x * io::kInvFp16One, p.y * io::kInvFp16One, p.z * io::kInvFp16One};
    params.dir_yaw_bam = yaw;
    params.dir_pitch_bam = pitch;
    params.ammo_index = ammo_index;
    params.adm_index = ammo_index;
    return params;
}

// [orig: Entity_FireWeaponAndSendPacket @0x42BD80]
void emit_ammo_round(World &world, RoundSpawnParams params, FixedVec3 p) {
    if (world.rules.cease_fire) return;
    // Suppress only the spawn's implicit launch; the caller presents its own copy.
    params.launch_presented = true;
    if (world.rules.mp_session && !world.rules.logic_authority) {
        world.out.source_fires.push_back(params);
        return;
    }
    RoundEvent event;
    event.shooter_handle = params.shooter_handle;
    event.origin_x = p.x;
    event.origin_y = p.y;
    event.origin_z = p.z;
    event.dir_yaw = params.dir_yaw_bam;
    event.dir_pitch = params.dir_pitch_bam;
    event.adm_index = params.adm_index;
    event.mode_flags = 1;
    world.out.rounds.add(event);
    world.round_sim.spawn(world, params);
}
} // namespace

// [orig: Weapon_FireProcess @0x53F5B0] This entry presents before its
// occupant/authority gate. A null source is the target/area script fire path.
void RoundSim::fire_source(
        World &world, Entity *source, FixedVec3 p, int32_t yaw, int32_t pitch, uint8_t ammo_index) {
    if (world.tables.ammo.by_index(ammo_index) == nullptr) return;
    const auto params = ammo_fire_params(source ? source->primary_occupant : EntityHandle{},
                                        p, yaw, pitch, ammo_index);
    RoundSpawnParams presentation = params;
    presentation.owner = source ? source->handle : EntityHandle{};
    presentation.shooter_handle = presentation.owner.packed;
    present_fire(world, presentation);
    if (world.rules.mp_session && !world.rules.logic_authority &&
            (!source || source->primary_occupant != world.cached.local_player))
        return;
    emit_ammo_round(world, params, p);
}

// [orig: WeaponSlot_FireAndSpawnEffects @0x53F440] NPC/rain fire gates the
// WHOLE entry on authority and emits the round before launch presentation.
// Cease-fire suppresses the round but does not suppress the presentation.
void RoundSim::fire_npc_ammo(World &world, EntityHandle shooter, FixedVec3 p,
        int32_t yaw, int32_t pitch, uint8_t ammo_index) {
    if (world.rules.mp_session && !world.rules.logic_authority) return;
    if (world.tables.ammo.by_index(ammo_index) == nullptr) return;
    const auto params = ammo_fire_params(shooter, p, yaw, pitch, ammo_index);
    emit_ammo_round(world, params, p);
    present_fire(world, params);
}

} // namespace opennova::world
