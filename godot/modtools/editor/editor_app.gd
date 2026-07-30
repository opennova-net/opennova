class_name EditorApp
extends Node

# App root of editor_main.tscn. Owns the application-level concerns — window
# sizing, the embedded MCP (agent) server, the shared environment document,
# and the boot wiring between the domain editors and the shell
# (EditorWorkstation). Domain editing lives in the workspaces; the terrain
# world (TerrainWorldRoot) belongs to the TerrainEditor node because mission
# edit mode renders into the same world through its own viewport.

const EDITOR_MIN_WINDOW_SIZE := Vector2i(1366, 768)
const EnvironmentEditorScript = preload("res://modtools/environment/environment_editor.gd")

@onready var terrain_editor: TerrainEditor = $TerrainEditor
@onready var workstation: Control = $CanvasLayer/EditorWorkstation

var environment_editor
var mcp_service: Node = null

var _previous_window_min_size: Vector2i = Vector2i.ZERO


func _ready() -> void:
	# Children are ready first: the terrain editor has built its world furniture
	# and loaded its state. Inject the shared documents, hand the shell its
	# editor, then seed the initial terrain — the same order the old
	# terrain-rooted boot used.
	get_tree().auto_accept_quit = false
	_configure_editor_window()
	environment_editor = EnvironmentEditorScript.new()
	environment_editor.name = "EnvironmentEditor"
	add_child(environment_editor)
	terrain_editor.set_environment_editor(environment_editor)
	terrain_editor.set_workstation(workstation)
	if workstation.has_method("set_editor"):
		workstation.set_editor(self)
	_init_mcp_service()
	terrain_editor.new_terrain()


func _exit_tree() -> void:
	var window := get_window()
	if window:
		window.min_size = _previous_window_min_size


# F11 fullscreen — the core-engine window concept (NovaWindow), shared with the
# game shell; the MCP's set_fullscreen tool routes to the same helper.
func _unhandled_key_input(event: InputEvent) -> void:
	if NovaWindow.is_toggle_event(event):
		NovaWindow.toggle_fullscreen(get_window())
		get_viewport().set_input_as_handled()


func get_terrain_editor() -> TerrainEditor:
	return terrain_editor


func get_environment_editor():
	return environment_editor


func _configure_editor_window() -> void:
	var window := get_window()
	if window == null:
		return
	_previous_window_min_size = window.min_size
	window.min_size = EDITOR_MIN_WINDOW_SIZE
	if window.mode == Window.MODE_WINDOWED:
		var next_size := window.size
		next_size.x = maxi(next_size.x, EDITOR_MIN_WINDOW_SIZE.x)
		next_size.y = maxi(next_size.y, EDITOR_MIN_WINDOW_SIZE.y)
		if next_size != window.size:
			window.size = next_size


# Boot the embedded MCP (agent) server. ONED-only by construction: the runtime
# export excludes modtools/*, so this is the single start path; the feature
# guard is belt-and-braces. Whether it actually listens is McpSettings'
# decision (on by default, never headless, --mcp-off/--mcp-port override).
# The service's editor node stays the TERRAIN editor: MCP tools sample heights
# and drive the camera through it.
func _init_mcp_service() -> void:
	if OS.has_feature("runtime_game"):
		return
	var service := EditorMcpService.new()
	service.name = "McpService"
	add_child(service)
	service.setup(
			terrain_editor,
			workstation,
			workstation.get_game_run_session(),
			workstation.run_game,
			workstation.stop_game)
	mcp_service = service
