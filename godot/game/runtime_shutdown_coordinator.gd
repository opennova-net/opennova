extends RefCounted

## Owns the process-exit lifecycle for MainGame. Normal world-to-menu teardown
## deliberately retains the mounted root and renderer caches; process exit must
## cancel every frame-awaited load leg, allow those stacks to resume, then drop
## renderer resources while the GDExtension and RenderingServer are still alive.

const VegAssetsScript := preload("res://engine/terrain/veg_assets.gd")
const DRAIN_FRAMES := 4

var _shell: Node = null
var _tree: SceneTree = null
var _prepared := false
var _quit_requested := false
var _frames_left := 0
var _resources_released := false
var _previous_auto_accept_quit := true
var _quit_policy_installed := false


func bind(shell: Node) -> void:
	_shell = shell


func install_quit_policy(shell: Node, tree: SceneTree) -> void:
	_shell = shell
	_tree = tree
	_previous_auto_accept_quit = tree.auto_accept_quit
	tree.auto_accept_quit = false
	_quit_policy_installed = true


## Returns true only for the exit-tree notification, so MainGame can stop its
## render sampler at the same boundary. WM-close merely starts the drained path.
func handle_notification(what: int) -> bool:
	if what == Node.NOTIFICATION_WM_CLOSE_REQUEST:
		request_quit()
		return false
	if what != Node.NOTIFICATION_EXIT_TREE:
		return false
	prepare()
	release_resources()
	_restore_quit_policy()
	return true


func request_quit() -> void:
	if _quit_requested:
		return
	prepare()
	_quit_requested = true
	_frames_left = DRAIN_FRAMES


## True while shutdown owns the frame. The first call may occur in the request
## frame, leaving three complete process-frame emissions before release: enough
## for NovaLoadingScreen's two-signal barrier and the outer continuation.
func process_frame() -> bool:
	if not _prepared:
		return false
	if _quit_requested:
		_frames_left -= 1
		if _frames_left <= 0:
			release_resources()
			_quit_requested = false
			if _tree != null:
				_tree.quit()
	return true


## Invalidate load ownership and tear down mission/session nodes, but leave the
## loading screen alive. Destroying that screen while _run_world_load awaits its
## two-frame prepare coroutine would strand the outer GDScriptFunctionState and
## its bound JoinTarget until after extension unload.
func prepare() -> void:
	if _prepared:
		return
	_prepared = true
	if not is_instance_valid(_shell):
		return
	_shell.set("_world_load_pending", false)
	_shell.set("_world_load_request_id",
			int(_shell.get("_world_load_request_id")) + 1)
	_shell.call("_cleanup_picker")
	for property in [
			"_player_presenter", "_armory_presenter",
			"_deploy_presenter", "_hud_presenter",
	]:
		var presenter: Variant = _shell.get(property)
		if presenter != null:
			presenter.teardown()
	var world: Variant = _shell.get("_world")
	if world != null:
		world.cancel_join_preload()
		world.cancel_join_admission()
		world.unload()
	var net: Variant = _shell.get("_net")
	if net != null:
		net.on_world_teardown()
	Input.set_mouse_mode(Input.MOUSE_MODE_VISIBLE)


## Idempotent synchronous release after the drain. The explicit EXIT_TREE path
## is a best-effort fallback when an external owner bypasses WM-close/request;
## normal application and parity-driver exits always cross the drain first.
func release_resources() -> void:
	if _resources_released:
		return
	prepare()
	_resources_released = true
	if is_instance_valid(_shell):
		_shell.call("_dismiss_loading_screen")
		_shell.call("_cleanup_picker")
		var menu_shell: Variant = _shell.get("_menu_shell")
		if is_instance_valid(menu_shell):
			menu_shell.release_runtime_renderer_resources()
		else:
			# EXIT_TREE can reach the parent after its menu child. The Input
			# singleton still owns the applied arrow cursor in that ordering.
			Input.set_custom_mouse_cursor(null, Input.CURSOR_ARROW)
		var world: Variant = _shell.get("_world")
		if world != null:
			world.release_runtime_renderer_resources()
		var resource_root: Variant = _shell.get("_root")
		if resource_root != null:
			resource_root.clear()
	VegAssetsScript.clear_cache()


func _restore_quit_policy() -> void:
	if _quit_policy_installed and _tree != null:
		_tree.auto_accept_quit = _previous_auto_accept_quit
	_quit_policy_installed = false
