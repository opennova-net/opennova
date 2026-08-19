class_name PlayerViewmodelRig
extends RefCounted

# The local player's first-person viewmodel presentation, split out of
# LocalPlayerPresenter (W4-4): the FP render pass (SubViewport + renderfov camera),
# the viewmodel node + parts lifetime, the weapon.def placement (pos/tpos ADS
# lerp, rotation bias, axis map), and the first-person control-register writers.
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
# The Sighted/ADS path swaps `pos` -> `tpos` (WeaponDef.AltCamOffset @0x10C, read when entity
# Flags & 2), eased by the sim's scope fraction. The view fields flow from the mounted root's weapon.def
# (_apply_viewmodel_def <- GameWorld.local_player_viewmodel_def, the fixed default weapon until
# equipped-weapon resolution lands); the values below are the witnessed JOX WPN_AK47AUTO line,
# kept as the no-def fallback. (The pre-def constant (10, 0, -201) turned out to be the
# REVX-era WPN_AK47AUTO `pos` — that SKU's def drives the AKM_1st viewmodel.)
# The witnessed scale + fallback placement values live at engine
# simassets/fp_viewmodel_spec.h, re-exported through Simulation statics.
# Tunable (vars, not consts) so debug drivers can sweep placements live; the values are
# the witnessed WPN_AK47AUTO def line + the current best facing.
var PLAYER_VIEWMODEL_POS_UNITS := Simulation.viewmodel_fallback_pos_units()
# The ADS/sighted view offset (weapon.def `tpos` -> WeaponDef.AltCamOffset @0x10C), blended
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
var PLAYER_VIEWMODEL_ROT := Vector3(0.0, 180.0, 0.0)
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
var PLAYER_VIEWMODEL_RENDERFOV_H_DEG := 80.0
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
# The FP render pass nodes (see PLAYER_VIEWMODEL_RENDERFOV_H_DEG).
var _vm_pass_layer: CanvasLayer = null
var _vm_viewport: SubViewport = null
var _vm_camera: Camera3D = null
var _vm_parts: Array[ObjectModel] = []  # the builder's typed viewmodel models
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
	# Deferred: the game shell calls setup() from its own _ready, while the root
	# viewport is still mid-scene-setup — a direct add_child into it fails then
	# ("parent busy"), which would leave the viewmodel layer masked off the player
	# camera with NO pass to draw it (an invisible FP viewmodel). The build's own
	# guards make the deferred call a no-op after teardown()/double setup().
	_build_viewmodel_pass.call_deferred()


func teardown() -> void:
	clear_viewmodel()
	_free_viewmodel_pass()
	_world = null
	_presenter = null
	_camera = null


# W4-2-style justified accessors: PlayerWeaponEffects resolves action
# userpoints against the FP viewmodel parts, and the presenter stamps lighting
# context onto them — both read through the presenter's vm_parts()/viewmodel()
# delegates, which land here (the rig OWNS the array and the node).
func vm_parts() -> Array[ObjectModel]:
	return _vm_parts


func viewmodel() -> Node3D:
	return _viewmodel


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


