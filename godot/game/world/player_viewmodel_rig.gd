class_name PlayerViewmodelRig
extends RefCounted

# The local player's first-person viewmodel presentation, split out of
# LocalPlayerPresenter (W4-4): the FP projection feed (the renderfov focal
# ratio + depth band the object shaders apply to flagged instances inside the
# beauty pass), the viewmodel node + parts lifetime, the weapon.def placement
# (pos/tpos ADS lerp, rotation bias, axis map), and the first-person
# control-register writers.
# The presenter keeps the player camera, the avatar/held-weapon presentation, and
# the third-person flag; per-frame inputs (the view snapshot, the weapon view,
# the camera mode, the debug force flag) arrive as update_viewmodel arguments.

# First-person weapon viewmodel placement, witnessed from weapon.def `pos` (hip) / `tpos` (ADS).
# The original adds the equipped weapon's view-bias offset to the eye in view-local space, rotated by
# the view orientation, then draws the gun (gfx1) + character arms at that view root
# [orig: Player_UpdateFirstPersonCamera @0x4dd380 -> g_view_euler_translation_out;
# Player_RenderFirstPersonViewModel @0x4ded60]. The weapon.def parser stores the pos/tpos POSITION as
# `atof(str) * 256.0` (a 16.16 fixed-point world coord; scale flt_7D1D70 @0x544770) and the ROTATION
# as degrees -> 32-bit BAM (`* 0x0B60B60` = 2^32/360) [orig: weapon.def 'tpos' handler @0x54471f].
# The camera ftol's the stored float and adds it straight onto g_view_pos (16.16), so the net WORLD
# offset is simply `file_value / 256` — see _viewmodel_offset for the axis map and derivation.
# The Sighted/ADS path eases `pos` -> `tpos` (the scope interp runs from the hip copy at
# WeaponDef+0x10C to the tpos at +0x124 and publishes the difference as the view bias
# [orig: Player_StepFpViewBiasInterp @0x4ddf53..0x4ddfc3]), carried here by the sim's scope
# fraction. (The `Flags & 2` leg of the camera is the DEAD/round-end camera, not ADS.) The view fields flow from the mounted root's weapon.def
# (_apply_viewmodel_def <- GameWorld.local_player_viewmodel_def, the fixed default weapon until
# equipped-weapon resolution lands); the values below are the witnessed JOX WPN_AK47AUTO line,
# kept as the no-def fallback. (The pre-def constant (10, 0, -201) turned out to be the
# REVX-era WPN_AK47AUTO `pos` — that SKU's def drives the AKM_1st viewmodel.)
# The witnessed scale + fallback placement values live at engine
# simassets/fp_viewmodel_spec.h, re-exported through Simulation statics.
# Tunable (vars, not consts) so debug drivers can sweep placements live; the values are
# the witnessed WPN_AK47AUTO def line + the current best facing.
var PLAYER_VIEWMODEL_POS_UNITS := Simulation.viewmodel_fallback_pos_units()
# The ADS/sighted view offset (weapon.def `tpos` -> WeaponDef.CamOffsetTpos @0x124), blended
# in by the sim's scope fraction; JOX AK47AUTO = (-62.33, 29.19, -152.56).
var PLAYER_VIEWMODEL_TPOS_UNITS := Simulation.viewmodel_fallback_tpos_units()
# The FP rig's model->camera AXIS MAP, euler DEGREES in CAMERA space. The FP rig is a
# T-posed character skeleton (BN01 Pelvis at the origin) that the wpn clips POSE into the
# hold facing downrange; the rig renders through the standard skeletal pipeline (import-
# flipped meshes + the bind-rotation rests, positions reconstructed from the model table),
# and the yaw-180 turns the rig's authored forward onto Godot's -Z camera forward — the
# structural equivalent of the original drawing its composed render-frame bone matrices
# with the raw view matrix [orig: Player_RenderFirstPersonViewModel @0x4ded60 root = view
# transform; the S*A^T*S copy loops @0x40c4d8..0x40c57c realize the model->render map
# inside the composition]. Sign pinned against retail: the stock/grip anchor bottom-RIGHT
# at the hip idle (the pre-train build renders identically and was retail-confirmed).
var PLAYER_VIEWMODEL_ROT := Vector3(0.0, Simulation.viewmodel_rig_yaw_deg(), 0.0)
# Witnessed per-weapon view-rotation bias: weapon.def `pos` rotation columns, DEGREES
# (yaw, pitch, roll) ADDED to the view angles — the weapon cant. AK47AUTO = 5.0 / 3.75 / 353.0.
# [orig: Player_UpdateFirstPersonCamera @0x4dd444: rot = view_rot + Def.Bone.rot; parser stores
# degrees -> BAM @0x54471f.] Sign map to Godot camera axes verified visually.
var PLAYER_VIEWMODEL_ROT_BIAS_DEF := Simulation.viewmodel_fallback_rot_bias_deg()
# The FP render pass: the original draws the viewmodel through its OWN projection — the
# weapon's `renderfov` (HORIZONTAL degrees; every JO weapon.def omits the key, so all use the
# record default 80.0) converted to vertical via the aspect, with the near plane swapped
# 0.2 -> 0.05 and the viewport depth range remapped so the world never overdraws it, then its
# own flush [orig: Player_RenderFirstPersonViewModel @0x4ded60: Render_SwapProjectionNearZ(0.05)
# @0x4dee29 / restore 0.2 @0x4df0aa, fov = WeaponDef+0x148 @0x4dee71 -> h->v conversion in
# Render_SetViewAndProjectionMatrices @0x58d900, depth remap Render_SetViewportDepth01 @0x58a7b0;
# default 80.0 = flt_7D1898 stored by AdmDef_InitEntryDefaults @0x53ff31; parser key 'renderfov'
# @0x54482a]. Ported as a SubViewport sharing the world, camera cull-masked to the viewmodel
# layer, composited over the finished frame (the depth-remap's visible equivalent).
var PLAYER_VIEWMODEL_RENDERFOV_H_DEG := Simulation.weapon_render_fov_h_deg_default()
# (near-z lives engine-side: Simulation.viewmodel_pass_near_z())
const CTRL_OWNER_FP_HEAT := "first_person:heat"
const CTRL_OWNER_FP_EMPLACED := "first_person:emplaced"
const CTRL_OWNER_FP_TEAM := "first_person:team"
const CTRL_OWNER_FP_ARMS_CAMO := "first_person:arms_camo"

