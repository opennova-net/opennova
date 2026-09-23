#include "ai.h"
#include "world.h"
#include "ai_detail.h"
#include <base/io/bam.h>
#include <runtime/world/angle.h>
#include <algorithm>

namespace opennova::world {
// [orig: Entity_CalcAverageGroundHeight @0x457230]
int32_t AiSystem::aircraft_ground_height(World &world, AiEntity &ai, int32_t radius) {
	Entity *entity = world.registry.get(ai.handle);
	if (entity == nullptr)
		return INT32_MIN;
	const int32_t offset = brain_ground_offset(world, *entity);
	int32_t pos[3] = { to_fixed(entity->position.x), to_fixed(entity->position.y),
		to_fixed(entity->position.z) };
	int32_t ground = INT32_MIN;
	if (collision != nullptr && collision->instance_count() != 0) {
		// Two ray kinds share the probe: the AndObject kind also stores its hit
		// entity, null on a miss, as the hull's ground link. The east and centre
		// taps are that kind, so every sample leaves the centre hit as the link
		// the carrier follow and the brake's carrier exclusion read.
		// [orig: Entity_RaycastGroundHeight @0x4142C0 north/south/west @0x45725D/
		//  @0x457281/@0x4572C1; Entity_RaycastGroundHeightAndObject @0x414320
		//  (`mov [esi+28h],eax` @0x414370) east @0x4572A1, centre @0x4572E0]
		const auto tap = [&](int32_t x, int32_t y, EntityHandle *link) {
			return collision->raycast_ground(
					world, entity->handle, pos, x, y, 65536, 3145728, link);
		};
		if (radius == 0) {
			// A zero radius is one centre ray, not five coincident taps: the
			// weighted average would otherwise return 6c/10 for negative ground.
			// [orig: the sampleRadius == 0 arm @0x457254 -> single
			// Entity_RaycastGroundHeightAndObject(0,0,0x10000,3145728) @0x45735d]
			ground = tap(0, 0, &entity->ground_target);
		} else {
			const int32_t n = tap(0, radius, nullptr), s = tap(0, -radius, nullptr),
						  e = tap(radius, 0, &entity->ground_target),
						  w = tap(-radius, 0, nullptr), c = tap(0, 0, &entity->ground_target);
			const int32_t top = std::max({ 0, n, s, e, w, c });
			int32_t sum = io::bam_add(io::bam_add(n, s), io::bam_add(e, w));
			sum = io::bam_add(sum, io::bam_dbl(io::bam_add(c, io::bam_dbl(top))));
			ground = std::max(c, sum / 10);
		}
	} else if (world.tables.terrain != nullptr) {
		ground = calc_average_ground_height(*world.tables.terrain, pos, radius, GroundClearance{});
		// Terrain alone under the taps: the AndObject rays store a null link.
		entity->ground_target = {};
	}
	if (entity->primary_occupant.valid())
		ground = std::max(ground, world.env.water_z);
	return ground == INT32_MIN ? ground : io::bam_add(ground, offset);
}

// [orig: AI_UpdateMovementTarget @0x460E40]
int AiSystem::update_movement_target(AiEntity &e, World &world) {
	auto &b = e.brain;
	const bool patrol = b.f[AiBrain::kCurState] == 7;
	int32_t speed = b.f[patrol ? AiBrain::kSpeedB : AiBrain::kSpeedA];
	const int32_t altitude = patrol ? e.profile.patrol_altitude : e.profile.field216;
	const int32_t climb = patrol ? e.profile.patrol_climb : e.profile.field220;
	const auto freeze = [&]() {
		std::copy_n(e.pos, 3, &b.f[AiBrain::kWorkPosX]);
		b.f[AiBrain::kWorkHeading] = e.heading;
		b.f[AiBrain::kWorkPitch] = e.pitch;
		b.f[AiBrain::kWorkRoll] = e.roll;
		b.f[AiBrain::kOutSpeed] = 0;
		return 0;
	};
	if (ai_waypoint_update_target(b, e.pos, nav) == -1)
		return freeze();
	if (b.f[AiBrain::kWpDistance] < b.f[AiBrain::kWpNodeVal]) {
		const int channel = b.f[AiBrain::kWpChannel];
		b.f[AiBrain::kStoredKeyTime] = b.f[AiBrain::kWpNodeVal];
		b.f[AiBrain::kAnimFlag] = 0;
		mark_waypoint_visited(e, world, channel, b.f[AiBrain::kWpNode]);
		++b.f[AiBrain::kWpNode];
		const NavChannel *route = nav.channel(channel);
		const int count = route != nullptr ? route->count : 0;
		if (b.f[AiBrain::kWpNode] >= count) {
			if (route != nullptr && (route->loopflag & 1) != 0) {
				b.f[AiBrain::kWpNode] = count - 1;
				b.f[AiBrain::kWpType] = 0;
				return freeze();
			}
			b.f[AiBrain::kWpNode] = 0;
		}
	}
	if ((e.profile.flight_flags & 1) == 0) {
		const int32_t delta = io::bam_sub(b.f[AiBrain::kWpDistance], b.f[AiBrain::kWpNodeVal]);
		const int32_t count = b.f[AiBrain::kCurState] == 8 ? 16 : b.f[AiBrain::kStep];
		const int32_t threshold = int32_t(uint32_t(speed) * uint32_t(count));
		if (delta < threshold)
			speed >>= 1;
	}
	const int kind = b.f[AiBrain::kWpType];
	// The resolved node is read through unchecked, like the refresh that set it
	// [orig: the brain+0x40 node-pointer loads @0x460F99 (X/Y) and @0x460FE5 (Z)].
	const NavEntry *node = kind == 1 ? &nav.slot(b.f[AiBrain::kWpResolved]) : nullptr;
	if (node != nullptr) {
		b.f[AiBrain::kWorkPosX] = node->f[1];
		b.f[AiBrain::kWorkPosY] = node->f[2];
	} else if (kind == 3) {
		b.f[AiBrain::kWorkPosX] = b.f[AiBrain::kWpCoordX];
		b.f[AiBrain::kWorkPosY] = b.f[AiBrain::kWpCoordY];
	}
	int32_t floor;
	if (b.f[AiBrain::kUseWaypointZones] != 0) {
		floor = aircraft_ground_height(world, e, 327680);
		b.f[AiBrain::kWorkPosZ] = node != nullptr ? node->f[3] : b.f[AiBrain::kWpCoordZ];
	} else {
		const int32_t ground = aircraft_ground_height(world, e, 0x200000);
		b.f[AiBrain::kWorkPosZ] = e.profile.subtype == 2 ? altitude : io::bam_add(ground, altitude);
		floor = io::bam_add(ground, e.profile.min_agl);
	}
	b.f[AiBrain::kWorkPosZ] = std::max(b.f[AiBrain::kWorkPosZ], floor);
	b.f[AiBrain::kWorkHeading] = b.f[AiBrain::kWpBearing];
	b.f[AiBrain::kWorkPitch] = b.f[AiBrain::kWorkRoll] = 0;
	b.f[AiBrain::kOutSpeed] = speed;
	b.f[138] = climb;
	return speed;
}

namespace {
using io::bam_add;
using io::bam_sub;
int32_t mul32(int32_t a, int32_t b) {
	return int32_t(uint32_t(a) * uint32_t(b));
}
Entity *target_entity(World &world, int32_t packed) {
	return packed != 0 ? world.registry.get(EntityHandle{ uint16_t(packed - 1) }) : nullptr;
}
int32_t target_heading(const AiEntity &ai, const Entity &target) {
	return detail::bearing_bam(bam_sub(to_fixed(target.position.y), ai.pos[1]),
			bam_sub(to_fixed(target.position.x), ai.pos[0]));
}
uint32_t folded_angle(int32_t angle) {
	const uint32_t a = uint32_t(angle) >> 24;
	return a >= 128 ? 256 - a : a;
}
int32_t planar_magnitude(int32_t x, int32_t y) {
	return int32_t(std::min(2147418112.0, std::sqrt(double(x) * x + double(y) * y)));
}
void flare_timer(AiEntity &ai, World &world) {
	auto &timer = ai.brain.f[AiBrain::kFireTimer];
	if ((ai.profile.flags96 & 0x10) != 0 && timer > 0)
		if (Entity *entity = world.registry.get(ai.handle))
			world.vehicles.release_flares(*entity);
	timer = timer <= 0 ? 0 : bam_sub(timer, ai.brain.f[AiBrain::kStep]);
}
void combat_alert(AiSystem &sys, AiEntity &ai, World &world) {
	ai.slot.bytes()[AiSlot::kAlertByte] = 2;
	ai.brain.f[AiBrain::kPrevAlert] = ai.brain.f[AiBrain::kAlert] = 2;
	if (const Entity *entity = world.registry.get(ai.handle))
		world.script.relations.group(entity->group_id).alert = TriggerRelations::kAlertRed;
	sys.alert_nearby_allies(world, ai, 0x640000);
}
} //namespace

// [orig: AI_EnterState_AircraftCombat @0x466330]
void AiSystem::enter_aircraft_combat(AiEntity &ai, World &world) {
	combat_alert(*this, ai, world);
	auto &b = ai.brain;
	b.f[AiBrain::kOutSpeed] = b.f[AiBrain::kSpeedA];
	ai.aircraft_controller = ai.profile.subtype == 2 ? 0x10005 : 0x10000;
	if (b.f[45] == 0) {
		aircraft_turn_sequence = bam_sub(aircraft_turn_sequence, 1);
		b.f[45] = (aircraft_turn_sequence & 1) != 0 ? -1 : 1;
	}
	b.f[AiBrain::kStep] = 1;
}

// [orig: AI_EnterState_HelicopterEvade @0x465F60 (aircraft evade enter)]
void AiSystem::enter_aircraft_evade(AiEntity &ai, World &world) {
	combat_alert(*this, ai, world);
	auto &b = ai.brain;
	b.f[AiBrain::kOutSpeed] = b.f[AiBrain::kSpeedA];
	if (b.f[AiBrain::kTargetSlot] == 0)
		ai_set_target(world, ai,
				target_entity(world, b.f[AiBrain::kDamageInfo]) != nullptr
						? EntityHandle{ uint16_t(b.f[AiBrain::kDamageInfo] - 1) }
						: EntityHandle{});
	const auto route = [&](int state) {
		b.set_pend(state);
		AiThinkCtx ctx{ this, &ai, &world, nullptr };
		row(state).enter(ctx);
	};
	if ((ai.profile.flags96 & 1) != 0) {
		route(b.f[AiBrain::kTargetSlot] != 0 ? 8 : 7);
		return;
	}
	if ((ai.profile.flags96 & 4) != 0) {
		route(8);
		return;
	}
	const Entity *entity = world.registry.get(ai.handle);
	if ((entity != nullptr ? entity->health : ai.health) <= 0) {
		AiEventEntry event{};
		event.f[0] = aircraft_ground_height(world, ai, 0) <= ai.pos[2] ? 3 : 4;
		event.f[1] = index_of(ai) << 16;
		events.queue(event);
		return;
	}
	b.f[AiBrain::kCombatTimer] = std::max(0, b.f[AiBrain::kCombatTimer]);
	if (b.f[45] == 0) {
		aircraft_turn_sequence = bam_sub(aircraft_turn_sequence, 1);
		b.f[45] = (aircraft_turn_sequence & 1) != 0 ? -1 : 1;
	}
	if (ai.profile.subtype == 2) {
		route(8);
		return;
	}
	const Entity *target = target_entity(world, b.f[AiBrain::kTargetSlot]);
	if (target == nullptr)
		target = target_entity(world, b.f[AiBrain::kDamageInfo]);
	ai.aircraft_controller = 3;
	ai.aircraft_result = ai.aircraft_phase = 0;
	if (target != nullptr) {
		const uint32_t angle = uint32_t(bam_sub(target_heading(ai, *target), ai.heading)) >> 24;
		const uint32_t folded = angle >= 128 ? 256 - angle : angle;
		if (folded <= 63) {
			ai.aircraft_side = angle >= 128 ? 1 : 0;
			ai.aircraft_controller = folded <= 10 ? 1 : folded <= 31 ? 0 : 2;
			const int32_t turn = folded <= 31 ? 1073741760 : 2135553056;
			b.f[AiBrain::kWorkHeading] = bam_add(ai.heading, ai.aircraft_side ? -turn : turn);
		}
	}
	b.f[AiBrain::kStep] = 16;
}

// [orig: AI_ProcessVehicleCombatState @0x461080 (IDB name; the aircraft evade-state tick:
//  subtype-2 flare-only arm @0x4610a2, health<=0 -> event 3/4 @0x4611a9..0x4611e2,
//  vtable+4 movement callback @0x46111f, fallback/8 @0x46112f..0x46113d, floor @0x461153)]
void AiSystem::aircraft_evade_tick(AiEntity &ai, World &world) {
	auto &b = ai.brain;
	if (ai.profile.subtype == 2) {
		flare_timer(ai, world);
		return;
	}
	const Entity *entity = world.registry.get(ai.handle);
	if ((entity != nullptr ? entity->health : ai.health) <= 0) {
		AiEventEntry event{};
		event.f[0] = aircraft_ground_height(world, ai, 0) <= ai.pos[2] ? 3 : 4;
		event.f[1] = index_of(ai) << 16;
		events.queue(event);
		return;
	}
	flare_timer(ai, world);
	ai.aircraft_result = aircraft_movement(ai, world);
	if (ai.aircraft_result == 0)
		b.set_pend((ai.profile.flags100 & 2) != 0 ? b.f[AiBrain::kFallback] : 8);
	b.f[AiBrain::kWorkPosZ] = std::max(b.f[AiBrain::kWorkPosZ],
			bam_add(aircraft_ground_height(world, ai, 0x200000), ai.profile.min_agl));
	if ((ai.profile.flags96 & 0x10) != 0) {
		if (b.f[AiBrain::kFireTimer] > 0) {
			if (Entity *vehicle = world.registry.get(ai.handle))
				world.vehicles.release_flares(*vehicle);
		} else
			b.f[AiBrain::kFireTimer] = 0;
	}
}

// Per-entity movement callback dispatch. Controller phase/result/side are not
// a shared AI scheduling budget. [orig: tables @0x8153B8/@0x8153E0;
// @0x461C30/@0x461CB0/@0x466C20/@0x466DB0/@0x4613A0/@0x461870]
// The two 0x1000x movers carry swapped-looking IDB names: AI_CalcGroundVehicleTarget
// @0x4613A0 is the HELICOPTER mover (controller 0x10000, table 0x8153E0[1]) and
// AI_CalcHelicopterTarget @0x461870 is the PLANE mover (0x10005, 0x815408[1]).
int AiSystem::aircraft_movement(AiEntity &ai, World &world) {
	auto &b = ai.brain;
	const auto &p = ai.profile;
	const auto reacquire = [&]() -> Entity * {
		Entity *target = target_entity(world, b.f[AiBrain::kTargetSlot]);
		if (target == nullptr && (p.flags100 & 2) == 0) {
			target = target_entity(world, b.f[AiBrain::kDamageInfo]);
			if (target == nullptr) {
				AiTarget found{};
				// Variant A [orig: AI_ProcessPatrolStep @0x466C20 (the AI_FindBestTarget
				//  call @0x466CC9); AI_ProcessMovementStep @0x466DB0 (the call @0x466E86)]
				if (acquire_target(world, ai, found, /*variant_a=*/true))
					target = world.registry.get(found.handle);
			}
			if (target != nullptr)
				ai_set_target(world, ai, target->handle);
		}
		return target;
	};
	// The fifth movement row uses the same per-entity phase as rows 0..3.
	// [orig: g_AIMoveStepFnTable row 4 @0x8153D8 -> AI_BeginUpdate @0x457B40]
	if (ai.aircraft_controller == 4)
		return begin_update(ai) ? 1 : 0;
	if (ai.aircraft_controller <= 3) {
		b.f[138] = mul32(3, p.field220 >> 1);
		b.f[AiBrain::kWorkPitch] = b.f[AiBrain::kWorkRoll] = 0;
		if (ai.aircraft_controller <= 2) {
			b.f[AiBrain::kTargetRef] = 0;
			const int limit = ai.aircraft_controller == 0 ? 248
					: ai.aircraft_controller == 1		  ? 372
														  : 434;
			if (ai.aircraft_controller == 1)
				b.f[AiBrain::kOutSpeed] = b.f[AiBrain::kSpeedA];
			if (ai.aircraft_phase >= limit) {
				ai.aircraft_controller = 3;
				ai.aircraft_phase = 0;
				return 0;
			}
			b.f[AiBrain::kWorkPosZ] = bam_add(aircraft_ground_height(world, ai, 0x200000), 327680);
			if (ai.aircraft_controller == 1) {
				if (ai.aircraft_phase < 186) {
					if (bam_add(ai.aircraft_phase, b.f[AiBrain::kStep]) >= 186)
						b.f[AiBrain::kWorkHeading] = bam_add(b.f[AiBrain::kWorkHeading],
								ai.aircraft_side ? 1073741760 : -1073741760);
				} else if (const Entity *target = reacquire())
					b.f[AiBrain::kWorkHeading] = target_heading(ai, *target);
			}
			ai.aircraft_phase = bam_add(ai.aircraft_phase, b.f[AiBrain::kStep]);
			return 1;
		}
		b.f[AiBrain::kOutSpeed] = 0;
		if (ai.aircraft_phase >= 744) {
			b.set_pend((p.flags100 & 2) != 0 ? b.f[AiBrain::kFallback] : 8);
			return 0;
		}
		b.f[AiBrain::kWorkPosX] = ai.pos[0];
		b.f[AiBrain::kWorkPosY] = ai.pos[1];
		b.f[AiBrain::kWorkPosZ] = bam_add(aircraft_ground_height(world, ai, 327680), 327680);
		b.f[AiBrain::kWorkHeading] =
				b.f[AiBrain::kNoTargetIdle] == 0 ? bam_add(ai.heading, INT32_MAX) : ai.heading;
		b.f[AiBrain::kTargetRef] = b.f[AiBrain::kNoTargetIdle] == 0 ? 0 : mul32(b.f[45], 16384);
		if (const Entity *target = reacquire()) {
			if ((p.flags100 & 2) == 0 && b.f[AiBrain::kNoTargetIdle] == 0) {
				b.f[AiBrain::kWorkHeading] = target_heading(ai, *target);
				if (ai.aircraft_phase > 372) {
					b.set_pend(8);
					return 0;
				}
			}
		}
		ai.aircraft_phase = bam_add(ai.aircraft_phase, b.f[AiBrain::kStep]);
		return 1;
	}
	if (ai.aircraft_controller != 0x10000 && ai.aircraft_controller != 0x10005)
		return 0;
	const bool plane = ai.aircraft_controller == 0x10005;
	// Only the helicopter mover clears the lateral-cyclic word at its head; the
	// plane mover never writes [127]. [orig: AI_CalcGroundVehicleTarget @0x4613ca;
	// no [127] store anywhere in AI_CalcHelicopterTarget @0x461870..0x461c1a]
	if (!plane)
		b.f[AiBrain::kTargetRef] = 0;
	const Entity *target = target_entity(world, b.f[AiBrain::kTargetSlot]);
	if (target == nullptr) {
		if (!plane && b.f[AiBrain::kNoTargetIdle] != 0) {
			// [orig: @0x4617a2..0x461811]
			b.f[AiBrain::kWorkPosX] = ai.pos[0];
			b.f[AiBrain::kWorkPosY] = ai.pos[1];
			b.f[AiBrain::kWorkPosZ] = bam_add(aircraft_ground_height(world, ai, 327680), 327680);
			b.f[AiBrain::kWorkHeading] = bam_add(ai.heading, 2147483520);
			b.f[AiBrain::kWorkPitch] = b.f[AiBrain::kWorkRoll] = 0;
			b.f[AiBrain::kOutSpeed] = 0;
			b.f[138] = mul32(3, p.field220 >> 1);
		} else {
			// [orig: helo @0x461812..0x461860; plane @0x461bc1..0x461c12]
			const int32_t timer = b.f[AiBrain::kCombatTimer];
			const int32_t turn = plane ? 417566240 : 1073741760;
			b.f[AiBrain::kWorkHeading] = bam_add(ai.heading,
					timer <= (plane ? 310 : 124)		   ? turn
							: timer <= (plane ? 620 : 310) ? 0
														   : -turn);
			b.f[AiBrain::kOutSpeed] = b.f[AiBrain::kSpeedB];
			b.f[138] = p.patrol_climb;
		}
		return 0;
	}
	const int32_t tx = to_fixed(target->position.x), ty = to_fixed(target->position.y);
	const int32_t bearing = target_heading(ai, *target);
	const uint32_t angle = folded_angle(bam_sub(bearing, ai.heading));
	b.f[AiBrain::kWorkHeading] =
			!plane || angle < 64 || b.f[AiBrain::kCombatTimer] > 186 ? bearing : ai.heading;
	const int32_t ground = aircraft_ground_height(world, ai, 0x200000);
	b.f[AiBrain::kWorkPosZ] = bam_add(b.f[51], plane ? p.field216 : bam_add(ground, p.field216));
	const int32_t distance =
			planar_magnitude(bam_sub(tx, ai.pos[0]), bam_sub(ty, ai.pos[1]));
	if (b.f[AiBrain::kNoTargetIdle] != 0) {
		// The retreat write. The helicopter returns here [orig: @0x461506..0x4615a1];
		// the plane continues into the common tail below [orig: @0x4619a4..0x461a07,
		// falling through to @0x461b3f].
		b.f[AiBrain::kWorkHeading] = bam_add(bearing, 2147483520);
		b.f[AiBrain::kWorkPosZ] = std::max(b.f[AiBrain::kWorkPosZ], bam_add(ground, p.min_agl));
		b.f[AiBrain::kWorkPitch] = b.f[AiBrain::kWorkRoll] = 0;
		b.f[AiBrain::kOutSpeed] = b.f[AiBrain::kSpeedA];
		b.f[138] = p.field220;
		if (!plane)
			return 0;
	} else if (distance > p.approach_cap) {
		if (b.f[AiBrain::kCombatTimer] > (plane ? 930 : 620)) {
			b.set_pend(b.f[AiBrain::kFallback]);
			ai_set_target(world, ai, EntityHandle{});
			return 0;
		}
	} else {
		if (distance >= p.max_chase) {
			if (angle <= (plane ? 63u : 64u) && aircraft_target_in_sight(ai, world))
				b.f[AiBrain::kCombatTimer] = 0;
			b.f[AiBrain::kOutSpeed] = b.f[AiBrain::kSpeedA];
		} else {
			b.f[AiBrain::kWorkPosZ] = bam_add(b.f[51], to_fixed(target->position.z));
			if (plane) {
				if (angle < 64 && aircraft_target_in_sight(ai, world))
					b.f[AiBrain::kCombatTimer] = 0;
				b.f[AiBrain::kOutSpeed] = b.f[AiBrain::kSpeedA];
				if (distance < p.min_chase) {
					b.f[AiBrain::kWorkHeading] = ai.heading;
					if (angle < 64)
						b.f[AiBrain::kOutSpeed] = planar_magnitude(
								bam_sub(tx, target->saved_live_pos[0]),
								bam_sub(ty, target->saved_live_pos[1]));
				}
			} else {
				bool fire_check = true;
				if (distance >= p.min_chase) {
					// [orig: @0x46168a..0x4616ae]
					b.f[AiBrain::kOutSpeed] = b.f[AiBrain::kSpeedA] >> 2;
					const int32_t remaining = bam_sub(distance, p.min_chase);
					if (remaining < mul32(b.f[AiBrain::kOutSpeed], b.f[AiBrain::kStep]) &&
							b.f[AiBrain::kStep] != 0)
						b.f[AiBrain::kOutSpeed] = remaining / b.f[AiBrain::kStep];
					fire_check = angle <= 64; // > 0x40 skips straight to LABEL_35
				} else {
					// [orig: @0x461607..0x461676]
					const AiEntity *other = for_handle(target->handle);
					b.f[AiBrain::kOutSpeed] = other != nullptr
							? other->brain.f[136]
							: planar_magnitude(target->veh.vel_x, target->veh.vel_y);
					const int32_t target_yaw = other != nullptr
							? other->heading
							: bam_heading_from_mission_yaw_deg(target->yaw);
					// The reverse bearing is its own truncated atan2 of the negated
					// deltas, not the forward bearing plus a half turn.
					// [orig: fild/fild/fpatan/fmul/fistp @0x461453..0x46148c, folded
					//  against target Yaw @0x46148c..0x46149e]
					const int32_t reverse = detail::bearing_bam(
							bam_sub(ai.pos[1], ty), bam_sub(ai.pos[0], tx));
					if (angle <= 64 && folded_angle(bam_sub(reverse, target_yaw)) <= 64)
						b.f[AiBrain::kOutSpeed] = bam_sub(0, b.f[AiBrain::kOutSpeed]);
				}
				// Only the combat-timer reset is gated on the fire point; the
				// lateral-cyclic word is written on every within-max_chase tick.
				// [orig: AI_IsTargetInSight @0x4616b1 -> [40] = 0 @0x4616bd;
				//  LABEL_35 [127] = [45] << 14 @0x4616c7, reached from both arms]
				if (fire_check && aircraft_target_in_sight(ai, world))
					b.f[AiBrain::kCombatTimer] = 0;
				b.f[AiBrain::kTargetRef] = mul32(b.f[45], 16384);
			}
		}
	}
	// The common tail: floor, ceiling re-target, speed clamp, zeroed work X/Y.
	// [orig: helo LABEL_40 @0x461702..0x461793; plane @0x461b3f..0x461bb3]
	{
		const int32_t floor_z = bam_add(ground, p.min_agl);
		if (b.f[AiBrain::kWorkPosZ] < floor_z)
			b.f[AiBrain::kWorkPosZ] = floor_z;
		// The helicopter re-targets from 12.5 u above ground, the plane from 50 u;
		// both replace with ground + [51] + 50 u. [orig: 819200 @0x46172c vs
		// 3276800 @0x461b5c; replacement 3276800 @0x46173b/@0x461b6b]
		else if (b.f[AiBrain::kWorkPosZ] > bam_add(ground, plane ? 3276800 : 819200))
			b.f[AiBrain::kWorkPosZ] = bam_add(bam_add(ground, b.f[51]), 3276800);
	}
	if (plane) {
		// [orig: @0x461b71..0x461b8b]
		const int32_t current = b.f[AiBrain::kOutSpeed];
		if (current < p.min_speed)
			b.f[AiBrain::kOutSpeed] = p.min_speed;
		else if (current > b.f[AiBrain::kSpeedA])
			b.f[AiBrain::kOutSpeed] = b.f[AiBrain::kSpeedA];
	} else {
		// [orig: abs32 @0x46174c, the two-limit test @0x461752..0x461764, sign
		//  restore @0x461768..0x46176c]
		const int32_t current = b.f[AiBrain::kOutSpeed];
		const int32_t magnitude = current < 0 ? bam_sub(0, current) : current;
		int32_t limit = p.min_speed;
		if (magnitude < limit || (limit = b.f[AiBrain::kSpeedA], magnitude > limit))
			b.f[AiBrain::kOutSpeed] = current < 0 ? bam_sub(0, limit) : limit;
	}
	b.f[AiBrain::kWorkPosX] = b.f[AiBrain::kWorkPosY] = 0;
	b.f[AiBrain::kWorkPitch] = b.f[AiBrain::kWorkRoll] = 0;
	b.f[138] = p.field220;
	return 0;
}
// [orig: AI_IsTargetInSight @0x456860 (weapon visibility check)]
bool AiSystem::aircraft_target_in_sight(AiEntity &ai, World &world) {
	auto &b = ai.brain;
	const Entity *target = target_entity(world, b.f[AiBrain::kTargetSlot]);
	if (target == nullptr)
		return false;
	// Retail preserves the incoming entity pointer in EAX if primary ammo is
	// empty, so that branch succeeds without testing the secondary weapon.
	if (b.f[AiBrain::kAmmoA] == 0)
		return true;
	int32_t pose[6] = { ai.pos[0], ai.pos[1], ai.pos[2],
		bam_add(ai.profile.fire_a.facing_bam, b.f[AiBrain::kActiveYaw]), ai.pitch, ai.roll };
	int32_t metrics[6];
	if (weapon_target_metrics(world, ai, *target, pose, 0, false, metrics))
		return true;
	if (b.f[AiBrain::kAmmoB] == 0)
		return false;
	pose[3] = bam_add(ai.heading, bam_add(ai.profile.fire_b.facing_bam, b.f[AiBrain::kActiveYaw]));
	return weapon_target_metrics(world, ai, *target, pose, 0, false, metrics);
}
// Aircraft combat has distinct stationary, locked-burst, continuation and
// processed-tick fire legs. In particular only primary shots set the fire bit,
// and a locked burst reuses all six saved relative pose components.
// [orig: AI_TickState_AircraftCombat @0x471710 (IDB name; the aircraft/vehicle
//  brain's combat-state fire leg, off_81523C[8]); the health<=0 head @0x471748..
//  0x472ded is h_aircraft_combat_tick's queue_aircraft_death]
void AiSystem::aircraft_combat_tick(AiEntity &ai, World &world) {
	auto &b = ai.brain;
	const auto &p = ai.profile;
	const int32_t step = b.f[AiBrain::kStep];
	b.f[AiBrain::kTickAccum] = bam_add(b.f[AiBrain::kTickAccum], step);
	b.f[AiBrain::kCooldownPair] = bam_add(b.f[AiBrain::kCooldownPair], mul32(65537, step));
	if ((p.flags100 & 0x40) != 0 && b.f[AiBrain::kBurstWindow] != 0 &&
			uint32_t(b.f[AiBrain::kBurstWindow]) <= 186)
		b.f[AiBrain::kBurstWindow] = bam_add(b.f[AiBrain::kBurstWindow], step);
	else
		b.f[AiBrain::kBurstWindow] = 0;
	const bool processed = b.f[AiBrain::kTickAccum] >= 16;
	if (processed) {
		if ((p.flags96 & 0x10) != 0 && b.f[AiBrain::kFireTimer] > 0)
			if (Entity *vehicle = world.registry.get(ai.handle))
				world.vehicles.release_flares(*vehicle);
		b.f[AiBrain::kCombatTimer] = bam_add(b.f[AiBrain::kCombatTimer], 16);
		b.f[AiBrain::kFireDelay] = std::max(0, bam_sub(b.f[AiBrain::kFireDelay], 16));
		b.f[AiBrain::kTickAccum] = 0;
		b.f[AiBrain::kFireTimer] = std::max(0, bam_sub(b.f[AiBrain::kFireTimer], 16));
		b.f[AiBrain::kRetargetTimer] = bam_add(b.f[AiBrain::kRetargetTimer], 16);
	}
	const auto clear_bone = [&]() { b.bytes()[AiBrain::kBoneFlagByte] = 0; };
	const auto follow = [&]() {
		if ((p.flags100 & 1) != 0)
			update_movement_target(ai, world);
	};
	const auto weapon = [&](int which) -> const AiProfile::WeaponFire & {
		return which == 1 ? p.fire_a : p.fire_b;
	};
	const auto ammo = [&](int which) -> int32_t & {
		return b.f[which == 1 ? AiBrain::kAmmoA : AiBrain::kAmmoB];
	};
	const auto interval = [&](int which) {
		return which == 1 ? p.fire_interval_a : p.fire_interval_b;
	};
	const auto cooldown = [&](int which) {
		const uint32_t pair = uint32_t(b.f[AiBrain::kCooldownPair]);
		return which == 1 ? pair & 0xffffu : pair >> 16;
	};
	const auto ready = [&](int which) {
		return weapon(which).ammo_index >= 0 && ammo(which) != 0 &&
				cooldown(which) >= uint32_t(interval(which));
	};
	const auto shoot = [&](int which, const int32_t out[6]) {
		if (ammo(which) > 0)
			--ammo(which);
		// The optional player-target missile substitution calls sub_545930,
		// which is literally `return 1`; both replacement arms are unreachable.
		fire_ai_round(world, ai, out, out[3], out[4], weapon(which).ammo_index);
		if (which == 1)
			b.bytes()[AiBrain::kBoneFlagByte] |= 0x40;
		b.f[AiBrain::kCooldownPair] = int32_t(
				uint32_t(b.f[AiBrain::kCooldownPair]) & (which == 1 ? 0xffff0000u : 0x0000ffffu));
	};
	const auto save_delta = [&](int which, const int32_t out[6]) {
		if ((p.flags100 & 0x40) == 0)
			return;
		const int base = which == 1 ? AiBrain::kSavedDeltaA : AiBrain::kSavedDeltaB;
		const int32_t pose[6] = { ai.pos[0], ai.pos[1], ai.pos[2], ai.heading, ai.pitch, ai.roll };
		for (int axis = 0; axis < 6; ++axis)
			b.f[base + axis] = bam_sub(out[axis], pose[axis]);
	};
	const auto post_mobile = [&]() {
		if ((p.flags100 & 0x20) != 0) {
			b.f[AiBrain::kSweepPhase] = bam_add(b.f[AiBrain::kSweepPhase], 10918);
			if (b.f[AiBrain::kSweepPhase] > 196608) {
				b.f[AiBrain::kSweepPhase] = -196608;
				b.f[AiBrain::kTargetSlot] = 0; // direct write: slot mirror/refcount are retained
			}
		} else if ((p.flags100 & 0x40) != 0 && b.f[AiBrain::kBurstWindow] == 0)
			b.f[AiBrain::kBurstWindow] = 1;
	};
	const auto scatter = [&](int32_t out[6]) {
		const int32_t mod = 6 - b.f[AiBrain::kAccuracy];
		if (mod <= 0)
			return;
		for (int axis = 3; axis <= 4; ++axis) {
			const int32_t angle = int32_t(double(uint16_t(prng_step_a()) % mod) * 8947848.0f);
			out[axis] = (angle & 1) != 0 ? bam_add(out[axis], angle) : bam_sub(out[axis], angle);
		}
	};
	const auto aim_offset = [&]() {
		return (p.flags100 & 0x20) != 0	   ? b.f[AiBrain::kSweepPhase]
				: (p.flags100 & 0x40) != 0 ? -196608
										   : 0;
	};
	Entity *target = target_entity(world, b.f[AiBrain::kTargetSlot]);
	if ((p.flags100 & 0x80) != 0) {
		if (b.bytes()[AiBrain::kGuardFireByte] != 0) {
			b.f[AiBrain::kCombatTimer] = 0;
			if (b.f[AiBrain::kNoTargetIdle] == 0) {
				bool fired = false;
				for (int which = 1; which <= 2 && !fired; ++which) {
					int32_t out[6];
					if (ready(which) &&
							solve_weapon_fire_transform(
									world, ai, target, weapon(which), 0, false, out)) {
						shoot(which, out);
						b.f[AiBrain::kLastWeapon] = which;
						fired = true;
					}
				}
				if (!fired) {
					clear_bone();
					b.f[AiBrain::kLastWeapon] = 0;
				}
			}
		} else {
			clear_bone();
			if (b.f[AiBrain::kCombatTimer] >= 620)
				b.set_pend(b.f[AiBrain::kFallback]);
		}
		if (processed) {
			ai_set_target(world, ai, EntityHandle{});
			follow();
		}
		return;
	}
	if ((p.flags100 & 0x40) != 0 && b.f[AiBrain::kBurstWindow] != 0) {
		const int which = b.f[AiBrain::kLastWeapon];
		if (b.f[AiBrain::kNoTargetIdle] != 0 || (which != 1 && which != 2))
			clear_bone();
		else if (ammo(which) != 0) {
			if (ready(which)) {
				const int base = which == 1 ? AiBrain::kSavedDeltaA : AiBrain::kSavedDeltaB;
				int32_t out[6] = { ai.pos[0], ai.pos[1], ai.pos[2], ai.heading, ai.pitch, ai.roll };
				for (int axis = 0; axis < 6; ++axis)
					out[axis] = bam_add(out[axis], b.f[base + axis]);
				shoot(which, out);
			} else if (cooldown(which) >= uint32_t(p.fire_interval_a - (p.fire_interval_a >> 2)))
				clear_bone();
		}
		if (processed)
			follow();
		return;
	}
	const auto retreat = [&](int32_t heading) {
		b.f[AiBrain::kWorkHeading] = heading;
		const int32_t ground = aircraft_ground_height(world, ai, 0x200000);
		b.f[AiBrain::kWorkPosZ] =
				bam_add(b.f[51], p.subtype == 2 ? p.field216 : bam_add(ground, p.field216));
		b.f[AiBrain::kWorkPosZ] = std::max(b.f[AiBrain::kWorkPosZ], bam_add(ground, p.min_agl));
		b.f[AiBrain::kWorkPitch] = b.f[AiBrain::kWorkRoll] = 0;
		b.f[AiBrain::kOutSpeed] = b.f[AiBrain::kSpeedA];
		b.f[138] = p.field220;
	};
	if (target == nullptr) {
		b.f[AiBrain::kSweepPhase] = -196608;
		b.f[AiBrain::kWorkPosX] = b.f[AiBrain::kWorkPosY] = 0;
		b.f[AiBrain::kWorkPosZ] =
				bam_add(aircraft_ground_height(world, ai, 0x200000), bam_add(b.f[51], p.field216));
		b.f[AiBrain::kWorkPitch] = b.f[AiBrain::kWorkRoll] = 0;
		b.f[AiBrain::kOutSpeed] = b.f[AiBrain::kSpeedA];
		b.f[138] = p.field220;
		if ((p.flags100 & 1) != 0)
			follow();
		else if ((p.flags100 & 4) != 0)
			retreat(ai.heading);
		else {
			aircraft_movement(ai, world);
			AiTarget found{};
			// [orig: AI_TickState_AircraftCombat @0x471710 (the AI_FindBestTarget
			//  call @0x472B45)]
			if (acquire_target(world, ai, found, /*variant_a=*/true)) {
				const Entity *self = world.registry.get(ai.handle),
							 *other = world.registry.get(found.handle);
				if (self != nullptr && other != nullptr)
					apply_engage_relations(world, *self, *other);
				ai_set_target(world, ai, found.handle);
				target_set_calls.push_back(found.net_id);
				b.f[AiBrain::kCombatTimer] = 0;
				b.f[AiBrain::kFireDelay] = p.field104;
				if (p.field104 != 0)
					b.f[AiBrain::kFireDelay] = bam_add(p.field104, uint16_t(prng_step_a()) % 62);
				return;
			}
		}
		if (b.f[AiBrain::kCombatTimer] >= (p.subtype == 2 ? 930 : 620)) {
			ai_set_target(world, ai, EntityHandle{});
			b.set_pend(b.f[AiBrain::kFallback]);
		}
		return;
	}
	if (target->health <= 0) {
		ai_set_target(world, ai, EntityHandle{});
		return;
	}
	if (!processed && b.f[AiBrain::kNoTargetIdle] == 0) {
		const int which = b.f[AiBrain::kLastWeapon];
		if (which == 1 || which == 2) {
			if (!ready(which))
				return;
			int32_t out[6];
			if (!solve_weapon_fire_transform(
						world, ai, target, weapon(which), aim_offset(), true, out)) {
				clear_bone();
				return;
			}
			save_delta(which, out);
			if (which == 2 || b.f[AiBrain::kAccuracy] != 4)
				scatter(out);
			shoot(which, out);
			post_mobile();
		} else {
			// No weapon on record: a TURRET-flagged (&1) block is re-solved so its
			// saved delta tracks the moving hull; primary first and done, else the
			// secondary, else nothing at all (the bone byte is left as it was).
			// [orig: primary block flags (profile+120+16) & 1 @0x471c18 -> solve,
			//  save, bone = 0, return @0x471d04..0x471d10; secondary block flags
			//  (profile+152+16) & 1 == 0 -> return @0x471d18; else solve, save,
			//  bone = 0 @0x471de9..0x471df5]
			const int slot = (weapon(1).flags & 1) != 0 ? 1 : (weapon(2).flags & 1) != 0 ? 2 : 0;
			if (slot == 0)
				return;
			int32_t out[6] = { ai.pos[0], ai.pos[1], ai.pos[2], ai.heading, ai.pitch, ai.roll };
			solve_weapon_fire_transform(
					world, ai, target, weapon(slot), aim_offset(), true, out);
			save_delta(slot, out);
			clear_bone();
		}
		return;
	}
	if (b.f[AiBrain::kFireDelay] != 0) {
		follow();
		return;
	}
	// Every rescan feeds its result to Entity_SetAITarget, so a scan that finds
	// nothing clears brain[38]; the bearing keeps the old pointer while the
	// solves below read brain[38] [orig: AIEntity_TryAcquireTarget @0x4716B0
	// (`call Entity_SetAITarget` @0x4716F0); caller `test eax,eax; jz`
	// @0x472351..0x472355].
	const Entity *tracked = target;
	if (b.f[AiBrain::kRetargetTimer] > 248) {
		b.f[AiBrain::kRetargetTimer] = 0;
		AiTarget fresh{};
		// A type-1 profile searches with variant A [orig: AIEntity_TryAcquireTarget
		// `cmp dword ptr [eax+10h],1` @0x4716D5 (the AI_FindBestTarget call @0x4716DD)].
		ai_set_target(world, ai,
				acquire_target(world, ai, fresh, ai.profile.type == 1) ? fresh.handle
				                                                       : EntityHandle{});
		target = target_entity(world, b.f[AiBrain::kTargetSlot]);
		if (target != nullptr)
			tracked = target;
	}
	const int32_t bearing = target_heading(ai, *tracked);
	if ((p.flags100 & 1) != 0)
		follow();
	else if ((p.flags100 & 4) != 0 || b.f[AiBrain::kNoTargetIdle] != 0)
		retreat(bam_add(bearing, 2147483520));
	else
		aircraft_movement(ai, world);
	// The fire-arc limit is the profile's secondary FOV byte read SIGNED
	// (movsx), OR'd with 1 at the head and with 2 at the gate, then shifted
	// arithmetically and compared UNSIGNED against the folded bearing delta —
	// so a byte >= 0x80 yields a negative limit that admits every bearing.
	// [orig: movsx eax, byte ptr [profile+67]; or eax, 1 @0x471736..0x47173a;
	//  or ecx, 2; sar ecx, 1; cmp eax, ecx; ja loc_472DF5 @0x472477..0x472481]
	const int32_t limit = (int32_t(int8_t(p.fov_secondary)) | 3) >> 1;
	// Out of arc jumps straight to the epilogue: the bone byte and last_weapon
	// keep their values so the continuation leg keeps firing between processed
	// ticks. [orig: ja loc_472DF5 @0x472481 -> pop/ret @0x472df5]
	if (folded_angle(bam_sub(bearing, ai.heading)) > uint32_t(limit))
		return;
	for (int which = 1; which <= 2; ++which) {
		int32_t out[6];
		if (!ready(which) ||
				!solve_weapon_fire_transform(
						world, ai, target, weapon(which), aim_offset(), false, out))
			continue;
		save_delta(which, out);
		if (b.f[AiBrain::kAccuracy] != 4)
			scatter(out);
		shoot(which, out);
		post_mobile();
		b.f[AiBrain::kLastWeapon] = which;
		return;
	}
	// In arc with neither block ready/solved. [orig: LABEL_209 @0x4729ed..0x4729f4]
	clear_bone();
	b.f[AiBrain::kLastWeapon] = 0;
}
} // namespace opennova::world
