extends ObjectListDetailInspector

## Anims workflow: load a model's .adm body-animation set, browse its clips with
## their metadata, and play or scrub the selected clip in the preview; the arms
## overlay (a second .3di riding the same skeleton) loads from here too. Promoted
## out of the PREVIEW smoke block — PREVIEW keeps playback/wire/collision only.

# How often the scrub slider follows the playhead while playing. Runs on a Timer
# parented to the detail dock (dies with it), paused unless playing — the debug
# overlay's sanctioned low-Hz pattern, never per-frame.
const FOLLOW_INTERVAL := 0.2

var _adm_name_edit: LineEdit
var _adm_status: Label
var _play_button: Button
var _scrub_slider: HSlider
var _time_label: Label
var _follow_timer: Timer
var _arms_status: Label
# A user drag owns the slider: the follow tick must not move it mid-gesture.
var _slider_dragging := false


# --- List hooks (clip keys + metadata rows) -------------------------------------

func _skeletal():
	return _preview.get_skeletal_anim() if _preview != null else null


func _list_items() -> Array:
	var sk = _skeletal()
	if sk == null:
		return []
	var keys: Array = []
	for k in sk.get_clip_keys():
		keys.append(String(k))
	return keys


func _list_item_text(item, _index: int) -> String:
	var key := String(item)
	var sk = _skeletal()
	if sk == null:
		return key
	var line := "%s — %d fr @ %.0f fps · %.2f s" % [key,
		sk.get_clip_frame_count(key), sk.get_clip_fps(key), sk.get_clip_length(key)]
	if sk.is_clip_looping(key):
		line += " · loops"
	return line


func _list_summary_text(count: int) -> String:
	if count == 0:
		return "No animation set loaded."
	var skinned: bool = _preview != null and _preview.has_skeleton()
	return "%d clip(s)%s" % [count, "" if skinned else "  (model has no skin to pose)"]


func _empty_detail_text() -> String:
	return "Load a .adm to play and scrub its clips."


func _list_node_name() -> StringName:
	return &"AnimsClipList"


func _list_panel_node_name() -> StringName:
	return &"AnimsListPanel"


func _detail_panel_node_name() -> StringName:
	return &"AnimsDetailPanel"


# --- Build -----------------------------------------------------------------------

func build_main(mount: Control) -> void:
	var box := _make_inspector_box(mount)
	_build_adm_row(box)
	_build_list_panel(box)


func build_detail(box: VBoxContainer) -> void:
	var detail_box := VBoxContainer.new()
	detail_box.name = _detail_panel_node_name()
	detail_box.add_theme_constant_override("separation", 8)
	detail_box.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	box.add_child(detail_box)
	var items := _list_items()
	if items.is_empty():
		_scrub_slider = null
		_play_button = null
		_time_label = null
		_follow_timer = null
		_add_empty_state(detail_box, _empty_detail_text(), "AnimsEmptyState")
	else:
		_selected_index = clampi(_selected_index, 0, items.size() - 1)
		_build_transport(detail_box)
	# The arms overlay loads regardless of a .adm: without one it renders static
	# at rest and rebinds when an animation set arrives (object_preview contract).
	_build_arms_rows(detail_box)
	_sync_transport_controls()


## Selecting a clip plays it and resyncs the transport in place — the detail
## dock (slider mid-drag included) is never torn down for a selection change.
func _resync_detail(index: int) -> bool:
	var items := _list_items()
	if _preview == null or index < 0 or index >= items.size():
		return false
	_preview.play_animation(String(items[index]))
	return _sync_transport_controls()


func refresh() -> void:
	# Resync in place; never rebuild the detail dock from a routine editor-state
	# sync (the DetailDockMount rebuild-only-on-real-change policy).
	_refresh_list()
	_sync_transport_controls()


# --- The .adm name row + status (moved from the PREVIEW smoke block) --------------

func _build_adm_row(box: VBoxContainer) -> void:
	var label := Label.new()
	label.text = "Skeletal animation (.adm)"
	box.add_child(label)

	var row := HBoxContainer.new()
	row.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	box.add_child(row)

	_adm_name_edit = LineEdit.new()
	_adm_name_edit.name = "AdmNameEdit"
	_adm_name_edit.placeholder_text = "model.adm"
	_adm_name_edit.text = _default_adm_name()
	_adm_name_edit.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	row.add_child(_adm_name_edit)

	var load_button := Button.new()
	load_button.name = "AdmLoadButton"
	load_button.text = "Load"
	row.add_child(load_button)

	_adm_status = Label.new()
	_adm_status.name = "AdmStatusLabel"
	_adm_status.theme_type_variation = &"Muted"
	_adm_status.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	box.add_child(_adm_status)

	# Load opens the in-app resource picker filtered to .adm (the chosen name
	# fills the field and loads); typing a name + Enter still works as a fallback.
	load_button.pressed.connect(func() -> void:
		_open_picker("Open .adm animation", _scan_resource_files(".adm"), func(picked: String) -> void:
			_adm_name_edit.text = picked.get_file()
			_do_load_adm(_adm_name_edit.text.strip_edges())
		)
	)
	_adm_name_edit.text_submitted.connect(func(_text: String) -> void:
		_do_load_adm(_adm_name_edit.text.strip_edges())
	)