# The world serves the viewmodel builder + def; the presenter serves the weapon
# effects (play-serial resync on rebuild). Untyped for the same reason as the
# presenter's _world: GUT harness worlds serve value-only doubles.
var _world
var _presenter
var _camera: Camera3D = null
var _viewmodel: Node3D = null
var _vm_parts: Array[ObjectModel] = []  # the builder's typed viewmodel models
# A world-only render capture hides the gun for its duration (the shell's
# capture session latches this; the per-frame submission gate ANDs it in).
var _capture_hidden := false
# The last per-frame submission verdict, so the capture latch can re-apply
# visibility without a frame.
var _submit_viewmodel := false
# The last FP projection feed pushed to the shader global (k, near, far).
var _projection_feed := Vector4.ZERO
# The showhud bit-0 FP-gun gate (GameHudPresenter cycles the flags and pushes
# the bit through LocalPlayerPresenter.set_fp_gun_visible). Default on = the
# boot flags value 3. [orig: g_FpWeaponViewFlags bit 0, tested by
# Player_RenderFirstPersonViewModel @0x4DEDEA before the FP submit]
var _fp_gun_visible := true


func set_fp_gun_visible(visible: bool) -> void:
	_fp_gun_visible = visible


func setup(world, presenter, camera: Camera3D) -> void:
	_world = world
	_presenter = presenter
	_camera = camera


func teardown() -> void:
	clear_viewmodel()
	_world = null
	_presenter = null
	_camera = null
	_capture_hidden = false
	_submit_viewmodel = false
	# The projection feed is process-wide: leave the project default behind
	# like the lighting block does, so a preview or the next mission never
	# inherits this one's focal ratio.
	if _projection_feed != Vector4.ZERO:
		var shipped: Dictionary = ProjectSettings.get_setting(
				"shader_globals/opennova_viewmodel_projection", {})
		if shipped.has("value"):
			RenderingServer.global_shader_parameter_set(
					&"opennova_viewmodel_projection", shipped["value"])
	_projection_feed = Vector4.ZERO


# W4-2-style justified accessors: PlayerWeaponEffects resolves action
# userpoints against the FP viewmodel parts, and the presenter stamps lighting
# context onto them — both read through the presenter's vm_parts()/viewmodel()
# delegates, which land here (the rig OWNS the array and the node).
func vm_parts() -> Array[ObjectModel]:
	return _vm_parts


