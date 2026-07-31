class_name AvatarsEditorWorkspace
extends EditorWorkspace

# Adapter for the Avatars workspace: opens, edits, and saves an Avatars.def
# character database (head/body/arms parts composed into combos under a
# nationality -> division tree). The main viewport shows a 3D preview that
# composes the selected combo's parts; the left lane offers three workflow
# inspectors — the nationality/division/combo Tree, the parts editor, and the
# combos editor. All edits flow through the AvatarsDocument (set_model -> the
# database's "changed" signal -> dirty), so the shell derives save/undo state.
#
# Format + behavior: docs/playerinfo/avatars-re.md
# ([orig: CAvatarDefs_Init @ 0x57b180], [orig: CAvatarDefs_ParseConfigLine @ 0x57a3f0]).

const AvatarsDocumentScript = preload("res://modtools/avatar/avatars_document.gd")
const AvatarPreviewScript = preload("res://engine/avatar/avatar_preview.gd")
const TreeInspectorScript = preload("res://modtools/avatar/ui/inspectors/tree_inspector.gd")
const PartsInspectorScript = preload("res://modtools/avatar/ui/inspectors/parts_inspector.gd")
const CombosInspectorScript = preload("res://modtools/avatar/ui/inspectors/combos_inspector.gd")

# TREE is the default workflow; ids are stable (never renumber — tests pin them).
enum Workflow { TREE, PARTS, COMBOS }

var document  # AvatarsDocument
var _active_workflow_id: int = Workflow.TREE
var _preview  # AvatarPreview
var _mount: ViewportMount
# Preview guide visibility, driven by the shell's View settings. Stored here so a
# re-mounted preview (ViewportMount rebuilds it) inherits the current choice.
var _grid_visible := true
var _axes_visible := true


func _init() -> void:
	document = AvatarsDocumentScript.new()
	# The document re-emits state_changed for loads, in-place edits, and saves;
	# refresh the shell + active inspector when it does. No .bind(document):
	# binding the doc into its own signal's callable cycles the RefCounted pair.
	document.state_changed.connect(_on_document_state_changed)


func _on_document_state_changed() -> void:
	if editor_shell != null and editor_shell.has_method("sync_from_editor_state"):
		editor_shell.sync_from_editor_state()
	var inspector := get_workflow_inspector(_active_workflow_id)
	if inspector != null and inspector.has_method("refresh"):
		inspector.refresh()


# --- Identity -----------------------------------------------------------------

func get_workspace_id() -> String:
	return "avatar"


func get_workspace_label() -> String:
	return "Avatars"


func get_workspace_tooltip() -> String:
	return "Edit Avatars.def: head/body/arms characters by nationality and division."


func get_project_title() -> String:
	var name := "Avatars"
	if not document.current_path.is_empty():
		name = document.current_path.get_file().get_basename()
	return "%s%s" % [name, "*" if document.is_dirty else ""]


func get_status_tool() -> String:
	match _active_workflow_id:
		Workflow.PARTS:
			return "Parts"
		Workflow.COMBOS:
			return "Characters"
		_:
			return "Avatars"


func get_status_context() -> String:
	if document.resource == null or not document.resource.is_loaded():
		return "No Avatars.def loaded"
	var text := "%d part(s), %d nationalities" % [
		document.resource.get_part_count(), document.resource.get_nationality_count()]
	var issues: int = document.resource.get_diagnostics().size() if document.resource.has_method("get_diagnostics") else 0
	if issues > 0:
		text += ", %d issue%s" % [issues, "" if issues == 1 else "s"]
	return text


# --- Domain document (drives dirty/undo via the base) -------------------------

func get_editor_document() -> Object:
	return document


# The avatar database the inspectors read + the resource root the preview resolves
# part .3di files against.
func db():
	return document.resource


func get_resource_root() -> NovaResourceRoot:
	return _resource_root()


# --- Viewport -----------------------------------------------------------------

func _ensure_mount() -> ViewportMount:
	if _mount == null:
		_mount = ViewportMount.new(&"AvatarPreview", _create_preview)
	return _mount


func _create_preview() -> Control:
	_preview = AvatarPreviewScript.new()
	_preview.set_resource_root(_resource_root())
	_preview.set_grid_visible(_grid_visible)
	_preview.set_axes_visible(_axes_visible)
	# Show the first available combo once the preview exists.
	call_deferred("_show_default_combo")
	return _preview


func mount_viewport(mount: Control) -> void:
	if mount == null:
		return
	_ensure_mount().mount(mount)


func unmount_viewport(_released: Control) -> void:
	if _mount != null:
		_mount.unmount()


func release_viewport() -> void:
	if _mount != null:
		_mount.release()
	_preview = null


func get_viewport_camera() -> Camera3D:
	if _preview != null and _preview.has_method("get_editor_camera"):
		return _preview.get_editor_camera()
	return null


func shows_view_guides() -> bool:
	return true


func set_grid_visible(value: bool) -> void:
	_grid_visible = value
	if _preview != null:
		_preview.set_grid_visible(value)


func set_axes_visible(value: bool) -> void:
	_axes_visible = value
	if _preview != null:
		_preview.set_axes_visible(value)


