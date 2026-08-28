// Mount-relation CTRL sources: the carrier-side HEAT_GLOW derivation and the
// emplaced-weapon turret phase pair. Moved verbatim from the shell binding's
// simulation internals (ADR 0028) — every input is world state, and both the
// legacy render/collision paths and the engine-side pose provider consume the
// same derivations (ADR 0016 one-impl).
#ifndef OPENNOVA_WORLD_MOUNT_CONTROLS_H
#define OPENNOVA_WORLD_MOUNT_CONTROLS_H

#include <runtime/world/ai.h>
#include <runtime/world/angle.h>
#include <runtime/world/turret_window.h>
#include <runtime/world/weapon_fsm.h>
#include <runtime/world/world.h>

#include <base/io/bam.h>

#include <cstdint>

namespace opennova::world {

// Derive HEAT_GLOW only for the carrier model participating in a live UseGun
// bone attachment. Retail does not publish this for every entity: infantry
// player/AI update calls Entity_AttachToBoneAndUpdateTransform only for
// parentSlot 3, and that helper caches the PARENT carrier's inline MountSlot
// immediately before transforming the parent's PANM/bones. Our retained model
// and collision paths therefore use the same validated carrier/child relation.
// The separate first-person writer has its own 0x10000 endpoint.
// [orig: Entity_UpdateInfantryPlayerBody @ 0x4B63BF..0x4B63C7;
//  Entity_UpdateInfantryAI @ 0x4BEC1B..0x4BEC23;
//  Entity_AttachToBoneAndUpdateTransform @ 0x546424..0x54652B;
//  HUD_CacheWeaponSlotInfo @ 0x440930]
inline bool world_model_heat_glow_for(
		const World &world,
		const Entity &carrier,
		int32_t &r_heat_glow) {
	r_heat_glow = 0;
	if (!carrier.primary_weapon_owner.valid()) return false;
	const Entity *child = world.registry.get(carrier.primary_weapon_owner);
	if (child == nullptr || !child->alive || child->health <= 0 ||
			!child->mounted ||
			child->mount_type != SeatType::Gunner ||
			child->mount_target != carrier.handle ||
			child->mount_seat < 0 ||
			child->mount_seat >= static_cast<int>(carrier.seats.size()))
		return false;
	const Seat &seat = carrier.seats[static_cast<size_t>(child->mount_seat)];
	if (seat.type != SeatType::Gunner ||
			seat.bone_index == 0 || seat.occupant != child->handle)
		return false;
	const WeaponTableEntry *weapon =
			world.weapons.by_index(carrier.primary_weapon_slot_adm);
	if (weapon == nullptr) return false;
	const int32_t tick = static_cast<int32_t>(world.logic_tick);
	r_heat_glow = weapon_slot_world_heat_glow(
			weapon->action_fsm, carrier.primary_weapon_slot, tick);
	return true;
}

struct EmplacedWeaponControls {
	bool valid = false;
	uint16_t gun_yaw = 0;
	uint16_t gun_pitch = 0;
};

inline uint16_t emplaced_control_phase(int32_t parent_bam, int32_t occupant_bam) {
	// Retail stores the high word of the wrapped parent-minus-occupant angle:
	// occupant Yaw/Pitch = parent Yaw/Pitch - turret control.
	// [orig: Entity_UpdateTransformAndTurret @0x441251..0x441263,
	//  @0x441298..0x4412b3]
	const int32_t delta = opennova::io::bam_sub(parent_bam, occupant_bam);
	return static_cast<uint16_t>(static_cast<uint32_t>(delta) >> 16);
}

// Degrees -> BAM clamp bound. A half-arc of 180 or more is the full circle
// (the "360" gun family) — no effective window; 0 tells callers to skip.
inline int32_t turret_limit_bam(int16_t degrees) {
	if (degrees <= 0 || degrees >= 180) return 0;
	return static_cast<int32_t>(
			bam_from_degrees_wrapped(static_cast<double>(degrees)));
}

// The turret-phase limit clamp, transliterated: the 0x1FFFF admission band,
// second write wins. Retail runs this every entity update so a phase implied
// beyond the gun's arc pins AT the arc edge — a "180 tripod" barrel can never
// present outside its authored traverse. [orig: Math_ClampAngleToBounds @0x540cc0; caller
// Entity_UpdateTransformAndTurret @0x441228..0x44128c]
inline bool emplaced_clamp_turret_bam(int32_t &value, int32_t upper,
		int32_t lower) {
	bool clamped = false;
	if (value > upper - 0x1FFFF) {
		value = upper;
		clamped = true;
	}
	if (value < lower + 0x1FFFF) {
		value = lower;
		return true;
	}
	return clamped;
}

inline bool emplaced_weapon_controls_for(
		const World &world,
		AiSystem *ai,
		const Entity &mount,
		EmplacedWeaponControls &out) {
	out = EmplacedWeaponControls{};
	if (ai == nullptr || !mount.primary_weapon_owner.valid()) return false;
	const Entity *occupant = world.registry.get(mount.primary_weapon_owner);
	if (occupant == nullptr || !occupant->alive || occupant->health <= 0 ||
			!occupant->mounted ||
			occupant->mount_type != SeatType::Gunner ||
			occupant->mount_target != mount.handle)
		return false;
	const AiEntity *gunner = ai->for_handle(occupant->handle);
	if (gunner == nullptr) return false;

	// The parent owns the embedded weapon/model while the organic owns live look.
	// A vehicle motor preserves sub-degree parent yaw in BAM; a static EWEAP uses
	// its mission-yaw field. Pitch has no separate motor accumulator.
	const int32_t parent_heading = mount.veh.yaw_seeded
			? mount.veh.yaw_bam
			: bam_heading_from_mission_yaw_deg(static_cast<double>(mount.yaw));
	const int32_t parent_pitch =
			bam_from_degrees_wrapped(static_cast<double>(mount.pitch));
	int32_t yaw_delta = opennova::io::bam_sub(parent_heading, gunner->heading);
	int32_t pitch_delta = opennova::io::bam_sub(parent_pitch, gunner->pitch);
	// Window source selection lives in world::select_turret_window — the
	// per-seat addeweap arc first, the weapon-def window second (the [orig]
	// map is on the helper). Per-seat clamps BOTH axes with the quartet
	// verbatim (an authored zero pair pins); the weapon-def leg keeps the
	// witnessed per-axis zero-means-no-window semantics.
	// [orig: the per-update clamp @0x441228..0x44128c via Math_ClampAngleToBounds]
	const TurretWindow window = select_turret_window(
			mount.emplacement_down_limit_bam,
			mount.emplacement_up_limit_bam,
			mount.emplacement_right_limit_bam,
			mount.emplacement_left_limit_bam,
			mount.primary_weapon_slot_adm != kAdmSlotNone
					? world.weapons.by_index(mount.primary_weapon_slot_adm)
					: nullptr);
	if (window.per_seat) {
		emplaced_clamp_turret_bam(yaw_delta, window.yaw_upper,
				window.yaw_lower);
		emplaced_clamp_turret_bam(pitch_delta, window.pitch_upper,
				window.pitch_lower);
	} else {
		if (window.yaw_upper != 0)
			emplaced_clamp_turret_bam(yaw_delta, window.yaw_upper,
					window.yaw_lower);
		if (window.pitch_upper != 0 || window.pitch_lower != 0)
			emplaced_clamp_turret_bam(pitch_delta, window.pitch_upper,
					window.pitch_lower);
	}
	out.valid = true;
	out.gun_yaw = static_cast<uint16_t>(static_cast<uint32_t>(yaw_delta) >> 16);
	out.gun_pitch =
			static_cast<uint16_t>(static_cast<uint32_t>(pitch_delta) >> 16);
	return true;
}

} // namespace opennova::world

#endif // OPENNOVA_WORLD_MOUNT_CONTROLS_H