func viewmodel() -> Node3D:
	return _viewmodel


## The gameplay camera the rig was set up with (probes read the FP placement
## relative to it).
func camera() -> Camera3D:
	return _camera


## A world-only render capture hides the gun for its duration. Applied at once:
## the fixture flow freezes the shell before it captures, so no per-frame
## update runs between the latch and the readback.
func set_capture_hidden(hidden: bool) -> void:
	_capture_hidden = hidden
	if _viewmodel != null and is_instance_valid(_viewmodel):
		_viewmodel.visible = _submit_viewmodel and not _capture_hidden


func is_capture_hidden() -> bool:
	return _capture_hidden


## The FP projection feed last pushed to `opennova_viewmodel_projection`
## (x = renderfov focal ratio, y = near, z = far).
func projection_feed() -> Vector4:
	return _projection_feed


## The viewmodel branch of the presenter's model lifetime: (re)build the FP model
## when missing, apply its weapon.def view record, and re-collect the parts.
func ensure_viewmodel() -> void:
	if _world == null:
		return
	if _viewmodel == null or not is_instance_valid(_viewmodel):
		_viewmodel = _world.build_local_player_viewmodel()
		if _viewmodel != null:
			_apply_viewmodel_def()
			_vm_parts = _world.local_player_viewmodel_parts().duplicate()
			# re-sync the clip serial: fresh parts replay the active clip
			_presenter.weapon_effects().reset_play_serial()


## Drop the built FP viewmodel so the next update pass rebuilds gun/arms/FSM
## from the (changed) equipped weapon — the armory ACCEPT re-mount [orig:
## WeaponLoadout_ApplyFromBuffer @0x565cd0 tail -> Player_MountWeaponSlot
## @0x4dfa40]. The parts array keeps its (freed) entries until the rebuild
## re-collects them; every reader guards with is_instance_valid.
func refresh_viewmodel() -> void:
	if _viewmodel != null and is_instance_valid(_viewmodel):
		_viewmodel.queue_free()
	_viewmodel = null


## The viewmodel leg of the presenter's clear-models path.
func clear_viewmodel() -> void:
	if _viewmodel != null and is_instance_valid(_viewmodel):
		_viewmodel.queue_free()
	_viewmodel = null
	_vm_parts.clear()


# The witnessed FP projection, fed to the object shaders as one global: the gun
# draws INSIDE the beauty pass (retail's "viewmodel first" into the same
# backbuffer) through the weapon renderfov — a HORIZONTAL fov in degrees,
# converted to the vertical through the live aspect [orig:
# Render_SetViewAndProjectionMatrices @0x58d900 fovY = 2*atan(tan(fovX/2)/aspect)]
# — with the near plane swapped to 0.05 [orig: Render_SwapProjectionNearZ @0x4dee29]
# and the depth range clamped to the nearest tenth [orig: Render_SetViewportDepth01
# @0x58a7b0]. Both frusta share the viewport aspect, so the shader needs only the
# focal ratio between the renderfov projection and the live beauty projection
# (shaders/viewmodel_pass.gdshaderinc applies it per flagged instance).
func _update_viewmodel_projection() -> void:
	if _camera == null or not _camera.is_inside_tree():
		return
	var size := _camera.get_viewport().get_visible_rect().size
	if size.x <= 0.0 or size.y <= 0.0:
		return
	var aspect := size.x / size.y
	var near := float(Simulation.viewmodel_pass_near_z())
	var fov_fp_v := Simulation.fov_vertical_from_horizontal(
			PLAYER_VIEWMODEL_RENDERFOV_H_DEG, aspect)
	var beauty := _camera.get_camera_projection()
	if is_zero_approx(beauty.y.y):
		return
	var fp := Projection.create_perspective(fov_fp_v, aspect, near, _camera.far)
	var feed := Vector4(fp.y.y / beauty.y.y, near, _camera.far, 1.0)
	if feed == _projection_feed:
		return
	_projection_feed = feed
	RenderingServer.global_shader_parameter_set(
			&"opennova_viewmodel_projection", feed)


