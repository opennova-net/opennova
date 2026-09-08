// The vehicle system: the per-item physics traits the item-traits sweep feeds,
// the motor pass the AI tick runs, and the seat/mount, motor, contact-solve,
// suspension, sound and part-anim verbs that reached the world through a
// `World &` first parameter before ADR 0043 slice E6. Bound to its world at
// construction (EntityCommands' precedent); every method body still names
// that world `world`, so the witnessed bodies read as they did.
#pragma once

#include <runtime/devtools/tick_profile.h>
#include <runtime/world/entity.h>
#include <runtime/world/vehicle_attach.h>
#include <runtime/world/vehicle_motor.h>
#include <runtime/world/vehicle_mount.h>
#include <runtime/world/vehicle_part_anim.h>
#include <runtime/world/vehicle_sound.h>
#include <runtime/world/vehicle_suspension.h>

#include <cstdint>
#include <string>
#include <vector>

namespace opennova::world {

class World;

class VehicleSystem {
public:
    explicit VehicleSystem(World &world) : world_(world) {}
    VehicleSystem(const VehicleSystem &) = delete;
    VehicleSystem &operator=(const VehicleSystem &) = delete;

    // Per-item vehicle physics traits (empty until the host's item-traits sweep feeds
    // it — Simulation::resolve_item_traits). The AI tick's vehicle pass runs the
    // ground-vehicle motor for pool-1 entities whose traits carry a non-zero `physics`
    // selector. [orig: ItemDef_ParsePhysicsProperty @0x49d870 fields consumed by
    // Entity_UpdateVehiclePhysics @0x48af00; vehicle_motor.h]
    VehicleTraitsTable traits;

    // The per-tick motor pass, in the AI tick's slot between the entity loop and
    // the AI event queue: the authority motors, then a joiner's prediction and
    // sound legs. Marks the SIM_AI_*VEHICLE* profile rows on the caller's lap.
    void tick_motors(bool is_authority, devtools::ProfileLap &lap);
	void initialize_mission_vehicles();
	void build_spawn_markers();
	void tick_spawn_markers();
	void capture_spawn_pose(Entity &vehicle);
	bool resolve_spawn_pose(const Entity &vehicle, int32_t pose[6]) const;
	void respawn(Entity &vehicle);
	void tick_dead(Entity &vehicle, AiEntity &brain);
	void release_flares(Entity &vehicle);
	void tick_flare_input(Entity &vehicle);
	void cleanup_destroyed_ref_group(Entity &vehicle);

