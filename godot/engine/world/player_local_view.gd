class_name PlayerLocalView
extends RefCounted

## The local player's view-state snapshot — the typed record behind
## `NovaSimulation.get_local_player_view()`'s transport Dictionary (ADR 0017).
## Ticked in the SIM at the world cadence (62.5 Hz), so the ADS ease, the fov
## policy, and the third-person anchor chase are render-rate independent; hosts
## only read it and place nodes. [orig: the 15-step scope interp @0x4df36e;
## g_scopeEngaged @0x82CE94; fov policy Player_ToggleWeaponScope @0x4df0c0..401;
## anchor chase ThirdPersonCamera_Update @0x437c8d]

var scope_engaged := false
var scope_fraction := 0.0     # 0 = hip .. 1 = sighted, in 1/15ths
var fov_h_deg := 80.0         # the main camera's HORIZONTAL fov (policy applied)
var tp_anchor := Vector3.ZERO # the chased eye anchor, Godot space
var tp_anchor_valid := false


## Decode one sim view dict; null on an empty dict (no sim).
static func from_view_dict(d: Dictionary) -> PlayerLocalView:
	if d.is_empty():
		return null
	var out := PlayerLocalView.new()
	out.scope_engaged = bool(d.get("scope_engaged", false))
	out.scope_fraction = float(d.get("scope_fraction", 0.0))
	out.fov_h_deg = float(d.get("fov_h_deg", 80.0))
	out.tp_anchor = d.get("tp_anchor", Vector3.ZERO)
	out.tp_anchor_valid = bool(d.get("tp_anchor_valid", false))
	return out
