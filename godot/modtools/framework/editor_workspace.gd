class_name EditorWorkspace
extends RefCounted

## New Componentized Editor Shell Contract.
## Workspaces now yield capabilities and commands instead of overriding monolithic methods.
##
## *COMPATIBILITY LAYER*: Temporarily bridges legacy methods (mount_viewport, etc.) 
## to the new Capabilities/Commands system until all workspaces are migrated.

var editor_shell: Node
var _cached_capabilities: Array[EditorCapability] = []

func set_editor_shell(value: Node) -> void:
	editor_shell = value

# --- Identity ---

func get_workspace_id() -> StringName:
	return &""

func get_workspace_label() -> String:
	return ""

func get_project_title() -> String:
	return get_workspace_label()

func get_status_tool() -> String:
	return get_workspace_label()

func get_status_context() -> String:
	return ""

func get_workspace_tooltip() -> String:
	return ""

func uses_asset_dock() -> bool:
	return false

func uses_left_lane() -> bool:
	return true

# --- Lifecycle ---

func bind_to_editor(_editor: Node) -> void:
	pass

func activate() -> void:
	pass

func deactivate() -> void:
	pass

# --- Component Providers ---

func get_capabilities() -> Array[EditorCapability]:
	if not _cached_capabilities.is_empty():
		return _cached_capabilities
	
	# COMPATIBILITY: Wrap legacy methods in Providers if overridden
	var caps: Array[EditorCapability] = []
	
	# If a subclass overrides mount_viewport, it has a Viewport capability
	var script: Script = get_script()
	if _overrides(script, "mount_viewport"):
		var vp = ViewportProvider.new(get_viewport_camera())
		# Hack to store the legacy mount methods for the shell to call later
		vp.set_meta("legacy_mount", Callable(self, "mount_viewport"))
		vp.set_meta("legacy_unmount", Callable(self, "unmount_viewport"))
		vp.set_meta("legacy_release", Callable(self, "release_viewport"))
		vp.set_meta("legacy_shows_guides", Callable(self, "shows_view_guides"))
		vp.set_meta("legacy_shows_camera", Callable(self, "shows_camera_status"))
		caps.append(vp)
		
	# If a subclass has workflows or a single pane inspector, it has an Inspector capability
	if _overrides(script, "build_inspector") or _overrides(script, "_build_inspector_defs"):
		var ip = InspectorProvider.new(null)
		# Store legacy references
		ip.set_meta("legacy_build", Callable(self, "build_inspector"))
		ip.set_meta("legacy_workflows", Callable(self, "get_workflows"))
		ip.set_meta("legacy_active_workflow", Callable(self, "get_active_workflow_id"))
		ip.set_meta("legacy_activate_workflow", Callable(self, "activate_workflow"))
		ip.set_meta("legacy_build_workflow", Callable(self, "build_workflow_inspector"))
		caps.append(ip)
		
	_cached_capabilities = caps
	return caps

func _overrides(script: Script, method_name: StringName) -> bool:
	# Godot 4: Check if the method belongs to a subclass rather than the base.
	if script == null: return false
	var methods = script.get_script_method_list()
	for m in methods:
		if m.name == method_name:
			return true
	return _overrides(script.get_base_script(), method_name)

func get_commands() -> Array[EditorCommand]:
	# COMPATIBILITY: Wrap legacy can_*/has_* in EditorCommand
	var cmds: Array[EditorCommand] = []
	
	if has_new_action():
		cmds.append(EditorCommand.new(&"new", get_new_action_label(), Callable(self, "new_current"), Callable(self, "can_new")))
	if has_open_action():
		# Note: open_file takes a string argument, which requires UI flow, 
		# so this wrapper just proxies the UI intent for now.
		var open_cmd = EditorCommand.new(&"open", get_open_action_label(), Callable(self, "trigger_legacy_open"), Callable(self, "can_open"))
		open_cmd.set_meta("legacy_open_filters", Callable(self, "get_open_dialog_filters"))
		cmds.append(open_cmd)
	if has_save_action():
		cmds.append(EditorCommand.new(&"save", get_save_action_label(), Callable(self, "save_current"), Callable(self, "can_save")))
	if has_save_as_action():
		cmds.append(EditorCommand.new(&"save_as", get_save_as_action_label(), Callable(self, "trigger_legacy_save_as"), Callable(self, "can_save_as")))
	if has_export_action():
		cmds.append(EditorCommand.new(&"export", get_export_action_label(), Callable(self, "trigger_legacy_export"), Callable(self, "can_export")))
		
	return cmds

