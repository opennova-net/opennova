extends Control

## Left pane for the Sound workspace: a per-selection property form. Three
## persistent panels (Set / Layer / Member) are built once; refresh() shows the
## one matching the controller's current selection and repopulates it in place via
## FieldBinder (so a focused SpinBox/LineEdit is not clobbered mid-keystroke).
## Every edit routes through the controller's silent live setters, which coalesce
## into one undo step per editing burst.

const SEL_NONE := 0
const SEL_SET := 1
const SEL_LAYER := 2
const SEL_MEMBER := 3


const MODE_OPTIONS := [
	{"id": 0, "label": "First"},
	{"id": 1, "label": "Random"},
	{"id": 2, "label": "Sequential"},
	{"id": 3, "label": "Random sequential"},
]

# Layer flag bools surfaced as checkboxes, in display order.
const LAYER_FLAGS := ["looping", "directional", "reverb", "heading", "preload", "stoppable", "internal", "external", "rapid"]

var _ws  # SoundEditorWorkspace
var _doc: SoundController

var _empty: Label
var _set_panel: VBoxContainer
var _layer_panel: VBoxContainer
var _member_panel: VBoxContainer

var _set_binder: FieldBinder
var _layer_binder: FieldBinder
var _member_binder: FieldBinder

var _files: FileDialogHelper


func setup(workspace) -> void:
	_ws = workspace
	_doc = workspace.get_document()
	if is_inside_tree():
		_build()


func _ready() -> void:
	if _ws != null:
		_build()


func _build() -> void:
	if _set_panel != null:
		return
	for c in get_children():
		c.queue_free()

	var box := InspectorForms.make_inspector_box(self)
	(box.get_parent().get_parent() as Control).set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)

	_empty = InspectorForms.add_empty_state(box, "Select a sound set, layer, or member.", "SoundInspectorEmpty")

	_build_set_panel(box)
	_build_layer_panel(box)
	_build_member_panel(box)
	refresh()


# --- Set panel ---

func _build_set_panel(box: VBoxContainer) -> void:
	_set_panel = VBoxContainer.new()
	_set_panel.name = "SoundSetPanel"
	box.add_child(_set_panel)
	InspectorForms.add_section_heading(_set_panel, "Sound Set")
	_set_binder = FieldBinder.new()

	var name_field := InspectorForms.add_detail_field(_set_panel, "Name")
	var name_edit := LineEdit.new()
	name_edit.name = "SoundSetName"
	name_edit.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	name_field.add_child(name_edit)
	_set_binder.bind_line(name_edit,
		func(info): return String(info.get("name", "")),
		func(v): _apply_set("name", v))

	var target := InspectorForms.add_spin_row(_set_panel, "SoundSetTargetId", "Target id", 0, 4294967295, 1)
	_set_binder.bind_spin(target,
		func(info): return float(info.get("target_id", 0)),
		func(v): _apply_set("target_id", int(v)))


# --- Layer panel ---

func _build_layer_panel(box: VBoxContainer) -> void:
	_layer_panel = VBoxContainer.new()
	_layer_panel.name = "SoundLayerPanel"
	box.add_child(_layer_panel)
	InspectorForms.add_section_heading(_layer_panel, "Layer")
	_layer_binder = FieldBinder.new()

	var mode_field := InspectorForms.add_detail_field(_layer_panel, "Selection mode")
	var mode := OptionButton.new()
	mode.name = "SoundLayerMode"
	mode.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	mode_field.add_child(mode)
	_layer_binder.bind_option(mode,
		func(info): return int(info.get("selection_mode", 0)),
		func(v): _apply_layer("selection_mode", int(v)),
		func(): return MODE_OPTIONS)

	var falloff := InspectorForms.add_spin_row(_layer_panel, "SoundLayerFalloffRadius", "Falloff radius", 0, 65535, 1)
	_layer_binder.bind_spin(falloff,
		func(info): return float(info.get("falloff_radius", 0)),
		func(v): _apply_layer("falloff_radius", int(v)))

	var min_dist := InspectorForms.add_spin_row(_layer_panel, "SoundLayerMinDistance", "Min distance", 0, 65535, 1)
	_layer_binder.bind_spin(min_dist,
		func(info): return float(info.get("min_distance", 0)),
		func(v): _apply_layer("min_distance", int(v)))

	InspectorForms.add_section_heading(_layer_panel, "Flags")
	for flag: String in LAYER_FLAGS:
		var cb := InspectorForms.add_checkbox(_layer_panel, "SoundLayerFlag_" + flag, flag.capitalize())
		var key: String = flag
		_layer_binder.bind_checkbox(cb,
			func(info): return bool(info.get(key, false)),
			func(v): _apply_layer(key, v))


# --- Member panel ---

