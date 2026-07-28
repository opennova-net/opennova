class_name SoundController
extends "res://modtools/editor/editor_document.gd"

## Authoring controller for a NovaLogic .lwf sound profile.
##
## Holds a single NovaLwfData for its whole lifetime and mutates it in place
## (open/new/undo reload the same object) so observers connect once. Undo/redo
## uses byte snapshots of the profile (NovaLwfData.to_bytes() <-> load_bytes),
## reusing the byte-exact-on-unmodified encoder in libs/lwf.
##
## Change channels mirror the Strings workspace:
##   - structure_changed: sets/layers/members added/removed/reordered, or a
##     open/new/undo/redo. Observers do a full Tree rebuild.
##   - selection_changed: the selected node moved. The inspector repopulates its
##     panel; the Tree highlights. No rebuild.
##   - edited: an editing session committed or a save changed dirtiness. Title-only.
## Scalar field edits are silent (model + dirty only) and are bracketed by
## begin_edit()/commit_edit() so one editing burst becomes one undo step.

signal structure_changed
signal selection_changed
signal edited

const DEFAULT_FILENAME := "sound.lwf"

# Selection node kinds.
const SEL_NONE := 0
const SEL_SET := 1
const SEL_LAYER := 2
const SEL_MEMBER := 3

var data: NovaLwfData

# Current selection: { kind:int, set:int, layer:int, member:int }. kind == SEL_NONE
# means nothing selected (set/layer/member are -1).
var selection: Dictionary = {"kind": SEL_NONE, "set": -1, "layer": -1, "member": -1}


func _init() -> void:
	data = NovaLwfData.new()
	data.create_empty()


# --- Lifecycle ---

func new_profile(mark_dirty_state: bool = true) -> void:
	data.create_empty()
	# Seed one default set + layer so the Tree is never empty and the inspector
	# has something to show.
	var si := data.add_set()
	data.add_layer(si)
	data.mark_clean()
	_select(SEL_SET, si, -1, -1, false)
	set_current_path("")
	clear_history()
	is_dirty = mark_dirty_state
	structure_changed.emit()


func open_lwf(path: String) -> Error:
	var err := data.open_file(path)
	if err != OK:
		return err
	return _finish_open(path)


func open_lwf_bytes(bytes: PackedByteArray, display_path: String) -> Error:
	if not data.load_bytes(bytes):
		return ERR_FILE_CORRUPT
	return _finish_open(display_path)


func _finish_open(path: String) -> Error:
	if data.get_set_count() > 0:
		_select(SEL_SET, 0, -1, -1, false)
	else:
		_select(SEL_NONE, -1, -1, -1, false)
	set_current_path(path)
	remember_open_path(path)
	clear_history()
	mark_clean()
	structure_changed.emit()
	return OK


func save_current() -> Error:
	if current_path.is_empty() or current_path.get_extension().to_lower() != "lwf":
		return ERR_INVALID_PARAMETER
	flush_edit()
	var err := data.save_file(current_path)
	if err == OK:
		mark_clean()
		edited.emit()
	return err


func save_as(dir_path: String) -> Error:
	if dir_path.is_empty():
		return ERR_INVALID_PARAMETER
	flush_edit()
	var mkdir_err := DirAccess.make_dir_recursive_absolute(dir_path)
	if mkdir_err != OK:
		return mkdir_err
	var path := dir_path.path_join(_export_filename())
	var err := data.save_file(path)
	if err == OK:
		set_current_path(path)
		remember_save_dir(dir_path)
		mark_clean()
		edited.emit()
	return err


func get_project_title() -> String:
	var name := current_path.get_file()
	if name.is_empty():
		name = "untitled.lwf"
	return "%s%s" % [name, "*" if is_dirty else ""]


func get_status_context() -> String:
	if data == null:
		return "No sound profile open."
	return "%d sound sets" % data.get_set_count()


# --- Selection ---

func get_selection() -> Dictionary:
	return selection.duplicate()

func select_set(si: int) -> void:
	_select(SEL_SET, si, -1, -1, true)

func select_layer(si: int, li: int) -> void:
	_select(SEL_LAYER, si, li, -1, true)

func select_member(si: int, li: int, mi: int) -> void:
	_select(SEL_MEMBER, si, li, mi, true)


func _select(kind: int, si: int, li: int, mi: int, emit: bool) -> void:
	flush_edit()
	selection = {"kind": kind, "set": si, "layer": li, "member": mi}
	if emit:
		selection_changed.emit()


# --- Structural mutations (one undo step each; observers rebuild) ---

func add_set() -> int:
	flush_edit()
	record_undo_step()
	var idx := data.add_set()
	mark_dirty()
	_select(SEL_SET, idx, -1, -1, false)
	structure_changed.emit()
	return idx