# First-person weapon viewmodel: sit it in front of the eye, tracking the camera 1:1,
# shown in first person only (in 3P the body avatar shows instead). The original biases
# the CAMERA by the weapon's `pos`/`tpos` view offset and draws the model at the view
# root [orig: Player_UpdateFirstPersonCamera @0x4dd380]; placing it in camera space is
# the faithful structural equivalent (camera.global_transform == the engine view
# transform here). Per-frame state arrives as arguments from the presenter's camera pass:
# the sim view snapshot, this tick's weapon view, the camera mode, the debug force flag.
func update_viewmodel(view: PlayerLocalView, weapon_view: PlayerWeaponView,
		third_person: bool, force_visible: bool) -> void:
	if _viewmodel == null or not is_instance_valid(_viewmodel) or _camera == null:
		return
	# The engine draws the FP model with the RAW VIEW MATRIX as its world transform, i.e. the
	# model lives in VIEW space [orig: Player_RenderFirstPersonViewModel @0x4ded60]. So the
	# viewmodel is parented to the CAMERA transform (view-relative), NOT oriented in world space —
	# a viewmodel must stay fixed to the view, not swing with the aim. PLAYER_VIEWMODEL_ROT lays
	# the model's forward down-range (and picks the correct handedness so a right-handed weapon
	# sits on the right); PLAYER_VIEWMODEL_POS_UNITS is the weapon.def `pos` view offset.
	# camera x bias(def rot, about the eye in view axes) x axis map(rig -> camera).
	# [orig: Player_UpdateFirstPersonCamera @0x4dd380 adds Def.Bone.rot to the view angles and
	# rotates Def.Bone.pos into view orientation before adding to the eye.]
	_place_viewmodel_at_camera()
	# The FP overlay rides its own visual layer: the beauty camera admits it and
	# every mesh instance applies the renderfov projection + depth band
	# (retail's "viewmodel first" draw into the same backbuffer [orig:
	# Player_RenderFirstPersonViewModel @ 0x4ded60]); the mirror, Q3, and
	# capture cameras exclude the layer. The gameplay camera admits the world
	# shadow-caster marker layers; the viewmodel policy strips those markers
	# so the gun never leaks into world shadows. Both stamps are edge-gated
	# on the model (policy value / scene build serial), never per frame.
	for part in _vm_parts:
		if is_instance_valid(part):
			part.set_presentation_layer(ObjectModel.PRESENTATION_LAYER_VIEWMODEL)
			part.set_viewmodel_pass(true)
	# The card switch: while the SIGHTS card is up, the FP model does not draw —
	# the frame shows one or the other [orig: selectors/clear @0x5ca299..0x5ca304;
	# the card path @0x5caaf3..0x5cab15 and the viewmodel candidate @0x5ca32c].
	var carded := view != null and view.scope_card_active
	var binoculars := view != null and view.binoculars_view_active
	# The showhud bit-0 gate ANDs into the retail submission decision [orig:
	# Player_RenderFirstPersonViewModel @0x4DEDEA — test g_FpWeaponViewFlags, 1
	# before the FP pass].
	# The SEAT gate, evaluated here because retail evaluates it inside the draw:
	# a pilot/driver/gunner carries no first-person weapon at all, so a
	# helicopter cockpit shows a clear screen instead of a rifle over the panel.
	# A PASSENGER keeps his and can still shoot out. Per-frame is what makes
	# MOUNTING take effect - the HUD-init path only ran on the showhud key.
	# [orig: Player_RenderFirstPersonViewModel guards the whole draw on
	#  `!vehicle || parentSlot not in {2,3,5} || (attrib & EWEAP && !PLAYERCONTROL)`;
	#  the condition itself is world::mount_hides_fp_viewmodel]
	var seat_hides_weapon := false
	var vm_sim: Simulation = _world.get_sim() if _world != null else null
	if vm_sim != null:
		seat_hides_weapon = vm_sim.local_player_fp_weapon_hidden()
	var retail_submit := not third_person and not carded and not binoculars \
			and _fp_gun_visible and not seat_hides_weapon
	# The debug override intentionally extends retail's submission scope, but a
	# model made visible by that probe still needs a coherent CTRL snapshot.
	var submit_viewmodel := retail_submit or force_visible
	_submit_viewmodel = submit_viewmodel
	_viewmodel.visible = submit_viewmodel and not _capture_hidden
	_apply_viewmodel_control_registers(submit_viewmodel, weapon_view)
	_update_viewmodel_projection()