func get_editor_document() -> EditorDocument:
	return null

func has_unsaved_changes() -> bool:
	var doc = get_editor_document()
	if doc and doc.has_method("is_dirty"):
		return doc.is_dirty()
	return false

# Legacy definitions for compatibility wrappers
func mount_viewport(_host: Control) -> void: pass
func unmount_viewport(_host: Control) -> void: pass
func release_viewport() -> void: pass
func get_viewport_camera() -> Camera3D: return null
func shows_camera_status() -> bool: return false
func shows_view_guides() -> bool: return false
func set_grid_visible(_value: bool) -> void: pass
func set_axes_visible(_value: bool) -> void: pass

func build_inspector(_host: Control) -> void: pass
func get_workflows() -> Array: return []
func get_active_workflow_id() -> int: return -1
func activate_workflow(_id: int) -> void: pass
func build_workflow_inspector(_id: int, _host: Control) -> void: pass

func has_new_action() -> bool: return false
func get_new_action_label() -> String: return "New"
func can_new() -> bool: return false
func new_current() -> Error: return ERR_UNAVAILABLE

func has_open_action() -> bool: return false
func get_open_action_label() -> String: return "Open"
func can_open() -> bool: return false
func trigger_legacy_open() -> void: pass
func get_open_dialog_filters() -> PackedStringArray: return PackedStringArray()

func has_save_action() -> bool: return false
func get_save_action_label() -> String: return "Save"
func can_save() -> bool: return false
func save_current() -> Error: return ERR_UNAVAILABLE

func has_save_as_action() -> bool: return false
func get_save_as_action_label() -> String: return "Save As"
func can_save_as() -> bool: return false
func trigger_legacy_save_as() -> void: pass

func has_export_action() -> bool: return false
func get_export_action_label() -> String: return "Export"
func can_export() -> bool: return false
func trigger_legacy_export() -> void: pass

func is_busy() -> bool: return false
func flush_pending_edits() -> Error: return OK
func sync_asset_dock() -> void: pass
func set_asset_dock(_dock: Control) -> void: pass

func can_undo() -> bool: return false
func can_redo() -> bool: return false
func undo() -> void: pass
func redo() -> void: pass

func shows_tile_gizmo() -> bool: return false
func focus_reference(_focus: Dictionary) -> Error: return OK
func get_current_resource_path() -> String: return ""

func supports_document_tabs() -> bool: return false
func get_document_tabs() -> Array: return []
func get_active_document_index() -> int: return -1
func activate_document(_index: int) -> void: pass
func close_document(_index: int) -> void: pass

func get_export_progress_title() -> String: return "Exporting..."
func get_export_progress_phase() -> String: return ""
func get_export_progress_message() -> String: return ""
func get_export_progress_current() -> int: return 0
func get_export_progress_total() -> int: return 0
func get_export_progress_ratio() -> float: return 0.0

func _resource_root() -> NovaResourceRoot:
	if editor_shell != null and editor_shell.has_method("get_resource_root"):
		return editor_shell.get_resource_root()
	return null

func _resource_root_or_settings() -> NovaResourceRoot:
	var root := _resource_root()
	if root != null:
		return root
	var dir := NovaResourceDirSettings.get_resource_dir()
	if dir.is_empty():
		return null
	var resources := NovaResourceRoot.new()
	return resources if resources.set_root_dir(dir) == OK else null
