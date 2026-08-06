class_name McpToolContext
extends RefCounted

## The `ctx` object handed to every curated MCP tool handler. Editor handlers
## use the editor/shell refs directly; shared engine handlers can use the
## narrow accessor methods without importing modtools.
##
## Everything editor-side is duck-typed Node/Object access — this class lives
## in engine/ and must not import modtools (the game runtime export excludes
## modtools/* but ships engine/*). Accessors return null when the surface is
## missing instead of erroring, so handlers can probe.

var editor: Node = null
var shell: Node = null
var tree: SceneTree = null
var args: Dictionary = {}
var cancelled := false
var logs: Array[String] = []
var images: Array = []
var log_sink: Callable = Callable()


## Append to the call's log (returned with the tool result) and mirror to the
## server's log hub when wired.
func log(value: Variant) -> void:
	var text := str(value)
	logs.append(text)
	if log_sink.is_valid():
		log_sink.call(text)


## Attach an Image (or anything with get_image(), e.g. a Texture2D) to the
## result as an MCP image content block.
func image(value: Variant) -> void:
	images.append(value)


## Show a transient message in the editor's status bar (visible to the human).
func status(message: String) -> void:
	if shell != null and shell.has_method("show_status_message"):
		shell.show_status_message(message)


## Await `count` process frames: `await ctx.frames(2)`. Lets asynchronous
## handlers yield to UI updates, deferred work, and the tool watchdog.
func frames(count: int) -> void:
	var scene_tree := main_tree()
	if scene_tree == null:
		return
	for i in range(count):
		await scene_tree.process_frame


## The mounted NovaResourceRoot, or null when no resource directory is set.
func root() -> Variant:
	return _shell_call("get_resource_root")


## The shell's NovaResourceIndex (asset listing), or null.
func index() -> Variant:
	return _shell_call("get_resource_index")


## A workspace (EditorWorkspace) by string id ("terrain", "mission", ...), or
## the active workspace when id is empty. Null when the shell is absent or the
## id is unknown. Workspaces are RefCounted adapters, not nodes.
func workspace(id := "") -> Variant:
	if shell == null:
		return null
	if id.is_empty():
		return _shell_call("_get_active_workspace")
	var table: Variant = shell.get("_workspaces")
	if table is Dictionary:
		for candidate in table.values():
			if candidate != null and candidate.has_method("get_workspace_id") and String(candidate.get_workspace_id()) == id:
				return candidate
	var popups: Variant = shell.get("_popup_workspaces")
	if popups is Dictionary:
		for candidate in popups.values():
			if candidate != null and candidate.has_method("get_workspace_id") and String(candidate.get_workspace_id()) == id:
				return candidate
	return null


## The mission workspace's MissionController (its editor document), or null
## when no mission workspace exists.
func mission() -> Variant:
	var mission_workspace: Variant = workspace("mission")
	if mission_workspace != null and mission_workspace.has_method("get_editor_document"):
		return mission_workspace.get_editor_document()
	return null


## The active workspace's 3D camera (terrain/mission/object views), or null in
## 2D workspaces.
func camera() -> Camera3D:
	var value: Variant = _shell_call("get_editor_camera")
	return value if value is Camera3D else null


## The SceneTree to await frames on: the wired one, else the running main loop
## (pure unit tests construct contexts without an editor).
func main_tree() -> SceneTree:
	if tree != null:
		return tree
	var loop := Engine.get_main_loop()
	return loop if loop is SceneTree else null


func _shell_call(method: String) -> Variant:
	if shell != null and shell.has_method(method):
		return shell.call(method)
	return null
