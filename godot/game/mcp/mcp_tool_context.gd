class_name McpToolContext
extends RefCounted

## The `ctx` object handed to every curated MCP tool handler. Editor handlers
## use the editor/shell refs directly; shared engine handlers can use the
## narrow accessor methods without importing modtools.
##
## The shell seam stays a plain Node reference: this class ships in the game
## export, which excludes modtools/*, so it cannot name EditorWorkstation /
## EditorWorkspace as types. The accessor methods below ARE the shell
## contract (a wired shell exposes every one of them — ONED's
## EditorWorkstation does); calls are direct, with null meaning "no shell
## wired" (the game runtime's MCP contexts carry no shell).

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


## Attach an Image or a Texture2D to the result as an MCP image content
## block.
func image(value: Variant) -> void:
	images.append(value)


## Show a transient message in the editor's status bar (visible to the human).
func status(message: String) -> void:
	if shell != null:
		shell.show_status_message(message)


## Await `count` process frames: `await ctx.frames(2)`. Lets asynchronous
## handlers yield to UI updates, deferred work, and the tool watchdog.
func frames(count: int) -> void:
	var scene_tree := main_tree()
	if scene_tree == null:
		return
	for i in range(count):
		await scene_tree.process_frame


## The mounted ResourceRoot, or null when no resource directory is set.
func root() -> Variant:
	return shell.get_resource_root() if shell != null else null


## The shell's ResourceIndex (asset listing), or null.
func index() -> Variant:
	return shell.get_resource_index() if shell != null else null


## A workspace (EditorWorkspace) by string id ("terrain", "mission", ...), or
## the active workspace when id is empty. Null when the shell is absent or the
## id is unknown. Workspaces are RefCounted adapters, not nodes; every
## registered workspace exposes get_workspace_id (EditorWorkspace base).
func workspace(id := "") -> Variant:
	if shell == null:
		return null
	if id.is_empty():
		return shell._get_active_workspace()
	var table: Variant = shell.get("_workspaces")
	if table is Dictionary:
		for candidate in table.values():
			if candidate != null and String(candidate.get_workspace_id()) == id:
				return candidate
	var popups: Variant = shell.get("_popup_workspaces")
	if popups is Dictionary:
		for candidate in popups.values():
			if candidate != null and String(candidate.get_workspace_id()) == id:
				return candidate
	return null


## The mission workspace's MissionController (its editor document), or null
## when no mission workspace exists (get_editor_document is EditorWorkspace
## base surface).
func mission() -> Variant:
	var mission_workspace: Variant = workspace("mission")
	if mission_workspace != null:
		return mission_workspace.get_editor_document()
	return null


## The active workspace's 3D camera (terrain/mission/object views), or null in
## 2D workspaces.
func camera() -> Camera3D:
	if shell == null:
		return null
	var value: Variant = shell.get_editor_camera()
	return value if value is Camera3D else null


## The SceneTree to await frames on: the wired one, else the running main loop
## (pure unit tests construct contexts without an editor).
func main_tree() -> SceneTree:
	if tree != null:
		return tree
	var loop := Engine.get_main_loop()
	return loop if loop is SceneTree else null