func _build_member_panel(box: VBoxContainer) -> void:
	_member_panel = VBoxContainer.new()
	_member_panel.name = "SoundMemberPanel"
	box.add_child(_member_panel)
	InspectorForms.add_section_heading(_member_panel, "Member")
	_member_binder = FieldBinder.new()

	var wav_field := InspectorForms.add_detail_field(_member_panel, "Wave file")
	var wav_row := HBoxContainer.new()
	wav_row.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	wav_field.add_child(wav_row)
	var wav_edit := LineEdit.new()
	wav_edit.name = "SoundMemberWav"
	wav_edit.placeholder_text = "filename.wav"
	wav_edit.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	wav_row.add_child(wav_edit)
	_member_binder.bind_line(wav_edit,
		func(info): return String(info.get("wav_path", "")),
		func(v): _apply_member("wav_path", v))
	var browse := Button.new()
	browse.text = "..."
	browse.focus_mode = Control.FOCUS_NONE
	browse.pressed.connect(_on_browse_wav)
	wav_row.add_child(browse)

	var pitch := InspectorForms.add_spin_row(_member_panel, "SoundMemberPitch", "Base pitch", 0.0, 4.0, 0.01)
	_member_binder.bind_spin(pitch,
		func(info): return float(info.get("base_pitch", 1.0)),
		func(v): _apply_member("base_pitch", float(v)))

	var rand_pitch := InspectorForms.add_spin_row(_member_panel, "SoundMemberRandPitch", "Random pitch", 0.0, 4.0, 0.01)
	_member_binder.bind_spin(rand_pitch,
		func(info): return float(info.get("rand_pitch", 0.0)),
		func(v): _apply_member("rand_pitch", float(v)))

	var volume := InspectorForms.add_spin_row(_member_panel, "SoundMemberVolume", "Volume", 0, 255, 1)
	_member_binder.bind_spin(volume,
		func(info): return float(info.get("volume", 255)),
		func(v): _apply_member("volume", int(v)))

	var clamp := InspectorForms.add_spin_row(_member_panel, "SoundMemberClampVolume", "Clamp volume", 0, 255, 1)
	_member_binder.bind_spin(clamp,
		func(info): return float(info.get("clamp_volume", 255)),
		func(v): _apply_member("clamp_volume", int(v)))

	var play := Button.new()
	play.name = "SoundMemberPlay"
	play.text = "▶ Play"
	play.pressed.connect(_on_play_member)
	_member_panel.add_child(play)


# --- Refresh ---

func refresh() -> void:
	if _set_panel == null:
		return
	if _doc == null and _ws != null:
		_doc = _ws.get_document()
	_set_binder.reset_guard()
	_layer_binder.reset_guard()
	_member_binder.reset_guard()

	var sel := _doc.get_selection() if _doc != null else {"kind": SEL_NONE}
	var kind := int(sel.get("kind", SEL_NONE))
	_set_panel.visible = kind == SEL_SET
	_layer_panel.visible = kind == SEL_LAYER
	_member_panel.visible = kind == SEL_MEMBER
	_empty.visible = kind == SEL_NONE

	match kind:
		SEL_SET:
			_set_binder.sync_from(_doc.data.get_set(int(sel["set"])))
		SEL_LAYER:
			_layer_binder.sync_from(_doc.data.get_layer(int(sel["set"]), int(sel["layer"])))
		SEL_MEMBER:
			_member_binder.sync_from(_doc.data.get_member(int(sel["set"]), int(sel["layer"]), int(sel["member"])))


# --- Apply (read live selection so the reused panels target the current node) ---

func _apply_set(key: String, value: Variant) -> void:
	var sel := _doc.get_selection()
	if int(sel.get("kind", SEL_NONE)) == SEL_SET:
		_doc.set_set_field_live(int(sel["set"]), key, value)


func _apply_layer(key: String, value: Variant) -> void:
	var sel := _doc.get_selection()
	if int(sel.get("kind", SEL_NONE)) == SEL_LAYER:
		_doc.set_layer_field_live(int(sel["set"]), int(sel["layer"]), key, value)


func _apply_member(key: String, value: Variant) -> void:
	var sel := _doc.get_selection()
	if int(sel.get("kind", SEL_NONE)) == SEL_MEMBER:
		_doc.set_member_field_live(int(sel["set"]), int(sel["layer"]), int(sel["member"]), key, value)


func _on_play_member() -> void:
	var sel := _doc.get_selection()
	if int(sel.get("kind", SEL_NONE)) == SEL_MEMBER and _ws != null:
		_ws.preview_member(int(sel["set"]), int(sel["layer"]), int(sel["member"]))


# --- Wave file browse ---

func _on_browse_wav() -> void:
	if _files == null:
		_files = FileDialogHelper.new(self)
	var dir := ""
	var root = _ws.get_resource_root() if _ws != null else null
	if root != null and root.has_method("get_root_dir"):
		dir = String(root.get_root_dir())
	_files.open("Choose a wave file", PackedStringArray(["*.wav,*.WAV ; Wave files"]), _on_wav_selected, dir)


func _on_wav_selected(path: String) -> void:
	# The .lwf string pool stores the bare filename; resolution is by name via the VFS.
	_apply_member("wav_path", path.get_file())
	refresh()