# Resolve a (nat, div, combo) selection and show it in the preview. Called by the
# Tree inspector on combo selection and from the default-combo seed.
func show_combo(nat_index: int, div_index: int, combo_index: int) -> void:
	if _preview == null:
		return
	var database = db()
	if database == null or not database.is_loaded():
		_preview.clear()
		return
	var resolved: Dictionary = database.resolve_combo(nat_index, div_index, combo_index)
	if resolved.is_empty():
		_preview.clear()
		return
	_preview.load_combo(resolved)


# Pick the first nationality/division that has a combo and show it.
func _show_default_combo() -> void:
	if _preview == null:
		return
	var database = db()
	if database == null or not database.is_loaded():
		return
	for n in range(database.get_nationality_count()):
		for d in range(database.get_division_count(n)):
			if database.get_combo_count(n, d) > 0:
				show_combo(n, d, 0)
				return


# --- Workflow inspectors ------------------------------------------------------

func get_active_workflow_id() -> int:
	return _active_workflow_id


func activate_workflow(workflow_id: int) -> void:
	_active_workflow_id = workflow_id


func build_inspector(mount: Control) -> void:
	var inspector := get_workflow_inspector(Workflow.TREE)
	if inspector != null:
		inspector.build_main(mount)


func build_workflow_inspector(workflow_id: int, mount: Control) -> void:
	_active_workflow_id = workflow_id
	var inspector := get_workflow_inspector(workflow_id)
	if inspector == null:
		inspector = get_workflow_inspector(Workflow.TREE)
	if inspector != null:
		inspector.build_main(mount)


func _build_inspector_defs() -> Array:
	return [
		InspectorDef.make(Workflow.TREE, "Tree", "Browse nationalities, divisions, and characters.", TreeInspectorScript),
		InspectorDef.make(Workflow.PARTS, "Parts", "Edit head, body, and arms parts.", PartsInspectorScript),
		InspectorDef.make(Workflow.COMBOS, "Characters", "Edit the characters of a division.", CombosInspectorScript),
	]


# Avatars inspectors take the owning workspace in their constructor.
func _instantiate_inspector(def: InspectorDef) -> Object:
	return def.inspector_script.new(self)


# Commit an edited whole-model Dictionary back through the document: set_model
# emits the database's "changed" signal, which the document turns into dirty +
# state_changed (the editing bridge). Re-seeds the preview afterward.
func apply_model(model: Dictionary) -> void:
	if document.resource == null:
		return
	document.resource.set_model(model)
	_show_default_combo()


# Focus fields owned here: part (+ part_kind; -1 = head) selects a part in the
# Parts workflow; nat/div/combo selects a tree combo.
func focus_reference(focus: FocusPayload) -> Error:
	var database = db()
	if database == null or not database.is_loaded() or focus.is_empty():
		return OK
	if not focus.part.is_empty():
		var kind := focus.part_kind if focus.part_kind >= 0 else NovaAvatarDatabase.PART_HEAD
		var name := focus.part
		if database.get_part(kind, name).is_empty():
			return ERR_DOES_NOT_EXIST
		activate_workflow(Workflow.PARTS)
		var parts := get_workflow_inspector(Workflow.PARTS)
		if parts != null and parts.has_method("focus_part"):
			return parts.focus_part(kind, name)
		return OK
	if focus.nat >= 0 and focus.div >= 0 and focus.combo >= 0:
		var nat := focus.nat
		var div := focus.div
		var combo := focus.combo
		if nat >= database.get_nationality_count():
			return ERR_DOES_NOT_EXIST
		if div >= database.get_division_count(nat):
			return ERR_DOES_NOT_EXIST
		if combo >= database.get_combo_count(nat, div):
			return ERR_DOES_NOT_EXIST
		activate_workflow(Workflow.TREE)
		var tree := get_workflow_inspector(Workflow.TREE)
		if tree != null and tree.has_method("focus_combo"):
			tree.focus_combo(nat, div, combo)
		show_combo(nat, div, combo)
		return OK
	return OK


# --- Document actions ---------------------------------------------------------

func can_new() -> bool:
	return true


func get_new_action_label() -> String:
	return "New Avatars.def"


func new_current() -> Error:
	return document.create_new()


func can_open() -> bool:
	return true


func get_open_action_label() -> String:
	return "Open Avatars.def..."


func get_open_dialog_title() -> String:
	return "Open Avatars.def"


func get_open_dialog_filters() -> PackedStringArray:
	return PackedStringArray(["*.def,*.DEF ; Avatars"])


func get_open_dialog_dir() -> String:
	return document.get_last_open_dir()


func get_open_resource_kind() -> String:
	return "avatar"


func get_current_resource_path() -> String:
	return document.current_path


func open_file(path: String) -> Error:
	var vfs := _vfs_root_for_open(path)
	var err: Error
	if vfs != null:
		err = document.open_avatars_from_root(vfs, path)
	else:
		err = document.open_avatars(path)
	return err


func can_save() -> bool:
	return document.has_unsaved_changes() and not document.current_path.is_empty()


func get_save_action_label() -> String:
	return "Save Avatars.def"


func save_current() -> Error:
	if document.current_path.is_empty():
		return ERR_INVALID_PARAMETER
	return document.save_current()


func can_save_as() -> bool:
	return document.resource != null


func get_save_as_action_label() -> String:
	return "Save Avatars.def As..."


func save_as(dir_path: String) -> Error:
	return document.save_as(dir_path)


func get_save_dialog_title() -> String:
	return "Choose where to save Avatars.def"


func get_save_dialog_dir() -> String:
	return document.get_last_save_dir()
