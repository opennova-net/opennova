#include <algorithm>
#include <base/io/bam.h>
#include <cmath>
#include <runtime/world/ai.h>
#include <runtime/world/angle.h>
#include <runtime/world/collision.h>
#include <runtime/world/hud_combat_feed.h>
#include <runtime/world/hud_impact.h>
#include <runtime/world/local_player.h>
#include <runtime/world/local_player_view.h>
#include <runtime/world/pose_provider.h>
#include <runtime/world/world.h>

namespace opennova::world {
void fill_hud_combat_view(World &world, LocalPlayerWeapon &weapon, LocalPlayerViewFrame &view,
		LocalPlayerViewTracker &tracker, bool can_fire) {
	const auto &service = tracker.hud_service;
	auto &out = view.hud_combat;
	auto &s = out.state;
	const Entity *player = world.registry.get(world.cached.local_player);
	if (!player)
		return;
	const AiEntity *body = world.ai.for_handle(player->handle);
	const Entity *mount = world.registry.get(player->mount_target);
	const int def_index = world.tables.weapons.index_of(weapon.def_name.c_str());
	const WeaponTableEntry *def =
			def_index >= 0 ? world.tables.weapons.by_index(uint8_t(def_index)) : nullptr;
	s.inset = view.inset_scope_active;
	s.inset_fov_over_zoom = view.inset_fov_over_zoom;
	s.hit_feedback = tracker.hud_hit_feedback_frames != 0;
	if (s.hit_feedback && view.scope_settled && (weapon.def.flags & 2) &&
			active_local_weapon_slot(world, weapon)->current != weapon_action::kSwitchFrom)
		view.hud_keep_crosshair_while_aimed = true;
	s.weapon_identity = weapon.active ? weapon.def_name : std::string{};
	if (def && weapon.active) {
		out.weapon_texture = def->hud_icon;
		out.custom_texture = def->crosshair;
		out.commander_texture = def->commanders_x;
	}
	// Combat target takes priority over the ordinary aim-ray fallback. Only
	// the combat target sets HUD+8 and therefore admits the cursor sprite.
	// [orig: HUD_BuildEntityInfo @0x4B87DA..0x4B882D]
	const Entity *target = body ? world.registry.get(body->inf.combat_target) : nullptr;
	s.target_cursor = target != nullptr && weapon.active;
	s.target_locked = body && (body->slot.f[2] & 1);
	if (!target && body)
		target = world.registry.get(body->inf.head_look_target);
	if (target) {
		s.target_friendly = target->team == player->team;
		world.ai.weapon_aim_origin(world, *target, out.target.data());
		out.target_valid = true;
		// These are mpattrib option bits, not a time-based animation. The
		// selector is the is_client bit, which connection modes 2 AND 3 set:
		// every instance that owns a local player (single player, the listen
		// host, a joiner) tests bit 3. The >> 8 leg belongs to a dedicated
		// host, which draws no HUD.
		// [orig: HUD_DrawCrosshair -- g_rules_flags @0x59265F,
		//  is_mp_session_peer @0x5926D4, shr 3 @0x5926DC / shr 8 @0x5926E1;
		//  CGameSession_SetConnectionMode @0x4C49F0]
		const bool client = world.cached.local_player.valid();
		const bool admitted = ((world.rules.mpattrib >> (client ? 3 : 8)) & 1u) == 0;
		const bool team = s.target_friendly || (target->team != 1 && target->team != 2);
		const bool team_mode =
				!world.rules.session_open || (world.match.rules().game_type & 0x10000u);
		s.inset_friendly = target->handle.pool() == 0 && admitted && team && team_mode &&
				!(mount &&
						(player->mount_type == SeatType::Controller ||
								player->mount_type == SeatType::Driver));
		s.target_name = target->display_name;
		s.target_brackets = target->handle.pool() == 0 && target->has_item_def &&
				target->item_type == 3 && admitted && team && team_mode &&
				!(mount &&
						(player->mount_type == SeatType::Controller ||
								player->mount_type == SeatType::Driver));
	}
	// OnlyScoped is the admission gate for the impact flags, independently
	// of the ordinary CanFire/Sighted/Scoped reticle predicates. Neither the
	// preview nor its two cues tests the dead bit or the death screen.
	// [orig: Entity_CheckWeaponSeatFlags @0x540D00; the preview admission
	//  Player_UpdatePerFrame @0x4DE760..0x4DE79D]
	const bool impact_admitted =
			weapon.active && (!(weapon.def.flags & 0x80000u) || view.scope_settled);
	if (impact_admitted && (weapon.def.flags & (0x8000u | 0x100000u))) {
		const int stance = (can_fire || (player->net_stance_bits & 1))
				? 0
				: 2 - ((player->net_stance_bits & 2) != 0);
		const int spread = def ? def->error_fp16[stance + (view.scope_details_scoped ? 3 : 0)] : 0;
		const auto impact = predict_hud_impact(world, weapon, view.scope_settled, spread);
		out.impact = impact.position;
		out.impact_valid = true;
		s.impact_map = impact.hit && (weapon.def.flags & 0x100000u);
		s.impact_distance = (weapon.def.flags & 0x800000u) != 0;
		s.designator = (weapon.def.flags & 0x8000u) != 0;
		s.impact_x_q16 = impact.position[0];
		s.impact_y_q16 = impact.position[1];
		s.impact_radius_q16 = impact.radius_q16;
		if (impact.hit && (weapon.def.flags & def::DEF_WEAPON_FLAG_USEDESIGNATOR)) {
			// UseDesignator is the nearest same-team type-1 link, not a
			// spawn-zone radius. A point inside its radius adopts that radius;
			// otherwise the preview radius is capped by the point distance.
			// [orig: @0x4DEBE1..0x4DEC62; nearest @0x5BBF10]
			const hud::HudDesignationPoint *nearest = nullptr;
			int32_t distance = INT32_MAX;
			for (const auto &point : tracker.hud_designations) {
				if (player->team && point.team != player->team)
					continue;
				const int32_t candidate = int32_t(
						std::min(std::hypot(double(io::bam_sub(impact.position[0], point.x)),
										 double(io::bam_sub(impact.position[1], point.y))),
								2147418100.0));
				if (candidate < distance) {
					distance = candidate;
					nearest = &point;
				}
			}
			if (nearest)
				s.impact_radius_q16 = distance < nearest->radius_q16
						? nearest->radius_q16
						: std::min(impact.radius_q16, distance);
		}
		if (impact.hit)
			tracker.hud_impact_distance_q16 = impact.distance_q16;
		s.impact_distance_m = uint16_t(uint32_t(tracker.hud_impact_distance_q16) >> 16);
		if (world.ai.terrain) {
			const int32_t start[] = { to_fixed(player->position.x),
				to_fixed(player->position.y) + 0x20000, to_fixed(player->position.z) };
			const int32_t end[] = { impact.position[0], impact.position[1] + 0x20000,
				impact.position[2] };
			s.designator_color =
					los_terrain_blocked(*world.ai.terrain, start, end) ? 0x60FF0000u : 0xFFFF0000u;
		}
		s.designator_scale =
				std::clamp(200.0f / std::max(int(s.impact_distance_m), 1), 0.25f, 2.0f);
	}
	s.driver_crosshair = view.camera_mode == 1 && mount &&
			(player->mount_type == SeatType::Controller || player->mount_type == SeatType::Driver);
	// Custom aim replaces the ordinary crosshair, even when its texture is
	// unresolved. The muzzle ray uses the same mounted userpoint as firing.
	// [orig: @0x592973..0x592AB8]
	s.custom_aim = weapon.active && (weapon.def.flags2 & 0x80u);
	if (s.custom_aim && mount && world.collision) {
		int32_t pose[6];
		const auto *slot = active_local_weapon_slot(world, weapon);
		local_weapon_fire_pose(world, weapon, slot ? slot->clip : 0, view.scope_settled, pose);
		int32_t end[3], forward[3] = { 65536000, 0, 0 };
		collision_matrix_from_euler(pose[3], pose[4], pose[5], pose).transform_point(forward, end);
		ProjectileTrace ray;
		ray.owner = player->handle;
		ray.start = { pose[0], pose[1], pose[2] };
		ray.end = { end[0], end[1], end[2] };
		ray.walk_terrain = !(player->flags & kEntityFlagIndoors);
		ray.include_wire_proxies = world.rules.mp_session && !world.rules.projectile_authority;
		const auto hit = world.collision->trace_aim(world, ray);
		out.aim = { hit.position_q16.x, hit.position_q16.y, hit.position_q16.z };
		out.aim_valid = true;
	}
	// Commander's scoped optical view follows the LAST admitted attached gun
	// in registry order. Its line originates at the gun's userpoint and ends
	// at its occupant's far camera aim, clipped by water, terrain and entities.
	// [orig: HUD_DrawScopeOverlayDetails @0x59E5B9..0x59E734]
	if (view.scope_details_active && (weapon.def.flags & 8u) && mount && world.collision) {
		const Entity *carrier = world.registry.get(mount->ground_target);
		if (carrier)
			world.registry.for_each([&](const Entity &gun) {
				if (!gun.item_id || (gun.flags & 1u) || !gun.has_item_def ||
						!(gun.item_attrib & kItemAttribEweap) ||
						gun.emplacement_parent != carrier->handle ||
						!(gun.emplacement_attachment_flags & 1u))
					return;
				const Entity *occupant = world.registry.get(gun.primary_occupant);
				if (!occupant || !occupant->mounted)
					return;
				int32_t origin[6] = { to_fixed(gun.position.x), to_fixed(gun.position.y),
					to_fixed(gun.position.z), 0, 0, 0 };
				const uint8_t point =
						weapon_userpoint_byte(gun, uint32_t(gun.primary_weapon_slot.clip) & 3u, 0);
				if (point && world.pose_provider)
					world.pose_provider->resolve_userpoint_transform(
							world, gun.handle, point, origin);
				const auto *aim_body = world.ai.for_handle(occupant->handle);
				const auto eye = player_eye_position(*occupant);
				const int32_t camera[3] = { to_fixed(eye.x), to_fixed(eye.y), to_fixed(eye.z) };
				const int32_t forward[3] = { 65536000, 0, 0 };
				int32_t end[3];
				collision_matrix_from_euler(aim_body
								? aim_body->heading
								: bam_heading_from_mission_yaw_deg(occupant->yaw),
						aim_body ? aim_body->pitch : bam_from_degrees_wrapped(occupant->pitch), 0,
						camera)
						.transform_point(forward, end);
				ProjectileTrace ray;
				ray.owner = occupant->handle;
				ray.start = { origin[0], origin[1], origin[2] };
				ray.end = { end[0], end[1], end[2] };
				ray.walk_terrain = !(occupant->flags & kEntityFlagIndoors);
				const auto hit = world.collision->trace_aim(world, ray);
				out.commander = { hit.position_q16.x, hit.position_q16.y, hit.position_q16.z };
				out.commander_valid = s.commander = true;
			});
	}
	// [orig: HUD_BuildEntityInfo @0x4B86CF..0x4B875F]
	s.vehicle_controls = mount && mount->has_item_def &&
			(player->mount_type == SeatType::Controller || player->mount_type == SeatType::Driver);
	if (s.vehicle_controls) {
		s.vehicle_identity = mount->registry_spawn_id;
		out.vehicle_texture = mount->hud_image;
		s.gear = (player->net_stance_bits & 2) ? 0 : 2 - ((player->net_stance_bits & 1) != 0);
		if (mount->item_unit_type == 3) {
			s.altitude_q16 = to_fixed(mount->position.z);
			s.altitude_agl_q16 = int32_t(std::clamp(int64_t(s.altitude_q16) -
							(mount->veh.ground_cache == INT32_MIN ? 0 : mount->veh.ground_cache),
					int64_t(0), int64_t(INT32_MAX)));
			s.vertical_velocity_q16 = mount->veh.slide_z;
		}
	}
	// [orig: HUD_DrawGameplayOverlays @0x5BDE60..0x5BE10E]
	// ARMORY_WAIT is unreachable: its enclosing preround condition is nonzero.
	if (service.preround_seconds)
		s.service_prompt = 1;
	else if (local_player_in_vehicle_loadout_zone(world) &&
			local_player_vehicle_zone_team_matches(world))
		s.service_prompt = 2;
	const Entity *station = s.vehicle_controls ? world.registry.get(mount->ground_target) : nullptr;
	if (station && station->has_item_def && (station->item_attrib2 & 0x2000u) &&
			(mount->carry_flags & 64u)) {
		const auto type = world.match.rules().game_type;
		const bool team = !(type & 0x10000u) || type == 65552 || type == 327696 ||
				station->team == player->team;
		const bool unlocked = !station->zone_number ||
				((1u << (station->zone_number & 31)) & service.owned_zone_mask);
		if (team && unlocked && service.reload_seconds != 255) {
			s.service_prompt = service.reload_seconds ? 3 : 4;
			s.service_wait_seconds = service.reload_seconds;
			s.service_above_declutter = true;
		}
	}
	fill_hud_vehicle_sights(world, weapon, view);
	s.parachute = (player->carry_flags & 16) != 0;
	s.armor = (player->carry_flags & 8) != 0;
	const Entity *cargo = world.registry.get(player->mounted_child);
	s.carrying = cargo && cargo->has_item_def;
	if (s.carrying) {
		out.cargo_texture = cargo->hud_image;
		// Texture defaults are filled from HUD_LoadAllTextures by the device.
		if (cargo->item_id == 4091 || cargo->item_id == 4093 || cargo->item_id == 4095)
			out.cargo_texture = "H_flag.tga";
		else if (out.cargo_texture.empty())
			out.cargo_texture = "H_docmnt.tga";
	}
}
} // namespace opennova::world
