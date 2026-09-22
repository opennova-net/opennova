#include <runtime/world/vehicle_system.h>
#include <runtime/world/vehicle_attach.h>

#include <bitset>
#include <cmath>
#include <cstdint>

#include <base/io/bam.h>
#include <runtime/world/ai.h>
#include <runtime/world/angle.h>
#include <runtime/world/collision.h>
#include <runtime/world/weapon_fsm.h>
#include <runtime/world/vehicle_panel_feed.h>
#include <runtime/world/world.h>

namespace opennova::world {

namespace {

constexpr std::size_t kEntityHandleDomain =
        static_cast<std::size_t>(EntityRegistry::kPoolCount) << 12;

bool is_blocking_enemy_rider(const Entity &entity, const Entity &requester) {
    if (entity.handle == requester.handle) return false;
    if (entity.health <= 0 || !entity.alive) return false;
    if (entity.team == requester.team) return false;
    return entity.mounted;
}

// Emplaced guns share hostile occupancy with their carrier. Vehicle-type
// EWeap targets remain roots. [orig: Vehicle_HasEnemyOccupant @0x4359F0]
EntityHandle occupancy_root(const World &world, const Entity &target) {
	if (target.has_item_def && (target.item_attrib & kItemAttribEweap) != 0 &&
			target.item_type != 1 && world.registry.get(target.ground_target) != nullptr)
		return target.ground_target;
	return target.handle;
}

bool rider_blocks_root(const World &world, const Entity &rider, EntityHandle root) {
	if (rider.mount_target == root)
		return true;
	const Entity *mount = world.registry.get(rider.mount_target);
	return mount != nullptr && mount->has_item_def &&
			(mount->item_attrib & kItemAttribEweap) != 0 && mount->ground_target == root;
}

bool vehicle_has_enemy_occupant(const World &world, const Entity &vehicle,
                                const Entity &requester) {
	const EntityHandle root = occupancy_root(world, vehicle);
	for (std::size_t slot = 0; slot < world.registry.pool_capacity(0); ++slot) {
		const Entity *entity = world.registry.get(EntityHandle::make(0, static_cast<int>(slot)));
		if (entity != nullptr && is_blocking_enemy_rider(*entity, requester) &&
				rider_blocks_root(world, *entity, root))
			return true;
	}
	return false;
}

// One requester-relative index per query, including carried-gun parents;
// attachment, death and team changes are observed without a retained cache.
class HostileMountIndex {
public:
	HostileMountIndex(const World &world, const Entity &requester, AttachLabelScanStats *stats,
			const VehicleOccupancySource *source) :
			world_(world) {
		if (stats != nullptr) ++stats->enemy_occupancy_registry_passes;
		for (std::size_t slot = 0; slot < world.registry.pool_capacity(0); ++slot) {
			const Entity *entity =
					world.registry.get(EntityHandle::make(0, static_cast<int>(slot)));
			if (entity != nullptr && is_blocking_enemy_rider(*entity, requester))
				mark(entity->mount_target);
		}
		if (source != nullptr) {
            std::vector<EntityHandle> remote;
            source->collect_hostile_mounts(requester, remote);
			for (EntityHandle target : remote)
				mark(target);
		}
	}

