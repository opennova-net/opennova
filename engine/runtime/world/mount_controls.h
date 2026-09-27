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

#include <formats/def/def.h>

#include <base/io/bam.h>

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
inline bool usegun_rider_writes_carrier(const World &world, const Entity &carrier) {
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
			: spawn_angle_bam(90 - mount.yaw);
}

inline int32_t emplaced_gun_frame_pitch(const Entity &mount) {
	return mount.veh.yaw_seeded ? mount.veh.air_pitch_bam
			: spawn_angle_bam(mount.pitch);
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
		EmplacedGunnerLook &gunner, int32_t gun_yaw, int32_t gun_pitch, bool is_turret) {
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
	yaw_step = emplaced_clamp_symmetric(yaw_step, kEmplacedTurretSlewPerTick);
	pitch_step = emplaced_clamp_symmetric(pitch_step, kEmplacedTurretSlewPerTick);
	channel.yaw = emplaced_bam_word(bam_add(yaw_step, prev_yaw));
	channel.pitch = emplaced_bam_word(
			bam_add(bam_sub(pitch_step, gunner.recoil_pitch), prev_pitch));
}

inline void tick_emplaced_gun_words(Entity &mount, const Entity &occupant, AiEntity &gunner) {
 EmplacedGunChannel channel{mount.emplaced_gun_yaw_word, mount.emplaced_gun_pitch_word};
 EmplacedGunnerLook look{gunner.heading, gunner.pitch, gunner.inf.recoil_pitch,
  gunner.inf.is_local_player, ((occupant.flags | occupant.engine_flags) & kEntityFlagPlayer) != 0};
 advance_emplaced_gun_channel(channel, look, emplaced_gun_frame_heading(mount),
  emplaced_gun_frame_pitch(mount), (mount.item_attrib2 & opennova::def::DEF_ITEM_ATTRIB2_ISTURRET) != 0);
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
// That aircraft slot is initialized from the parent's authored primary_weapon,
// the same field used for an ewep's +0x2B4 slot. Resolve that existing table row.
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
// The gun's inline slot Def. Retail binds it at spawn from the def's
// primary weapon name; the port binds the slot on its first use, so an
// unbound gun resolves the same name.
// [orig: WeaponSlot_InitFromEntityDef @0x5466C0 (name lookup @0x5466E8,
//  the slot Def @0x546709..0x54670A)]
inline const WeaponTableEntry *emplaced_slot_def(const World &world, const Entity &gun) {
	if (gun.primary_weapon_slot_adm != kAdmSlotNone)
		return world.tables.weapons.by_index(gun.primary_weapon_slot_adm);
	const int index = world.tables.weapons.index_of(gun.primary_weapon.c_str());
	return index >= 0 && index <= 0xFF
			? world.tables.weapons.by_index(static_cast<uint8_t>(index))
			: nullptr;
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
	const int weapon_index = world.tables.weapons.index_of(parent.primary_weapon.c_str());
	if (weapon_index < 0) return out;
	const WeaponTableEntry &parent_weapon = world.tables.weapons.entries[weapon_index];
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

inline void publish_emplaced_gun_words_to_parent(World &world,
		const Entity &mount) {
	if (!mount.emplaced_update || !mount.emplacement_parent.valid()) return;
	if (emplaced_slot_def(world, mount) == nullptr) return;
	// A child promoted onto an authored userpoint (index > 0) that rides the
	// parent root; an unstamped subobject (-1) keeps the leg off.
	if (mount.emplacement_bone == 0 || mount.emplacement_anchor_subobject != 0)
		return;
	AiEntity *parent_ai = world.ai.for_handle(mount.emplacement_parent);
	if (parent_ai == nullptr || parent_ai->brain.f[AiBrain::kOwner] == 0)
		return;
	const Entity *parent = world.registry.get(mount.emplacement_parent);
	if (parent == nullptr) return;
	const EmplacedParentGunWords words = emplaced_parent_gun_words(world, *parent,
			parent_ai->profile.type, mount.emplaced_gun_yaw_word, mount.emplaced_gun_pitch_word);
	if (words.yaw) {
		parent_ai->brain.f[AiBrain::kActiveYaw] = words.yaw_bam;
		parent_ai->brain.f[AiBrain::kStagingBlock + 3] = words.yaw_bam;
	}
	if (words.pitch) {
		parent_ai->brain.f[AiBrain::kActivePitch] = words.pitch_bam;
		parent_ai->brain.f[AiBrain::kStagingBlock + 4] = words.pitch_bam;
	}
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

// One occupied tick of the gun channel in retail order: the ai-fn refresh,
// then the class update's parent-brain publication and its window clamp +
// occupant write-back. The class update's two legs stop at its dead-hull
// exit; the ai-fn refresh is a call of its own and still runs.
// [orig: Entity_UpdatePool1Slot @0x4b8dd0 — ai-fn @0x4b8e3c, class update
//  @0x4b8e53 (the exit @0x440CBF..0x440CE1, then the publication
//  @0x440f04..0x441020 before the window leg @0x4411d1..0x4412b3)]
inline void tick_emplaced_weapon_channel(World &world, Entity &mount,
		const Entity &occupant, AiEntity &gunner) {
	tick_emplaced_gun_words(mount, occupant, gunner);
	if (mount.emplaced_update &&
			emplaced_carrier_is_dead_hull(world.registry.get(mount.emplacement_parent)))
		return;
	publish_emplaced_gun_words_to_parent(world, mount);
	clamp_emplaced_gun_words_to_window(world, mount, gunner);
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
		mount.yaw = 90; // engine heading 0
		mount.pitch = 0;
		mount.roll = 0;
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
