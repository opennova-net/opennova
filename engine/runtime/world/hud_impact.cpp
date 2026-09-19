#include <algorithm>
#include <base/io/bam.h>
#include <cmath>
#include <runtime/terrain_query/height_field.h>
#include <runtime/world/collision.h>
#include <runtime/world/hud_impact.h>
#include <runtime/world/local_player.h>
#include <runtime/world/round_sim.h>
#include <runtime/world/world.h>
namespace opennova::world {
HudImpact predict_hud_impact(World &world, LocalPlayerWeapon &weapon, bool scope_settled,
		int32_t spread_q16) {
	HudImpact out;
	const Entity *player = world.registry.get(world.cached.local_player);
	const int index = world.tables.weapons.index_of(weapon.def_name.c_str());
	const auto *def = index >= 0 ? world.tables.weapons.by_index(uint8_t(index)) : nullptr;
	const auto *ammo = def ? world.tables.ammo.by_index(def->ammo_index) : nullptr;
	const auto *terrain = world.ai.terrain;
	if (!player)
		return out;
	out.position = { to_fixed(player->position.x), to_fixed(player->position.y),
		to_fixed(player->position.z) };
	if (!ammo || !terrain || !terrain->valid())
		return out;
	int32_t pose[6];
	const auto *slot = active_local_weapon_slot(world, weapon);
	local_weapon_fire_pose(world, weapon, slot ? slot->clip : 0, scope_settled, pose);
	const int32_t speed = int32_t(uint32_t(ammo->velocity) << 16) / 62;
	const double pitch = double(int32_t(uint32_t(pose[4]) & 0xFFFF0000u)) * io::kRadiansPerBam;
	const double yaw = double(int32_t(uint32_t(pose[3]) & 0xFFFF0000u)) * io::kRadiansPerBam;
	const int32_t cp = int32_t(std::cos(pitch) * 4194304), sp = int32_t(std::sin(pitch) * 4194304);
	const int32_t cy = int32_t(std::cos(yaw) * 4194304), sy = int32_t(std::sin(yaw) * 4194304);
	// The round descriptor is a mission bearing: X=cos, Y=sin.
	FixedVec3 velocity{ int32_t((int64_t(speed) * ((int64_t(cp) * cy) >> 22)) >> 22),
		int32_t((int64_t(speed) * ((int64_t(cp) * sy) >> 22)) >> 22),
		int32_t((int64_t(speed) * sp) >> 22) };
	FixedVec3 position{ pose[0], pose[1], pose[2] };
	// Retail benchmarks two warm-up steps before consuming the lifetime.
	// The completed result is independent of that machine's benchmark speed.
	for (int64_t step = 0; step < int64_t(std::max(ammo->max_age_ticks, 0)) + 2; ++step) {
		const int32_t previous_z = position.z;
		projectile_apply_drag(velocity, *ammo, position.z, world.env.water_z);
		position.x = io::bam_add(position.x, velocity.x);
		position.y = io::bam_add(position.y, velocity.y);
		velocity.z = io::bam_sub(velocity.z, 167);
		position.z = io::bam_add(position.z, velocity.z);
		if (position.z <= world.env.water_z && previous_z > world.env.water_z) {
			position.z = world.env.water_z;
			out.hit = true;
		} else {
			const float x = float(from_fixed(position.x)), y = float(-from_fixed(position.y));
			if (position.z < to_fixed(terrain::height_field_height_world(*terrain, x, y))) {
				position.z = to_fixed(terrain::height_field_height_world_bilinear(*terrain, x, y));
				out.hit = true;
			}
		}
		if (out.hit)
			break;
	}
	if (!out.hit)
		return out;
	out.position = { position.x, position.y, position.z };
	const double dx = double(to_fixed(player->position.x)) - position.x;
	const double dy = double(to_fixed(player->position.y)) - position.y;
	out.distance_q16 = int32_t(std::min(std::hypot(dx, dy), 2147418112.0));
	// The map radius is twice the lateral dispersion displacement.
	// [orig: @0x4DE9F8..0x4DEAD5; map-slot +28 @0x4DEC71]
	const int32_t error_bam = int32_t((int64_t(spread_q16) * 0xB60B60 + 0x8000) >> 16);
	const double spread_yaw = double(io::bam_add(pose[3], error_bam)) * io::kRadiansPerBam;
	const int32_t offset_x =
			int32_t((int64_t(out.distance_q16) * int32_t(std::cos(spread_yaw) * 4194304)) >> 22);
	const int32_t offset_y =
			int32_t((int64_t(out.distance_q16) * int32_t(std::sin(spread_yaw) * 4194304)) >> 22);
	out.radius_q16 = error_bam
			? int32_t(uint32_t(int32_t(
							  std::min(std::hypot(dx + offset_x, dy + offset_y), 2147418112.0))) *
					  2u)
			: int32_t(uint32_t(def->hud_splash_radius) << 16);
	return out;
}
} // namespace opennova::world
