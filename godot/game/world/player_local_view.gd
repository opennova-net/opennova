class_name PlayerLocalView
extends RefCounted

## The local player's view-state snapshot — the typed record behind
## `Simulation.get_local_player_view()`'s transport Dictionary (ADR 0017).
## Ticked in the SIM at the world cadence (62.5 Hz), so the ADS ease, the fov
## policy, and the third-person anchor chase are render-rate independent; owners
## only read it and place nodes. [orig: the 15-step scope interp @0x4df36e;
## g_scopeEngaged @0x82CE94; fov policy Player_ToggleWeaponScope @0x4df0c0..401;
## anchor chase ThirdPersonCamera_Update @0x437c8d]

var scope_engaged := false
var mounted := false
# The RESOLVED camera mode, the chase preference behind it, and whether the
# mounted camera leg is engaged (a control seat in third person). The sim's
# arbiter resolves the mode every tick from the preference and the seat
# [orig: g_camera_mode @0xA890C8; g_camera_third_person_selected @0xA860DF;
#  Render_ProcessMainSceneFrame @0x5ca1d2].
var third_person := false
var third_person_selected := true
var camera_mounted := false
var vehicle_attack_context := false
var scope_fraction := 0.0     # 0 = hip .. 1 = sighted, over the toggle's ease steps
# The NoCardSwitch reload rule: true while the slot is mid-RELOAD on a weapon
# WITHOUT NoCardSwitch — the FP view bias is dropped for the frame (the presenter
# reads the eased fraction as 0). [orig: Player_UpdateFirstPersonCamera
# @0x4dd439/@0x4dd4cc; predicate Player_IsReloadingCardSwitchWeapon @0x4dcdd0]
var suppress_view_bias := false
# The standard card switch: a Scoped or Sighted weapon at FULL raise in first
# person shows its SIGHTS rows INSTEAD of the FP viewmodel. NoCardSwitch clears
# both selectors unless ForceScoped overrides it. [orig: Render_ProcessMainSceneFrame
# @0x5ca299..0x5ca304 / @0x5caaf3..0x5cab15]
var scope_card_active := false
var binoculars_requested := false
var binoculars_raised := false
var binoculars_view_active := false
# One fixed-radius random displacement, generated on the raw toggle-on edge and
# retained through movement/death/third-person suppression.
var binocular_yaw_offset_deg := 0.0
var binocular_pitch_offset_deg := 0.0
var nvg_active := false
var nvg_visible := false
var nvg_gain := 0
# The main camera's HORIZONTAL fov (policy applied); default = the engine base
# fov (world/player_view.h kPlayerCameraFovHDeg, Simulation.DEFAULT_PLAYER_FOV_H_DEG).
var fov_h_deg: float = Simulation.DEFAULT_PLAYER_FOV_H_DEG
var tp_anchor := Vector3.ZERO # the chased eye anchor, Godot space
var tp_anchor_valid := false
# Retail adds twice the live recoil accumulator to the first-person camera pitch.
# This is kept separate from authoritative look pitch because third-person and
# aim-ray consumers do not inherit that camera-only doubling.
# [orig: Camera_ComputeThirdPersonView @0x437fc7]
var fp_pitch_recoil_deg := 0.0
# The FP camera roll in degrees, composed in the sim: torsoRoll + lean/4
# (torsoRoll chases the slope roll; lean is the Q/E ramp — both entity BAM state)
# [orig: the on-foot person leg @0x437fe6 — roll = entity+0x2DC + (entity+0xB0)/4].
var fp_roll_deg := 0.0
# The composed camera pose (world/player_view.h, S8): the engine runs the
# witnessed FP/TP composition — the eye floor + pull-back, the recoil-doubled
# pitch, the roll, the chased-anchor pivot nudge and march-landed back-off —
# and the presenter only converts frames and stamps the Camera3D node.
# [orig: Camera_ComputeThirdPersonView @0x437d10 — the on-foot person leg
#  @0x437f9c..0x438031, the TP leg @0x438100..0x4383e2]
var camera_pose_valid := false
var camera_eye := Vector3.ZERO  # Godot space
var camera_yaw_deg := 0.0       # mission-euler view angles
var camera_pitch_deg := 0.0
var camera_roll_deg := 0.0


## Decode one sim view dict; null on an empty dict (no sim).
static func from_view_dict(d: Dictionary) -> PlayerLocalView:
	if d.is_empty():
		return null
	var out := PlayerLocalView.new()
	out.scope_engaged = bool(d.get("scope_engaged", false))
	out.mounted = bool(d.get("mounted", false))
	out.third_person = bool(d.get("third_person", false))
	out.third_person_selected = bool(d.get("third_person_selected", true))
	out.camera_mounted = bool(d.get("camera_mounted", false))
	out.vehicle_attack_context = bool(d.get("vehicle_attack_context", false))
	out.scope_fraction = float(d.get("scope_fraction", 0.0))
	out.suppress_view_bias = bool(d.get("suppress_view_bias", false))
	out.scope_card_active = bool(d.get("scope_card_active", false))
	out.binoculars_requested = bool(d.get("binoculars_requested", false))
	out.binoculars_raised = bool(d.get("binoculars_raised", false))
	out.binoculars_view_active = bool(d.get("binoculars_view_active", false))
	out.binocular_yaw_offset_deg = float(d.get("binocular_yaw_offset_deg", 0.0))
	out.binocular_pitch_offset_deg = float(d.get("binocular_pitch_offset_deg", 0.0))
	out.nvg_active = bool(d.get("nvg_active", false))
	out.nvg_visible = bool(d.get("nvg_visible", false))
	out.nvg_gain = clampi(int(d.get("nvg_gain", 0)), 0, 4)
	# Default = the engine's base camera fov (world/player_view.h
	# kPlayerCameraFovHDeg, bound as Simulation.DEFAULT_PLAYER_FOV_H_DEG).
	out.fov_h_deg = float(d.get("fov_h_deg", Simulation.DEFAULT_PLAYER_FOV_H_DEG))
	out.tp_anchor = d.get("tp_anchor", Vector3.ZERO)
	out.tp_anchor_valid = bool(d.get("tp_anchor_valid", false))
	out.fp_pitch_recoil_deg = float(d.get("fp_pitch_recoil_deg", 0.0))
	out.fp_roll_deg = float(d.get("fp_roll_deg", 0.0))
	out.camera_pose_valid = bool(d.get("camera_pose_valid", false))
	out.camera_eye = d.get("camera_eye", Vector3.ZERO)
	out.camera_yaw_deg = float(d.get("camera_yaw_deg", 0.0))
	out.camera_pitch_deg = float(d.get("camera_pitch_deg", 0.0))
	out.camera_roll_deg = float(d.get("camera_roll_deg", 0.0))
	return out
