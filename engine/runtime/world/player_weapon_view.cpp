// The local player's equipped-weapon FSM view (player_weapon.h
// LocalPlayerWeaponView): one snapshot per tick from the live world and the
// local weapon aggregate. Pushed down from the Godot binding (ADR 0040 ladder
// E0); every field witness rides the fill.
#include <runtime/world/player_weapon.h>

#include <base/io/bam.h>
#include <runtime/world/ai.h>
#include <runtime/world/entity.h>
#include <runtime/world/infantry.h>
#include <runtime/world/mount_controls.h>
#include <runtime/world/pose_inputs.h>
#include <runtime/world/round_ring.h>
#include <runtime/world/round_sim.h>
#include <runtime/world/weapon_fsm.h>
#include <runtime/world/weapon_inventory.h>
#include <runtime/world/weapon_table.h>
#include <runtime/world/world.h>

#include <algorithm>
#include <cstddef>
#include <string>

namespace opennova::world {

LocalPlayerWeaponView local_player_weapon_view(const World &world, const LocalPlayerWeapon &w,
		const WeaponInventory &inventory) {
	LocalPlayerWeaponView v;
	v.active = w.active;
	if (!w.active) return v;
	const WeaponSlotState &active_slot = *active_local_weapon_slot(world, w);
	v.current_action = active_slot.current;
	v.next_action = active_slot.next;
	v.phase = static_cast<int>(active_slot.phase);
	v.switch_deferred_action = w.switch_deferred_action;
	v.switch_in_flight = w.switch_in_flight;
	v.pending_combo = inventory.pending_combo;
	v.anim_key = w.anim_key;
	v.anim_variant = w.anim_variant;
	// The FP clip channel position: gated per-tick advances since the play, not
	// wall-clock age — the presenter poses the parts at advance * tick_dt and
	// nothing free-runs the playhead [orig: the counter-gated
	// AnimChannel_AdvanceDispatch @ 0x40b960 callers, net-re §5.40].
	v.anim_advance_ticks = static_cast<int32_t>(w.anim_key.empty() ? 0u : w.anim_advance_ticks);
	v.play_serial = static_cast<int32_t>(w.play_serial);
	// The last-started action's audio/effect legs remain useful snapshot diagnostics;
	// ordered delivery uses drain_local_player_weapon_events().
	// [orig: ActionSlot_ExecuteActionWithEffect
	// @ 0x541860 -> ActionSlot_SpawnEffect @ 0x401f20].
	v.action_serial = static_cast<int32_t>(w.action_serial);
	if (w.action_started >= 0 && w.action_started < weapon_action::kCount) {
		const WeaponFsmAction &act = w.def.actions[w.action_started];
		v.action_started = w.action_started;
		v.action_soundset = act.soundset;
		v.action_particle = act.particle;
		v.action_particle_userpoint = act.particle_userpoint;
	} else {
		v.action_started = -1;
		v.action_soundset = std::string();
		v.action_particle = std::string();
		v.action_particle_userpoint = std::string();
	}
	// The latest END-leg snapshot diagnostic: fire rows carry the per-shot gunshot
	// here (GS_*), reload rows the completion sound. Ordered delivery uses the batch.
	// [orig: ActionSlot_FinishActivePhase @ 0x53f7b0 -> the end shim @ 0x401100 plays
	//  ActionDef+12 at the owner entity].
	v.action_end_serial = static_cast<int32_t>(w.action_end_serial);
	if (w.action_finished >= 0 && w.action_finished < weapon_action::kCount) {
		v.action_end_soundset = w.def.actions[w.action_finished].soundsetend;
	} else {
		v.action_end_soundset = std::string();
	}
	// The PowerThrow windup for the HUD charge bar [orig: HUD_DrawPowerThrowChargeBar
	// @ 0x599830 (ex kong "HUD_DrawWeaponReloadBar" misnomer — it only draws the
	// windup): gates = def+8 sign bit, g_fireChargeStartTick != 0, ammo available;
	// the drawer derives the fill from held ticks].
	const bool windup_active = (w.def.flags & weapon_flag::kPowerThrow) != 0 &&
			w.power_throw_start_tick != 0 && (active_slot.clip > 0 || w.def.clip_capacity < 0);
	v.windup_active = windup_active;
	v.windup_held_ticks = windup_active
			? static_cast<int32_t>(world.logic_tick - w.power_throw_start_tick)
			: 0;
	v.fired_serial = static_cast<int32_t>(w.fired_serial);
	// The per-slot tracer cadence byte (retail weaponSlot+0x80): each weapon
	// keeps its own phase across switches — the F3 weapon row shows it.
	v.tracer_counter = static_cast<int32_t>(active_slot.tracer_shot_counter);
	v.dry_serial = static_cast<int32_t>(w.dry_serial);
	v.reload_serial = static_cast<int32_t>(w.reload_serial);
	v.reload_applied_serial = static_cast<int32_t>(w.reload_applied_serial);
	v.reload_received_serial = static_cast<int32_t>(w.reload_received_serial);
	v.reload_received_entity = static_cast<int32_t>(w.reload_received_entity);
	v.reload_received_param = static_cast<int32_t>(w.reload_received_param);
	v.unscope_serial = static_cast<int32_t>(w.unscope_serial);
	v.rescope_serial = static_cast<int32_t>(w.rescope_serial);
	v.clip = active_slot.clip;
	v.reserve = active_slot.reserve;
	v.kick = static_cast<int>(active_slot.kick);
	// Crosshair spread remains in retail's exact integer domains through the
	// presentation edge: choose the stance triplet, then add the two arithmetic
	// shifts. Category order is prone/crouch/stand; airborne or submerged forces
	// stand, and a parent attachment finally forces crouch. The +3 triplet is the
	// shared Player_CanFireWeapon verdict stamped before the body tick.
	// [orig: HUD_DrawCrosshair @0x592b07..0x592b87]
	{
		int32_t recoil_pitch = 0;
		int32_t weapon_weight_spread = 0;
		int category = 2;
		bool aimed_shot_available = false;
		const Entity *local = nullptr;
		const AiEntity *body = nullptr;
		if (world.cached.local_player.valid()) {
			local = world.registry.get(world.cached.local_player);
			body = world.ai.for_handle(world.cached.local_player);
		}
		if (body != nullptr) {
			recoil_pitch = body->inf.recoil_pitch;
			weapon_weight_spread = body->inf.weapon_weight_spread;
			aimed_shot_available = body->inf.aimed_shot_available;
			// The shared witnessed eye-projection classifier
			// (world/round_sim.h entity_eye_below_water)
			// [orig: HUD_DrawCrosshair @0x592b35]
			const bool below_water = entity_eye_below_water(
					world, body->pos[2], local != nullptr ? local->eye_offset_z : 0);
			if (body->inf.stance == InfantryState::Stance::kProne)
				category = 0;
			else if (body->inf.stance == InfantryState::Stance::kCrouch)
				category = 1;
			if (body->inf.airborne || below_water ||
					(local != nullptr &&
					 ((local->flags | local->engine_flags) &
							(kEntityFlagInAir | kEntityFlagDrowning)) != 0))
				category = 2;
		}
		if (local != nullptr && local->mounted) category = 1;
		const int row = category + (aimed_shot_available ? 3 : 0);
		const WeaponTableEntry *weapon =
				local != nullptr ? world.tables.weapons.by_index(local->equipped_adm_index) : nullptr;
		const int32_t authored_error = weapon != nullptr ? weapon->error_fp16[row] : 0;
		const int32_t live_error = io::bam_add(authored_error,
				io::bam_add(io::bam_sar(recoil_pitch, 7), io::bam_sar(weapon_weight_spread, 7)));
		v.recoil_pitch_bam = recoil_pitch;
		v.weapon_weight_spread_bam = weapon_weight_spread;
		v.aimed_shot_available = aimed_shot_available;
		v.hud_spread_row = row;
		v.hud_spread_fp16 = live_error;
	}
	// Weapon heat has two retail consumers with different clamps: HUD info stops
	// at 0xFFFF, while the first-person model publishes HEAT_GLOW on the signed
	// CTRL bus through the exact 0x10000 endpoint.
	// [orig: HUD_BuildEntityInfo @ 0x4B852E..0x4B854D;
	//  Player_RenderFirstPersonViewModel @ 0x4DEEC2..0x4DEEF5]
	{
		const int32_t heat = weapon_slot_accumulated_heat(
				w.def, active_slot, static_cast<int32_t>(world.logic_tick));
		v.heat = heat > weapon_heat::kFull ? weapon_heat::kFull : heat;
		v.heat_glow = std::clamp(heat, 0, 0x10000);
	}
	v.borrowed_usegun_slot = w.usegun_slot_active;
	v.usegun_mount_handle = w.usegun_slot_active ? w.usegun_mount.packed : EntityHandle::kInvalid;
	// The def half of the pump's FP bit: the weapon's gfx1 model loaded
	// (first_person_model_adm is stamped only once the GUN model loaded) and
	// its anim object built from `animadm` (the installed anim_map).
	// [orig: WeaponAction_ProcessFrame FP bit @0x540E8C..0x540ECF: Def+0x16C
	//  @0x540EA5, Def+0x174 @0x540EC3]
	v.first_person_action_model = w.first_person_model_adm != 0xFF && !w.anim_map.empty();
	v.emplaced_controls_valid = false;
	v.emplaced_gun_yaw = 0;
	v.emplaced_gun_pitch = 0;
	if (world.cached.local_player.valid()) {
		const Entity *local = world.registry.get(world.cached.local_player);
		const Entity *mount = local != nullptr && local->mounted &&
						local->mount_type == SeatType::Gunner
				? world.registry.get(local->mount_target)
				: nullptr;
		EmplacedWeaponControls emplaced;
		if (mount != nullptr && mount->primary_weapon_owner == local->handle &&
				emplaced_weapon_controls_for(world, *mount, emplaced)) {
			v.emplaced_controls_valid = true;
			v.emplaced_gun_yaw = static_cast<int>(emplaced.gun_yaw);
			v.emplaced_gun_pitch = static_cast<int>(emplaced.gun_pitch);
			v.emplaced_spin_phase = emplaced.spin;
		}
	}
	// Read-only diagnostics for the local FIRE -> RoundData_AddRound seam. The last
	// row lets parity tests pin the observed tag-2 mode byte without exposing mutable
	// ring state. [orig: ((MountSlot.clip & 3) << 4) | 2 sampled before consume
	// @ WeaponAction_Fire 0x542c11 / 0x542c75].
	v.round_ring_count = world.out.rounds.count;
	if (world.out.rounds.count > 0) {
		const int last = world.out.rounds.cursor == 0 ? RoundRing::kCapacity - 1
													  : world.out.rounds.cursor - 1;
		const RoundEvent &round = world.out.rounds.records[static_cast<std::size_t>(last)];
		v.last_round_flags = round.mode_flags;
		v.last_round_subtype = round.subtype;
		v.last_round_slot_byte = round.slot_byte;
		v.last_round_seq = round.shot_seq;
	}
	// The 3P body's weapon channel (the entity's secondary AnimMap channel): the clip key
	// + its own playhead for the shell's mask-bone override. The key remains populated
	// when the state id matches the primary because the two playheads are independent.
	// Empty means the override gate is off (weapon in hands + allowed mount class +
	// primary state flag 0x40).
	// [orig: gate @ 0x4b14a7; producer @ 0x4b5dad; world-wac-ai-re.md §14.8].
	v.body_anim_key = std::string();
	v.body_anim_phase = 0;
	// The secondary channel's cross-fade + served variant ride beside the key: the
	// outgoing clip at its own playhead, the ramping weight, and the ring entry each
	// play latched [orig: the AnimMap_UpdateEntity @0x40b5f0 re-init + the +68
	// latch, see docs/world/world-wac-ai-re.md §14.8.7].
	v.body_anim_prev_key = std::string();
	v.body_anim_prev_phase = 0;
	v.body_anim_blend_weight = 1.0f;
	v.body_anim_variant = 0;
	v.body_anim_prev_variant = 0;
	if (world.cached.local_player.valid()) {
		const AiEntity *p = world.ai.for_handle(world.cached.local_player);
		const Entity *entity = world.registry.get(world.cached.local_player);
		const bool blocked_mount = entity != nullptr && mount_blocks_weapon_channel(*entity);
		if (p && entity && infantry_weapon_channel_visible(p->inf, w.active, blocked_mount)) {
			v.body_anim_key = infantry_anim_key(p->inf.weapon_clip_state());
			v.body_anim_phase = p->inf.wpn_clip_phase;
			v.body_anim_variant = p->inf.wpn_variant;
			if (p->inf.weapon_blend_active()) {
				v.body_anim_prev_key = infantry_anim_key(p->inf.wpn_prev);
				v.body_anim_prev_phase = p->inf.wpn_prev_clip_phase;
				v.body_anim_blend_weight = p->inf.wpn_blend_weight;
				v.body_anim_prev_variant = p->inf.wpn_prev_variant;
			}
		}
	}
	return v;
}

} // namespace opennova::world