# The gun's world pose is load-bearing for the beauty-pass fold (the shader
# folds the camera's view of the instance's WORLD transform), so the root sits
# at the camera every frame.
func _place_viewmodel_at_camera() -> void:
	# The def cant as camera euler radians: the engine's axis map
	# (simassets/fp_viewmodel_spec.h viewmodel_bias_euler_rad).
	var bias := Basis.from_euler(
			Simulation.viewmodel_bias_euler_rad(PLAYER_VIEWMODEL_ROT_BIAS_DEF))
	var vm_basis := bias * Basis.from_euler(Vector3(
		deg_to_rad(PLAYER_VIEWMODEL_ROT.x),
		deg_to_rad(PLAYER_VIEWMODEL_ROT.y),
		deg_to_rad(PLAYER_VIEWMODEL_ROT.z)))
	# The ADS pos -> tpos blend, the /256 scale, and the NoCardSwitch reload
	# suppression run in the SIM (world/player_view.h player_view_bias_view_units,
	# S8) — one blended VIEW-FRAME offset per frame; _viewmodel_view_offset maps
	# the view axes onto Godot camera axes. Harness sim doubles implement the
	# same seam.
	# [orig: Player_UpdateFirstPersonCamera @0x4dd380 adds Bone(+0xF4) + the interp
	#  bias; the interp CNetPlayerInterp_Setup @0x4df36e runs +0x10C -> +0x124]
	var sim = _world.get_sim() if _world != null else null
	var view_offset := _viewmodel_offset(PLAYER_VIEWMODEL_POS_UNITS)
	if sim != null:
		# Sampling the viewport size is this rig's device work; the
		# 4:3-or-narrower RULE the z drop keys on lives engine-side
		# (world/player_view.h player_view_narrow_aspect; retail: the
		# viewport block @0x4dd571). Out-of-tree (teardown frames) passes
		# 0x0, which the engine rule reads as narrow — the same posture as
		# the old 4:3 default.
		var vs := Vector2.ZERO
		if _world.is_inside_tree():
			vs = _world.get_viewport().get_visible_rect().size
		view_offset = _viewmodel_view_offset(
				sim.local_player_viewmodel_bias_view_units(
						PLAYER_VIEWMODEL_POS_UNITS, PLAYER_VIEWMODEL_TPOS_UNITS,
						int(vs.x), int(vs.y)))
	# An unchanged write would still dirty every part into the flush's
	# transform notifications; a still view writes nothing.
	var next := _camera.global_transform * Transform3D(vm_basis, bias * view_offset)
	if _viewmodel.global_transform != next:
		_viewmodel.global_transform = next


## Re-place the gun and re-push its projection feed against the camera's
## CURRENT pose without advancing any sim state: a frozen-shell fixture moves
## the beauty camera after the per-frame pass stopped, and the fold draws the
## gun where the root last sat.
func restamp_at_camera() -> void:
	if _viewmodel == null or not is_instance_valid(_viewmodel) or _camera == null:
		return
	_place_viewmodel_at_camera()
	_update_viewmodel_projection()


