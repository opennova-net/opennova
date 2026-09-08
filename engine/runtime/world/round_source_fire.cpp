#include <runtime/world/round_sim.h>
#include <runtime/world/world.h>
#include <base/io/fixed.h>

namespace opennova::world {

// [orig: Weapon_FireProcess @0x53F5B0 -> Entity_FireWeaponAndSendPacket @0x42BD80]
void RoundSim::fire_source(
		World &world, Entity &source, FixedVec3 p, int32_t yaw, int32_t pitch, uint8_t ammo_index) {
	if (world.tables.ammo.by_index(ammo_index) == nullptr)
		return;
	RoundSpawnParams params;
	params.owner = source.primary_occupant;
	params.shooter_handle = params.owner.packed;
	params.origin = { p.x * io::kInvFp16One, p.y * io::kInvFp16One, p.z * io::kInvFp16One };
	params.dir_yaw_bam = yaw;
	params.dir_pitch_bam = pitch;
	params.ammo_index = ammo_index;
	params.adm_index = ammo_index;
	RoundSpawnParams presentation = params;
	presentation.owner = source.handle;
	presentation.shooter_handle = source.handle.packed;
	present_fire(world, presentation);
	if (world.rules.mp_session && !world.rules.logic_authority &&
			source.primary_occupant != world.cached.local_player)
		return;
	if (world.rules.cease_fire)
		return;
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
	event.dir_yaw = yaw;
	event.dir_pitch = pitch;
	event.adm_index = ammo_index;
	event.mode_flags = 1;
	world.out.rounds.add(event);
	spawn(world, params);
}

} // namespace opennova::world