# Build the dedicated FP render pass (see PLAYER_VIEWMODEL_RENDERFOV_H_DEG): a SubViewport
# sharing this presenter's World3D whose camera draws ONLY the viewmodel layer through the
# weapon renderfov projection, composited over the world frame below the HUD (layer 0 —
# the game HUD CanvasLayers sit at 1+). The container ignores the mouse so gameplay
# input passes through.
func _build_viewmodel_pass() -> void:
	if _camera == null or _vm_pass_layer != null or not _camera.is_inside_tree():
		return
	_vm_pass_layer = CanvasLayer.new()
	_vm_pass_layer.name = "ViewmodelPass"
	_vm_pass_layer.layer = 0
	var container := SubViewportContainer.new()
	container.stretch = true
	container.mouse_filter = Control.MOUSE_FILTER_IGNORE
	container.set_anchors_preset(Control.PRESET_FULL_RECT)
	_vm_viewport = SubViewport.new()
	# Render the CAMERA's World3D: the pass re-renders the SAME scene, culled to the
	# viewmodel layer [orig: one scene, second projection + depth window @0x4ded60].
	# Assigned explicitly — this presenter node may live OUTSIDE the play viewport
	# (tests presenter the rig off the game tree), so tree-inherited world/canvas
	# targets would be the presenter window's, not the game's.
	_vm_viewport.world_3d = _camera.get_world_3d()
	_vm_viewport.transparent_bg = true
	_vm_viewport.handle_input_locally = false
	_vm_camera = Camera3D.new()
	_vm_camera.cull_mask = Water.VISUAL_LAYER_VIEWMODEL
	_vm_camera.near = Simulation.viewmodel_pass_near_z()  # [orig: Render_SwapProjectionNearZ(0.05) @0x4dee29]
	_vm_viewport.add_child(_vm_camera)
	container.add_child(_vm_viewport)
	_vm_pass_layer.add_child(container)
	# Composite INTO the viewport the player camera renders (the play viewport), sized
	# to it via the full-rect container — not into this node's own ancestor viewport.
	_camera.get_viewport().add_child(_vm_pass_layer)


func _free_viewmodel_pass() -> void:
	if _vm_pass_layer != null and is_instance_valid(_vm_pass_layer):
		_vm_pass_layer.queue_free()
	_vm_pass_layer = null
	_vm_viewport = null
	_vm_camera = null


# Track the player camera 1:1 and rebuild the witnessed projection: renderfov is a
# HORIZONTAL fov in degrees, converted to Godot's vertical fov through the live aspect
# [orig: Render_SetViewAndProjectionMatrices @0x58d900 fovY = 2*atan(tan(fovX/2)/aspect)].
func _update_viewmodel_pass() -> void:
	if _vm_camera == null or _camera == null:
		return
	_vm_camera.global_transform = _camera.global_transform
	_vm_camera.far = _camera.far
	_vm_camera.attributes = _camera.attributes
	_vm_camera.environment = _camera.environment
	var size := _vm_viewport.size
	if size.x > 0 and size.y > 0:
		_vm_camera.fov = Simulation.fov_vertical_from_horizontal(
				PLAYER_VIEWMODEL_RENDERFOV_H_DEG, float(size.x) / float(size.y))


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
	var b := PLAYER_VIEWMODEL_ROT_BIAS_DEF
	var bias := Basis.from_euler(Vector3(
		deg_to_rad(_wrap180(b.y)),    # their pitch -> Godot x
		deg_to_rad(_wrap180(b.x)),    # their yaw   -> Godot y
		deg_to_rad(-_wrap180(b.z))))  # their roll  -> Godot z (opposite sense)
	var vm_basis := bias * Basis.from_euler(Vector3(
		deg_to_rad(PLAYER_VIEWMODEL_ROT.x),
		deg_to_rad(PLAYER_VIEWMODEL_ROT.y),
		deg_to_rad(PLAYER_VIEWMODEL_ROT.z)))
	# The ADS pos -> tpos blend, the /256 scale, and the NoCardSwitch reload
	# suppression run in the SIM (world/player_view.h player_view_bias_view_units,
	# S8) — one blended VIEW-FRAME offset per frame; _viewmodel_view_offset maps
	# the view axes onto Godot camera axes. Harness sim doubles implement the
	# same seam.
	# [orig: Player_UpdateFirstPersonCamera @0x4dd380, entity Flags & 2 ->
	#  AltCamOffset; the interp CNetPlayerInterp_Setup @0x4df36e]
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
	_viewmodel.global_transform = _camera.global_transform * Transform3D(
		vm_basis, bias * view_offset)
	# The FP overlay never enters the water mirror OR the main camera: retail draws it
	# as its own renderfov/near-Z pass over the finished frame [orig:
	# Player_RenderFirstPersonViewModel @ 0x4ded60]; in the port, the dedicated layer is drawn
	# only by the pass camera (and excluded by the mirror camera's cull_mask).
	# The gameplay camera admits the world shadow-caster marker layers. Strip
	# those markers here rather than preserving ObjectModel's defaults: this
	# dedicated near-Z pass must never leak its arms/weapon into world shadows.
	set_visual_layers(_viewmodel, Water.VISUAL_LAYER_VIEWMODEL, false)
	set_shadow_casting(_viewmodel, GeometryInstance3D.SHADOW_CASTING_SETTING_OFF)
	# The card switch: while the SIGHTS card is up, the FP model does not draw —
	# the frame shows one or the other [orig: selectors/clear @0x5ca299..0x5ca304;
	# the card path @0x5caaf3..0x5cab15 and the viewmodel candidate @0x5ca32c].
	var carded := view != null and view.scope_card_active
	var binoculars := view != null and view.binoculars_view_active
	# The showhud bit-0 gate ANDs into the retail submission decision [orig:
	# Player_RenderFirstPersonViewModel @0x4DEDEA — test g_FpWeaponViewFlags, 1
	# before the FP pass].
	var retail_submit := not third_person and not carded and not binoculars \
			and _fp_gun_visible
	# The debug override intentionally extends retail's submission scope, but a
	# model made visible by that probe still needs a coherent CTRL snapshot.
	var submit_viewmodel := retail_submit or force_visible
	_viewmodel.visible = submit_viewmodel
	_apply_viewmodel_control_registers(submit_viewmodel, weapon_view)
	_update_viewmodel_pass()


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
			var team := int(sim.get_local_player_team()) & 0xFF
			if team >= 0x80:
				team -= 0x100
			visual.set_ctrl_override(CTRL_OWNER_FP_TEAM, "TEX_TEAM", team)
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


