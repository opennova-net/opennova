class_name HudPreviewWorkspace
extends EditorWorkspace

## Read-only ONED workspace that previews the in-game HUD layout from hudpos.def.
## It opens a hudpos.def (loose or via the mounted VFS), renders the witnessed HUD
## elements at their design-space positions in a 2D preview, and lists the parsed
## values in a read-only inspector. There is no editing or saving — hudpos.def
## authoring is out of scope. Model + element semantics: docs/interface/hud-re.md.

const HudLayoutPreviewScript = preload("res://modtools/hud/hud_preview.gd")

var _hudpos: NovaHudPos
var _path: String = ""
var _preview: HudLayoutPreview
var _inspector_root: Control


func _init() -> void:
	_hudpos = NovaHudPos.new()


# --- Identity ---
func get_workspace_id() -> String:
	return "hud"


func get_workspace_label() -> String:
	return "HUD"


func get_workspace_tooltip() -> String:
	return "Preview the in-game HUD layout from hudpos.def: element positions, colors, stances, fonts."


func get_project_title() -> String:
	if _path.is_empty():
		return "hudpos"
	return _path.get_file().get_basename()


func get_status_tool() -> String:
	return "HUD"


func get_status_context() -> String:
	if _hudpos == null or not _hudpos.is_loaded():
		return ""
	return "%d stance(s)" % _hudpos.get_stances().size()


# --- Viewport (the 2D HUD preview; a Control, not a 3D view) ---
func mount_viewport(mount: Control) -> void:
	if mount == null:
		return
	if _preview == null:
		_preview = HudLayoutPreviewScript.new()
		_preview.set_anchors_preset(Control.PRESET_FULL_RECT)
		_preview.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		_preview.size_flags_vertical = Control.SIZE_EXPAND_FILL
	if _preview.get_parent() == null:
		mount.add_child(_preview)
		_preview.set_anchors_preset(Control.PRESET_FULL_RECT)
	_refresh_preview()


func unmount_viewport(_released: Control) -> void:
	if _preview != null and _preview.get_parent() != null:
		_preview.get_parent().remove_child(_preview)


func release_viewport() -> void:
	if _inspector_root != null and is_instance_valid(_inspector_root):
		_inspector_root.queue_free()
		_inspector_root = null
	if _preview != null and _preview.get_parent() != null:
		_preview.get_parent().remove_child(_preview)
	if _preview != null:
		_preview.free()
		_preview = null


func _refresh_preview() -> void:
	if _preview != null:
		_preview.set_source(_hudpos, _resource_root())


# --- Inspector (read-only) ---
func build_inspector(mount: Control) -> void:
	if _inspector_root != null and is_instance_valid(_inspector_root):
		_inspector_root.queue_free()
	_inspector_root = _make_inspector()
	mount.add_child(_inspector_root)
	_populate_inspector()


func _make_inspector() -> Control:
	var margin := MarginContainer.new()
	for side in ["left", "right", "top", "bottom"]:
		margin.add_theme_constant_override("margin_%s" % side, 12)
	margin.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	margin.size_flags_vertical = Control.SIZE_EXPAND_FILL

	var box := VBoxContainer.new()
	box.name = "Box"
	box.add_theme_constant_override("separation", 6)
	box.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	margin.add_child(box)

	var title := Label.new()
	title.name = "Title"
	title.theme_type_variation = &"Heading"
	title.clip_text = true
	box.add_child(title)

	var detail := Label.new()
	detail.name = "Detail"
	detail.theme_type_variation = &"Muted"
	detail.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	box.add_child(detail)
	return margin


func _populate_inspector() -> void:
	if _inspector_root == null or not is_instance_valid(_inspector_root):
		return
	var box := _inspector_root.get_node_or_null("Box")
	if box == null:
		return
	var title: Label = box.get_node("Title")
	var detail: Label = box.get_node("Detail")

	if _hudpos == null or not _hudpos.is_loaded():
		title.text = "No HUD loaded"
		detail.text = "Open a hudpos.def to inspect its layout."
		return

	title.text = get_project_title()
	var stances := _hudpos.get_stances()
	var stance_names := PackedStringArray()
	for s in stances:
		stance_names.append(String((s as Dictionary).get("name", "?")))
	var health := _hudpos.get_health_rect()
	var spin := _hudpos.get_spinmap_bounds()
	var lines := PackedStringArray()
	lines.append("Design space: %d x %d" % [NovaHudPos.DESIGN_WIDTH, NovaHudPos.DESIGN_HEIGHT])
	lines.append("Fonts: %s / %s" % [
		_or_dash(_hudpos.get_font_hi()), _or_dash(_hudpos.get_font_lo())])
	lines.append("Health rect: %d,%d %dx%d" % [health.position.x, health.position.y, health.size.x, health.size.y])
	lines.append("Stance anchor: %d,%d" % [_hudpos.get_stance_pos().x, _hudpos.get_stance_pos().y])
	lines.append("Radar bounds: %d,%d %dx%d" % [spin.position.x, spin.position.y, spin.size.x, spin.size.y])
	lines.append("Stances (%d): %s" % [stances.size(), ", ".join(stance_names)])
	lines.append("Static frames: %d" % _hudpos.get_static_frames().size())
	detail.text = "\n".join(lines)


func _or_dash(s: String) -> String:
	return s if not s.is_empty() else "-"


# --- Open (read-only; no new/save/export — base defaults keep those off) ---
func can_open() -> bool:
	return true


func get_open_action_label() -> String:
	return "Open hudpos.def..."


func get_open_dialog_title() -> String:
	return "Open hudpos.def"


func get_open_dialog_filters() -> PackedStringArray:
	return PackedStringArray(["*.def ; HUD position definitions"])


func get_open_resource_kind() -> String:
	return "hudpos"


func get_current_resource_path() -> String:
	return _path


func open_file(path: String) -> Error:
	var err: Error
	var vfs := _vfs_root_for_open(path)
	if vfs != null:
		err = _hudpos.load_from_resource_root(vfs, path)
		if err == OK:
			_path = _vfs_display_path(vfs, path)
	else:
		err = _hudpos.load(path)
		if err == OK:
			_path = path
	if err == OK:
		_refresh_preview()
		_populate_inspector()
	return err