# Bind a .adm to the previewed model (builds the Skeleton3D + Skin), refresh the
# clip list, and play the first clip. Shared by the picker and the Enter fallback.
func _do_load_adm(adm_name: String) -> void:
	if _preview == null:
		return
	var root: Variant = _ws.get_resource_root() if _ws != null and _ws.has_method("get_resource_root") else null
	var keys: PackedStringArray = _preview.load_animation_set(adm_name, root)
	if keys.is_empty():
		_adm_status.text = "No animations loaded: %s" % _preview.get_animation_error()
	else:
		var skinned: bool = _preview.has_skeleton()
		_adm_status.text = "%d clip(s) loaded%s" % [keys.size(), "" if skinned else "  (model has no skin to pose)"]
	_selected_index = 0
	_refresh_list()
	if not keys.is_empty():
		_preview.play_animation(String(keys[0]))
	_rebuild_detail_dock()


# --- Transport + scrub (detail dock) ----------------------------------------------

func _build_transport(detail_box: VBoxContainer) -> void:
	var transport := HBoxContainer.new()
	transport.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	detail_box.add_child(transport)

	_play_button = Button.new()
	_play_button.name = "AnimsPlayButton"
	_play_button.toggle_mode = true
	transport.add_child(_play_button)
	_play_button.toggled.connect(func(pressed: bool) -> void:
		if _preview != null:
			_preview.set_playing(pressed)
		_sync_transport_controls()
	)

	var reset := Button.new()
	reset.name = "AnimsResetButton"
	reset.text = "Reset"
	transport.add_child(reset)
	reset.pressed.connect(func() -> void:
		if _preview != null:
			_preview.reset_animation_time()
			_preview.set_animation_playhead(0.0)
		_sync_transport_controls()
	)

	var scrub_row := HBoxContainer.new()
	scrub_row.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	detail_box.add_child(scrub_row)

	_scrub_slider = HSlider.new()
	_scrub_slider.name = "AnimsScrubSlider"
	_scrub_slider.min_value = 0.0
	_scrub_slider.step = 0.01
	_scrub_slider.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_scrub_slider.size_flags_vertical = Control.SIZE_SHRINK_CENTER
	scrub_row.add_child(_scrub_slider)
	_scrub_slider.value_changed.connect(func(value: float) -> void:
		if _preview != null:
			_preview.set_animation_playhead(value)
		_update_time_label()
	)
	_scrub_slider.drag_started.connect(func() -> void: _slider_dragging = true)
	_scrub_slider.drag_ended.connect(func(_changed: bool) -> void: _slider_dragging = false)

	_time_label = Label.new()
	_time_label.name = "AnimsTimeLabel"
	_time_label.custom_minimum_size = Vector2(96, 0)
	scrub_row.add_child(_time_label)

	_follow_timer = Timer.new()
	_follow_timer.name = "AnimsFollowTimer"
	_follow_timer.wait_time = FOLLOW_INTERVAL
	_follow_timer.autostart = true
	_follow_timer.timeout.connect(_on_follow_tick)
	detail_box.add_child(_follow_timer)


func _selected_clip_key() -> String:
	var items := _list_items()
	if _selected_index < 0 or _selected_index >= items.size():
		return ""
	return String(items[_selected_index])


# Re-sync the transport controls in place from the preview's live state. Returns
# false when the controls are not built (callers fall back to a dock rebuild).
func _sync_transport_controls() -> bool:
	if _scrub_slider == null or not is_instance_valid(_scrub_slider) or _preview == null:
		return false
	var sk = _skeletal()
	var key := _selected_clip_key()
	var length: float = sk.get_clip_length(key) if sk != null and not key.is_empty() else 0.0
	var max_len := maxf(length, 0.01)
	# Park the value inside the NEW range (no signal) BEFORE setting max:
	# Range.set_max re-clamps the held value and EMITS value_changed, so a
	# shrink while the slider still holds the previous clip's position would
	# fire a phantom user scrub — a freshly selected one-shot clip would jump
	# straight to its final frame. (While a drag is in progress the clip cannot
	# have changed, so max is unchanged and set_max cannot clamp.)
	if not _slider_dragging:
		_scrub_slider.set_value_no_signal(clampf(_preview.get_animation_playhead(), 0.0, max_len))
	_scrub_slider.max_value = max_len
	var playing: bool = _preview.is_playing()
	_play_button.set_pressed_no_signal(playing)
	_play_button.text = "Pause" if playing else "Play"
	if _follow_timer != null and is_instance_valid(_follow_timer):
		_follow_timer.paused = not playing
	_update_time_label()
	return true