	bool blocks(EntityHandle target) const {
		const Entity *entity = world_.registry.get(target);
		if (entity != nullptr)
			target = occupancy_root(world_, *entity);
		return target.valid() && target.packed < blocked_.size() && blocked_.test(target.packed);
	}

private:
	void mark(EntityHandle target) {
		if (!target.valid())
			return;
		if (target.packed < blocked_.size())
			blocked_.set(target.packed);
		const Entity *mount = world_.registry.get(target);
		if (mount != nullptr && mount->has_item_def &&
				(mount->item_attrib & kItemAttribEweap) != 0 && mount->ground_target.valid() &&
				mount->ground_target.packed < blocked_.size())
			blocked_.set(mount->ground_target.packed);
	}
	const World &world_;
	std::bitset<kEntityHandleDomain> blocked_;
};

bool candidate_relevant_for_mode(const Entity &candidate, bool armory_mode) {
    return armory_mode ? !candidate.armory_points.empty() : !candidate.seats.empty();
}

// Shared host attach write block. Retail splits UseGun from ordinary vehicle slots at
// the flags write; the remaining relationship fields are common.
void attach_apply(World &world, Entity &occ, Entity &veh, int seat_idx, uint8_t bone) {
    world.vehicles.presnap_attach_heading(occ, veh, veh.seats[seat_idx]);
    veh.seats[seat_idx].occupant = occ.handle; // [orig: mountHandles[idx] = handle @0x494746]
    occ.mount_type = veh.seats[seat_idx].type; // [orig: parentSlot(0x168) = slotType]
    static_assert((kEntityFlagDrowning | kEntityFlagInAir) == 0xA000u,
                  "the witnessed gunner-mount scrub mask");
    static_assert((kEntityFlagDrowning | kEntityFlagInAir | kEntityFlagMounted) == 0xA040u,
                  "the witnessed vehicle-mount scrub+set mask (~mask == 0xFFFF5FBF)");
    if (occ.mount_type == SeatType::Gunner) {
        occ.flags &= ~(kEntityFlagDrowning | kEntityFlagInAir);
        occ.engine_flags &= ~(kEntityFlagDrowning | kEntityFlagInAir);
        // [orig: Entity_AttachToUseGunSlot @0x546c56-0x546c7c]
    } else {
        occ.flags = (occ.flags & ~(kEntityFlagDrowning | kEntityFlagInAir | kEntityFlagMounted)) |
                    kEntityFlagMounted;
        occ.engine_flags =
                (occ.engine_flags & ~(kEntityFlagDrowning | kEntityFlagInAir | kEntityFlagMounted)) |
                kEntityFlagMounted;
        // [orig: Entity_AttachToVehicleSlot @0x494752-0x494775]
    }
    // The infantry motor's copy of the 0x2000 bit follows the same scrub.
    if (AiEntity *body = world.ai.for_handle(occ.handle)) body->inf.airborne = false;
    occ.mount_target = veh.handle;                 // [orig: parentEntity(0x16C) = vehicle]
    occ.mount_target_net_id = veh.net_id;
    occ.mount_target_bms_id = veh.bms_id;
    occ.mount_target_spawn_origin = veh.spawn_origin;
    occ.mount_bone = bone;                          // [orig: attachBoneId(0x157) = bone]
    occ.mount_seat = static_cast<int8_t>(seat_idx); // port-local dense seat index
    occ.mounted = true;
    occ.mounted_config_valid = veh.emplaced_config_valid;
    occ.mounted_config = veh.emplaced_config_valid ? veh.emplaced_config : 0;
    if (occ.mount_type == SeatType::Gunner)
        world.vehicles.bind_use_gun_slot(occ, veh);
    world.vehicles.pose_mounted_occupant(occ, veh, veh.seats[seat_idx]);
    // Success clears the movement stance bits [orig: MoveOrder &= ~0x300 @0x435c42 + the
    // prone/crouch latch clears @0x435c54/@0x435c59].
    occ.net_stance_bits = 0;
    world.vehicles.claim_primary_occupant(veh, occ.handle, occ.mount_type); // [orig: +368 @0x4946d0]
}

// Local-point world position: the same local rotate the per-tick pose applies
// (pose_mounted_occupant) through the carrier's FULL orientation frame, our
// stand-in for the posed bone transform
// [orig: build_bone_attachment_matrix @0x56c630 in the scan @0x435fe7].
Vec3 local_point_world_pos(const Entity &veh, const Vec3 &local) {
    return entity_local_point_world(veh, local);
}

Vec3 seat_world_pos(World &world, const Entity &veh, const Seat &s) {
    // USE scores every seat kind (sitex, ctrlx, drvrx and UseGun alike) at
    // its live bone through the carrier's attachment build: the carrier's
    // render-class CTRL callback, then its PANM/bones. That is the provider's
    // attachment-frame form, whatever the seat's own kind. A rest-only point
    // targets the wrong hatch when a turret is animated.
    // [orig: Entity_FindNearestSeatOrArmory seat kinds @0x435F6C..0x435FDF ->
    //  build_bone_attachment_matrix @0x435FFA (its def+0x144 call
    //  @0x56C6DC..0x56C6F3); labels @0x5A3553]
    Seat query = s;
    query.type = SeatType::Gunner;
    query.attachment_frame = true;
    MountedPose pose;
    if (world.pose_provider != nullptr &&
            world.pose_provider->resolve_mounted_pose(world, veh, query, pose))
        return pose.position;
    return local_point_world_pos(veh, s.seat_local);
}

bool seat_allowed_for_selection(SeatType type, SeatSelectionMode mode) {
    switch (mode) {
        case SeatSelectionMode::PassengerOnly:
            return type == SeatType::Passenger;
        case SeatSelectionMode::RejectController:
            return type != SeatType::Controller;
        case SeatSelectionMode::Any:
        default:
            return true;
    }
}

} // namespace

bool seat_selection_mode_for_command(int command_id, SeatSelectionMode &out) {
    switch (command_id) {
        case kCommandAttachPassengerOnly: out = SeatSelectionMode::PassengerOnly; return true;
        case kCommandAttachSkipController: out = SeatSelectionMode::RejectController; return true;
        case kCommandAttachAnySeat: out = SeatSelectionMode::Any; return true;
        default: return false;
    }
}

int32_t seat_priority_weight(SeatType type, bool root_seat) {
    // [orig: Entity_FindBestSeatSlot @0x4351F0 — the per-type weights]
    switch (type) {
        case SeatType::Controller:
        case SeatType::Driver:
            return 0x2000;
        case SeatType::Passenger:
            return root_seat ? 0x200000 : 0x2000000;
        case SeatType::Gunner:
        default:
            return 0x20000;
    }
}

int predict_seat_selection(const std::vector<SeatCandidate> &seats,
                           const SeatSelectionMode *mode,
                           std::vector<SeatVerdict> &verdicts) {
    verdicts.assign(seats.size(), SeatVerdict::kSkippedCommand);
    int best = -1;
    int32_t best_weight = 0x7fffffff;
    for (size_t i = 0; i < seats.size(); ++i) {
        const SeatCandidate &s = seats[i];
        if (s.occupied) {
            verdicts[i] = SeatVerdict::kSkippedOccupied;
            continue;
        }
        const bool allowed = mode != nullptr && s.type != SeatType::None &&
                             seat_allowed_for_selection(s.type, *mode);
        if (!allowed) continue; // kSkippedCommand
        verdicts[i] = SeatVerdict::kEligible;
        const int32_t weight = seat_priority_weight(s.type, true);
        if (weight < best_weight) {
            best_weight = weight;
            best = static_cast<int>(i);
        }
    }
    if (best >= 0) verdicts[static_cast<size_t>(best)] = SeatVerdict::kSelected;
    return best;
}

VehicleSeatOccupancy vehicle_seat_occupancy(
        const World &world, const Entity &carrier, const Seat &seat,
        EntityHandle requester, const VehicleOccupancySource *source) {
    if (source != nullptr) return source->seat_occupancy(carrier, seat, requester);
    VehicleSeatOccupancy result;
    result.occupied = seat.occupant.valid();
    if (const Entity *rider = world.registry.get(seat.occupant)) {
        result.rider_resolved = true;
        result.player = (rider->flags & kEntityFlagPlayer) != 0;
        result.own_seat = seat.occupant == requester;
        result.health = rider->health;
        result.max_health = rider->health_max;
    }
    return result;
}

// Every retail client attaches remote players and AI to their seats from the
// decoded records, so +368 names a remote driver there too; a joiner answers the
// same readers through its occupancy source.
// [orig: NetPacket_SerializePlayerState @0x4C1317 /
//  NetPacket_SerializeInfantryEntityState @0x4C0678 -> Entity_TryAttachOrDetach ->
//  Entity_ProcessVehicleAttach @0x435AA0 -> Entity_AttachToVehicleSlot +368
//  stores @0x4947D2 / @0x4948D8]
const Entity *VehicleSystem::claimant(const Entity &vehicle, Entity &scratch) const {
    if (vehicle.primary_occupant.valid()) return world_.registry.get(vehicle.primary_occupant);
    if (occupancy_source != nullptr && occupancy_source->remote_claimant(vehicle, scratch))
        return &scratch;
    return nullptr;
}

bool VehicleSystem::claimant_present(const Entity &vehicle) const {
    if (vehicle.primary_occupant.valid()) return true;
    if (occupancy_source == nullptr) return false;
    Entity scratch;
    return occupancy_source->remote_claimant(vehicle, scratch);
}

// [orig: Entity_FindAvailableSeat @0x436790..0x4368BB]
bool find_numbered_vehicle_seat(const World &world, const Entity &player, int index,
        VehicleSeatSelection &out, const VehicleOccupancySource *source) {
    out = {};
    if (index < 0 || index >= kVehiclePanelSlotMax) return false;
    const EntityHandle root = vehicle_panel_root(world, player);
    VehiclePanelSlotList slots;
    if (index >= build_vehicle_panel_slots(world, root, slots)) return false;
    const VehiclePanelSlot &slot = slots[index];
    const Entity *carrier = world.registry.get(slot.entity);
    if (carrier == nullptr) return false;
    for (int i = 0; i < static_cast<int>(carrier->seats.size()); ++i) {
        const Seat &seat = carrier->seats[static_cast<size_t>(i)];
        if (seat.retail_slot != slot.type || seat.bone_index == 0 ||
                (seat.type != SeatType::Passenger && seat.type != SeatType::Controller &&
                 seat.type != SeatType::Driver && seat.type != SeatType::Gunner)) continue;
        const auto occupied = vehicle_seat_occupancy(world, *carrier, seat, player.handle, source);
        // The requester-side AI exception is not a force-attach. Retail's
        // authority still rejects occupied slots @0x435BA9..0x435BB1.
        if (occupied.occupied && ((player.flags & kEntityFlagPlayer) == 0 ||
                !occupied.rider_resolved || occupied.player)) return false;
        out = {carrier->handle, i, seat.type};
        return true;
    }
    return false;
}

bool find_best_vehicle_seat(
        const World &world, EntityHandle root_vehicle, EntityHandle occupant,
        VehicleSeatSelection &out, SeatSelectionMode mode, const VehicleOccupancySource *source) {
    out = {};
    const Entity *root = world.registry.get(root_vehicle);
    if (root == nullptr) return false;

    int32_t best_weight = 65536000; // [orig: bestWeight sentinel @0x43520A]
    const auto consider = [&](const Entity &candidate, bool is_root) {
        // An unmodeled model pointer manifests as an empty seat vector in the
        // portable world. Each root/child entry has its own dead gate.
        if (!candidate.alive || candidate.health <= 0 ||
            (candidate.flags & kEntityFlagDead) != 0)
            return;
        for (int i = 0; i < static_cast<int>(candidate.seats.size()); ++i) {
            const Seat &seat = candidate.seats[static_cast<size_t>(i)];
            if (seat.type == SeatType::None ||
                !seat_allowed_for_selection(seat.type, mode))
                continue;
            if (!is_root && is_vehicle_control_seat(seat.type)) continue;
            const auto occupancy = vehicle_seat_occupancy(world, candidate, seat, occupant, source);
            if (occupancy.occupied && !occupancy.own_seat) continue;

            const int32_t weight = seat_priority_weight(seat.type, is_root);
            if (weight >= best_weight) continue;
            best_weight = weight;
            out.vehicle = candidate.handle;
            out.seat_index = i;
            out.type = seat.type;
        }
    };

    consider(*root, true);
    world.registry.for_each([&](const Entity &candidate) {
        if (candidate.handle != root_vehicle &&
            candidate.ground_target == root_vehicle)
            consider(candidate, false);
    });
    return out.vehicle.valid();
}

bool vehicle_can_enter(const World &world, const Entity *rider, const Entity &carrier) {
	if (!carrier.has_item_def || (carrier.flags & kEntityFlagDead) != 0 || !carrier.alive ||
			carrier.health <= 0)
		return false;
	VehicleSeatSelection seat;
	if (!find_best_vehicle_seat(
				world, carrier.handle, rider != nullptr ? rider->handle : EntityHandle{}, seat))
		return false;
	if (rider != nullptr && rider->ground_target == carrier.handle)
		return true;
	if (vehicle_at_spawn_anchor(world, carrier))
		return true;
	if ((carrier.flags & kEntityFlagInAir) != 0)
		return false;
	// The comparison is 16 raw 16.16 counts, not sixteen world units.
	// [orig: Entity_CanEnterVehicle @0x4354C7..0x435509]
	const int64_t x = static_cast<int32_t>(carrier.position.x * 65536.0f);
	const int64_t y = static_cast<int32_t>(carrier.position.y * 65536.0f);
	return std::abs(x - carrier.saved_live_pos[0]) <= 16 &&
			std::abs(y - carrier.saved_live_pos[1]) <= 16;
}

bool VehicleSystem::attach_to_seat(EntityHandle player, const VehicleSeatSelection &selection) {
    World &world = world_;
    Entity *occ = world.registry.get(player);
    Entity *veh = world.registry.get(selection.vehicle);
    if (occ == nullptr || veh == nullptr) return false;
    if (occ->health <= 0 || !occ->alive ||
        (occ->flags & kEntityFlagDead) != 0)
        return false;
    if (veh->health <= 0 || !veh->alive ||
        (veh->flags & kEntityFlagDead) != 0)
        return false;
    if (selection.seat_index < 0 ||
        selection.seat_index >= static_cast<int>(veh->seats.size()))
        return false;
    if (vehicle_has_enemy_occupant(world, *veh, *occ)) return false;
    const Seat &seat = veh->seats[static_cast<size_t>(selection.seat_index)];
    if (seat.type == SeatType::None || seat.type != selection.type) return false;
    if (seat.occupant.valid() && seat.occupant != player) return false;
    if (occ->mounted)
        world.vehicles.detach(player); // [orig: @0x435BCE]
    attach_apply(world, *occ, *veh, selection.seat_index, seat.bone_index);
    world.out.scars.clear_entity(player); // [orig: Scar_ClearEntriesByEntity @0x5CCEC0]
    return true;
}

bool VehicleSystem::process_attach(EntityHandle player, EntityHandle vehicle, uint8_t bone) {
    World &world = world_;
    Entity *occ = world.registry.get(player);
    Entity *veh = world.registry.get(vehicle);
    // 1. Resolve + dead gates [orig: @0x435b01 — null vehicle/itemDef/player or either
    //    Flags & 2 reject]. Our authoritative dead store is health/alive; the flags bit-1
    //    movement/spawn gate also rejects (a mid-spawn player cannot mount).
    if (occ == nullptr || veh == nullptr) return false;
    if (occ->health <= 0 || !occ->alive || (occ->flags & 2u) != 0) return false;
    if (veh->health <= 0 || !veh->alive || (veh->flags & 2u) != 0) return false;

    // 2. Seat classification by the exact wire bone. The byte is a 1-based index into
    //    the model USRP table (48-byte rows, name at +32), which is exactly how production
    //    seat specs assign bone_index. Unknown/unrecognized rows reject; retail never
    //    substitutes another free seat.
    //    [orig: Entity_GetBoneSlotType @0x434ED0 -> slotType 0 reject @0x435B14;
    //    seat-block index @0x435BA9]
    int seat_idx = -1;
    for (int i = 0; i < static_cast<int>(veh->seats.size()); ++i) {
        if (veh->seats[i].type == SeatType::None) continue;
        if (veh->seats[i].bone_index == bone) {
            seat_idx = i;
            break;
        }
    }
    if (seat_idx < 0) return false;

    VehicleSeatSelection selection;
    selection.vehicle = vehicle;
    selection.seat_index = seat_idx;
    selection.type = veh->seats[static_cast<size_t>(seat_idx)].type;
    return world.vehicles.attach_to_seat(player, selection);
}

// The client receive arm, distinct from the authority's request validation.
// [orig: Entity_TryAttachOrDetach @0x436610 -> Entity_ProcessVehicleAttach
// @0x435AA0; authority-only enemy gate @0x435B3C, occupied-seat replacement
// @0x435BA9..0x435BBA, unchanged driver-claim check @0x4366DD]
bool VehicleSystem::apply_confirmed_mount(
        EntityHandle player, EntityHandle vehicle, uint8_t bone) {
    World &world = world_;
    Entity *occ = world.registry.get(player);
    if (occ == nullptr) return false;
    if (!vehicle.valid() || bone == 0) return detach(player);
    Entity *veh = world.registry.get(vehicle);
    if (veh == nullptr || !occ->alive || occ->health <= 0 ||
        (occ->flags & kEntityFlagDead) != 0 || !veh->alive || veh->health <= 0 ||
        (veh->flags & kEntityFlagDead) != 0) return false;

    int seat_idx = -1;
    for (int i = 0; i < static_cast<int>(veh->seats.size()); ++i) {
        if (veh->seats[i].type != SeatType::None && veh->seats[i].bone_index == bone) {
            seat_idx = i;
            break;
        }
    }
    if (seat_idx < 0) return false;
    Seat &seat = veh->seats[seat_idx];
    const bool claims_control = is_vehicle_control_seat(seat.type) ||
                                seat.type == SeatType::Gunner;
    if (occ->mounted && occ->mount_target == vehicle && occ->mount_bone == bone &&
        seat.occupant == player && (!claims_control || veh->primary_occupant == player))
        return false;

    // The host has already selected this occupant. Retail's client first
    // detaches the previous slot holder, whereas its authority rejects it.
    // A streamed handle can name a not-yet-materialized pool-0 row; release
    // its retained slot/claim too, just as detaching retail's fixed empty row.
    const EntityHandle previous = seat.occupant;
    if (previous.valid()) {
        detach(previous);
        release_primary_occupant(*veh, previous);
        seat.occupant = {};
    }
    if (occ->mounted) detach(player);
    attach_apply(world, *occ, *veh, seat_idx, bone);
    world.out.scars.clear_entity(player);
    return true;
}

bool VehicleSystem::detach(EntityHandle player) {
    World &world = world_;
    Entity *occ = world.registry.get(player);
    if (occ == nullptr || !occ->mounted) return false;
    const bool claim_capable_seat = occ->mount_type != SeatType::Passenger &&
                                    occ->mount_type != SeatType::None;
    const uint16_t target_net_id = occ->mount_target_net_id;
    const int32_t target_bms_id = occ->mount_target_bms_id;
    const uint32_t target_spawn_origin = occ->mount_target_spawn_origin;
    const uint16_t target_wire_handle = occ->mount_target.packed;
    Entity *veh = world.registry.get(occ->mount_target);
    // [orig: Entity_DetachFromVehicle @0x4355F0] MoveOrder &= ~0x300 (stance clear), then
    // every matching seat handle on the mount target releases (all 10 slots in the
    // original; our seat vector sweeps by occupant), Flags &= ~0x40 and the mount trio
    // clears. The EquippedSlot backup is restored for a player and cleared for an
    // NPC below. The claimant-only ground sound clear/stop and the vehicle
    // slot's counter cut ride vehicle_release_primary_occupant; the
    // attached-effect release remains unmodeled (D-NET-157).
    occ->net_stance_bits = 0;
    if (veh != nullptr) {
        for (Seat &s : veh->seats) {
            if (s.occupant == player) s.occupant = EntityHandle{}; // [orig: -> 0xFFFF]
        }
    }
    vehicle_release_use_gun_slot(*occ, veh);
    occ->flags &= ~kEntityFlagMounted;          // [orig: Flags &= ~0x40]
    occ->engine_flags &= ~kEntityFlagMounted;
    occ->mount_target = EntityHandle{}; // [orig: +0x16C = 0]
    occ->mount_target_net_id = 0;
    occ->mount_target_bms_id = 0;
    occ->mount_target_spawn_origin = 0;
    occ->mount_bone = 0;           // [orig: +0x157 = 0]
    occ->mount_seat = -1;          // [orig: +0x168 = 0]
    occ->mount_type = SeatType::None;
    occ->mounted = false;
    occ->mounted_config_valid = false;
    occ->mounted_config = 0;
    if (veh != nullptr) {
        // [orig: the +368 leg @0x4356e9..0x43577c — runs only for the claimant]
        world.vehicles.release_primary_occupant(*veh, player);
    } else if (claim_capable_seat) {
        // The vehicle is already gone; the stored identity carries the stop (host
        // cleanup — a spurious stop is idempotent downstream).
        world.vehicles.emit_control_stopped(target_net_id, target_bms_id,
                                     target_spawn_origin, target_wire_handle);
    }
    return true;
}

namespace {

// Both queries start at Position. Only scan scoring reads CameraOffset.
// The scan's sixth argument is allowAllTypes=1; labels use the sector query.
// [orig: Entity_FindNearestSeatOrArmory @0x436174..0x436188;
// draw_vehicle_seat_and_armory_labels @0x5A35F6..0x5A360E (the label ray endpoint
// is groundEntity, else the player; HUD_DrawEntityLabel @0x5a39b0 is the friendly
// tag drawer, not this site)]
bool point_los_clear(World &world, const Entity &player, const Entity &cand, const Vec3 &point,
		bool label = false) {
	const int32_t a[3] = { static_cast<int32_t>(player.position.x * 65536.0f),
		static_cast<int32_t>(player.position.y * 65536.0f),
		static_cast<int32_t>(player.position.z * 65536.0f) };
	const int32_t b[3] = { static_cast<int32_t>(point.x * 65536.0f),
		static_cast<int32_t>(point.y * 65536.0f),
		static_cast<int32_t>(point.z * 65536.0f) + (label ? 0 : 12288) };
	if (label) {
		const EntityHandle endpoint =
				cand.ground_target.valid() ? cand.ground_target : player.handle;
		return world.ai.line_of_sight_clear(world, a, b, cand.handle, endpoint);
	}
	EntityHandle endpoint = cand.handle;
	if (cand.has_item_def && (cand.item_attrib & kItemAttribEweap) != 0 &&
			(cand.item_attrib & kItemAttribPlayerControl) == 0) {
		const Entity *ground = world.registry.get(cand.ground_target);
		if (ground != nullptr && ground->has_item_def && ground->item_type == 1)
			endpoint = ground->handle;
	}
	if (world.collision != nullptr)
		return world.collision->entity_los_clear(world, player.handle, endpoint, a, b, 0, true);
	return world.ai.line_of_sight_clear(world, a, b, player.handle, endpoint);
}

// The shared per-entity reject set of the scan and the label pass
// [orig: @0x435e28..0x435eae / @0x5a335a..0x5a3395 — dead/destroyed skip, itemDef/model
// presence, enemy-occupant and carried-object rejects].
bool scan_entity_rejected(const World &world, const Entity &cand, const Entity &player,
		const HostileMountIndex &hostile_mounts) {
	if (cand.handle == player.handle) return true;
    if (!cand.alive || cand.health <= 0) return true; // [orig: Flags & 2 skip]
    if ((cand.flags & 2u) != 0) return true;
	if (hostile_mounts.blocks(cand.handle))
		return true;
	const Entity *ground = world.registry.get(cand.ground_target);
	if (cand.item_type == 6 && ground != nullptr && ground->has_item_def &&
			(ground->item_attrib & kItemAttribPlayerControl) != 0)
		return !ground->alive || ground->health <= 0 || (ground->flags & kEntityFlagDead) != 0 ||
				hostile_mounts.blocks(ground->handle);
	return false;
}

// The scan container: the player's proximity slice when the per-tick tables are
// live [orig: entity+444/448 @0x435d60], the registry sweep only for sliceless
// worlds (headless callers that never ran the table build). An absent slice on
// a live world scans nothing — retail's BSS-zero start behaves the same way.
template <typename Fn>
void for_each_scan_candidate(World &world, const Entity &player, Fn &&fn) {
    CollisionWorld *cw = world.collision;
    if (cw != nullptr && cw->attach_candidate_slices_authoritative()) {
        int32_t n = 0;
        const EntityHandle *slice = cw->candidate_slice(player.handle, n);
        for (int32_t i = 0; i < n; ++i) {
            const Entity *c = world.registry.get(slice[i]);
            if (c != nullptr) fn(*c);
        }
        return;
    }
    world.registry.for_each(fn);
}

} // namespace

static bool find_nearest_free_seat_impl(World &world, const Entity &player,
                                        VehicleSeatSelection &out, bool armory_mode,
                                        const HostileMountIndex &hostile_mounts,
                                        const VehicleOccupancySource *source) {
    // The aim cone, verbatim [orig: @0x435d90 maxDistance = 0x3FFFFFC0, the mounted
    // override @0x435d9a = 0x38E38E0]. Both are BAM32 look-offset radii, not
    // distances: 0x3FFFFFC0 is a hair under 90 deg for a standing player, 0x38E38E0
    // is 5.0 deg for a seated one, so a rider only swaps onto a seat he is looking
    // at and USE dismounts when the cone is empty [orig: Entity_ToggleVehicleMount
    // @0x4369ac..0x4369c7].
    const int32_t max_aim_bam = player.mounted ? 59652320 : 1073741760;
    // The scan's view frame is the entity Yaw/Pitch (+0x10/+0x14) [orig: the
    // subtractions @0x4360b6 / @0x4360c6]: the body's BAM32 heading and pitch for
    // every real player, the mission-degree mirror for a bare entity.
    int32_t view_heading = bam_heading_from_mission_yaw_deg(player.yaw);
    int32_t view_pitch = static_cast<int32_t>(player.pitch * kBamPerDegree);
    if (const AiEntity *body = world.ai.for_handle(player.handle)) {
        view_heading = body->heading;
        view_pitch = body->pitch;
    }
    // The eye is Position + CameraOffset in 16.16; the candidate point carries
    // the +0x3000 (0.1875 u) scan bias [orig: @0x435fff..0x436041, the
    // Position.Y subtract @0x436010].
    const int32_t eye_x = to_fixed(player.position.x) + player.eye_offset_x;
    const int32_t eye_y = to_fixed(player.position.y) + player.eye_offset_y;
    const int32_t eye_z = to_fixed(player.position.z) + player.eye_offset_z;
    // _ftol2_sse over the 0x4EFFFE00 (2^31 - 16384) float clamp the block keeps
    // on the x87 stack [orig: flt_7C19E0 @0x43604d / @0x43607d / @0x4360f4];
    // an out-of-range conversion is the x87 indefinite integer.
    const auto ftol_clamped = [](double v) -> int32_t {
        if (v > 2147467264.0) v = 2147467264.0;
        return static_cast<int32_t>(v);
    };
    const auto ftol = [](double v) -> int32_t {
        if (v >= 2147483648.0 || v < -2147483648.0) return INT32_MIN;
        return static_cast<int32_t>(v);
    };

	int32_t best_score = 0x7FFFFFFF; // [orig: v60 init]
    bool found = false;

    // One candidate point [orig: the shared score/gate block @0x435fe7..0x4361c2 (seats) =
    // @0x43624d..0x436417 (armory points)]. Returns true when it becomes the best hit.
    const auto consider = [&](const Entity &cand, const Vec3 &sp, int index, SeatType type) {
        const int32_t dx = to_fixed(sp.x) - eye_x;
        const int32_t dy = to_fixed(sp.y) - eye_y;
        const int32_t dz = to_fixed(sp.z) - eye_z + 0x3000;
        // The horizontal and 3D reaches [orig: @0x436047..0x43608a].
        const double horiz_sq = static_cast<double>(dx) * dx + static_cast<double>(dy) * dy;
        const int32_t horiz = ftol_clamped(std::sqrt(horiz_sq));
        const int32_t d3 = ftol_clamped(std::sqrt(horiz_sq + static_cast<double>(dz) * dz));
        // The point's yaw (atan2(dy, dx)) and pitch (atan2(dz, horiz)) in the
        // view frame, each clamped to +100 deg (0x471C7180) on the positive side
        // only [orig: @0x43608f..0x4360de; dbl_7C19D8 = 2^32 / 2pi].
        int32_t yaw_d = io::bam_sub(
                ftol(std::atan2(static_cast<double>(dy), static_cast<double>(dx)) *
                     io::kBamPerRadian),
                view_heading);
        int32_t pitch_d = io::bam_sub(
                ftol(std::atan2(static_cast<double>(dz), static_cast<double>(horiz)) *
                     io::kBamPerRadian),
                view_pitch);
        if (yaw_d > 0x471C7180) yaw_d = 0x471C7180;
        if (pitch_d > 0x471C7180) pitch_d = 0x471C7180;
        // The aim offset magnitude [orig: @0x4360e2..0x436103].
        const int32_t aim = ftol_clamped(std::sqrt(
                static_cast<double>(yaw_d) * yaw_d + static_cast<double>(pitch_d) * pitch_d));
        // Score = 3D reach + aim/512 [orig: @0x43610c..0x436111]; the reach gate is
        // 4.0 u @0x436113 and the cone gate @0x43611f..0x436123.
        const int32_t score = d3 + (aim >> 9);
        if (d3 > 0x40000 || aim > max_aim_bam) return;
        // The LOS gate, then the best-score compare [orig: the LOS call
        // @0x436183, `cmp ebx, [esp+0A0h+var_74]` @0x43618F].
        if (!point_los_clear(world, player, cand, sp)) return;
        if (score >= best_score) return;
        best_score = score;
        out.vehicle = cand.handle;
        out.seat_index = index;
        out.type = type;
        found = true;
    };

    // The original walks the player's proximity list [orig: entity+444/448
    // @0x435d60 — the slice Entity_BuildProximityListsFromPools fills @0x4b8eb0].
    // for_each_scan_candidate below walks that same slice; the former
    // whole-registry sweep (the container rebase) was behavior-equal inside the
    // 4.0 u gate but ran O(world) per frame. Hostile occupancy is indexed once
    // per query above so its witnessed pool-0 scan is not multiplied here.
    for_each_scan_candidate(world, player, [&](const Entity &cand) {
        if (!candidate_relevant_for_mode(cand, armory_mode)) return;
		if (scan_entity_rejected(world, cand, player, hostile_mounts))
			return;
		if (!armory_mode) {
            // [orig: the searchMode-0 seat loop @0x435f1e]
            for (int i = 0; i < static_cast<int>(cand.seats.size()); ++i) {
                const Seat &s = cand.seats[i];
                if (s.type == SeatType::None) continue; // [orig: boneIdx == 0 skip]
                if (vehicle_seat_occupancy(world, cand, s, player.handle, source).occupied)
                    continue; // [orig: mountHandles != 0xFFFF]
                consider(cand, seat_world_pos(world, cand, s), i, s.type);
            }
            return;
        }
        // [orig: the armory leg @0x4361ee — attrib 0x80000 + "armory*" points, no
        // occupancy, seatType 4]. armory_points is non-empty only for Armory-attrib items.
        for (int i = 0; i < static_cast<int>(cand.armory_points.size()); ++i)
            consider(cand, local_point_world_pos(cand, cand.armory_points[i]), i,
                     SeatType::ArmoryPoint);
    });
    return found;
}

bool VehicleSystem::find_nearest_free_seat(const Entity &player, VehicleSeatSelection &out, bool armory_mode, const VehicleOccupancySource *source) {
    World &world = world_;
    const HostileMountIndex hostile_mounts(world, player, nullptr, source);
    return find_nearest_free_seat_impl(
            world, player, out, armory_mode, hostile_mounts, source);
}

void VehicleSystem::collect_attach_labels(const Entity &player, bool armory_mode, bool can_fire, std::vector<AttachLabel> &out, AttachLabelScanStats *stats, const VehicleOccupancySource *source) {
    World &world = world_;
    const HostileMountIndex hostile_mounts(world, player, stats, source);
    // No nearest hit -> no labels at all [orig: the Entity_FindNearestSeatOrArmory gate
    // @0x5a32e2 brackets the whole pass].
    VehicleSeatSelection nearest;
    if (!find_nearest_free_seat_impl(
                world, player, nearest, armory_mode, hostile_mounts, source))
        return;

    // One label point [orig: the shared draw block @0x5a3553..0x5a36c9 — the +0.1875 u
    // lift, the 4.0 u 3D gate from the player POSITION, LOS, then the draw].
    const auto emit = [&](const Entity &cand, const Vec3 &point, int index, SeatType type,
                          bool armory) {
        Vec3 lifted = point;
        lifted.z += 0.1875f; // [orig: point.z = boneZ + 12288 @0x5a3585]
        const double dx = static_cast<double>(lifted.x) - static_cast<double>(player.position.x);
        const double dy = static_cast<double>(lifted.y) - static_cast<double>(player.position.y);
        const double dz = static_cast<double>(lifted.z) - static_cast<double>(player.position.z);
        const double d3 = std::sqrt(dx * dx + dy * dy + dz * dz);
        // [orig: the label radius @0x5a35f0 — dist < 0x40000 (4.0 u), FULL 3D, from the
        // entity position (not the eye)]
        if (static_cast<int32_t>(d3 * 65536.0) >= 0x40000) return;
		if (!point_los_clear(world, player, cand, lifted, true))
			return;
		AttachLabel label;
        label.entity = cand.handle;
        label.seat_index = index;
        label.type = type;
        label.armory = armory;
        label.nearest = cand.handle == nearest.vehicle && index == nearest.seat_index;
        label.world_pos = lifted;
        if (type == SeatType::Gunner && !cand.primary_weapon.empty()) {
            // [orig: Entity_GetWeaponSlots slot0 -> def+0x3A0 @0x5a351d]
            const int wi = world.tables.weapons.index_of(cand.primary_weapon.c_str());
            if (wi >= 0)
                label.attach_text_key =
                        world.tables.weapons.entries[static_cast<size_t>(wi)].attach_text_id;
        }
        out.push_back(label);
    };

    for_each_scan_candidate(world, player, [&](const Entity &cand) {
        // A ready weapon limits labels to the nearest entity [orig: !Player_CanFireWeapon()
        // || entity == nearest_entity @0x5a3354].
        if (can_fire && cand.handle != nearest.vehicle) return;
        if (!candidate_relevant_for_mode(cand, armory_mode)) return;
		if (scan_entity_rejected(world, cand, player, hostile_mounts))
			return;
		if (!armory_mode) {
            // [orig: the seat-label loop @0x5a3464; occupied seats never label @0x5a348f]
            for (int i = 0; i < static_cast<int>(cand.seats.size()); ++i) {
                const Seat &s = cand.seats[i];
                if (s.type == SeatType::None) continue;
                if (vehicle_seat_occupancy(world, cand, s, player.handle, source).occupied)
                    continue;
                emit(cand, seat_world_pos(world, cand, s), i, s.type, false);
            }
            return;
        }
        // [orig: the armory-label walk @0x5a36f5..@0x5a38e2]
        for (int i = 0; i < static_cast<int>(cand.armory_points.size()); ++i)
            emit(cand, local_point_world_pos(cand, cand.armory_points[i]), i,
                 SeatType::ArmoryPoint, true);
    });
}

bool VehicleSystem::player_toggle_mount(EntityHandle player) {
    World &world = world_;
    Entity *p = world.registry.get(player);
    if (p == nullptr || !p->alive || p->health <= 0) return false;
	// WAC's lock is local-player USE only; explicit AI/script detaches bypass it.
	// [orig: Entity_ToggleVehicleMount @0x43698B]
	if (p->mounted && player == world.cached.local_player && world.script.wac_values.seatbelt != 0)
		return false;

	if (!p->mounted) {
        // Two arms, keyed on the queued Co-op spawn-marker mount latch alone:
        // with Flags 0x200 set, the best free seat of the groundEntity carrier
        // (+0x180 restored it in the body update) and NOTHING else — no seat
        // there returns 0 without the scan; with it clear, the look-cone/LOS
        // nearest-seat scan. Standing on a deck does not set 0x200: a player on
        // a truck bed boards the seat he looks at, never the priority seat.
        // [orig: Entity_TryEnterNearestVehicle @0x4368CF (Flags & 0x200) ->
        //  Entity_FindBestSeatSlot(player, groundEntity) @0x4368E0, no target ->
        //  return 0 @0x436903; else Entity_FindNearestSeatOrArmory @0x43691A;
        //  the latch's writer Server_PositionPlayerForSpawn @0x50D442..0x50D45A]
        if (((p->flags | p->engine_flags) & kEntityFlagQueuedMount) != 0) {
            Entity *g = world.registry.get(p->ground_target);
            VehicleSeatSelection selection;
            if (g != nullptr && find_best_vehicle_seat(world, g->handle, player, selection))
                return world.vehicles.attach_to_seat(player, selection);
            return false;
        }
        VehicleSeatSelection hit;
        if (world.vehicles.find_nearest_free_seat(*p, hit, false))
            return world.vehicles.attach_to_seat(player, hit);
        return false;
    }

    // Mounted: a seat in scan reach swaps [orig: @0x4369ac -> TryEnterNearestVehicle],
    // else detach [orig: Entity_SendDetachPacket @0x4369c7 — the authority applies
    // directly through the same server leg].
    VehicleSeatSelection hit;
    if (world.vehicles.find_nearest_free_seat(*p, hit, false))
        return world.vehicles.attach_to_seat(player, hit);
    return world.vehicles.detach(player);
}

bool weapon_state_allows_mount_toggle(int32_t current_action, int32_t next_action) {
    return current_action < 2 || current_action == weapon_action::kEmpty ||
            next_action == weapon_action::kOverheated;
}

bool VehicleSystem::find_mount_toggle_candidate(const Entity &player, VehicleSeatSelection &r_hit, const VehicleOccupancySource *source) {
    World &world = world_;
    // The candidate-PREVIEW form of player_toggle_mount's unmounted search (a
    // joiner picks its request target without mutating L): the same two arms
    // — the queued-mount latch selects the groundEntity carrier's best seat or
    // nothing, otherwise the nearest scan [orig: Entity_TryEnterNearestVehicle
    // @0x4368CF / @0x4368E0 / @0x436903 / @0x43691A].
    if (!player.mounted && ((player.flags | player.engine_flags) & kEntityFlagQueuedMount) != 0) {
        const Entity *ground = world.registry.get(player.ground_target);
        return ground != nullptr &&
                find_best_vehicle_seat(
                        world, ground->handle, player.handle, r_hit, SeatSelectionMode::Any, source);
    }
    return world.vehicles.find_nearest_free_seat(player, r_hit, false, source);
}

} // namespace opennova::world