	// Validate + apply one C2S 0x26 attach request: `player` mounts `vehicle` at model-bone
	// `bone` (1-based, the wire byte — the occupancy/echo key). Returns true iff attached.
	// Ported validation order [orig: Entity_ProcessVehicleAttach @0x435AA0]:
	//   1. resolve both handles; reject a missing entity or either side dead
	//      (Flags & 2 / health <= 0) [orig: @0x435b01];
	//   2. seat classification: the vehicle seat whose bone_index matches the wire bone
	//      [orig: Entity_GetBoneSlotType @0x434ED0 classifies the MODEL USRP row name —
	//      48-byte rows, name +32, 1-based index; sitex 1 / ctrlx 2 / drvrx 5 / UseGun 3].
	//      Production seat specs preserve that exact enumeration; unknown rows reject;
	//   3. enemy-occupant gate: reject when a LIVE ENEMY already occupies the vehicle
	//      [orig: Vehicle_HasEnemyOccupant @0x4359F0 — scans pool 0, skips dead/self/
	//      same-team; same-team occupants never block multi-seat co-boarding];
	//      (the weapon-busy gate on EquippedSlot->currentAction [orig: @0x435b29] is
	//      unmodeled — no weapon action state server-side; tracked in D-NET-157)
	//   4. seat occupancy: an occupied matching seat / an already-taken wire bone rejects
	//      [orig: @0x435ba9 mountHandles[idx] != 0xFFFF];
	//   5. an already-mounted requester detaches first [orig: @0x435bce];
	//   6. writes: seat occupant + mount_target/mount_bone/mount_type/mount_seat, then
	//      stance bits clear [orig: MoveOrder &= ~0x300 @0x435c42 + input latches
	//      @0x435c54]. Generic vehicle slots clear 0xA000 and set Flags 0x40
	//      [orig: Entity_AttachToVehicleSlot @0x494752]; UseGun clears 0xA000 without
	//      setting 0x40 and binds the parent weapon slot
	//      [orig: Entity_AttachToUseGunSlot @0x546c42-0x546c7c].
	bool process_attach(EntityHandle player, EntityHandle vehicle, uint8_t bone);
    // Apply an authority-confirmed relation on a client. Replaces the previous
    // seat occupant and repairs a lost Controller/Driver/Gunner claim even when
    // the carrier and bone are unchanged; bone 0 / no carrier detaches.
    // Returns true when the local relationship changed.
    // [orig: Entity_TryAttachOrDetach @0x436610; client attach @0x435BBA]
    bool apply_confirmed_mount(EntityHandle player, EntityHandle vehicle, uint8_t bone);
    // Apply one C2S 0x27 detach: release every seat this occupant holds on its mount target,
    // clear the mount fields + any generic-slot 0x40 flag + the stance bits, and run the
    // +368 primary-occupant release (the engine-stop edge fires only for the claimant).
    // [orig: Entity_DetachFromVehicle @0x4355F0 — MoveOrder &= ~0x300, EquippedSlot
    //  restore for parentSlot 2/3 then non-player clear @0x435671..0x4356aa (ported),
    //  the +368/+0x170 claimant leg @0x4356e9..0x43577c,
    //  all matching mountHandles -> 0xFFFF, Flags &= ~0x40, +0x16C/+0x157/+0x168 cleared.]
    // Returns true iff the entity was mounted.
    bool detach(EntityHandle player);
    // Validate and atomically attach to an already selected seat. This is the one
    // authoritative relationship-write path used by wire requests, script mounts,
    // use-key mounts, and mobile-spawn deployment.
    // [orig: Entity_RequestVehicleAttach @0x4364A0 ->
    //  Entity_ProcessVehicleAttach @0x435AA0]
    bool attach_to_seat(EntityHandle player, const VehicleSeatSelection &selection);
    // The use-key nearest-seat scan [orig: Entity_FindNearestSeatOrArmory @0x435d50]: for
    // every live seat-bearing entity, test each FREE seat's world position against the player
    // eye: 3D distance <= 4.0 u (0x40000 16.16) and the point inside the view cone —
    // the yaw/pitch offset from the entity Yaw/Pitch, each clamped to +100 deg, as a
    // BAM32 magnitude <= 0x3FFFFFC0 (just under 90 deg) standing / 0x38E38E0 (5.0 deg)
    // seated — LOS-gated, score = dist3d + aim/512, lowest wins; a seated USE therefore
    // swaps only onto a seat the rider looks at and dismounts otherwise. Enemy-occupied
    // vehicles are skipped
    // [orig: Vehicle_HasEnemyOccupant reject @0x435e58]. armory_mode = the original's
    // searchMode != 0 [orig: @0x435f12]: instead of seats, "armory*" points of Armory-attrib
    // items are scanned with the same math (no occupancy), reporting SeatType::ArmoryPoint
    // [orig: the attrib 0x80000 walk @0x4361ee, seatType 4 @0x436417] — the label highlight
    // pick while the player stands in the armory volume; the mount toggle always scans seats.
    // The candidate set is the player's proximity slice (for_each_scan_candidate walks the
    // same list the original does), the eye is the live entity+0x6C/+0x70/+0x74 offset the
    // body tick restamps, the candidate's own hull never occludes its seats (the USE LOS
    // walker skips the endpoint entity and both parent slots, so a mounted USE swaps to
    // any free seat in reach and dismounts only when the scan is empty [orig:
    // raycast_against_entity_pool ctx[17..20] @0x538832..0x538859]), and an emplaced
    // gun's LOS endpoint is its carrier (def attrib 0x20 ->
    // groundEntity) with the reject legs in place. Regressions: vehicle_mount_test.
    bool find_nearest_free_seat(const Entity &player, VehicleSeatSelection &out, bool armory_mode, const VehicleOccupancySource *source = nullptr);
    // The toggle's seat candidate, in the witnessed search order: an unmounted
    // player standing on a seat-bearing ground target takes that carrier's best
    // seat first; otherwise the nearest-free-seat scan above (seats mode).
    bool find_mount_toggle_candidate(const Entity &player, VehicleSeatSelection &r_hit, const VehicleOccupancySource *source = nullptr);
    // The floating seat/armory label list for the local player, a structural translation of
    // the selection half of [orig: draw_vehicle_seat_and_armory_labels @0x5a3290]:
    //  - no nearest scan hit -> no labels at all [orig: the Entity_FindNearestSeatOrArmory
    //    gate @0x5a32e2];
    //  - can_fire limits labels to the nearest entity; when the player cannot fire, every
    //    in-range candidate labels [orig: !Player_CanFireWeapon() || entity == nearest
    //    @0x5a3354];
    //  - per entity: dead/destroyed skip, enemy-occupant reject [orig: @0x5a3373/@0x5a3395];
    //  - armory_mode false: every FREE seat within 4.0 u 3D of the player position
    //    (point lifted +0.1875 u) with clear LOS labels [orig: the seat loop @0x5a3464,
    //    occupancy skip @0x5a348f, distance @0x5a35f0, raycast @0x5a3609];
    //  - armory_mode true: the "armory*" points of Armory-attrib items label instead
    //    [orig: @0x5a36f5..@0x5a38e2].
    // Labels append to out in scan order; nearest marks the scan winner's own label.
    void collect_attach_labels(const Entity &player, bool armory_mode, bool can_fire, std::vector<AttachLabel> &out, AttachLabelScanStats *stats = nullptr, const VehicleOccupancySource *source = nullptr);
    // The use-key mount toggle [orig: Entity_ToggleVehicleMount @0x436950 +
    // Entity_TryEnterNearestVehicle @0x4368c0]:
    //  - unmounted, standing ON a seat-bearing carrier (our generic ground_target
    //    stands in for the Flags 0x200 deck latch; this is unrelated to CL) -> best free seat
    //    on the carrier
    //    [orig: Entity_FindBestSeatSlot @0x4351f0];
    //  - unmounted otherwise -> the nearest-seat scan;
    //  - mounted -> a seat in scan reach swaps [orig: @0x4369ac], else detach.
    // The weapon-busy gate (EquippedSlot currentAction @0x436958) and the WAC no-dismount
    // global (dword_C6EADC @0x43698b) are the caller's/session's concern (D-AI-11).
    // Returns true iff a mount/swap/detach was applied.
    bool player_toggle_mount(EntityHandle player);
    // Host-facing lifecycle for effects that exist only while a vehicle has its single
    // tracked primary occupant (the +368 claimant). Payload fields are the target vehicle's
    // net_id, bms_id, spawn_origin, and packed runtime wire handle.
    // [orig: occupied spawn in Entity_UpdateHeloRotorSpin (ex entity_update_damage_accumulator_and_shadow) Entity_UpdateHeloRotorSpin @0x48fa70 gate
    // @0x48faad (attrib&0x40 && occupantEntity(+368)); release in Entity_DetachFromVehicle
    // @0x4355f0 stop leg @0x4356e9..0x435759 — runs ONLY when the detacher IS the claimant.]
    void emit_control_started(const Entity &vehicle);
    void emit_control_stopped(uint16_t target_net_id, int32_t target_bms_id, uint32_t target_spawn_origin, uint16_t target_wire_handle);
    void emit_control_stopped(const Entity &vehicle);
    // The +368 primary-occupant claim: Controller/Driver seats claim when the slot is empty
    // or already theirs; a Gunner claims only when empty (the emplaced-gun UseGun leg);
    // Passengers never claim. Emits vehicle_control_started on the empty -> claimed edge.
    // Returns true when the occupant holds the claim after the call.
    // [orig: Entity_AttachToVehicleSlot @0x4946d0 — +368 writes @0x4947d2 (ctrlx,
    // empty-or-same), @0x4948d8 (drvrx, empty-or-same), @0x49495e (UseGun, empty only)]
    bool claim_primary_occupant(Entity &vehicle, EntityHandle occupant, SeatType seat);
    // Clears the claim and emits vehicle_control_stopped iff `occupant` IS the claimant —
    // a second control-seat occupant staying aboard does NOT keep the engine running.
    // [orig: Entity_DetachFromVehicle @0x4355f0 — `occupantEntity == entity` gate @0x4356e9,
    // emitter release + engine-stop sound @0x435716..0x435759, +368 clear @0x43577c]
    bool release_primary_occupant(Entity &vehicle, EntityHandle occupant);
    // Swap a UseGun occupant to the parent's embedded weapon slot, preserving the
    // personal equipped AdmDef for detach. Returns true once the parent weapon resolves.
    // [orig: Entity_AttachToUseGunSlot @0x546b80..0x546c73]
    bool bind_use_gun_slot(Entity &occupant, Entity &vehicle);
    // Resolve an entity's authored primary_weapon into its embedded MountSlot and
    // seed clip/reserve when the weapon table becomes available. Entity promotion
    // can precede armory loading, so callers use this at the first live slot edge.
    bool prepare_weapon_slot(Entity &vehicle);
    const WeaponSlotState *resolve_mounted_ammo_slot(const Entity &mount) const;
    // Resolve the EWeap MountSlot selected by retail's live route bit. A type-1
    // vehicle uses its own embedded slot. An attached non-vehicle EWeap uses its
    // own slot until redirect_to_parent_slot is set, then follows the exact
    // groundEntity relationship to a live type-1 EWeap carrier. Invalid/stale
    // routes return null rather than falling back to attachment metadata.
    // [orig: shared helper @0x5460e0; NetPacket_WritePlayerState @0x4ffe18]
    WeaponSlotState *resolve_mounted_ammo_slot(Entity &mount);
    // Entity_RequestVehicleAttach snaps the requester yaw to the chosen seat before
    // authority applies the relationship. Keep the registry Entity and our split
    // AiEntity/local-player look target coherent so the first mounted tick cannot
    // restore the pre-attach look. Pitch is deliberately untouched.
    // [orig: Entity_RequestVehicleAttach @0x4364a0; UseGun yaw @0x43656c]
    void presnap_attach_heading(Entity &occupant, const Entity &vehicle, const Seat &seat);
    // Snap a mounted occupant onto its seat. A model-aware host resolves retail's live
    // seat-bone frame; otherwise the portable fallback is vehicle.position +
    // rotate(seat.seat_local, -vehicle.yaw), with Gunner yaw at vehicle.yaw-yaw_offset
    // and other seats at vehicle.yaw+yaw_offset. Shared by every attach path and the AI
    // tick's per-frame seat follow. [orig: the UseGun attach Entity_AttachToBoneAndUpdateTransform @0x5463d0; ordinary seats Entity_GetBoneTransformAndOrientation @0x4b0c50]
    void pose_mounted_occupant(Entity &occ, const Entity &vehicle, const Seat &seat);
    // The controlling occupant of a PlayerControl vehicle: the first live, internally
    // consistent Controller/Driver seat occupant, with the per-tick stale-slot sweep and the
    // +368 claimant validation. [orig: the occupant sweep @0x48b8a1-0x48b944 in
    // Entity_UpdateVehiclePhysics @0x48af00]
    Entity *resolve_controller(Entity &veh);
    // One authority tick of the ground-vehicle motor for `veh` (a pool-1 entity whose
    // traits carry a non-zero `physics` selector). Consumes the controlling occupant's
    // replicated input (or the AI-driver command when the controller is an NPC), advances
    // Entity::position / Entity::yaw and the persistent Entity::veh motor state.
    // [orig: Entity_UpdateVehiclePhysics @0x48af00 — the authority drive core;
    // block-level cites inline]
    void tick_motor(Entity &veh, const VehicleTraits &traits, const VehicleDriveCmd *ai_cmd = nullptr);
	// Watercraft prediction runs the received-register chase, local-driver reconciliation and the
	// full motor/platform solve. The shared state includes speed, steer, heave, pitch, roll,
	// planing, water/air flags and wreck latches; authority-only damage remains gated.
	// Witness sites: [orig: @0x48D480, @0x481870, @0x48ECE7, @0x45AEA0]
	void watercraft_platform_solve(Entity &veh, const VehicleTraits &traits);
	void watercraft_client_tick(Entity &veh, const VehicleTraits &traits);
    // One AUTHORITY tick of the watercraft motor (the host-side cbot mover): the
    // occupant/AI/parked input staging behind the witnessed gate, capsize damage,
    // then the same steer/thrust/drag/contact/integration core the client subset
    // runs. The AI command block comes from AiSystem::watercraft_ai_drive.
    // [orig: Entity_UpdateWatercraftPhysics @0x48D480 — the authority path behind
    // the @0x48DF8C..0x48DFA2 input gate; witnessed 2026-08-06]
    void tick_watercraft_motor(Entity &veh, const VehicleTraits &traits, const VehicleDriveCmd *ai_cmd = nullptr);
    // The GROUND/Bike prediction leg: shared chase + local-driver input or mirrored
    // remote registers driving tick_vehicle_motor's core with the input block bypassed;
    // the Bike family selects its witnessed gravity/contact/yaw deltas in that core.
    void ground_client_tick(Entity &veh, const VehicleTraits &traits);
	// Aircraft prediction shares the CHel/cpln motor, input/remote register staging, attitude and
	// altitude servo, aero response and full aircraft contact solve.
	// Witness sites: [orig: @0x490310]
	void aircraft_client_tick(Entity &veh, const VehicleTraits &traits);
	// 1. The tracked / tank crash tests [orig: tracked @0x47d745..0x47d7a8 +
    //  the client window @0x47e793..0x47e7ee; tank @0x477760..0x4777bf +
    //  @0x478b6c..0x478bd6]: (a) |up.z| under the flip bound (the ABSOLUTE
    //  value: `cdq; xor; sub` @0x47d722..0x47d726 / @0x477748..0x477753 — an
    //  inverted hull does not tip-test), or — tracked only — the replicated bit
    //  set, while airborne; (b) the authority: |slide_z| > 0x7000 — a client:
    //  airborne with the bit; (c) the CLIENT window: with fresh_2f1 == 0,
    //  !crashed (and, tank only, !settle_2f0): stamp the airborne tick once,
    //  request while the stamp is under 10 ticks old, else clear the stamp and
    //  raise fresh_2f1.
    void suspension_crash_tests(Entity &veh, const VehicleTraits &traits, int32_t up_z16, SuspensionFamily family);
    // 3a. The GROUNDED spring loop [orig: @0x47E960..0x47EC1F], per wheel with
    //  spring != 0: the free-fall catch-up (corner −= max(sink − growth, 0)), the
    //  landing IMPULSE (sink > thr && contact → energy += 0.5·mass·sink², the
    //  impact sink += 1.25·energy), the settle term (energy <= 0: e = depth −
    //  minDepth − amp_0 (wheel 0's amplitude — the witnessed unindexed read) →
    //  energy += spring·min(e,4095)²), then energy > 0 → compress by
    //  min(ftol(sqrt(energy/spring)), 4095) else amp != 0 && all sinks < 2000 →
    //  free decay; the resolved `depth −= Δ` lifts the corner. `depth` is
    //  in/out; `corner_adj` receives the catch-up term.
    void suspension_grounded_loop(Entity &veh, const VehicleTraits &traits, int wheels, int32_t depth[4], const bool contact[4], int32_t growth, int32_t corner_adj[4]);
    // 3b. The AIRBORNE spring loop [orig: @0x47E283..0x47E344], gated on
    //  settle_2f0 == 0: energy > 0 → compress by the full 4095 step, else amp != 0
    //  → free decay; then the catch-up under !crashed && !crash_request.
	void suspension_tank_loop(Entity &veh, const VehicleTraits &traits, bool on_ground,
			int32_t depth[4], const bool contact[4], int32_t corner_adj[4]);
	void suspension_airborne_loop(Entity &veh, const VehicleTraits &traits, int wheels,
			int32_t growth, int32_t corner_adj[4]);
	// 4. Arming — the seed all three families share [orig:
    //  Entity_ProcessWheeledVehicleSuspension @0x46b1a6..0x46b213; the tank twin
    //  @0x469933..0x46999e; the bike twin @0x468b00..0x468b3b which also EJECTS
    //  every occupant]. Returns true when the latch set this tick.
	bool suspension_arm(Entity &veh, bool eject_occupants, const int32_t corners[4][3] = nullptr,
			const bool *contacts = nullptr);
	// Refresh the active idle/forward/reverse emitter lanes from the vehicle's final
	// motor state for this physics tick. PlayerControl vehicles key engine-running
	// state on primary_occupant, identically for NPC and player claimants. A hull
	// collision clears both motion lanes and forces a full idle refresh; wreck/all-zero
	// clears motion without refreshing idle.
	void update_ground_sound(Entity &vehicle, const VehicleTraits &traits, bool wrecked, bool collided);
    // Claimant-only detach/stale-claim leg: clear the motion lanes and fire the
    // authored engine-stop one-shot when strictly above the mission water plane. The
    // idle lane is not refreshed and expires from its 30-tick keep-alive, matching
    // the original zero-argument movement-sound call.
	void stop_ground_sound(Entity &vehicle, int32_t water_clearance_q16 = 0);
	void update_traction_sound(Entity &vehicle, const VehicleTraits &traits);
	void update_engine_sound(Entity &vehicle, const VehicleTraits &traits);
	void play_contact_sound(Entity &vehicle, const VehicleTraits &traits, int slot);
	void update_rotor_sound(Entity &vehicle, const VehicleTraits &traits);
	void play_rotor_start_sound(Entity &vehicle, const VehicleTraits &traits);
	// ---------------------------------------------------------------------------
	// Ground calls at the tail; aircraft calls at the head before its lift gate.
	// The called rotor machine must match the brain's profile type (a brainless row — a lib
	// embedder's loose vehicle, a unit rig — has no profile, and its family stands in: the
	// Helicopter/Plane movers are where retail calls the HELO twin from), runs it
	// (seeding the rate from the shared PRNG when a non-player-control item needs
	// a roll — the seed path is the ONLY PRNG consumer here, and it draws exactly
	// once per unoccupied tick for such an item). Ground advances wheel phase
	// from speed; aircraft has no wheel-phase write. A WATERCRAFT runs no rotor machine at
	// all — its mover Entity_UpdateWatercraftPhysics @0x48D480 calls neither
	// @0x4928B0 nor @0x48FA70 — only the wheel phase.
	// `occupied` is the engine-running latch, Entity::primary_occupant (the +0x170
	// occupantEntity read @0x4928E8); `player_control` is the item's attrib 0x40.
	void part_anim_tick(Entity &veh, const VehicleTraits &traits);
	// Shared mover-head health cadence [orig: cveh @0x48AFFD, cbik @0x4840DD,
	// ctan @0x488BAD, cbot @0x48D561, CHel/cpln @0x4903F4].
	void tick_health(Entity &veh, const VehicleTraits &traits);
	void tick_simple_motor(
			Entity &, const VehicleTraits &, const VehicleDriveCmd *, bool prediction);
	void slew_turret(Entity &veh, int32_t step);
	// ActionDef+52 rocks the tank carrying an occupied emplacement.
	// [orig: WeaponAction_Fire @0x542B10; ActionSlot_ExecuteAction @0x4020A0]
	void weapon_recoil(const Entity &shooter, int32_t amplitude, int32_t yaw, int32_t pitch);
	void projectile_impact(
			Entity &target, int32_t weight, const int32_t normal[3], const int32_t hit[3]);

private:
    World &world_;
	struct SpawnMarker {
		EntityHandle marker, zone;
	};
	std::vector<SpawnMarker> spawn_markers_;
	std::vector<EntityHandle> spawn_groups_[5];
	bool spawn_markers_enabled_ = false;
	std::vector<EntityHandle>
			pass_handles_; // per-tick scratch for the motor pass (reused, no realloc)
};

} // namespace opennova::world
