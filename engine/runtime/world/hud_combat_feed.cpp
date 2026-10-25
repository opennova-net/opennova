#include <algorithm>
#include <base/io/bam.h>
#include <cmath>
#include <runtime/hud/hud_texture_names.h>
#include <runtime/world/ai.h>
#include <runtime/world/angle.h>
#include <runtime/world/collision.h>
#include <runtime/world/geom.h>
#include <runtime/world/hud_combat_feed.h>
#include <runtime/world/hud_impact.h>
#include <runtime/world/local_player.h>
#include <runtime/world/local_player_view.h>
#include <runtime/world/pose_provider.h>
#include <runtime/world/round_sim.h>
#include <runtime/world/world.h>

namespace opennova::world {
namespace {
// The HUD target cursor's entity as a HUD reader takes it at its read: a registry entity the
// record names (none once destroyed, hud_target_cursor_entity), else, on a joiner, the decoded
// remote person the record names through its row (World::wire_person), as retail's client
// reads the pool-0 entity it built for that person: its team byte and the type of the def its
// record's type word indexes (none at index 0). A round lock names no pool entity.
// [orig: NapiNPClientMsg_0x00C @0x42E730 - ItemList_FindIndexByTypeId @0x42E84A,
//  Entity_InitFromItemDef called @0x42E84D; Entity_InitFromItemDef @0x49E550 - the zero test
//  @0x49E559]
struct CursorSubject {
	bool pool0 = false;
	uint8_t team = 0;
	uint8_t item_type = 0; // the ItemDef's type, 0 with no def
};
CursorSubject cursor_subject(const World &world, const HudTargetCursor &cursor) {
	CursorSubject out;
	if (const Entity *e = hud_target_cursor_entity(world, cursor)) {
		out.pool0 = e->handle.pool() == 0;
		out.team = e->team;
		out.item_type = e->has_item_def ? e->item_type : 0;
		return out;
	}
	World::WirePerson person;
	if (cursor.wire_person != 0xFFFF && world.wire_person &&
			world.wire_person(cursor.wire_person, person)) {
		out.pool0 = EntityHandle{cursor.wire_person}.pool() == 0;
		out.team = person.team;
		const int32_t index = item_index_by_type_id(world, person.type_id);
		out.item_type = index != 0 ? world.tables.item_rows[static_cast<size_t>(index)].item_type : 0;
	}
	return out;
}
// The friendly test the crosshair and the inset share: the cursor's team is the local
// player's, or neither team 1 nor team 2 [orig: HUD_DrawCrosshair @0x592692..0x5926B4;
//  Render_WeaponInsetScene @0x5C9F84..0x5C9FAA].
bool cursor_team_admitted(uint8_t cursor_team, uint8_t local_team) {
	return cursor_team == local_team || (cursor_team != 1 && cursor_team != 2);
}
} // namespace

void fill_hud_combat_view(World &world, LocalPlayerWeapon &weapon, LocalPlayerViewFrame &view,
		LocalPlayerViewTracker &tracker, bool can_fire) {
	const auto &service = tracker.hud_service;
	auto &out = view.hud_combat;
	auto &s = out.state;
	const Entity *player = world.registry.get(world.cached.local_player);
	out.local_valid = player != nullptr;
	if (!player)
		return;
	out.local_position = { player->position.x.q16, player->position.y.q16,
		player->position.z.q16 };
	out.local_team = player->team;
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
	// The cursor the crosshair reads is this pass's record (tracker.hud_cursor): the lock word
	// (an entity, or a round when the heat lock took a flare, D-NET-64), else, under a zero
	// word only, the head-look target, on a joiner perhaps a decoded remote person. Only a
	// nonzero word sets the cursor flag HUD+8, so the cursor quad draws only for a lock: at the
	// word's fire origin (a round's Position) tinted by the word's team byte (+0x162, a round's
	// own). The friendly brackets need the cursor's pool-0 index (a round has none) and a
	// person def. [orig: HUD_BuildEntityInfo @0x4B8440 - the slot word into +0x168
	//  @0x4B87ED..0x4B87F3, the zero test @0x4B8816..0x4B881D, the head-look fallback
	//  @0x4B881F..0x4B8825, +8 = 1 @0x4B882D; HUD_DrawCrosshair - the cursor
	//  @0x592776..0x592790, the team compare @0x5927AA..0x5927BD,
	//  Entity_ComputeWeaponFireOrigin @0x592803; the pool test (the call @0x592675), the team
	//  read @0x592692; the bracket gate's def type @0x592CE8..0x592CF3]
	// On a joiner the word may name a decoded remote person (the heat lock's pool-0 leg): the
	// quad sits at the client-built entity's fire origin, tinted by its team byte.
	const HudTargetCursor &cursor = tracker.hud_cursor;
	const Entity *target = hud_target_cursor_entity(world, cursor);
	const bool round_cursor = cursor.lock.round >= 0 &&
			static_cast<size_t>(cursor.lock.round) < world.round_sim.rounds.size();
	World::WirePerson wire_lock;
	const bool wire_cursor = cursor.lock.wire_person != 0xFFFF && world.wire_person &&
			world.wire_person(cursor.lock.wire_person, wire_lock);
	s.target_cursor = cursor.lock.valid() && (target != nullptr || round_cursor || wire_cursor) &&
			weapon.active;
	s.target_locked = body && (body->slot.f[2] & 1);
	if (round_cursor) {
		s.target_friendly =
				world.round_sim.rounds[static_cast<size_t>(cursor.lock.round)].team == player->team;
		round_target_origin(world, cursor.lock.round, out.target.data());
		out.target_valid = true;
	} else if (wire_cursor) {
		s.target_friendly = wire_lock.team == player->team;
		wire_person_target_origin(world, cursor.lock.wire_person, out.target.data());
		out.target_valid = true;
	} else if (target) {
		s.target_friendly = target->team == player->team;
		world.ai.weapon_aim_origin(world, *target, out.target.data());
		out.target_valid = true;
	}
	// These are mpattrib option bits, not a time-based animation. The
	// selector is the is_client bit, which connection modes 2 AND 3 set:
	// every instance that owns a local player (single player, the listen
	// host, a joiner) tests bit 3. The >> 8 leg belongs to a dedicated
	// host, which draws no HUD.
	// [orig: HUD_DrawCrosshair -- g_RulesFlags @0x59265F,
	//  is_mp_session_peer @0x5926D4, shr 3 @0x5926DC / shr 8 @0x5926E1;
	//  CGameSession_SetConnectionMode @0x4C49F0]
	const bool client = world.cached.local_player.valid();
	const bool admitted = ((world.rules.mpattrib >> (client ? 3 : 8)) & 1u) == 0;
	// Outside a session every target is a teammate candidate; inside one
	// only a team game type admits them. Single player is outside.
	// [orig: HUD_DrawCrosshair -- `cmp g_NapiNPCtx.is_in_session`
	//  @0x5926C0, `test g_GameType,10000h` @0x5926C4, the clear @0x5926D0]
	const bool team_mode =
			!world.rules.mp_session || (world.match.rules().game_type & 0x10000u);
	const bool driving = mount &&
			(player->mount_type == SeatType::Controller || player->mount_type == SeatType::Driver);
	const CursorSubject crosshair = cursor_subject(world, cursor);
	s.target_brackets = crosshair.pool0 && crosshair.item_type == 3 && admitted &&
			cursor_team_admitted(crosshair.team, player->team) && team_mode && !driving;
	// The weapon inset reads the cursor ahead of this frame's pass, as the scene frame found
	// it: the last pass's record (tracker.hud_cursor_at_scene). Its brackets and friendly
	// marker take the same pool-0, team, session and seat tests over that record, with no def
	// test. [orig: Render_ProcessMainSceneFrame @0x5CA0F0 - the Render_WeaponInsetScene call
	//  @0x5CA949 ahead of the HUD_RenderAllOverlays calls @0x5CACE3 / @0x5CAD04;
	//  Render_WeaponInsetScene @0x5C9740 - g_HUDTargetCursorEntity @0x5C9F5D, the pool test
	//  (the call @0x5C9F6D), the team compare @0x5C9F84..0x5C9F98, the session clear
	//  @0x5C9FAF..0x5C9FC4, the rules bit @0x5C9FC6..0x5C9FDE, the seat test
	//  @0x5C9FE1..0x5C9FFC, the gate @0x5CA002..0x5CA00C]
	const CursorSubject inset = cursor_subject(world, tracker.hud_cursor_at_scene);
	s.inset_friendly = inset.pool0 && admitted && cursor_team_admitted(inset.team, player->team) &&
			team_mode && !driving;
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
			const int32_t start[] = { player->position.x.q16,
				player->position.y.q16 + 0x20000, player->position.z.q16 };
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
				// The gun's point for its slot: that def's gfx3 launch point,
				// else the gun's byte for the barrel and field the slot's live
				// clip and action pair select; the raw gun position otherwise.
				// [orig: Entity_ComputeUserpointWorldTransform @0x59E680, the
				//  occupant's slot +0x118 @0x59E66A]
				int32_t origin[6] = { gun.position.x.q16, gun.position.y.q16,
					gun.position.z.q16, 0, 0, 0 };
				const WeaponSlotState &slot = gun.primary_weapon_slot;
				carrier_weapon_userpoint(world, gun,
						world.tables.weapons.by_index(gun.primary_weapon_slot_adm), slot.clip,
						weapon_userpoint_field(slot), origin);
				const auto *aim_body = world.ai.for_handle(occupant->handle);
				const auto eye = player_eye_position(*occupant);
				const int32_t camera[3] = { to_fixed(eye.x), to_fixed(eye.y), to_fixed(eye.z) };
				const int32_t forward[3] = { 65536000, 0, 0 };
				int32_t end[3];
				collision_matrix_from_euler(aim_body
								? aim_body->heading
								: occupant->heading,
						aim_body ? aim_body->pitch : occupant->pitch_bam, 0,
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
		// The silhouette's key is the record's root entity, the mount's pool-slot pointer: the
		// pools are allocated once per process, so a slot keeps its address across spawns and
		// missions, and the key is the slot's handle here.
		// [orig: HUD_BuildEntityInfo - the root entity +0x234 @0x4B8655;
		//  HUD_DrawTargetEntityOverlay - g_HUDRootEntity @0x59A5E9, the key compare @0x59A5F7,
		//  its store @0x59A604; EntityPool_Allocate @0x442130 (its one malloc, the call
		//  @0x44214C), called once, by Game_Run (the call @0x4A7FC5)]
		s.vehicle_identity = mount->handle.packed;
		out.vehicle_texture = mount->hud_image;
		s.gear = (player->net_stance_bits & 2) ? 0 : 2 - ((player->net_stance_bits & 1) != 0);
		if (mount->item_unit_type == 3) {
			s.altitude_q16 = mount->position.z.q16;
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
	// The nearest FARP: the proximity list (every pool-1/2 entity whose def
	// carries attrib2 0x2000) filtered to unnumbered or owned zones, the 2D
	// distance clamped to flt_7C19E0 and truncated, the first strictly
	// nearer than the running best (seeded 0x40000000) wins; the HUD info
	// block is zeroed every frame, so no FARP clears it.
	// [orig: HUD_BuildEntityInfo @0x4b891a..0x4b89e3 — hudInfo+0x10 = 1,
	//  hudInfo+388..396 = the position (g_TrackedTargetPos); the list
	//  Entity_BuildProximityListFromPools @0x43ED60; the memset @0x5a80b1]
	s.farp_present = false;
	{
		int32_t best = 0x40000000;
		const Entity *nearest = nullptr;
		const int32_t px = player->position.x.q16;
		const int32_t py = player->position.y.q16;
		world.registry.for_each([&](const Entity &e) {
			const int pool = e.handle.pool();
			if (pool != 1 && pool != 2) return;
			if (!e.has_item_def || (e.item_attrib2 & 0x2000u) == 0) return;
			if (e.zone_number != 0 &&
					((1u << (e.zone_number & 31u)) & service.owned_zone_mask) == 0)
				return;
			const double dx = static_cast<double>(static_cast<int32_t>(
					static_cast<uint32_t>(e.position.x.q16) - static_cast<uint32_t>(px)));
			const double dy = static_cast<double>(static_cast<int32_t>(
					static_cast<uint32_t>(e.position.y.q16) - static_cast<uint32_t>(py)));
			const int32_t dist = static_cast<int32_t>(
					std::min(std::sqrt(dx * dx + dy * dy), 2147418112.0));
			if (dist < best) {
				best = dist;
				nearest = &e;
			}
		});
		if (nearest != nullptr) {
			s.farp_present = true;
			s.farp_x_q16 = nearest->position.x.q16;
			s.farp_y_q16 = nearest->position.y.q16;
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
		const bool flag = cargo->item_id == 4091 || cargo->item_id == 4093 || cargo->item_id == 4095;
		if (flag)
			out.cargo_texture = hud::kHudCargoFlagTexture;
		else if (out.cargo_texture.empty())
			out.cargo_texture = hud::kHudCargoDocumentTexture;
		out.cargo_texture_alpha = !flag && !cargo->hud_image.empty();
	}
}
} // namespace opennova::world
