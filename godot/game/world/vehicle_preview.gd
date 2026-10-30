class_name VehiclePreview
extends Control

## The VEHICLE screen's ITEM_DISPLAY pane: the selected vehicle group's model turning in the
## widget's rect, as the game's custom draw renders it (engine inmatch::vehicle_preview_view
## and vehicle_menu_preview_graphic carry the witnesses). The rect clears to the draw's colour;
## with a model, the model's box centre sits at the origin, the camera stands three box
## half-diagonals out, 60 degrees round and a quarter of that up, its field 30 degrees across
## the rect's width, and the model turns a full circle every 16384 ms of the process clock.
## The draw's own light block (a fixed key, sky and ground colour) is the game's preview state
## alone; this pane's model reads the shared light block the world sets (named in
## docs/interface/hud-re.md D-HUD-14).

const SAMPLER_MIP_BIAS := -2.0 # the draw's D3DSAMP_MIPMAPLODBIAS on stages 0 and 1
const NEAR := 0.1
const FAR := 1000.0

var _resource_root: ResourceRoot = null
var _container: SubViewportContainer
var _viewport: SubViewport
var _environment: WorldEnvironment
var _camera: Camera3D
var _spin: Node3D
var _model: ObjectModel = null
var _graphic := ""


func _ready() -> void:
	mouse_filter = Control.MOUSE_FILTER_IGNORE
	_container = SubViewportContainer.new()
	_container.name = "VehiclePreviewContainer"
	_container.stretch = true
	_container.mouse_filter = Control.MOUSE_FILTER_IGNORE
	_container.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	add_child(_container)
	_viewport = SubViewport.new()
	_viewport.own_world_3d = true
	_viewport.transparent_bg = false
	_viewport.handle_input_locally = false
	_viewport.gui_disable_input = true
	_viewport.texture_mipmap_bias = SAMPLER_MIP_BIAS
	_viewport.render_target_update_mode = SubViewport.UPDATE_WHEN_VISIBLE
	_container.add_child(_viewport)
	var root := Node3D.new()
	_viewport.add_child(root)
	# Retail shaders write gamma-domain values and rely on one terminal display decode per 3D
	# view, as the PLAYER_INFO portrait's (AvatarPreview).
	var display_decode := DisplayDecode.new()
	display_decode.name = "VehiclePreviewDisplayDecode"
	root.add_child(display_decode)
	_environment = WorldEnvironment.new()
	_environment.environment = Environment.new()
	_environment.environment.background_mode = Environment.BG_COLOR
	_environment.environment.background_color = VehicleMenuController.preview_clear_color()
	root.add_child(_environment)
	_spin = Node3D.new()
	_spin.name = "VehiclePreviewSpin"
	root.add_child(_spin)
	_camera = Camera3D.new()
	_camera.keep_aspect = Camera3D.KEEP_WIDTH
	_camera.fov = VehicleMenuController.preview_fov()
	_camera.near = NEAR
	_camera.far = FAR
	_camera.current = true
	root.add_child(_camera)


func set_resource_root(root: ResourceRoot) -> void:
	_resource_root = root


## The model the pane shows ("" none) and its turn at the process clock `tick_ms`.
func show_graphic(graphic: String, tick_ms: int) -> void:
	if graphic != _graphic:
		_graphic = graphic
		_load(graphic)
	if _spin != null:
		_spin.basis = VehicleMenuController.preview_model_basis(tick_ms)


## The graphic the pane was last asked for, and whether its model loaded.
func graphic() -> String:
	return _graphic


func has_model() -> bool:
	return _model != null


func preview_camera() -> Camera3D:
	return _camera


func preview_viewport() -> SubViewport:
	return _viewport


func _load(graphic: String) -> void:
	if _model != null and is_instance_valid(_model):
		_model.queue_free()
	_model = null
	if graphic.is_empty() or _resource_root == null or _spin == null:
		return
	var data := ObjectData.new()
	if data.open_from_resource_root(_resource_root, graphic) != OK:
		return
	_model = ObjectModel.new()
	_model.name = "VehiclePreviewModel"
	_spin.add_child(_model)
	_model.set_object_data(data)
	_model.set_active_lod(0)
	# The box centre at the origin, the turn about it; the eye from the box.
	var bounds := _model.get_model_bounds()
	_model.position = -bounds.get_center()
	var eye := VehicleMenuController.preview_eye(bounds)
	if eye.length() > 0.0:
		_camera.look_at_from_position(eye, Vector3.ZERO, Vector3.UP)