func _apply_viewmodel_control_registers(submit_viewmodel: bool,
		weapon_view: PlayerWeaponView) -> void:
	# setup()'s world contract already includes get_sim; LocalPlayerPresenter and its
	# value-only harness doubles both use that same explicit seam.
	var sim = _world.get_sim() if _world != null else null
	for visual in _vm_parts:
		if not is_instance_valid(visual):
			continue
		visual.begin_ctrl_update()
		# TEX_TEAM is a signed-byte store immediately before the FP lighting,
		# heat and model-submit path. Hidden/carded/binocular/third-person frames
		# never execute that retail writer.
		# [orig: Player_RenderFirstPersonViewModel @0x4DEE96..0x4DEE9F]
		if submit_viewmodel and sim != null:
			visual.set_ctrl_override(CTRL_OWNER_FP_TEAM, "TEX_TEAM",
					Simulation.viewmodel_team_byte(int(sim.get_local_player_team())))
		else:
			visual.clear_ctrl_override(CTRL_OWNER_FP_TEAM, "TEX_TEAM")
		# Retail publishes accumulated heat independently for every FP model
		# submit, clamped through the exact 0x10000 endpoint.
		# [orig: Player_RenderFirstPersonViewModel @0x4DEEC2..0x4DEEF5]
		if submit_viewmodel and weapon_view != null:
			visual.set_ctrl_override(CTRL_OWNER_FP_HEAT,
					"HEAT_GLOW", weapon_view.heat_glow)
		else:
			visual.clear_ctrl_override(CTRL_OWNER_FP_HEAT, "HEAT_GLOW")
		if submit_viewmodel and weapon_view != null \
				and weapon_view.emplaced_controls_valid:
			visual.set_ctrl_override(CTRL_OWNER_FP_EMPLACED,
					"EWEAP_GUNYAW", weapon_view.emplaced_gun_yaw)
			visual.set_ctrl_override(CTRL_OWNER_FP_EMPLACED,
					"EWEAP_GUNPITCH", weapon_view.emplaced_gun_pitch)
		else:
			visual.clear_ctrl_override(CTRL_OWNER_FP_EMPLACED,
					"EWEAP_GUNYAW")
			visual.clear_ctrl_override(CTRL_OWNER_FP_EMPLACED,
					"EWEAP_GUNPITCH")
		# The character arms' own raw camo triplet is stored immediately before
		# each arms submit -- the same per-submit writer family, arms part only.
		# [orig: Avatar_SetArmsCamoCtrl @0x57a3b0 at @0x4df008/@0x4df070]
		if String(visual.get_meta("avatar_part", "")) == "arms":
			if submit_viewmodel:
				AvatarDatabase.apply_part_camo(visual,
						visual.get_meta("avatar_camo", []), CTRL_OWNER_FP_ARMS_CAMO)
			else:
				for register in AvatarDatabase.part_camo_registers():
					visual.clear_ctrl_override(CTRL_OWNER_FP_ARMS_CAMO, register)
		visual.end_ctrl_update()


# Pull the resolved weapon.def view record from the world; null when no weapon.def (or the
# weapon) resolves in the mounted root — the witnessed JOX WPN_AK47AUTO constants above stay
# in force. The def rows carry xyz raw file units + yaw/pitch/roll degrees
# [orig: weapon.def 'pos'/'tpos' handlers @0x54471f; 'renderfov' @0x54482a, default 80.0].
func _apply_viewmodel_def() -> void:
	if _world == null:
		return
	var def: PlayerViewmodelDef = _world.local_player_viewmodel_def()
	if def == null:
		return
	PLAYER_VIEWMODEL_POS_UNITS = def.pos_units
	PLAYER_VIEWMODEL_ROT_BIAS_DEF = def.rot_bias_deg
	PLAYER_VIEWMODEL_TPOS_UNITS = def.tpos_units
	PLAYER_VIEWMODEL_RENDERFOV_H_DEG = def.renderfov_h_deg
	# flags / scope_max_mag / clipsize stay with the SIM (the weapon dict feeds
	# set_local_player_weapon): the ADS gates + fov policy run there (ADR 0016).


# Map a VIEW-FRAME offset (world units, from the sim's blended bias) onto Godot
# camera-local axes. The view/def frame is X = FORWARD, Y = LEFT, Z = UP —
# proven by the aim ray's far point being {+65536000, 0, 0} through the SAME
# transform [orig: HUD_DrawCrosshair @0x592a0f aim_direction = (1000.0, 0, 0)
# q16; the view-local rotate Math_FixedPointTransformPoint22 @0x4dd5d8].
# Godot camera-local is (x right, y up, -z forward), so:
#   view x (forward) -> Godot -z   (M4 tpos x -50.9 = ~0.2u BACK into the shoulder)
#   view y (left)    -> Godot -x
#   view z (up)      -> Godot  y   (e.g. MP5SD pos.z -183 -> grip ~0.715u below the eye)
# (The 2026-07-11 grill REFUTED the earlier x=right/y=forward reading: the AK's
# |x| ~= |y| masked the swap; the JOX/REVX M4 tpos made it glare — the canted-ADS
# report. oscarmike's onhook-derived map agrees with the witnessed frame.) The
# velocity lead (>>7, clamps @0x4dd4f2..) and the prone Z drop (-1280 @0x4dd578)
# are recorded unported tails.
func _viewmodel_view_offset(view_units: Vector3) -> Vector3:
	return Simulation.viewmodel_camera_local_from_view(view_units)


# The raw-def-units fallback for a null-sim harness: the same axis map over the
# /256 scale the sim's blend otherwise applies [orig: flt_7D1D70=256 @0x544770].
func _viewmodel_offset(units: Vector3) -> Vector3:
	return _viewmodel_view_offset(units / Simulation.weapon_def_pos_scale())


