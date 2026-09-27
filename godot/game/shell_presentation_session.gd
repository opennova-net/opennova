class_name ShellPresentationSession
extends RefCounted

## Owns shell presentation visibility across menu entry, mission loading, and
## reversible render captures. MainGame retains lifecycle state and public
## callbacks; this module owns the CanvasItem mutations, capture validation,
## and load-signal wiring that make each transition atomic.


class SavedVisibility extends RefCounted:
	var layer: CanvasLayer
	var visible: bool

	func _init(p_layer: CanvasLayer) -> void:
		layer = p_layer
		visible = p_layer.visible


var _capture_active := false
var _saved_visibility: Array[SavedVisibility] = []
# The presenter whose FP gun a world-only capture hid (restored on finish).
var _capture_presenter: LocalPlayerPresenter = null
var _hud_hidden_capture_active := false


## Hide the world and mounted HUD items before the menu is raised. This keeps
## the pre-existing menu/load lifecycle policy separate from the reversible
## layer-level transaction used by world-only captures below.
func enter_menu(world: GameWorld, hud: CanvasLayer) -> void:
	world.visible = false
	_set_layer_children_visible(hud, false)


## Every SP and network mission start crosses this presentation edge: only the
## loading screen remains visible until the corresponding completion callback.
## [orig: Game_StartMission -> Render_LoadingScreen @ 0x521d10 /
## LoadingScreen_UpdateAndPresent @ 0x586be0, released by
## LoadingScreen_ReleaseEffect @ 0x525d52]
func begin_world_load(
		menu_shell: MenuShell,
		world: GameWorld,
		hud: CanvasLayer,
		on_world_loaded: Callable,
		on_load_failed: Callable) -> void:
	menu_shell.hide_menu()
	enter_menu(world, hud)
	if not world.world_loaded.is_connected(on_world_loaded):
		world.world_loaded.connect(on_world_loaded)
	if not world.load_failed.is_connected(on_load_failed):
		world.load_failed.connect(on_load_failed)


## Complete the loading presentation and reveal the world and HUD together.
func finish_world_load(
		world_load: WorldLoadCoordinator,
		world: GameWorld,
		hud: CanvasLayer) -> void:
	world_load.finish_presentation()
	world.visible = true
	_set_layer_children_visible(hud, true)


## The frozen-shell capture seam: after a fixture moves the beauty camera with
## processing disabled, the FP viewmodel (drawn inside the beauty pass at its
## world pose) is re-placed at the camera without a frame.
func restamp_viewmodel_for_capture(player_presenter: LocalPlayerPresenter) -> void:
	if player_presenter != null and is_instance_valid(player_presenter):
		player_presenter.restamp_viewmodel_at_camera()


func begin_world_only_capture(
		hud: CanvasLayer,
		menu_layer: CanvasLayer,
		player_presenter: LocalPlayerPresenter) -> Error:
	if _capture_active:
		return ERR_BUSY
	if hud == null or not is_instance_valid(hud) \
			or menu_layer == null or not is_instance_valid(menu_layer):
		return ERR_UNCONFIGURED

	_saved_visibility.clear()
	var layers: Array[CanvasLayer] = []
	_append_layer_tree(hud, layers)
	_append_layer_tree(menu_layer, layers)
	for layer in layers:
		_saved_visibility.append(SavedVisibility.new(layer))

	_capture_active = true
	for saved in _saved_visibility:
		# Hide at the layer boundary. This also suppresses nested CanvasLayers
		# (notably the dev tools) while leaving descendant state free to follow a
		# real menu/HUD transition during the asynchronous capture.
		saved.layer.visible = false
	# The FP gun draws inside the beauty pass; the presenter's capture latch
	# keeps it out of a world-only frame.
	_capture_presenter = player_presenter
	if player_presenter != null and is_instance_valid(player_presenter):
		player_presenter.set_viewmodel_capture_hidden(true)
	return OK


## Idempotent so capture error and teardown paths can share cleanup.
func finish_world_only_capture() -> void:
	if not _capture_active:
		return
	for saved in _saved_visibility:
		if is_instance_valid(saved.layer):
			saved.layer.visible = saved.visible
	_saved_visibility.clear()
	if _capture_presenter != null and is_instance_valid(_capture_presenter):
		_capture_presenter.set_viewmodel_capture_hidden(false)
	_capture_presenter = null
	_capture_active = false


func begin_hud_hidden_capture(
		hud_presenter: GameHudPresenter,
		hud: CanvasLayer) -> Error:
	if _hud_hidden_capture_active:
		return ERR_BUSY
	if hud_presenter == null or not is_instance_valid(hud_presenter) \
			or hud == null or not is_instance_valid(hud):
		return ERR_UNCONFIGURED
	var error := hud_presenter.begin_hud_hidden_capture()
	if error != OK:
		return error
	_hud_hidden_capture_active = true
	return OK


func finish_hud_hidden_capture(hud_presenter: GameHudPresenter) -> void:
	if not _hud_hidden_capture_active:
		return
	if hud_presenter != null and is_instance_valid(hud_presenter):
		hud_presenter.finish_hud_hidden_capture()
	_hud_hidden_capture_active = false


func hud_hidden_capture_witness(
		hud_presenter: GameHudPresenter,
		hud: CanvasLayer) -> HudHiddenCaptureWitness:
	if not _hud_hidden_capture_active:
		var inactive := HudHiddenCaptureWitness.new()
		inactive.error = "HUD-hidden capture presentation is not active"
		return inactive
	if hud_presenter == null or not is_instance_valid(hud_presenter) \
			or hud == null or not is_instance_valid(hud):
		var unavailable := HudHiddenCaptureWitness.new()
		unavailable.error = "HUD-hidden capture presentation is unconfigured"
		return unavailable
	var witness := hud_presenter.hud_hidden_capture_witness()
	if witness.is_valid():
		witness.hud_canvas_layer_active = hud.visible
	return witness


static func _set_layer_children_visible(layer: CanvasLayer, visible: bool) -> void:
	if layer == null or not is_instance_valid(layer):
		return
	for child in layer.get_children():
		if child is CanvasItem:
			(child as CanvasItem).visible = visible


## CanvasLayer visibility deliberately does not propagate to CanvasLayer
## descendants, so each nested layer is an independent capture boundary.
static func _append_layer_tree(
		layer: CanvasLayer,
		out: Array[CanvasLayer]) -> void:
	if layer == null or out.has(layer):
		return
	out.append(layer)
	_collect_nested_layers(layer, out)


static func _collect_nested_layers(
		node: Node,
		out: Array[CanvasLayer]) -> void:
	for child in node.get_children():
		if child is CanvasLayer and not out.has(child as CanvasLayer):
			out.append(child as CanvasLayer)
		_collect_nested_layers(child, out)


