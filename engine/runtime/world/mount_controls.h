// Mount-relation CTRL sources: the carrier-side HEAT_GLOW derivation and the
// emplaced-weapon turret phase pair. Moved verbatim from the shell binding's
// simulation internals (ADR 0028) — every input is world state, and both the
// legacy render/collision paths and the engine-side pose provider consume the
// same derivations (ADR 0016 one-impl).
#pragma once

#include <runtime/world/ai.h>
#include <runtime/world/angle.h>
#include <runtime/world/turret_window.h>
#include <runtime/world/weapon_fsm.h>
#include <runtime/world/fire_sound.h>
#include <runtime/world/world.h>

#include <runtime/audio/sound_profile.h>

#include <formats/def/def.h>

#include <base/io/bam.h>
#include <base/io/crt_ftol.h>

#include <cmath>
#include <cstdint>

namespace opennova::world {

// ---------------------------------------------------------------------------
// The ewep class's CTRL writer: three registers published from the gun's own
// state -- the stored gun words (EWEAP_GUNYAW/GUNPITCH), the barrel spin word
// (WEAP_SPIN) and the inline MountSlot's heat (HEAT_GLOW: literal zero once the
// heat window has lapsed, else the accumulated heat capped at the largest
// unsigned word; the separate first-person writer has its own 0x10000
// endpoint). The 'ewep' render class installs it as def+0x144, so it runs
// before every render and every userpoint transform of the gun, occupied or
// not: nothing clears the words on a detach, and an emptied turret holds its
// last traverse. A UseGun rider's seat attachment also calls it directly on
// the PARENT carrier, whatever that carrier's render class, before posing the
// carrier's PANM/bones.
// [orig: HUD_CacheWeaponSlotInfo @ 0x440930 (words @0x440934..0x440948, spin
//  @0x44094E..0x440955, heat @0x44095B..0x440991); the 'ewep' render-class row
//  @0x82CFA0 through BoneCallback_LookupByTag @0x4E32ED..0x4E3306; callers
//  Entity_RenderVehicleModel @0x440852..0x440866,
//  Entity_ComputeUserpointWorldTransform @0x545CA3..0x545CAE,
//  Entity_ComputeUserpointTransform @0x545A89..0x545A94,
//  Bone_BuildAttachmentMatrix @0x56C6DC..0x56C6F3; the seat call
//  Entity_AttachToBoneAndUpdateTransform @ 0x546424..0x54652B (the call
//  @0x546517..0x546518), reached for parentSlot 3 from
//  Entity_UpdateInfantryPlayerBody @ 0x4B63BF..0x4B63C7 and
//  Entity_UpdateInfantryAI @ 0x4BEC1B..0x4BEC23. The words' only writers are
//  Entity_UpdateChildAttachment @0x440B23/@0x440B45/@0x440B58, the window
//  clamp @0x44125C/@0x4412A4, the carrier-destruction reset @0x5470F9/@0x547100
//  and the lag pip's save/restore @0x59EA6A..0x59EAF3]
// ---------------------------------------------------------------------------

// The writer's heat leg on the gun's own inline slot.
inline int32_t emplaced_slot_heat_glow(const World &world, const Entity &gun) {
	const WeaponTableEntry *weapon = gun.primary_weapon_slot_adm != kAdmSlotNone
			? world.tables.weapons.by_index(gun.primary_weapon_slot_adm)
			: nullptr;
	if (weapon == nullptr) return 0;
	return weapon_slot_world_heat_glow(weapon->action_fsm, gun.primary_weapon_slot,
			static_cast<int32_t>(world.logic_tick));
}

// The seat call: a living parentSlot-3 rider whose carrier seat names a
// userpoint runs the writer on its PARENT carrier every tick, whatever the
// carrier's render class. A carrier of another class publishes nothing of
// its own, so the rider's words stay on the global CTRL bus for that
// carrier's render, collision and attachment frames while the rider holds
// the seat.
// [orig: Entity_AttachToBoneAndUpdateTransform userpoint gate
//  @0x546424..0x54643F, the call @0x546517..0x546518]
// On a joiner the rider may be a remote person the world does not hold; the
// replica's gun channel marks its carrier (Entity::replica_usegun_rider).
// [orig: Entity_UpdateInfantryPlayerBody @0x4B63A5..0x4B63C7]
inline bool usegun_rider_writes_carrier(const World &world, const Entity &carrier) {
	if (carrier.replica_usegun_rider) return true;
	if (!carrier.primary_weapon_owner.valid()) return false;
	const Entity *rider = world.registry.get(carrier.primary_weapon_owner);
	if (rider == nullptr || !rider->alive || rider->health <= 0 ||
			!rider->mounted ||
			rider->mount_type != SeatType::Gunner ||
			rider->mount_target != carrier.handle ||
			rider->mount_seat < 0 ||
			rider->mount_seat >= static_cast<int>(carrier.seats.size()))
		return false;
	const Seat &seat = carrier.seats[static_cast<size_t>(rider->mount_seat)];
	if (seat.type != SeatType::Gunner ||
			seat.bone_index == 0 || seat.occupant != rider->handle)
		return false;
	return world.ai.for_handle(rider->handle) != nullptr;
}

// HEAT_GLOW as the carrier's frames read it: an 'ewep' render class
// publishes it from its inline slot, occupied or not; any other class only
// through a UseGun rider's seat call.
inline bool world_model_heat_glow_for(
		const World &world,
		const Entity &carrier,
		int32_t &r_heat_glow) {
	r_heat_glow = 0;
	if (!carrier.emplaced_ctrl_publisher && !usegun_rider_writes_carrier(world, carrier))
		return false;
	r_heat_glow = emplaced_slot_heat_glow(world, carrier);
	return true;
}

// ---------------------------------------------------------------------------
// The emplaced gun channel: the two stored words on the ewep entity
// (Entity::emplaced_gun_yaw_word / emplaced_gun_pitch_word = retail
// entity+0x322 / +0x324, the gun's yaw/pitch relative to the emplacement's own
// frame as BAM32 high words) and the two per-tick legs that own them.
//
// Producer — Entity_UpdateChildAttachment @0x4409A0, the 'ewep' ai_function
// fn1 (ai-fn table row @0x813090), run every tick the gun is occupied
// (Entity_UpdatePool1Slot @0x4b8dd0 calls the ai-fn @0x4b8e3c whenever the
// +0x2AC age is <= 0, and the occupied leg never re-arms that age). Two paths
// on the item def's ItemDefAttrib2 IsTurret bit (0x1000, @0x440a36):
//   immediate: word322 = (gun.Yaw - occ.Yaw) >> 16,
//              word324 = (gun.Pitch - occ.recoilPitch - occ.Pitch) >> 16
//              [@0x440b39..0x440b58];
//   IsTurret:  the words slew toward the occupant at most 0x92CF34 BAM
//              (0.806 deg, ~50 deg/s at 62 Hz) per tick, and the OCCUPANT's
//              own Yaw is pulled back to within +-0x3FFFFFC0 (90 deg) of the
//              turret for the local player (mirrored into the local look yaw)
//              or +-0x2D82D80 (4 deg) for a non-Player occupant; a remote
//              Player occupant gets neither tether. The recoil term is
//              subtracted AFTER the pitch rate clamp [@0x440a43..0x440b37].
// Consumer — Entity_UpdateTransformAndTurret @0x440ca0, the 'ewep' class
// update (@0x4b8e53, right after fn1): first an addeweap child riding the
// parent ROOT publishes its words to the parent's AI brain turret channel
// [@0x440f04..0x441020, publish_emplaced_gun_words_to_parent]; then, while
// the UseGun claimant's parent is this gun, the words are clamped to the
// seat/weapon window through Math_ClampAngleToBounds and, on a clamp, the
// OCCUPANT's Yaw/Pitch are STORED at the arc edge (the local look yaw
// mirrored too) [@0x4411d1..0x4412b3]. The model's EWEAP_GUNYAW/GUNPITCH
// CTRL pair then reads the words as stored — emplaced_weapon_controls_for
// below.
//
// Both legs run inside the pool-1 walk, which precedes the pool-0 organic
// walk [orig: Entity_UpdateAllEntities @0x4c2100]: the occupant's own body
// update sees the tethered/pinned look. Our AiSystem runs them at the head
// of the gunner's pose_if_mounted for the same effect.
// ---------------------------------------------------------------------------

struct EmplacedWeaponControls {
	bool valid = false;
	uint16_t gun_yaw = 0;
	uint16_t gun_pitch = 0;
	uint16_t spin = 0;
};

// The ewep class update tail, before pool-0 attachment poses and the global
// weapon-action pump. Read/decrement the inline slot kick byte even without
// an occupant, then integrate the wrapping angle word. The later weapon pump
// also decays that SAME byte. The audio catch-up gate never gates spin.
// docs/threedi/3di-gp-format-re.md (D-3DI-4).
// [orig: Entity_UpdateTransformAndTurret @0x44139D..0x441447]
inline void tick_emplaced_weapon_animation(World &world, Entity &mount) {
	if (!mount.emplaced_update) return;
	const auto *weapon = world.tables.weapons.by_index(mount.primary_weapon_slot_adm);
	if (weapon == nullptr) return; // inline slot Def +0x2D4 gate @0x4411C9
	uint8_t &kick = mount.primary_weapon_slot.kick;
	if (world.rules.last_tick_of_batch && kick != 0 && weapon->action_fsm.soundfireloop[0] != 0) {
		WeaponFsmEvents sound;
		sound.fireloop_lifetime_ticks = static_cast<int8_t>(kick);
		weapon_sound_publish(world, mount, weapon->action_fsm, sound);
	}
	if (kick != 0) {
		mount.emplaced_spin_ticks = 60;
		--kick;
		if (kick == 0)
			world.out.fire_sounds.play_with_distance_delay(weapon->action_fsm.soundtrailoff,
					mount.position, mount.bms_id, mount.handle.packed);
	}
	if (mount.emplaced_spin_ticks != 0) {
		--mount.emplaced_spin_ticks;
		if (mount.emplaced_spin_ticks != 0) {
			const int32_t speed = mount.emplaced_spin_ticks < 128
					? mount.emplaced_spin_ticks : int32_t(mount.emplaced_spin_ticks) - 256;
			mount.emplaced_spin_phase = static_cast<uint16_t>(mount.emplaced_spin_phase + 32 * speed);
		}
	}
}

// The IsTurret per-tick traverse rate and the two gunner-yaw tethers, BAM32.
// [orig: 0x92CF34 @0x440ae1/@0x440af0 (yaw) and @0x440afc/@0x440b0b (pitch);
//  0x3FFFFFC0 / 0xC0000040 @0x440a7e/@0x440a8c (the local player);
//  0x2D82D80 / 0xFD27D280 @0x440abd/@0x440acb (a non-Player occupant)]
inline constexpr int32_t kEmplacedTurretSlewPerTick = 0x92CF34;
inline constexpr int32_t kEmplacedLocalGunnerYawTether = 0x3FFFFFC0;
inline constexpr int32_t kEmplacedNpcGunnerYawTether = 0x2D82D80;
// dfx2.exe's IsTurret traverse holds the step's length on the yaw / pitch plane to the rate,
// not each axis (DFX2-26), and a slower rate while the local player mans a gun whose carrier
// counts one occupied seat at most (emplaced_carrier_occupied_seats).
// [orig: Entity_UpdateChildAttachment @0x4407A0 (dfx2.exe) - the rate 0x92CF34 @0x440937,
//  0x3AB948 @0x440941 under the gate @0x440875..0x4408A7]
inline constexpr int32_t kDfx2EmplacedTurretSoloSlewPerTick = 0x3AB948;

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

// The emplacement's own frame — retail's entity Yaw/Pitch of the ewep (+0x10 /
// +0x14). A vehicle motor preserves sub-degree parent yaw in BAM; a static
// EWEAP holds its placement angles in the spawn form. Attached EWEAPs retain
// the bone's full pitch in the same BAM attitude fields as their carrier.
// [orig: Entity_UpdateChildAttachment @0x440A51/@0x440A58; Entity_SpawnFromBMSRecord
//  @0x40EB42..0x40EB86]
inline int32_t emplaced_gun_frame_heading(const Entity &mount) {
	return mount.veh.yaw_seeded
			? mount.veh.yaw_bam
			: mount.heading;
}

inline int32_t emplaced_gun_frame_pitch(const Entity &mount) {
	return mount.veh.yaw_seeded ? mount.veh.air_pitch_bam
			: mount.pitch_bam;
}

inline int32_t emplaced_gun_frame_roll(const Entity &mount) {
	return mount.veh.yaw_seeded ? mount.veh.air_roll_bam : mount.roll_bam;
}

// An entity's +0x28 word, which our model splits: a rider's mount, an attached child's
// carrier, else the ground it stands on (entity_commands.cpp carrier_of carries the witness).
inline const Entity *entity_ground_word(const World &world, const Entity &e) {
	if (e.mounted && e.mount_target.valid()) return world.registry.get(e.mount_target);
	if (e.emplacement_parent.valid()) return world.registry.get(e.emplacement_parent);
	return world.registry.get(e.ground_target);
}

// The occupied seats of `carrier` and of every entity of its candidate slice that rides it
// (its +0x28 the carrier), the carrier last; any of them dead, or without a def, a model or a
// model userpoint, makes the whole count 0. A carrier with no slice counts its own seats.
// [orig: Entity_CountActiveWeaponSlots @0x434E70 (dfx2.exe; Jointops.exe's @0x435070 has no
//  caller) - the slice +0x1BC / +0x1C0 @0x434E76..0x434E99, the walk to the count inclusive
//  (the carrier at the count) @0x434EB2..0x434FD4, the +0x28 test @0x434EC3, the dead bit
//  @0x434ECC, the def +0x20, the model +0x30 and its userpoint count +0xBC @0x434ED9..0x434EF3
//  (each a 0), the ten seat bytes def+0x25D.. against the mount words +0x190..
//  @0x434EF9..0x434FC8]
inline int emplaced_carrier_occupied_seats(const World &world, const Entity &carrier) {
	int32_t slice_count = 0;
	const EntityHandle *slice = world.ai.collision != nullptr
			? world.ai.collision->candidate_slice(carrier.handle, slice_count) : nullptr;
	if (slice == nullptr) slice_count = 0;
	int occupied = 0;
	for (int32_t i = 0; i <= slice_count; ++i) {
		const Entity *e = i == slice_count ? &carrier : world.registry.get(slice[i]);
		if (e == nullptr) continue;
		if (e != &carrier && entity_ground_word(world, *e) != &carrier) continue;
		if (((e->flags | e->engine_flags) & kEntityFlagDead) != 0 || !e->has_item_def ||
				!e->has_graphic_model || e->graphic_model_userpoints == 0)
			return 0;
		for (const Seat &seat : e->seats)
			if (seat.type != SeatType::None && seat.occupant.valid()) ++occupied;
	}
	return occupied;
}

// A stored word back to the BAM32 the IsTurret leg integrates: the raw 16
// bits shifted up plus the 0x8000 half-step. [orig: `movzx / shl 10h /
// add 8000h` @0x440a43..0x440a69]
inline int32_t emplaced_word_bam_rounded(int16_t word) {
	return opennova::io::bam_add(
			static_cast<int32_t>(
					static_cast<uint32_t>(static_cast<uint16_t>(word)) << 16),
			0x8000);
}

// The window clamp's input form: the raw word shifted up, no half-step.
// [orig: `movzx / shl 10h` @0x4411f0/@0x4411fe and @0x4411f7/@0x44120d]
inline int32_t emplaced_word_bam(int16_t word) {
	return static_cast<int32_t>(
			static_cast<uint32_t>(static_cast<uint16_t>(word)) << 16);
}

// The word store: the high 16 bits of the BAM32 (`sar 10h; mov [..],ax` in
// the producer, `shr 10h; mov [..],cx` in the consumer — the same 16 bits).
inline int16_t emplaced_bam_word(int32_t bam) {
	return static_cast<int16_t>(
			static_cast<uint16_t>(static_cast<uint32_t>(bam) >> 16));
}

// The symmetric +-bound clamp the tethers and the rate limit use — two
// compares, the bound wins. [orig: the cmp/jle/mov, cmp/jge/mov pairs
// @0x440a7e..0x440a93, @0x440abd..0x440ad2, @0x440ae1..0x440b13]
inline int32_t emplaced_clamp_symmetric(int32_t value, int32_t bound) {
	if (value > bound) return bound;
	if (value < -bound) return -bound;
	return value;
}

// The producer: refresh the stored words from the UseGun occupant. `gunner`
// is the occupant's live look (retail's one entity Yaw/Pitch: our AiEntity
// heading/pitch, the local player's input-owned look mirrored in
// inf.target_heading / inf.look_pitch); `occupant` is its registry row (the
// Player-class bit). Writes the gunner's heading on the IsTurret tethers.
// [orig: Entity_UpdateChildAttachment @0x4409A0, occupant leg @0x440a1c..0x440b58]
struct EmplacedGunChannel {
 int16_t yaw = 0;
 int16_t pitch = 0;
};
struct EmplacedGunnerLook {
 int32_t heading = 0;
 int32_t pitch = 0;
 int32_t recoil_pitch = 0;
 bool is_local_player = false;
 bool is_player = false;
};

inline void advance_emplaced_gun_channel(EmplacedGunChannel &channel,
		EmplacedGunnerLook &gunner, int32_t gun_yaw, int32_t gun_pitch, bool is_turret,
		int32_t planar_rate = 0) {
	using opennova::io::bam_add;
	using opennova::io::bam_sub;
	if (!is_turret) {
		// The immediate path: the words follow the occupant's look this tick,
		// pitch less the occupant's recoil accumulator (entity+0x380).
		// [orig: @0x440a36 jz -> @0x440b39..0x440b58; `sub ecx,[edx+380h]`
		//  @0x440b4c]
		channel.yaw =
				emplaced_bam_word(bam_sub(gun_yaw, gunner.heading));
		channel.pitch = emplaced_bam_word(
				bam_sub(bam_sub(gun_pitch, gunner.recoil_pitch), gunner.pitch));
		return;
	}
	// The IsTurret path. edi/ebx = the previous words as rounded BAM32;
	// eax = gun.Yaw - edi - occ.Yaw; ecx = gun.Pitch - occ.Pitch - ebx.
	// [orig: @0x440a43..0x440a74]
	const int32_t prev_yaw = emplaced_word_bam_rounded(channel.yaw);
	const int32_t prev_pitch =
			emplaced_word_bam_rounded(channel.pitch);
	int32_t yaw_step = bam_sub(bam_sub(gun_yaw, prev_yaw), gunner.heading);
	int32_t pitch_step = bam_sub(bam_sub(gun_pitch, gunner.pitch), prev_pitch);
	if (gunner.is_local_player) {
		// The local player's own yaw is pulled back to within +-90 deg of the
		// turret and the look-yaw global mirrors it. [orig: `cmp edx,
		// g_LocalPlayerEntity` @0x440a76; clamp @0x440a7e..0x440a93;
		// occ.Yaw = gun.Yaw - eax - edi @0x440a98..0x440a9c;
		// g_LocalPlayerLookYaw = occ.Yaw @0x440aa8]
		yaw_step = emplaced_clamp_symmetric(yaw_step, kEmplacedLocalGunnerYawTether);
		gunner.heading = bam_sub(bam_sub(gun_yaw, yaw_step), prev_yaw);
	}
	if (!gunner.is_player) {
		// A non-Player occupant (an NPC gunner) is tethered to +-4 deg; a
		// remote Player skips both tethers. [orig: `test [edx+24h],100h`
		// @0x440ab4 jnz; clamp @0x440abd..0x440ad2; store @0x440ad7..0x440ade]
		yaw_step = emplaced_clamp_symmetric(yaw_step, kEmplacedNpcGunnerYawTether);
		gunner.heading = bam_sub(bam_sub(gun_yaw, yaw_step), prev_yaw);
	}
	// The per-tick traverse rate on both axes, then the integrate: yaw word =
	// (eax + edi) >> 16; pitch word = (ecx - occ.recoilPitch + ebx) >> 16 —
	// the recoil term lands after the rate clamp, so it is never rate-limited.
	// [orig: @0x440ae1..0x440b13; @0x440b1e..0x440b23; @0x440b2a..0x440b58]
	if (planar_rate > 0) {
		// dfx2.exe's: the step's length on the plane held to the rate, both axes scaled by the
		// rate over the length and each truncated (DFX2-26).
		// [orig: Entity_UpdateChildAttachment @0x4407A0 (dfx2.exe) - fild / fmul / faddp /
		//  fsqrt @0x440949..0x44095B, _ftol2_sse(length) against the rate @0x44095F..0x440968,
		//  fidivr @0x44096A, the two _ftol2_sse @0x440974 / @0x44097D, the words
		//  @0x44098C..0x4409AB]
		const double length = std::sqrt(static_cast<double>(yaw_step) * yaw_step +
				static_cast<double>(pitch_step) * pitch_step);
		if (opennova::io::retail_ftol_sse2(length) > planar_rate) {
			const double scale = static_cast<double>(planar_rate) / length;
			yaw_step = opennova::io::retail_ftol_sse2(static_cast<double>(yaw_step) * scale);
			pitch_step = opennova::io::retail_ftol_sse2(static_cast<double>(pitch_step) * scale);
		}
	} else {
		yaw_step = emplaced_clamp_symmetric(yaw_step, kEmplacedTurretSlewPerTick);
		pitch_step = emplaced_clamp_symmetric(pitch_step, kEmplacedTurretSlewPerTick);
	}
	channel.yaw = emplaced_bam_word(bam_add(yaw_step, prev_yaw));
	channel.pitch = emplaced_bam_word(
			bam_add(bam_sub(pitch_step, gunner.recoil_pitch), prev_pitch));
}

// dfx2.exe's planar traverse rate for a gun (0: Jointops.exe's per-axis one): the slower one
// while the local player mans it and the gun's +0x28 carrier counts one occupied seat at most;
// a gun with no +0x28 (whose count retail reads through a null pointer) takes the full one.
// [orig: Entity_UpdateChildAttachment @0x4407A0 (dfx2.exe) - `cmp eax, g_LocalPlayerEntity`
//  @0x440875, Entity_CountActiveWeaponSlots(+0x28) @0x44088E, `cmp eax, 1; jg` @0x44089A]
inline int32_t emplaced_turret_planar_rate(const World &world, const Entity &mount,
		bool local_gunner) {
	if (world.rules.title != gameprofile::GAME_DFX2) return 0;
	const Entity *carrier = entity_ground_word(world, mount);
	if (local_gunner && carrier != nullptr && emplaced_carrier_occupied_seats(world, *carrier) <= 1)
		return kDfx2EmplacedTurretSoloSlewPerTick;
	return kEmplacedTurretSlewPerTick;
}

inline bool emplaced_counter_rotates(const World &world, const Entity &child);

inline void tick_emplaced_gun_words(const World &world, Entity &mount, const Entity &occupant,
		AiEntity &gunner) {
 EmplacedGunChannel channel{mount.emplaced_gun_yaw_word, mount.emplaced_gun_pitch_word};
 EmplacedGunnerLook look{gunner.heading, gunner.pitch, gunner.inf.recoil_pitch,
  gunner.inf.is_local_player, ((occupant.flags | occupant.engine_flags) & kEntityFlagPlayer) != 0};
 // dfx2.exe's counter-rotating child slews against its frame before this tick's pose (DFX2-25;
 // its counter-rotation follows the publication, tick_emplaced_weapon_channel).
 const bool counter = mount.saved_live_valid && emplaced_counter_rotates(world, mount);
 advance_emplaced_gun_channel(channel, look,
  counter ? mount.saved_live_yaw : emplaced_gun_frame_heading(mount),
  counter ? mount.saved_live_pitch : emplaced_gun_frame_pitch(mount),
  (mount.item_attrib2 & opennova::def::DEF_ITEM_ATTRIB2_ISTURRET) != 0,
  emplaced_turret_planar_rate(world, mount, gunner.inf.is_local_player));
 mount.emplaced_gun_yaw_word = channel.yaw;
 mount.emplaced_gun_pitch_word = channel.pitch;
 gunner.heading = look.heading;
 if (gunner.inf.is_local_player) gunner.inf.target_heading = look.heading;
}

// The consumer's window leg: clamp the stored words to the seat/weapon window
// and, on a clamp, STORE the pinned look into the occupant — the gunner's
// view cannot rotate past the gun's limits while mounted. Window source
// selection lives in world::select_turret_window — the per-seat addeweap arc
// first, the weapon-def window second (the [orig] map is on the helper).
// Both legs clamp BOTH axes unconditionally, so a zero bound pins its axis
// (a weapon-def yawrange of 0 locks the traverse); only a gun with neither
// an authored quartet nor a weapon def has no window.
// [orig: Entity_UpdateTransformAndTurret @0x440ca0 — gate `occupant &&
//  occupant->parentEntity == this` @0x4411d1..0x4411ea; the words read
//  @0x4411f0/@0x4411f7; Entity_GetWeaponTurretLimits @0x441228;
//  yaw: Math_ClampAngleToBounds @0x44123c, on 1: word @0x44125c, occ.Yaw =
//  gun.Yaw - clamped @0x44124c/@0x441251/@0x441263, g_LocalPlayerLookYaw =
//  occ.Yaw when local @0x44126c/@0x441277; pitch: clamp @0x44128c, on 1:
//  word @0x4412a4, occ.Pitch = gun.Pitch - clamped @0x44129c/@0x4412b1/
//  @0x4412b3 (no look global for pitch — the entity Pitch IS the look)]
inline void clamp_emplaced_gun_channel(EmplacedGunChannel &channel,
 EmplacedGunnerLook &gunner, int32_t gun_yaw, int32_t gun_pitch, const TurretWindow &window) {
 int32_t yaw = emplaced_word_bam(channel.yaw);
 int32_t pitch = emplaced_word_bam(channel.pitch);
	if (!window.active) return;
	const bool yaw_clamped = emplaced_clamp_turret_bam(yaw, window.yaw_upper,
			window.yaw_lower);
	const bool pitch_clamped = emplaced_clamp_turret_bam(pitch, window.pitch_upper,
			window.pitch_lower);
	if (yaw_clamped) {
		channel.yaw = emplaced_bam_word(yaw);
		gunner.heading = opennova::io::bam_sub(gun_yaw, yaw);
	}
	if (pitch_clamped) {
		channel.pitch = emplaced_bam_word(pitch);
		gunner.pitch = opennova::io::bam_sub(gun_pitch, pitch);
	}
}

inline void clamp_emplaced_gun_words_to_window(const World &world, Entity &mount, AiEntity &gunner) {
	const TurretWindow window = select_turret_window(
			mount.emplacement_down_limit_bam,
			mount.emplacement_up_limit_bam,
			mount.emplacement_right_limit_bam,
			mount.emplacement_left_limit_bam,
			mount.primary_weapon_slot_adm != kAdmSlotNone
					? world.tables.weapons.by_index(mount.primary_weapon_slot_adm)
					: nullptr);
 EmplacedGunChannel channel{mount.emplaced_gun_yaw_word, mount.emplaced_gun_pitch_word};
 EmplacedGunnerLook look{gunner.heading, gunner.pitch};
 clamp_emplaced_gun_channel(channel, look, emplaced_gun_frame_heading(mount),
  emplaced_gun_frame_pitch(mount), window);
 mount.emplaced_gun_yaw_word = channel.yaw;
 mount.emplaced_gun_pitch_word = channel.pitch;
 gunner.heading = look.heading;
 gunner.pitch = look.pitch;
 if (gunner.inf.is_local_player) {
  gunner.inf.target_heading = look.heading;
  gunner.inf.look_pitch = look.pitch;
 }
}

// The game text the tank-command leg posts (Chat_AddMessageChannel1 in g_HUDColors'
// fifth entry, yellow, for 930 ticks).
// [orig: Entity_UpdateTransformAndTurret @0x440CA0 - "HUD_COM2GUN" @0x44132C, the section
//  "HUD" @0x7C4E64 pushed @0x441331, `mov ecx, g_HUDColors+10h` @0x44134B, `push 3A2h` @0x441351]
inline const HudChatLine kEmplacedCommandCallLine{"HUD", "HUD_COM2GUN", 0xFFF0F000u};

// The window leg's tail while the occupant rides this gun: the tank-command call. With the
// local player in a UseGun seat (attach type 3) of a gun whose ground entity (+0x28) is this
// gun's own (two guns with none match), an occupant holding the packed jump bit (MoveOrder
// 0x20) more than 124 ticks past the gun's last call (+0x31C, a signed difference) re-stamps
// it; an occupant other than the local player then plays its TANK_COMAND composite on the
// interface channel and posts HUD_COM2GUN to the CHAT ring.
// [orig: Entity_UpdateTransformAndTurret @0x440CA0 - g_LocalPlayerEntity @0x4412B6..0x4412BD,
//  `cmp dword ptr [eax+168h], 3` @0x4412C3, its +0x16C @0x4412D0, `cmp eax, [ebp+28h]`
//  @0x4412DE..0x4412E1, `test byte ptr [eax+12Ch], 20h` @0x4412F0, the tick test
//  @0x4412FD..0x44130E, the stamp @0x441310, the local test @0x441316, SoundProfile_FindByEntityAndType
//  (occupant, 8) @0x441321, Sound_PlayInterfaceTriggerSet @0x441327, GameText_GetString @0x441336,
//  Chat_AddMessageChannel1 @0x44135C]
// `ground` and `stamp` are the updated entity's +0x28 and +0x31C: an item gun's ground_target and
// emplaced_command_stamp, a person body's ground_target and its spawn backup's Y word.
inline void emplaced_command_call(World &world, EntityHandle ground, uint32_t &stamp,
		bool occupant_is_local, uint8_t occupant_move_order, int occupant_anim_slot) {
	const Entity *local = world.registry.get(world.cached.local_player);
	if (local == nullptr || !local->mounted || local->mount_type != SeatType::Gunner) return;
	const Entity *local_gun = world.registry.get(local->mount_target);
	if (local_gun == nullptr || local_gun->ground_target != ground) return;
	if ((occupant_move_order & Entity::kMoveOrderJump) == 0) return;
	if (static_cast<int32_t>(world.logic_tick - stamp) <= 124) return;
	stamp = world.logic_tick;
	if (occupant_is_local) return;
	char name[64];
	ScriptSoundEvent call;
	call.kind = ScriptSoundEvent::Kind::Interface;
	call.name = audio::compose_entity_sound_set(occupant_anim_slot, audio::kEntitySoundTankCommand,
			name, sizeof(name));
	if (!call.name.empty()) world.out.script_sounds.push_back(std::move(call));
	world.out.hud_chat_lines.push_back(kEmplacedCommandCallLine);
}

// The parent-brain publication: an addeweap child whose anchor userpoint
// rides the parent ROOT hands its gun words to the parent's AI brain turret
// channel — the live/staged yaw (and, for a helicopter parent, the pitch) the
// hull model's turret CTRL reads (vehicle_motor.cpp gun_yaw/gun_pitch from
// kActiveYaw/kActivePitch), so a player-gunned tank hull turret follows the
// slewed word. Gates, in order: the child hangs on a parent (+0x28), the
// parent model has a userpoint table and the child's 1-based userpoint
// index (+0x319) addresses it, that record's subobject is 0, the parent has
// a brain. Then by the parent's AI profile type: GROUND (2) takes the raw
// yaw word into +0x1D8 / +0x1F0; HELO (1) whose def carries EWeap (attrib
// 0x20) clamps BOTH words by the parent's weaponSlots[1] def turret limits
// (slot 1 at parent+0x474, its def +0x494: targetpitchmax +0x13C /
// targetpitchmin +0x140 / targetyawrange +0x144) and writes yaw to
// +0x1D8/+0x1F0 and pitch to +0x1DC/+0x1F4. It runs BEFORE the window leg
// below, on the words the refresh just produced. It belongs to the ewep class
// update, which runs every tick whether or not the gun is occupied, behind
// the gun's own weapon Def: an emptied turret keeps driving the hull turret
// channel with its held words.
// [orig: Entity_UpdatePool1Slot @0x4B8E41..0x4B8E53 (the class update, no
//  age or occupant gate); Entity_UpdateTransformAndTurret Def gate
//  @0x440E8C..0x440EA0]
// That aircraft slot is the one the parent's class init bound from its authored
// primary_weapon, the field an ewep's +0x2B4 slot binds from: its Def is the
// parent's primary_weapon_slot_adm row.
// [orig: Entity_InitInfantryBoneData @0x490160, call @0x49017b;
// WeaponSlot_InitFromEntityDef @0x5466c0, primary_weapon @0x5466d8]
// [orig: Entity_UpdateTransformAndTurret @0x440ca0: parent @0x440cbf,
//  model/table/index gates @0x440f04..0x440f34, entry = table +
//  48*(index-1) @0x440f3a..0x440f40, `cmp [ebx+18h],0` @0x440f50, brain
//  [edi+64h] @0x440f5a, profile type [brain+4]+0x10 @0x440f65..0x440f6b;
//  type 2 @0x440f70..0x440f8a; type 1 `test [def+54h],20h` @0x440fa1,
//  slot-1 def [edi+494h] @0x440fbc, Math_ClampAngleToBounds yaw (+0x144,
//  -0x144) @0x440fea and pitch (+0x13C, +0x140) @0x440ffa, stores
//  @0x441007/@0x44100d (yaw) and @0x44101a/@0x441020 (pitch)]
// The gun's inline slot Def (+0x2D4): the weapon its class init bound from the def's
// primary weapon name at the spawn (VehicleSystem::run_weapon_slot_class_init), null
// for a gun whose name bound none.
// [orig: WeaponSlot_InitFromEntityDef @0x5466C0 (name lookup @0x5466E8,
//  the slot Def @0x546709..0x54670A), from Entity_InitBoneReferences @0x441482]
inline const WeaponTableEntry *emplaced_slot_def(const World &world, const Entity &gun) {
	return world.tables.weapons.by_index(gun.primary_weapon_slot_adm);
}

// The publication's value leg, by the parent brain's profile type: GROUND
// (2) takes the raw yaw word; HELO (1) under an EWeap def takes both words
// clamped by the parent's own weapon window, in the parser's raw BAM (the
// integer multiply and wrap: 180 spans the signed domain, zero locks an
// axis). [orig: type 2 @0x440f70..0x440f8a; type 1 @0x440fa1..0x441020;
//  WeaponDefs_ParseLineCallback @0x543680, integer conversions @0x5443ec /
//  @0x544424 / @0x544441..0x54446e]
struct EmplacedParentGunWords {
	bool yaw = false;
	bool pitch = false;
	int32_t yaw_bam = 0;
	int32_t pitch_bam = 0;
};
inline EmplacedParentGunWords emplaced_parent_gun_words(const World &world,
		const Entity &parent, int32_t profile_type, int16_t yaw_word, int16_t pitch_word) {
	EmplacedParentGunWords out;
	out.yaw_bam = emplaced_word_bam(yaw_word);
	if (profile_type == 2) {
		out.yaw = true;
		return out;
	}
	if (profile_type != 1 || (parent.item_attrib & kItemAttribEweap) == 0) return out;
	const WeaponTableEntry *bound = world.tables.weapons.by_index(parent.primary_weapon_slot_adm);
	if (bound == nullptr) return out;
	const WeaponTableEntry &parent_weapon = *bound;
	out.pitch_bam = emplaced_word_bam(pitch_word);
	const int32_t yaw_range = turret_window_limit_bam(parent_weapon.turret_yaw_range_deg);
	emplaced_clamp_turret_bam(out.yaw_bam, yaw_range, opennova::io::bam_sub(0, yaw_range));
	emplaced_clamp_turret_bam(out.pitch_bam,
			turret_window_limit_bam(parent_weapon.turret_pitch_max_deg),
			opennova::io::bam_sub(0,
					turret_window_limit_bam(parent_weapon.turret_pitch_min_deg)));
	out.yaw = out.pitch = true;
	return out;
}

// The publication's write: the parent's brain (its +0x64; ours the parent's AiEntity, live
// while its owner word is set) takes the two words by its profile type. The ewep class update
// runs it over an addeweap child's gun words and over a person body's +0x322 / +0x324 words
// alike (person_ewep_anchored_attach).
// [orig: Entity_UpdateTransformAndTurret @0x440ca0 - brain [edi+64h] @0x440f5a, the stores
//  @0x440f7a / @0x440f8a, @0x441007 / @0x44100d and @0x44101a / @0x441020]
inline void publish_gun_words_to_brain(World &world, EntityHandle parent_handle,
		int16_t yaw_word, int16_t pitch_word) {
	AiEntity *parent_ai = world.ai.for_handle(parent_handle);
	if (parent_ai == nullptr || parent_ai->brain.f[AiBrain::kOwner] == 0)
		return;
	const Entity *parent = world.registry.get(parent_handle);
	if (parent == nullptr) return;
	const EmplacedParentGunWords words = emplaced_parent_gun_words(world, *parent,
			parent_ai->profile.type, yaw_word, pitch_word);
	if (words.yaw) {
		parent_ai->brain.f[AiBrain::kActiveYaw] = words.yaw_bam;
		parent_ai->brain.f[AiBrain::kStagingBlock + 3] = words.yaw_bam;
	}
	if (words.pitch) {
		parent_ai->brain.f[AiBrain::kActivePitch] = words.pitch_bam;
		parent_ai->brain.f[AiBrain::kStagingBlock + 4] = words.pitch_bam;
	}
}

inline void publish_emplaced_gun_words_to_parent(World &world,
		const Entity &mount) {
	if (!mount.emplaced_update || !mount.emplacement_parent.valid()) return;
	if (emplaced_slot_def(world, mount) == nullptr) return;
	// A child promoted onto an authored userpoint (index > 0) that rides the
	// parent root; an unstamped subobject (-1) keeps the leg off.
	if (mount.emplacement_bone == 0 || mount.emplacement_anchor_subobject != 0)
		return;
	publish_gun_words_to_brain(world, mount.emplacement_parent, mount.emplaced_gun_yaw_word,
			mount.emplaced_gun_pitch_word);
}

// The ewep class update's first gate: a carrier (groundEntity +0x28, the
// emplacement parent here) that is a dead PlayerControl hull. The update then
// hides the child (Flags bit 0) and ends before every later leg: the vehicle
// block, the publication, the matrix, the window leg and the spin tail.
// [orig: Entity_UpdateTransformAndTurret @0x440CBF..0x440CE1 — the carrier
//  @0x440CBF, its def @0x440CCA..0x440CCF, `test byte [def+54h],40h`
//  @0x440CD1, `test byte [carrier+24h],2` @0x440CD7, `or [child+24h],1`
//  @0x440CDD, the jump to the epilogue @0x440CE1]
// A live PlayerControl carrier's block then fills the child's anchor index
// (+0x319) from the carrier def's slot table when it reads 0xFF. No store ever
// leaves 0xFF there: the pools start zeroed and the ewep class init always
// stores the slot's table byte, on every peer, so emplacement_bone (that init's
// value) is the whole port of it. [orig: @0x440CE6..0x440CFD, the same test in
// Entity_GetMountSlotBoneIndex @0x546680; Pool_Clear @0x442060;
// Entity_InitBoneReferences @0x4415E1..0x4415FF]
inline bool emplaced_carrier_is_dead_hull(const Entity *carrier) {
	return carrier != nullptr && carrier->has_item_def &&
			(carrier->item_attrib & kItemAttribPlayerControl) != 0 &&
			((carrier->flags | carrier->engine_flags) & kEntityFlagDead) != 0;
}

// dfx2.exe's class update turns an IsTurret child's gun words against the turn its rebuilt
// attachment matrix took (DFX2-25), so the gun holds its world aim on a turning carrier: the old
// matrix's inverse times the new one, that delta's yaw added to the yaw word and its pitch taken
// from the pitch word, each rounded to the word; the update's window leg then clamps the turned
// words. Only an occupied child posed on a carrier userpoint takes it (the root copy rebuilds no
// matrix), behind the update's dead-hull and slot-Def exits. Ours poses the child ahead of its
// gunner's channel, so the channel runs the retail update's three legs in its order: the slew
// against the frame before this tick's pose (savedLivePose, which the pose stamps first, as the
// update copies +4..+0x18 to +0x80..+0x94 @0x440F33..0x440F6E), the counter-rotation from that
// frame to the posed one, then the window over the posed one. It builds the two matrices from the
// frame angles, which the matrices' euler gave.
// [orig: Entity_UpdateTransformAndTurret @0x440B20 (dfx2.exe) - the dead-hull exit
//  @0x440B44..0x440B61, the slot Def +0x2D4 @0x440D2F, the old matrix copied @0x440D27, the
//  userpoint test @0x440DAE..0x440DDB, Bone_BuildAttachmentMatrix @0x440F2E,
//  Math_FixedPointMatrixToEulerAngles @0x440F74, IsTurret @0x440F87, the occupant +0x170
//  @0x440F94, Matrix_Transpose3x3WithNegateCol3 @0x440FAE, Matrix_Multiply3x4_FixedPoint
//  @0x440FC4, the euler @0x440FD6, the words @0x440FDB..0x440FFE, then the window leg
//  @0x4410EA..0x4411D9]
inline bool emplaced_counter_rotates_frame(const World &world, const Entity &child) {
	if (world.rules.title != gameprofile::GAME_DFX2 || !child.emplacement_pose_metadata_resolved)
		return false;
	// The ewep class update's (the physics-table row @0x82ABE0) alone.
	if (!child.emplaced_update) return false;
	const Entity *parent = world.registry.get(child.emplacement_parent);
	if (parent == nullptr || emplaced_carrier_is_dead_hull(parent)) return false;
	if (emplaced_slot_def(world, child) == nullptr) return false;
	return child.emplacement_bone != 0 && parent->has_graphic_model &&
			child.emplacement_bone <= parent->graphic_model_userpoints &&
			(child.item_attrib2 & opennova::def::DEF_ITEM_ATTRIB2_ISTURRET) != 0;
}

// The host's gate: the frame's and the child's own occupant (+0x170).
inline bool emplaced_counter_rotates(const World &world, const Entity &child) {
	return child.primary_occupant.valid() && emplaced_counter_rotates_frame(world, child);
}

inline void counter_rotate_emplaced_gun_channel(EmplacedGunChannel &channel,
		const int32_t before[3], const int32_t after[3]) {
	const int32_t origin[3] = {0, 0, 0};
	const CollisionMatrix old_m = collision_matrix_from_euler(before[0], before[1], before[2],
			origin);
	const CollisionMatrix new_m = collision_matrix_from_euler(after[0], after[1], after[2], origin);
	CollisionMatrix inverse;
	old_m.invert_into(inverse);
	// One rounding bias per dot product, then >> 22 [orig: Matrix_Multiply3x4_FixedPoint
	//  @0x613940; dfx2.exe's @0x612890].
	CollisionMatrix delta;
	for (int r = 0; r < 3; ++r)
		for (int c = 0; c < 3; ++c) {
			uint64_t sum = 0x200000u;
			for (int k = 0; k < 3; ++k)
				sum += static_cast<uint64_t>(
						static_cast<int64_t>(inverse.m[4 * r + k]) * new_m.m[4 * k + c]);
			delta.m[4 * r + c] = static_cast<int32_t>(static_cast<uint32_t>(sum >> 22));
		}
	int32_t euler[3];
	collision_matrix_to_euler(delta, euler);
	const auto word = [](int32_t angle) {
		return static_cast<uint16_t>(opennova::io::bam_add(angle, 0x8000) >> 16);
	};
	channel.yaw = static_cast<int16_t>(static_cast<uint16_t>(channel.yaw) + word(euler[0]));
	channel.pitch = static_cast<int16_t>(static_cast<uint16_t>(channel.pitch) - word(euler[1]));
}

// One occupied tick of the gun channel in retail order: the ai-fn refresh,
// then the class update's parent-brain publication and its window clamp +
// occupant write-back. The class update's two legs stop at its dead-hull
// exit; the ai-fn refresh is a call of its own and still runs.
// [orig: Entity_UpdatePool1Slot @0x4b8dd0 — ai-fn @0x4b8e3c, class update
//  @0x4b8e53 (the exit @0x440CBF..0x440CE1, then the publication
//  @0x440f04..0x441020 before the window leg @0x4411d1..0x4412b3)]
inline void tick_emplaced_weapon_channel(World &world, Entity &mount,
		const Entity &occupant, AiEntity &gunner) {
	tick_emplaced_gun_words(world, mount, occupant, gunner);
	if (mount.emplaced_update &&
			emplaced_carrier_is_dead_hull(world.registry.get(mount.emplacement_parent)))
		return;
	publish_emplaced_gun_words_to_parent(world, mount);
	// dfx2.exe's counter-rotation, after the publication and ahead of the window leg (DFX2-25):
	// the brain takes the slewed words, the window the turned ones.
	// [orig: Entity_UpdateTransformAndTurret @0x440B20 (dfx2.exe) - the publication
	//  @0x440E01..0x440EB1, the counter-rotation @0x440FF1 / @0x440FFE, the window @0x4410EA]
	if (mount.saved_live_valid && emplaced_counter_rotates(world, mount)) {
		const int32_t before[3] = {mount.saved_live_yaw, mount.saved_live_pitch,
				mount.saved_live_roll};
		const int32_t after[3] = {emplaced_gun_frame_heading(mount), emplaced_gun_frame_pitch(mount),
				emplaced_gun_frame_roll(mount)};
		EmplacedGunChannel channel{mount.emplaced_gun_yaw_word, mount.emplaced_gun_pitch_word};
		counter_rotate_emplaced_gun_channel(channel, before, after);
		mount.emplaced_gun_yaw_word = channel.yaw;
		mount.emplaced_gun_pitch_word = channel.pitch;
	}
	// The ewep update's window leg and the tank-command call behind it sit behind its slot Def
	// (+0x2D4): a gun with none ends its update ahead of both, with a ground or without one.
	// [orig: Entity_UpdateTransformAndTurret @0x440CA0 - the slot Def read @0x440E8E, `jz` with a
	//  ground @0x440EA0, the gate without one @0x4411C9..0x4411CB]
	if (mount.emplaced_update && emplaced_slot_def(world, mount) == nullptr) return;
	clamp_emplaced_gun_words_to_window(world, mount, gunner);
	// The window leg's tail: the tank-command call over the occupant's packed MoveOrder byte.
	if (mount.emplaced_update)
		emplaced_command_call(world, mount.ground_target, mount.emplaced_command_stamp,
				occupant.handle == world.cached.local_player, occupant.net_move_input,
				occupant.anim_slot);
}

// The ewep class update's every-tick legs outside the occupied channel, in
// retail order. A dead PlayerControl carrier hides the child and ends the
// update. With a live carrier the update needs the inline slot Def; on a
// vehicle carrier (def type 1) it then clears the child's hide and kill bits
// (Flags & ~7, a clear on both words), copies the carrier's 16-bit Health
// word and its blink quad. The vehicle block ends by calling the carrier's
// render-class CTRL writer (def+0x144), which writes only the global CTRL
// bus; the port composes that bus whenever it poses the carrier's
// attachments (compose_vehicle_pose_controls), as the matrix build's own
// def+0x144 call does. Then
// the parent-brain publication, and the child's matrix rebuilt from its
// carrier, marked by the matrix bit (Flags 0x20000, homed on engine_flags).
// The spin tail runs last, behind the slot Def, with or without a carrier.
// An occupied gun publishes again from tick_emplaced_weapon_channel once its
// producer has refreshed the words, so the brain ends the tick on the fresh
// words.
// [orig: Entity_UpdatePool1Slot @0x4B8E41..0x4B8E53 -> Entity_UpdateTransformAndTurret
//  @0x440ca0: the exit @0x440CBF..0x440CE1; the carrier and slot Def gates
//  @0x440E8C..0x440EA0; def type 1 @0x440EA6..0x440EB1, `and [child+24h],
//  0FFFFFFF8h` @0x440EB3, the Health word @0x440EB7..0x440EBE, the quad
//  @0x440EC5..0x440EEF, the def+0x144 call @0x440EF5..0x440EFF; the
//  publication @0x440f04..0x441020; the matrix copy @0x4411BC and the OR
//  @0x4411C2; the spin tail @0x44139D..0x441447]
inline void tick_emplaced_weapon_class_update(World &world, Entity &mount) {
	if (!mount.emplaced_update) return;
	const Entity *carrier = world.registry.get(mount.emplacement_parent);
	if (emplaced_carrier_is_dead_hull(carrier)) {
		mount.flags |= kEntityFlagCarried;
		return;
	}
	if (carrier != nullptr) {
		if (emplaced_slot_def(world, mount) == nullptr) return;
		if (carrier->has_item_def && carrier->item_type == 1) {
			constexpr uint32_t kHideAndKillBits =
					kEntityFlagCarried | kEntityFlagDead | kEntityFlagHusk;
			const bool was_husk = ((mount.flags | mount.engine_flags) & kEntityFlagHusk) != 0;
			mount.flags &= ~kHideAndKillBits;
			mount.engine_flags &= ~kHideAndKillBits;
			// The port mirrors the Dead bit in `alive`, and the renderer's husk
			// read of bit 4 in a presenter swap, so the clear revives both.
			mount.alive = true;
			if (was_husk) {
				HuskSwapEvent intact;
				intact.net_id = mount.net_id;
				intact.wire_handle = mount.handle.packed;
				intact.bms_id = mount.bms_id;
				intact.spawn_origin = mount.spawn_origin;
				intact.item_id = mount.item_id;
				intact.pos = mount.position;
				intact.restore_intact = true;
				world.out.destruction.husk_swaps.push_back(intact);
			}
			const int16_t health = static_cast<int16_t>(carrier->health);
			mount.health = health;
			if (AiEntity *brain = world.ai.for_handle(mount.handle)) brain->health = health;
			for (int i = 0; i < 4; ++i) mount.blink_hits[i] = carrier->blink_hits[i];
		}
		publish_emplaced_gun_words_to_parent(world, mount);
		mount.engine_flags |= kEntityFlagMatrixBuilt;
	} else if (mount.emplacement_parent.valid()) {
		// The carrier's row was destroyed under the child, which only a child
		// outside the carrier's EWeap refNum group survives (the destroy takes
		// the rest, EntityCommands::remove_ssn). Retail keeps the child's
		// pointer to that row, which Entity_Destroy zeroed: no def, no Flags,
		// no model, so no hide and no vehicle block, and the root copy takes
		// the zeroed pose (the world origin, zero angles) every update. A later
		// occupant of the row becomes the child's carrier.
		// [orig: Entity_Destroy memset(entity, 0, 0x2B4) @0x43EA70, Flags +0x24,
		//  def +0x20, model +0x30 all inside it, its refNum walk @0x43E9CD ->
		//  EntityReference_DestroyEWeapGroup @0x546F30; Entity_UpdateTransformAndTurret
		//  def test @0x440EA6..0x440EAB, root copy @0x4410EA..0x4411BC]
		if (emplaced_slot_def(world, mount) == nullptr) return;
		mount.position = Vec3{};
		mount.heading = 0; // the memset Yaw word
		mount.pitch_bam = 0;
		mount.roll_bam = 0;
		if (mount.veh.yaw_seeded) {
			mount.veh.yaw_bam = 0;
			mount.veh.air_pitch_bam = 0;
			mount.veh.air_roll_bam = 0;
		}
		mount.engine_flags |= kEntityFlagMatrixBuilt;
	}
	tick_emplaced_weapon_animation(world, mount);
}

// The writer's three words, verbatim: the words are what the channel left
// (slewed, tethered, window-pinned) or held since the last gunner left,
// exactly what the model's EWEAP_GUNYAW/GUNPITCH and WEAP_SPIN registers read.
// [orig: HUD_CacheWeaponSlotInfo @0x440934..0x440948 (+0x322/+0x324 high
//  words), @0x440955 (unsigned spin word +0x320)]
inline EmplacedWeaponControls emplaced_weapon_controls_of(const Entity &mount) {
	EmplacedWeaponControls out;
	out.valid = true;
	out.gun_yaw = static_cast<uint16_t>(mount.emplaced_gun_yaw_word);
	out.gun_pitch = static_cast<uint16_t>(mount.emplaced_gun_pitch_word);
	out.spin = mount.emplaced_spin_phase;
	return out;
}

// The words as the carrier's frames read them: an 'ewep' render class
// publishes them whether or not anyone is seated; any other class only
// through a UseGun rider's seat call.
// [orig: the 'ewep' render-class row @0x82CFA0 -> HUD_CacheWeaponSlotInfo
//  @0x440930, no occupant test; the seat call @0x546517..0x546518; the
//  0x440ca0 leg publishes the same words to the parent brain
//  @0x440f70..0x441020]
inline bool emplaced_weapon_controls_for(
		const World &world,
		const Entity &mount,
		EmplacedWeaponControls &out) {
	out = EmplacedWeaponControls{};
	if (!mount.emplaced_ctrl_publisher && !usegun_rider_writes_carrier(world, mount))
		return false;
	out = emplaced_weapon_controls_of(mount);
	return true;
}

} // namespace opennova::world
