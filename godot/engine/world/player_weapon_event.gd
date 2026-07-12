class_name PlayerWeaponEvent
extends RefCounted

## One logic tick's ordered presentation outputs from the local weapon FSM. The C++
## binding encodes these records as Dictionaries at the transport seam; GameWorld
## decodes them immediately so hosts consume a typed contract (ADR 0017). Within a
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
var action_finished := -1
var action_end_soundset := ""


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
	out.action_finished = int(d.get("action_finished", -1))
	out.action_end_soundset = String(d.get("action_end_soundset", ""))
	return out
