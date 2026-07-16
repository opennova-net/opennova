class_name PlayerWeaponView
extends RefCounted

## The local player's equipped-weapon FSM view — the typed record behind
## `NovaSimulation.get_local_player_weapon_state()`'s transport Dictionary (ADR 0017:
## the record is the contract, the dict is its C++-binding encoding). Serials are
## monotonic diagnostics and rebuild cursors; lossless clip/begin/end delivery uses
## GameWorld's ordered PlayerWeaponEvent drain because several 62.5 Hz logic ticks can
## run per render frame. `fired`/`dry`/`reload`/`unscope`/`rescope` mirror the handler
## counters. [orig: WeaponAction_ProcessFrame @0x540e60 + the wpn_std_*
## handlers; docs/net/novaworld-net-re.md §5.62]

var active := false
var current_action := 0    # world::weapon_action id (0 idle .. 11 overheated)
var anim_key := ""         # the .adm clip key of the last play event
# The served VARIANT of that key — multi-clip .adm rows rotate round-robin and the
# sim's ring latches which variant this play consumed; every viewmodel part plays
# the same latched index. [orig: AnimMap_PlayAnimBySlot @0x40bda0 latches the served
# ring entry at animState+68 while the head advances]
var anim_variant := 0
var anim_age_ticks := 0    # fixed ticks since the last FP clip play
var play_serial := 0
# Snapshot diagnostics for the last action begin: `action_serial` bumps when an
# action's ACTIVE phase begins and the latest ACTION row rides alongside. Presentation
# consumes PlayerWeaponEvent records instead; these fields are not a delivery queue.
# [orig: ActionSlot_ExecuteActionWithEffect @0x541860 -> ActionSlot_SpawnEffect @0x401f20]
var action_serial := 0
var action_started := -1   # the started action's slot id (weapon_action::*; -1 = none)
var action_soundset := ""
var action_particle := ""
var action_particle_userpoint := ""
# Snapshot diagnostics for the last action end: `action_end_serial` bumps when an
# ACTIVE phase finishes and the latest row's soundsetend rides alongside — fire rows
# carry the per-shot gunshot here (GS_*), reload rows the completion sound.
# [orig: ActionSlot_FinishActivePhase @0x53f7b0 -> the end shim @0x401100 plays
#  ActionDef+12, gated on the phase byte being 2 (ACTIVE)]
var action_end_serial := 0
var action_end_soundset := ""
var fired_serial := 0
var dry_serial := 0
var reload_serial := 0
var unscope_serial := 0
var rescope_serial := 0
var clip := 0              # rounds in the magazine
var reserve := 0           # carried pool, rounds
var kick := 0              # recoil kick intensity 0..20 [orig: MountSlot+0x5B]
# The 3P body's upper-body weapon channel (the entity's SECONDARY AnimMap channel):
# the body .adm clip key (e.g. "anim_reload") posed at its OWN playhead on the mask
# bones. Equal primary/secondary state ids still carry a key because their playheads
# are independent; empty means the override gate is off.
# [orig: producer @0x4b5dad + gate @0x4b14a7; docs/world/world-wac-ai-re.md §14.8]
var body_anim_key := ""
var body_anim_phase := 0   # half-frame ticks, the play_body_clip_at convention


## Decode one weapon-state dict; null when the FSM is inactive (no weapon installed).
static func from_state_dict(d: Dictionary) -> PlayerWeaponView:
	if d.is_empty() or not bool(d.get("active", false)):
		return null
	var out := PlayerWeaponView.new()
	out.active = true
	out.current_action = int(d.get("current", 0))
	out.anim_key = String(d.get("anim_key", ""))
	out.anim_variant = int(d.get("anim_variant", 0))
	out.anim_age_ticks = int(d.get("anim_age_ticks", 0))
	out.play_serial = int(d.get("play_serial", 0))
	out.action_serial = int(d.get("action_serial", 0))
	out.action_started = int(d.get("action_started", -1))
	out.action_soundset = String(d.get("action_soundset", ""))
	out.action_particle = String(d.get("action_particle", ""))
	out.action_particle_userpoint = String(d.get("action_particle_userpoint", ""))
	out.action_end_serial = int(d.get("action_end_serial", 0))
	out.action_end_soundset = String(d.get("action_end_soundset", ""))
	out.fired_serial = int(d.get("fired_serial", 0))
	out.dry_serial = int(d.get("dry_serial", 0))
	out.reload_serial = int(d.get("reload_serial", 0))
	out.unscope_serial = int(d.get("unscope_serial", 0))
	out.rescope_serial = int(d.get("rescope_serial", 0))
	out.clip = int(d.get("clip", 0))
	out.reserve = int(d.get("reserve", 0))
	out.kick = int(d.get("kick", 0))
	out.body_anim_key = String(d.get("body_anim_key", ""))
	out.body_anim_phase = int(d.get("body_anim_phase", 0))
	return out
