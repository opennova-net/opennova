class_name PlayerWeaponEvent
extends RefCounted

## One logic tick's ordered presentation outputs from the local weapon FSM. The C++
## binding encodes these records as Dictionaries at the transport seam; GameWorld
## decodes them immediately so owners consume a typed contract (ADR 0017). Within a
## record the observable order is clip start, ACTION begin leg, ACTION end leg. The
## production-tick position keeps delayed 3D audio spatially faithful during catch-up.
var age_ticks := 0
var world_position := Vector3.ZERO
var anim_key := ""
var anim_variant := 0
var action_started := -1
var action_soundset := ""
var action_particle := ""
var action_particle_userpoint := ""
## Action-routing state at the production tick, after the retail view promoter.
## Catch-up frames must not reuse one final view snapshot for every queued event.
var scope_settled := false
var third_person := false
var vehicle_attack_context := false
var action_finished := -1
var action_end_soundset := ""
## The recoil-row DIRECT effect leg at the recoil arbiter tick. Usually casing
## eject / bolt smoke, but data may author a muzzle effect here (REVX02 M4).
## Retail spawns it with no scope gate and no live-handle suppression
## [orig: WeaponAction_Recoil @ 0x542dd0 gate @ 0x542efa, spawn @ 0x542f64].
var action_effect := -1
var effect_particle := ""
var effect_particle_userpoint := ""
## A committed weapon switch: the newly equipped weapon.def name — the shell
## reinstalls the FP viewmodel/FSM for it [orig: the mount's model re-resolve;
## equippedAdmIndex stamp @ 0x4dd727]. Empty = no switch this tick.
var switch_to_weapon := ""
## The committed target has no weapon definition (for example, an originally
## unarmed player detaching from a UseGun). This is distinct from no switch event.
var clear_weapon := false
## UseGun switches borrow the parent's persistent MountSlot and restore the saved
## personal slot. Rebuilding presentation must not initialize either slot again.
## [orig: Entity_AttachToUseGunSlot @0x546c25; detach restore @0x43565f]
var preserve_slot_state := false
## The switch-walk wrap-around refusal — the deny sound seam
## [orig: PlaySoundOnDedicatedServer(dword_24E08C4) @ 0x4e0354].
var switch_denied := false


static func from_event_dict(d: Dictionary) -> PlayerWeaponEvent:
	var out := PlayerWeaponEvent.new()
	out.age_ticks = int(d.get("age_ticks", 0))
	out.world_position = Vector3(d.get("world_position", Vector3.ZERO))
	out.anim_key = String(d.get("anim_key", ""))
	out.anim_variant = int(d.get("anim_variant", 0))
	out.action_started = int(d.get("action_started", -1))
	out.action_soundset = String(d.get("action_soundset", ""))
	out.action_particle = String(d.get("action_particle", ""))
	out.action_particle_userpoint = String(d.get("action_particle_userpoint", ""))
	out.scope_settled = bool(d.get("scope_settled", false))
	out.third_person = bool(d.get("third_person", false))
	out.vehicle_attack_context = bool(d.get("vehicle_attack_context", false))
	out.action_finished = int(d.get("action_finished", -1))
	out.action_end_soundset = String(d.get("action_end_soundset", ""))
	out.action_effect = int(d.get("action_effect", -1))
	out.effect_particle = String(d.get("effect_particle", ""))
	out.effect_particle_userpoint = String(d.get("effect_particle_userpoint", ""))
	out.switch_to_weapon = String(d.get("switch_to_weapon", ""))
	out.clear_weapon = bool(d.get("clear_weapon", false))
	out.preserve_slot_state = bool(d.get("preserve_slot_state", false))
	out.switch_denied = bool(d.get("switch_denied", false))
	return out