func _on_follow_tick() -> void:
	if _preview == null or not _preview.is_playing() or _slider_dragging:
		return
	if _scrub_slider == null or not is_instance_valid(_scrub_slider):
		return
	_scrub_slider.set_value_no_signal(_preview.get_animation_playhead())
	_update_time_label()


func _update_time_label() -> void:
	if _time_label == null or not is_instance_valid(_time_label) or _preview == null:
		return
	var sk = _skeletal()
	var key := _selected_clip_key()
	var length: float = sk.get_clip_length(key) if sk != null and not key.is_empty() else 0.0
	_time_label.text = "%.2f / %.2f s" % [_preview.get_animation_playhead(), length]


# --- Arms overlay rows (moved from the PREVIEW smoke block) ------------------------
# Load a SECOND .3di (e.g. ArmsG.3di) that rides the same .adm skeleton, so the
# first-person arms animate (and scrub) together with the model.

func _build_arms_rows(detail_box: VBoxContainer) -> void:
	var arms_row := HBoxContainer.new()
	arms_row.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	detail_box.add_child(arms_row)

	var arms_load := Button.new()
	arms_load.name = "ArmsLoadButton"
	arms_load.text = "Load Arms"
	arms_row.add_child(arms_load)

	var arms_clear := Button.new()
	arms_clear.name = "ArmsClearButton"
	arms_clear.text = "Clear"
	arms_row.add_child(arms_clear)

	_arms_status = Label.new()
	_arms_status.name = "ArmsStatusLabel"
	_arms_status.theme_type_variation = &"Muted"
	_arms_status.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_arms_status.clip_text = true
	arms_row.add_child(_arms_status)
	if _preview != null and _preview.has_arms():
		_arms_status.text = "Arms loaded"

	arms_load.pressed.connect(func() -> void:
		_open_picker("Open arms .3di", _scan_resource_files(".3di"), func(picked: String) -> void:
			if _preview == null:
				return
			var root: Variant = _ws.get_resource_root() if _ws != null and _ws.has_method("get_resource_root") else null
			if _preview.load_arms(picked.get_file(), root):
				_arms_status.text = "Arms: %s" % picked.get_file()
			else:
				_arms_status.text = "Arms failed: %s" % _preview.get_arms_error()
		)
	)
	arms_clear.pressed.connect(func() -> void:
		if _preview != null:
			_preview.clear_arms()
		_arms_status.text = ""
	)


# --- Pickers / helpers (moved from the PREVIEW smoke block) -------------------------

# Open the in-app resource picker (via the workspace's shell seam) over an
# explicit, scoped file list — used for .adm (which the resource index does not
# register) and the arms .3di. No-op headless (keeps tests safe).
func _open_picker(title: String, files: PackedStringArray, on_pick: Callable) -> void:
	if _ws != null:
		_ws.open_file_picker(title, files, on_pick)


# Flat (top-level) basenames under the mounted resource root with the given
# suffix, for the scoped picker. .adm is not indexed, so we scan the directory
# directly; the loaders read by basename.
func _scan_resource_files(suffix: String) -> PackedStringArray:
	var out := PackedStringArray()
	var root: Variant = _ws.get_resource_root() if _ws != null and _ws.has_method("get_resource_root") else null
	if root == null:
		return out
	var dir := String(root.get_root_dir())
	if dir.is_empty():
		return out
	var lower := suffix.to_lower()
	for f in DirAccess.get_files_at(dir):
		if String(f).to_lower().ends_with(lower):
			out.push_back(f)
	out.sort()
	return out


# Best-guess .adm name for the loaded model: its basename + ".adm" (the
# convention an item .def follows — graphic "US01" / anim_def "US01").
func _default_adm_name() -> String:
	if object_editor == null or object_editor.object_data == null:
		return ""
	var name := String(object_editor.object_data.get_object_name()).get_file().get_basename().strip_edges()
	return "" if name.is_empty() or name == "untitled" else name + ".adm"
