extends "res://modtools/mission/controller/controller_section.gd"

# Live simulation transport (Play the mission) + the in-editor
# PLAYPARTANIM preview.
# Moved verbatim from mission_controller.gd (F5); state stays on the
# controller, reached through `_c`.

# Registry over the placed (edit-mode) container, rebuilt only when the entity set changes.
func _get_preview_registry():
	if _c._preview_registry == null or _c._preview_registry_rev != _c._membership_rev:
		_c._preview_registry = _c.MissionEntityRegistry.new()
		_c._preview_registry.build(_c._objects_container(), _c._mission)
		_c._preview_registry_rev = _c._membership_rev
	return _c._preview_registry


# Resolve a PLAYPARTANIM action to one live animatable model: its explicit target (param1) by
# action_type, else an animated current selection, else null.
func _resolve_part_anim_node(action: Dictionary) -> Node3D:
	var registry = _get_preview_registry()
	var target := int(action.get("param1", 0))
	var nodes: Array = []
	match int(action.get("action_type", -1)):
		_c._ACT_CHANGE_SINGLE_AI:
			var hit = registry.resolve_single(target)  # registry is untyped here; no := inference
			if hit != null:
				nodes = [hit]
		_c._ACT_CHANGE_GROUP_AI:
			nodes = registry.resolve_group(target)
		_c._ACT_AREA_AI_RED, _c._ACT_AREA_AI_BLUE:
			nodes = registry.resolve_zone(target)
	for n in nodes:
		if n != null and is_instance_valid(n) and n.has_method("play_part_anim"):
			return n
	# Fallback: an animated current selection (e.g. previewing while an object is selected).
	if _c._selected_node != null and is_instance_valid(_c._selected_node) and _c._selected_node.has_method("play_part_anim"):
		return _c._selected_node
	return null


## True when the given scripting action can be previewed (a target model resolves).
func can_preview_part_anim(action: Dictionary) -> bool:
	return not action.is_empty() and _resolve_part_anim_node(action) != null


## Play the action's part animation on its target model (clean restart from rest). Returns false when no
## target resolves. channel = param2, play_type = param3, time = param4 (16.16 seconds -> seconds).
func preview_part_anim(action: Dictionary) -> bool:
	var node := _resolve_part_anim_node(action)
	if node == null:
		return false
	stop_preview()
	_c._preview_node = node
	var channel := int(action.get("param2", 0))
	var play_type := int(action.get("param3", 0))
	var time_s := float(int(action.get("param4", 0))) / 65536.0
	if node.has_method("set_playing"):
		node.set_playing(true)
	if node.has_method("reset_animation_time"):
		node.reset_animation_time()
	if node.has_method("restart_part_anim"):
		node.restart_part_anim(channel, play_type, time_s)
	elif node.has_method("play_part_anim"):
		node.play_part_anim(channel, play_type, time_s)
	return true


## Stop any running preview and return the previewed model to rest.
func stop_preview() -> void:
	if _c._preview_node != null and is_instance_valid(_c._preview_node):
		if _c._preview_node.has_method("clear_part_anims"):
			_c._preview_node.clear_part_anims()
		if _c._preview_node.has_method("clear_ctrl_values"):
			_c._preview_node.clear_ctrl_values()
		if _c._preview_node.has_method("reset_animation_time"):
			_c._preview_node.reset_animation_time()
	_c._preview_node = null


func can_simulate() -> bool:
	return _c.is_loaded() and _c._objects_container() != null

func is_simulating() -> bool:
	return _c._sim_driver != null and is_instance_valid(_c._sim_driver)

# One gate for every mutating entry point (viewport gestures, inspector setters, undo/redo,
# delete/place): while the sim runs, reject the edit with a status line instead of racing the
# present pass. Returns true when the caller must bail.
func _reject_edit_while_simulating() -> bool:
	if not is_simulating():
		return false
	_c._report("Stop the simulation to edit.", true)
	return true

func is_sim_playing() -> bool:
	return is_simulating() and _c._sim_driver.is_playing()


# The live MissionRuntime while simulating (null otherwise) — the seam the
# debug overlay's runtime source resolves through, same shape as
# NovaWorld.get_runtime() on the game side.
func get_sim_runtime() -> Node:
	return _c._sim_driver if is_simulating() else null

func _ensure_sim_driver() -> bool:
	if is_simulating():
		return true
	if not _c.is_loaded():
		return false
	var container = _c._objects_container()
	if container == null:
		_c._report("Load a mission on a terrain before simulating.", true)
		return false
	# Entering sim mode ends any half-finished edit gesture / armed tool, and hides the
	# hover box + transform gizmo (the present pass owns the nodes now).
	_c._viewport.cancel_drag()
	_c._placement.disarm_placement()
	_c._viewport._clear_hover()
	_c._viewport._clear_selected_user_points()
	_c._sim_driver = _c.MissionRuntime.new()
	_c._sim_driver.name = "MissionRuntime"
	container.add_child(_c._sim_driver)
	# self_tick + the sim's default loco_scale: the exact options the game's GameWorld path
	# runs, so the preview IS the game's pacing. The old every-process tick + loco_scale 4096
	# combo (32768/8, a slowed compensation for uncapped editor fps) was an editor-only
	# divergence. The driver builds its present index over `container`. Pass the editor's
	# loaded terrain so the preview grounds AI exactly like the game runtime, and the resource
	# root so soldiers get their .adm/.bad root-motion clips (without them they stand still).
	var terrain_data = _c.terrain_editor.get_data() if _c.terrain_editor != null and _c.terrain_editor.has_method("get_data") else null
	var sim_root: NovaResourceRoot = _c._resource_root()
	if int(_c._sim_driver.setup(_c._mission, container, {
			"self_tick": true,
			"playable": false,
			"terrain": terrain_data,
			"resource_root": sim_root,
			"item_db": _c._placement._item_db(),
		})) <= 0:
		sim_stop()
		_c._report("No AI entities to simulate in this mission.", false)
		return false
	_c._viewport._refresh_gizmo()
	return true

func sim_play() -> void:
	if not _ensure_sim_driver():
		return
	_c._sim_driver.play()
	_c._report("Simulating mission (%d AI)." % int(_c._sim_driver.entity_count()), false)
	_c.changed.emit()

func sim_pause() -> void:
	if is_simulating():
		_c._sim_driver.pause()
		_c.changed.emit()

func sim_step() -> void:
	if not _ensure_sim_driver():
		return
	_c._sim_driver.step_once()
	_c.changed.emit()

func sim_stop() -> void:
	if not is_simulating():
		return
	_c._sim_driver.stop()
	_c._sim_driver.queue_free()
	_c._sim_driver = null
	# Editing is unlocked again: bring the transform gizmo back for the surviving selection.
	_c._viewport._refresh_gizmo()
	_c.changed.emit()


## The debug overlay (C12) drives the live sim driver directly — it is
## host-neutral and bypasses the sim_* methods above. The workspace relays its
## play/pause/step presses here so `changed` still fires and the sim bar
## re-reads the driver state. (Overlay Stop relays to sim_stop() instead: it
## must also free the driver to unlock editing.)
func notify_sim_transport_changed() -> void:
	if is_simulating():
		_c.changed.emit()