func remove_set(si: int) -> void:
	if si < 0 or si >= data.get_set_count():
		return
	flush_edit()
	record_undo_step()
	data.remove_set(si)
	mark_dirty()
	_clamp_selection()
	structure_changed.emit()


func move_set(from_index: int, to_index: int) -> void:
	flush_edit()
	record_undo_step()
	data.move_set(from_index, to_index)
	mark_dirty()
	_select(SEL_SET, clampi(to_index, 0, maxi(0, data.get_set_count() - 1)), -1, -1, false)
	structure_changed.emit()


func add_layer(si: int) -> int:
	if si < 0 or si >= data.get_set_count():
		return -1
	flush_edit()
	record_undo_step()
	var idx := data.add_layer(si)
	mark_dirty()
	_select(SEL_LAYER, si, idx, -1, false)
	structure_changed.emit()
	return idx


func remove_layer(si: int, li: int) -> void:
	flush_edit()
	record_undo_step()
	data.remove_layer(si, li)
	mark_dirty()
	_clamp_selection()
	structure_changed.emit()


func move_layer(si: int, from_index: int, to_index: int) -> void:
	flush_edit()
	record_undo_step()
	data.move_layer(si, from_index, to_index)
	mark_dirty()
	structure_changed.emit()


func add_member(si: int, li: int) -> int:
	if si < 0 or li < 0:
		return -1
	flush_edit()
	record_undo_step()
	var idx := data.add_member(si, li)
	mark_dirty()
	_select(SEL_MEMBER, si, li, idx, false)
	structure_changed.emit()
	return idx


func remove_member(si: int, li: int, mi: int) -> void:
	flush_edit()
	record_undo_step()
	data.remove_member(si, li, mi)
	mark_dirty()
	_clamp_selection()
	structure_changed.emit()


func move_member(si: int, li: int, from_index: int, to_index: int) -> void:
	flush_edit()
	record_undo_step()
	data.move_member(si, li, from_index, to_index)
	mark_dirty()
	structure_changed.emit()


# --- Scalar editing session (silent: model + dirty only) ---

# Editing session + undo/redo: the shared EditorDocument snapshot history.
# The hooks supply this domain's snapshot shape (NovaLwfData bytes) + signals.

func _snapshot() -> Variant:
	return data.to_bytes() if data != null else null


func _apply_snapshot(snap: Variant) -> void:
	data.load_bytes(snap)


func _history_applied(kind: String) -> void:
	if kind == "commit":
		edited.emit()
	elif kind == "undo" or kind == "redo":
		_clamp_selection()
		structure_changed.emit()


func set_set_field_live(si: int, key: String, value: Variant) -> void:
	# Skip no-op writes: applying an unchanged value would flip NovaLwfData to its
	# canonical (non-byte-exact) encode path and spuriously dirty the document.
	var cur := data.get_set(si)
	if cur.is_empty() or cur.get(key) == value:
		return
	begin_edit()
	data.set_set_field(si, key, value)
	mark_dirty()


func set_layer_field_live(si: int, li: int, key: String, value: Variant) -> void:
	var cur := data.get_layer(si, li)
	if cur.is_empty() or cur.get(key) == value:
		return
	begin_edit()
	data.set_layer_field(si, li, key, value)
	mark_dirty()


func set_member_field_live(si: int, li: int, mi: int, key: String, value: Variant) -> void:
	var cur := data.get_member(si, li, mi)
	if cur.is_empty() or cur.get(key) == value:
		return
	begin_edit()
	data.set_member_field(si, li, mi, key, value)
	mark_dirty()


# --- Undo / redo ---

# --- Internal ---

func _export_filename() -> String:
	if not current_path.is_empty():
		return current_path.get_file()
	return DEFAULT_FILENAME


# Re-clamp the selection to still-valid indices after a removal / reload, then
# emit selection_changed so the inspector repopulates.
func _clamp_selection() -> void:
	var set_count := data.get_set_count()
	if set_count == 0:
		selection = {"kind": SEL_NONE, "set": -1, "layer": -1, "member": -1}
		selection_changed.emit()
		return
	var si := clampi(int(selection.get("set", 0)), 0, set_count - 1)
	var kind := int(selection.get("kind", SEL_NONE))
	if kind == SEL_NONE:
		kind = SEL_SET
	var li := int(selection.get("layer", -1))
	var mi := int(selection.get("member", -1))
	var layer_count := data.get_layer_count(si)
	if kind >= SEL_LAYER:
		if layer_count == 0:
			kind = SEL_SET
			li = -1
			mi = -1
		else:
			li = clampi(li, 0, layer_count - 1)
			if kind == SEL_MEMBER:
				var member_count := data.get_member_count(si, li)
				if member_count == 0:
					kind = SEL_LAYER
					mi = -1
				else:
					mi = clampi(mi, 0, member_count - 1)
	selection = {"kind": kind, "set": si, "layer": li, "member": mi}
	selection_changed.emit()