# Stamp `layer_mask` onto every VisualInstance3D under `root` (inclusive).
# VisualInstance3D.layers is per-instance - a container's value does not
# propagate to children - and both player models are ObjectModel subtrees
# (mesh instances under Robj/Skeleton3D nodes) whose rebuild() recreates them
# on the default layer, so the callers (this rig's viewmodel stamp and the
# presenter's avatar/held-weapon stamps) re-stamp every frame.
static func set_visual_layers(root: Node, layer_mask: int,
		preserve_shadow_caster_layers: bool = true) -> void:
	if root is VisualInstance3D:
		var visual := root as VisualInstance3D
		var caster_layers := visual.layers & Water.VISUAL_LAYER_SHADOW_CASTER_MASK \
				if preserve_shadow_caster_layers else 0
		visual.layers = layer_mask | caster_layers
	for child in root.get_children():
		set_visual_layers(child, layer_mask, preserve_shadow_caster_layers)


# Stamp a Godot shadow-submission policy onto every geometry instance under
# `root`. Kept beside set_visual_layers because ObjectModel.rebuild() can
# recreate mesh children between frames, so player presentation reapplies both.
static func set_shadow_casting(root: Node, setting: int) -> void:
	if root is GeometryInstance3D:
		(root as GeometryInstance3D).cast_shadow = setting
	for child in root.get_children():
		set_shadow_casting(child, setting)


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
	return Vector3(-view_units.y, view_units.z, -view_units.x)


# The raw-def-units fallback for a null-sim harness: the same axis map over the
# /256 scale the sim's blend otherwise applies [orig: flt_7D1D70=256 @0x544770].
func _viewmodel_offset(units: Vector3) -> Vector3:
	return _viewmodel_view_offset(units / Simulation.weapon_def_pos_scale())


# Fold degrees into (-180, 180] (def rot columns store e.g. 353 for -7).
func _wrap180(degrees: float) -> float:
	var out := fmod(degrees + 180.0, 360.0)
	if out < 0.0:
		out += 360.0
	return out - 180.0
