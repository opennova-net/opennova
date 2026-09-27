#include <runtime/world/vehicle_system.h>

#include <runtime/world/ai.h>
#include <runtime/world/angle.h>
#include <runtime/world/vehicle_attach.h>
#include <runtime/world/vehicle_motor.h>
#include <runtime/world/vehicle_part_anim.h>
#include <runtime/world/world.h>

namespace opennova::world {

// One pool-1 row's +0x1C4 mover leg, run from the row's own pool-1 visit
// (World::update_all_entities): every row with vehicle traits (the items.def
// class callback, including selector zero) runs its family's drive core on
// the authority -- ground/bike through the cveh core, watercraft through the
// cbot mover, CHel/cpln through the shared aircraft mover -- consuming a
// mounted ctrl/drvr player's replicated input. A joiner's copies are
// wire-posed; its leg is the client prediction/presentation subset.
// [orig: Entity_UpdatePool1Slot @0x4B8E41..0x4B8E53 -> the class row's mover:
//  Entity_DispatchPhysics_cveh @0x48efc0 -> Entity_UpdateVehiclePhysics
//  @0x48af00 / _cbot @0x48EFA3 -> Entity_UpdateWatercraftPhysics @0x48D480;
//  authority drive gates @0x48b0ff / @0x48DF8C]
void VehicleSystem::update_motor(Entity &row, bool is_authority) {
    World &world = world_;
    const EntityHandle h = row.handle;
    Entity *veh = &row;
    if (veh->motor_suspended || this->traits.get(veh->item_id) == nullptr) return;
    if (is_authority) {
			// The installed death callback replaces +0x1C4's live mover.
			// The AI death-state callback at +0x1C8 can also run this tick.
			// [orig: Entity_UpdatePool1Slot @0x4B8E2A..0x4B8E53]
			if (veh->death_motion != DeathMotionMode::None)
				return;
			if (!veh->veh.spawn_pose_valid)
				capture_spawn_pose(*veh);
			const VehicleTraits *traits = this->traits.get(veh->item_id);
			if (traits == nullptr)
				return;
			// catv's callback switches the entire mover (including input,
			// sound and trails) using the previous tick's afloat flag.
			// [orig: Entity_DispatchPhysicsUpdate @ 0x48F010]
			VehicleTraits afloat_traits;
			if (traits->amphibian && (veh->flags & 0x8000u) != 0) {
				afloat_traits = *traits;
				afloat_traits.family = VehicleFamily::Watercraft;
				traits = &afloat_traits;
			}
			// Mover-entry savedLivePose [orig: the +0x80..+0x94 prologue
			// stamps every mover carries; rider deltas read (current - saved)].
			// The ground-family movers stamp at their own head (tick_motor).
			if (traits->family == VehicleFamily::Watercraft ||
					vehicle_family_uses_direct_air_mover(traits->family))
				stamp_saved_live_pose(*veh);
			// The family movers promote a freshly allocated brain out of the
			// allocator's state 0 at their HEAD, before the occupant/AI-driver
			// block reads the word: the AI leg's PRETTY -> FOLLOWWP hand-back
			// (22 -> 16 @0x48bc16, 14 -> 7 @0x49158a) has to see PRETTY, not 0.
			// Our drive staging runs ahead of the motor (and of the motor-side
			// twin in tick_health), so the promotion is hoisted with it: a driven
			// hull left at 0 here never handed back — the state machine commits
			// the pending 0 straight back every tick. The mover must still
			// hand the authored state back to FOLLOWWP; a fresh brain retains
			// its zero working speed until a real movement controller sets it.
			// [orig: Entity_UpdateVehiclePhysics `cmp [edi+10h],0; jnz; mov
			//  [edi+10h],16h` @0x48afac..0x48afb2, ahead of the occupant block
			//  @0x48b949; Entity_UpdateAircraftPhysics @0x490377..0x49037d
			//  (0 -> 14); the boat family shares the ground stamp]
			if (AiEntity *brain = world.ai.for_handle(h)) {
				if (brain->brain.f[AiBrain::kCurState] == 0)
					brain->brain.f[AiBrain::kCurState] =
							vehicle_family_uses_direct_air_mover(traits->family) ? 14 : 22;
			}
			// Direct CHel/cpln rows never reach the ground cmd/motor leg: the
            // class table routes them to the shared aircraft mover, whose AI
            // brain leg and physics live in one function. A live PLAYER pilot
            // drives through the predicted path instead. [orig: the class table
            // dispatch -> Entity_UpdateAircraftPhysics @0x490310, never the
            // ground core @0x48af00]
            if (vehicle_family_uses_direct_air_mover(traits->family)) {
                if (!veh->veh.net_predicted) {
                    Entity *actrl = world.vehicles.resolve_controller(*veh);
                    const bool actrl_alive = actrl != nullptr && actrl->alive &&
                                             actrl->health > 0;
                    // A player pilot whose eye sits at or below the water plane
                    // loses the stick to the AI leg, as in the staged families.
                    // [orig: Entity_UpdateAircraftPhysics `test [ebp+24h],100h`
                    //  @0x490F36, the eye test @0x490F3F..0x490F4B]
                    const bool aplayer = actrl_alive && actrl->handle.pool() == 0 &&
                                         actrl->player_class != 0 &&
                                         !watercraft_driver_submerged(world, *actrl);
                    if (aplayer) {
                        // The player leg parks the brain at PRETTY every visit, so
                        // an AI state left from an earlier pilot stops running.
                        // [orig: Entity_UpdateAircraftPhysics `mov dword ptr
                        //  [ebx+10h],0Eh` @0x490F6A]
                        if (AiEntity *brain = world.ai.for_handle(h))
                            brain->brain.f[AiBrain::kCurState] = 14;
                        // A PLAYER pilot still runs the shared mover: retail has
                        // ONE aircraft function, and its occupant-input block
                        // (our stage_air_vehicle_input) stages the same
                        // fwd/lat/steer/altitude registers the AI leg fills.
                        // Skipping the mover here left a player in the pilot
                        // seat with no physics at all - the aircraft simply did
                        // not respond.
                        // [orig: Entity_UpdateAircraftPhysics @0x490310 — the
                        //  input gate is `(occ->Flags & 0x100) && (occ ==
                        //  g_LocalPlayerEntity || is_authority)`, not a
                        //  separate mover]
                        world.vehicles.aircraft_client_tick(*veh, *traits);
                    } else {
                        world.ai.chel_ai_drive(world, *veh, actrl_alive ? actrl : nullptr,
                                      *traits);
                        world.vehicles.aircraft_client_tick(*veh, *traits);
						if (AiEntity *brain = world.ai.for_handle(h)) {
							brain->brain.f[AiBrain::kWorkPosZ] = veh->veh.net_alt_target;
							brain->brain.f[AiBrain::kWorkHeading] = veh->veh.steer_target_bam;
							brain->brain.f[135] = veh->veh.cmd_lateral_speed;
							brain->brain.f[136] = veh->veh.cmd_speed;
							brain->brain.f[137] = veh->veh.net_climb;
						}
					}
				} else {
					// A predicted row skips the mover, so the mover's tail call
                    // never runs for it. Retail's client has no such skip — it
                    // runs the aircraft function (and therefore the tail) for
                    // every vehicle it is not driving, seeding the drive
                    // command from the wire — so advancing the accumulator here
                    // restores that, it does not add a new one.
                    // [orig: the HELO twin @0x48FA70 called from the aircraft
                    //  mover's tail @0x4905A6; the not-driven client leg is
                    //  @0x48B7F0]
                    world.vehicles.part_anim_tick(*veh, *traits);
				}
				// The installed +0x1C4 callback: the saved mover above, then the
				// same-refNum children ride the 'agun' points [orig:
				// Entity_UpdateAttachedChildren @0x45D550, @0x45D573 then @0x45D578..].
				world.vehicles.update_attached_children(*veh);
				if (AiEntity *ve = world.ai.for_handle(h)) {
                    ve->pos[0] = to_fixed(veh->position.x);
                    ve->pos[1] = to_fixed(veh->position.y);
                    ve->pos[2] = to_fixed(veh->position.z);
					ve->pitch = veh->veh.air_pitch_bam;
					ve->roll = veh->veh.air_roll_bam;
					ve->heading = veh->veh.yaw_seeded
							? veh->veh.yaw_bam
							: bam_heading_from_mission_yaw_deg(static_cast<double>(veh->yaw));
				}
				return;
            }
            // Stage the drive input class the motor will consume: a live PLAYER controller
            // keeps the occupant leg; an AI controller (or none) routes through the brain
            // (state stamps + the witnessed steer/speed leg). [orig: the occupant class
            // switch inside Entity_UpdateVehiclePhysics @0x48b949-0x48c034]
            VehicleDriveCmd cmd;
            if (traits->player_control) {
                Entity *ctrl = world.vehicles.resolve_controller(*veh);
                // A DEAD controller parks the vehicle. The infantry death edge detaches
                // first; this guard preserves the same result if the vehicle pass happens
                // to observe the controller earlier in the frame.
                // [orig: infantry death detach @0x4b9c57..0x4b9c60]
                const bool ctrl_alive =
                        ctrl != nullptr && ctrl->alive && ctrl->health > 0;
                bool player_ctrl = ctrl_alive && ctrl->handle.pool() == 0 &&
                                   ctrl->player_class != 0;
                // A PLAYER driver whose head is under the water plane is driven
                // by the AI leg in every family this staging serves, not only the
                // boat. [orig: the submerged-driver cuts — cveh @0x48B9A0..0x48B9AC
                // -> @0x48BC12; ctan @0x489579..0x489585 -> @0x4897DB; cbik
                // @0x484AC6..0x484AD2 -> @0x484DB8; cbot @0x48DFD3..0x48DFDF
                // -> @0x48E247]
                if (player_ctrl && watercraft_driver_submerged(world, *ctrl))
                    player_ctrl = false;
                if (player_ctrl) {
                    // A player drive freezes the SM mover exactly like the parked leg —
                    // the route never advances under a human driver [orig: the player
                    // leg forces SM state 22 too @0x48b993 / the boat leg @0x48DFF5].
                    if (AiEntity *ve = world.ai.for_handle(h)) {
                        ve->brain.f[AiBrain::kCurState] = 22;
                    }
                } else if (traits->family == VehicleFamily::Watercraft) {
                    world.ai.watercraft_ai_drive(world, *veh, ctrl_alive ? ctrl : nullptr,
                                        *traits, cmd);
                } else {
                    world.ai.vehicle_ai_drive(world, *veh, ctrl_alive ? ctrl : nullptr, *traits,
                                     cmd);
                }
            }
            // Per-family motor dispatch, the class-table split [orig:
            // Entity_DispatchPhysics_cbot @0x48EFA3 -> the cbot mover @0x48D480
            // vs _cveh @0x48efc0 -> the ground core @0x48af00].
            if (traits->family == VehicleFamily::Watercraft) {
                world.vehicles.tick_watercraft_motor(*veh, *traits, &cmd);
            } else {
                world.vehicles.tick_motor(*veh, *traits, &cmd);
            }
            // The installed +0x1C4 callback: the saved mover above, then the
            // same-refNum children ride the 'agun' points [orig:
            // Entity_UpdateAttachedChildren @0x45D550, @0x45D573 then @0x45D578..].
            world.vehicles.update_attached_children(*veh);
            // Mirror the integrated transform back into the brain entity — one struct in
            // the original; the SM mover and the present snapshot read pos[]/heading.
            if (AiEntity *ve = world.ai.for_handle(h)) {
                ve->pos[0] = to_fixed(veh->position.x);
                ve->pos[1] = to_fixed(veh->position.y);
                ve->pos[2] = to_fixed(veh->position.z);
                ve->heading = veh->veh.yaw_seeded
                        ? veh->veh.yaw_bam
                        : bam_heading_from_mission_yaw_deg(static_cast<double>(veh->yaw));
                // The mover's command registers ARE brain[132]/[136]/[137]
                // (brain+0x210/+0x220/+0x224, the mover's moveMode base = entity
                // +0x64): the next think reads the mover's steer target back, e.g.
                // the evade tick's arrival test. The aircraft branch above carries
                // its own mirror. [orig: ctan stores @0x489952 / @0x489811, cveh
                // @0x48BD89 / @0x48BC35; the base load @0x488ACA..0x488AD9; the
                // reader AI_UpdatePatrolBehavior @0x457DD4]
                ve->brain.f[AiBrain::kWorkHeading] = veh->veh.steer_target_bam;
                ve->brain.f[136] = veh->veh.cmd_speed;
                ve->brain.f[137] = veh->veh.steer_ramp_bam;
            }
        return;
    }
    // A joiner does not integrate its replicated pool-1 vehicle copies here, but
    // retail still executes the per-entity ground callback's presentation leg on
    // clients. Evaluate sound from the current wire/local state, leaving
    // position, heading, and motor accumulators untouched. Collision contact is
    // authority-physics state and therefore unavailable on this path; an
    // explicit replicated collision bit can replace `false` later.
    // [orig: Entity_UpdateVehiclePhysics @0x48af00; movement-sound call
    // @0x48d181..0x48d1c4]
    {
			// The installed death callback replaces +0x1C4's live mover.
			// The AI death-state callback at +0x1C8 can also run this tick.
			// [orig: Entity_UpdatePool1Slot @0x4B8E2A..0x4B8E53]
			if (veh->death_motion != DeathMotionMode::None)
				return;
			if (!veh->veh.spawn_pose_valid)
				capture_spawn_pose(*veh);
			const VehicleTraits *traits = this->traits.get(veh->item_id);
			if (traits == nullptr)
				return;
			// catv's callback switches the entire mover (including input,
			// sound and trails) using the previous tick's afloat flag.
			// [orig: Entity_DispatchPhysicsUpdate @ 0x48F010]
			VehicleTraits afloat_traits;
			if (traits->amphibian && (veh->flags & 0x8000u) != 0) {
				afloat_traits = *traits;
				afloat_traits.family = VehicleFamily::Watercraft;
				traits = &afloat_traits;
			}
			// Mover-entry savedLivePose, stamped BEFORE the prediction gates
			// so a frozen/parked hull reads as zero rider delta — retail
			// stamps in every mover prologue regardless of the later bails
			// [orig: the +0x80..+0x94 prologue stamps; the deck-ride reads
			// @0x4b530b../@0x4ba47f..]. The ground-family client mover stamps
			// at its own head, ahead of its chase.
			const bool ground_client_mover = veh->veh.net_predicted && veh->health > 0 &&
					(traits->family == VehicleFamily::Ground ||
							traits->family == VehicleFamily::Bike ||
							traits->family == VehicleFamily::Tank);
			if (!ground_client_mover)
				stamp_saved_live_pose(*veh);
			// The joiner-side family prediction (net-re §5.38e B-facet, all
            // four families landed): each mover chases the staged wire target
            // and predicts between records from the mirrored speed/steer
            // registers — the client-executed subset of its family mover
            // [orig: cbot @0x48D480; CHel/cpln via the @0x45D6F0 thunk;
            // ground @0x48af00 core]. The embedding sim clears net_predicted
            // for wire-frozen rows (bit0 / dead-pose / carried), so a wreck
            // never keeps driving (D-NET-66).
            if (veh->veh.net_predicted && veh->health > 0 &&
                    traits->family == VehicleFamily::Watercraft) {
                world.vehicles.watercraft_client_tick(*veh, *traits);
            } else if (veh->veh.net_predicted && veh->health > 0 &&
                    (traits->family == VehicleFamily::Helicopter ||
                     traits->family == VehicleFamily::Plane)) {
                world.vehicles.aircraft_client_tick(*veh, *traits);
            } else if (ground_client_mover) {
                // Runs the motor core, whose tail already ticks the movement
                // sound — skip the separate sound call below for this row.
                // Bikes and tanks ride the same entry; the core branches on
                // the family tag for the witnessed cbik deltas (gravity 250,
                // vZ up-cap, contact-gated integration, always-applied yaw)
                // and the ctan deltas (gravity 250, contact-gated integration
                // with the ±2·decel reversal clamps, full-basis velocity,
                // crash-settle-gated yaw with the airborne quarter-rate)
                // [orig: @0x483FE0 / @0x488AB0 vs @0x48AF00].
                world.vehicles.ground_client_tick(*veh, *traits);
                return;
            }
            // The shared aircraft mover has no movement-sound call. In retail,
            // Entity_ProcessMovementSoundEffects @0x5294A0 is reached from the
            // ground/bike/water paths, but neither CHel @0x490310 nor cpln's
            // thunk calls it. Physicsless air rows are newly eligible above, so
            // keep them out of the ground-sound presentation tail in every
            // prediction/death state.
            if (vehicle_family_uses_direct_air_mover(traits->family)) return;
            world.vehicles.update_ground_sound(*veh, *traits,
                                        /*wrecked=*/veh->health <= 0,
                                        /*collided=*/false);
    }
}

} // namespace opennova::world
