class_name TerrainViewport
extends Control

const InputRouterScript = preload("res://modtools/terrain/terrain_viewport_input_router.gd")

var terrain_editor: Node
var edit_input_enabled: bool = true

var _container: SubViewportContainer
var _viewport: SubViewport
var _input_router: Node
# Optional non-terrain input consumer (e.g. the Mission workspace controller). Held
# here so it survives a deferred _build_viewport, then forwarded to the router.
var _input_target: Object


func _ready() -> void:
	mouse_filter = Control.MOUSE_FILTER_STOP
	clip_contents = true
	_build_viewport()
	set_process(true)
	_attach_terrain_world()


func _enter_tree() -> void:
	if _viewport != null:
		call_deferred("_attach_terrain_world")


func _exit_tree() -> void:
	if terrain_editor != null and terrain_editor.has_method("set_viewport_active"):
		terrain_editor.set_viewport_active(false, false)


func _process(_delta: float) -> void:
	if terrain_editor != null and _viewport != null and terrain_editor.has_method("set_viewport_mouse_position"):
		terrain_editor.set_viewport_mouse_position(_viewport.get_mouse_position())


func set_terrain_editor(value: Node) -> void:
	terrain_editor = value
	if _input_router != null:
		_input_router.terrain_editor = terrain_editor
	if is_node_ready():
		_attach_terrain_world()


func set_edit_input_enabled(value: bool) -> void:
	edit_input_enabled = value
	if _input_router != null:
		_input_router.edit_input_enabled = value
	if terrain_editor != null and terrain_editor.has_method("set_viewport_active") and is_inside_tree():
		terrain_editor.set_viewport_active(true, edit_input_enabled)


# Route viewport input to a second consumer alongside (or instead of) terrain editing.
# Pass null to detach. See TerrainViewportInputRouter.input_target.
func set_input_target(value: Object) -> void:
	_input_target = value
	if _input_router != null:
		_input_router.input_target = value


func _build_viewport() -> void:
	if _container != null:
		return
	_container = SubViewportContainer.new()
	_container.name = "TerrainViewportContainer"
	_container.set_anchors_preset(Control.PRESET_FULL_RECT)
	_container.stretch = true
	_container.mouse_filter = Control.MOUSE_FILTER_STOP
	add_child(_container)

	_viewport = SubViewport.new()
	_viewport.name = "TerrainSubViewport"
	_viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	_viewport.transparent_bg = false
	_viewport.handle_input_locally = true
	# Own physics world so the engine steps it: the mission workspace ray-picks placed
	# objects via this viewport's World3D.direct_space_state, which only sees collision
	# bodies if the world is stepped. A shared (default) SubViewport world is not stepped
	# here, so picks silently miss. (Same pattern as object/veg_preview.gd.)
	_viewport.own_world_3d = true
	_container.add_child(_viewport)

	_input_router = InputRouterScript.new()
	_input_router.name = "TerrainViewportInputRouter"
	_input_router.terrain_editor = terrain_editor
	_input_router.edit_input_enabled = edit_input_enabled
	_input_router.input_target = _input_target
	_viewport.add_child(_input_router)


func _attach_terrain_world() -> void:
	if terrain_editor == null or _viewport == null or not terrain_editor.has_method("get_terrain_world_root"):
		return
	var world_root: Node = terrain_editor.get_terrain_world_root()
	if world_root == null:
		return
	if world_root.get_parent() != _viewport:
		var old_parent: Node = world_root.get_parent()
		if old_parent != null:
			old_parent.remove_child(world_root)
		_viewport.add_child(world_root)
	if terrain_editor.has_method("set_viewport_active"):
		terrain_editor.set_viewport_active(is_inside_tree(), edit_input_enabled)
