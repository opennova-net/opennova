class_name PlayerWeaponView
extends RefCounted

## The local player's equipped-weapon FSM view — the typed record behind
## `NovaSimulation.get_local_player_weapon_state()`'s transport Dictionary (ADR 0017:
## the record is the contract, the dict is its C++-binding encoding). Serials are
## monotonic event counters (several 62.5 Hz logic ticks can run per render frame, so
## edge events surface as counts, never booleans): `play_serial` bumps each time the
## FSM starts a clip (`anim_key`), `fired`/`dry`/`reload`/`unscope`/`rescope` mirror
## the handler events. [orig: WeaponAction_ProcessFrame @0x540e60 + the wpn_std_*
## handlers; docs/net/novaworld-net-re.md §5.62]

var active := false
var current_action := 0    # world::weapon_action id (0 idle .. 11 overheated)
var anim_key := ""         # the .adm clip key of the last play event
var play_serial := 0
var fired_serial := 0
var dry_serial := 0
var reload_serial := 0
var unscope_serial := 0
var rescope_serial := 0
var clip := 0              # rounds in the magazine
var reserve := 0           # carried pool, rounds
var kick := 0              # recoil kick intensity 0..20 [orig: MountSlot+0x5B]


## Decode one weapon-state dict; null when the FSM is inactive (no weapon installed).
static func from_state_dict(d: Dictionary) -> PlayerWeaponView:
	if d.is_empty() or not bool(d.get("active", false)):
		return null
	var out := PlayerWeaponView.new()
	out.active = true
	out.current_action = int(d.get("current", 0))
	out.anim_key = String(d.get("anim_key", ""))
	out.play_serial = int(d.get("play_serial", 0))
	out.fired_serial = int(d.get("fired_serial", 0))
	out.dry_serial = int(d.get("dry_serial", 0))
	out.reload_serial = int(d.get("reload_serial", 0))
	out.unscope_serial = int(d.get("unscope_serial", 0))
	out.rescope_serial = int(d.get("rescope_serial", 0))
	out.clip = int(d.get("clip", 0))
	out.reserve = int(d.get("reserve", 0))
	out.kick = int(d.get("kick", 0))
	return out
