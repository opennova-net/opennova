class_name ResourceTable
extends VBoxContainer
## The embeddable resource list: a search box over a sortable multi-column
## Tree (Name / Type / Size / Modified). Extracted from the modal resource
## browser so the persistent browser pane and the picker dialog share one
## implementation. Pure list-shaped state: callers push entries in
## (set_entries) and listen for entry_activated / selection_changed /
## list_changed; directory labels, hints, and pick semantics stay caller-side.
##
## Node names are load-bearing: the shell's tests resolve the Tree as
## "ResourceBrowserList" and the search box as "ResourceBrowserSearch".

signal entry_activated(entry: Dictionary)
signal selection_changed(has_selection: bool)
## Emitted after every rebuild with the post-filter row count, so owners can
## drive hint/empty-state copy without reaching into the Tree.
signal list_changed(visible_count: int)

# Tint applied to the row whose resource is the workspace's currently-open
# file, in place of a "(open)" text suffix.
const OPEN_ROW_COLOR := Color(0.49, 0.73, 1.0)
const COLUMN_TITLES: PackedStringArray = ["Name", "Type", "Size", "Modified"]
const COLUMN_NAME := 0
const COLUMN_TYPE := 1
const COLUMN_SIZE := 2
const COLUMN_MODIFIED := 3

var search: LineEdit
var tree: Tree

var _entries: Array = []
var _visible_entries: Array = []
var _current_path := ""
var _sort_column := COLUMN_NAME
var _sort_ascending := true
var _drag_payload_provider := Callable()


func _init() -> void:
	add_theme_constant_override("separation", 10)
	size_flags_horizontal = Control.SIZE_EXPAND_FILL
	size_flags_vertical = Control.SIZE_EXPAND_FILL

	search = SearchField.new("Search resources")
	search.name = "ResourceBrowserSearch"
	search.search_changed.connect(func(_text: String) -> void: refresh())
	# Enter activates the selected row; Down arrow drops focus into the list.
	search.text_submitted.connect(_on_search_submitted)
	search.gui_input.connect(_on_search_gui_input)
	add_child(search)

	tree = Tree.new()
	tree.name = "ResourceBrowserList"
	tree.columns = COLUMN_TITLES.size()
	tree.column_titles_visible = true
	tree.hide_root = true
	tree.select_mode = Tree.SELECT_ROW
	# Our LineEdit owns search; the Tree's own incremental type-search would
	# only fight it for keystrokes once the list has focus.
	tree.allow_search = false
	tree.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	tree.size_flags_vertical = Control.SIZE_EXPAND_FILL
	# Name takes the slack; the metadata columns size to their content.
	tree.set_column_expand(COLUMN_NAME, true)
	tree.set_column_clip_content(COLUMN_NAME, true)
	tree.set_column_expand(COLUMN_TYPE, false)
	tree.set_column_expand(COLUMN_SIZE, false)
	tree.set_column_expand(COLUMN_MODIFIED, false)
	tree.set_column_custom_minimum_width(COLUMN_TYPE, 64)
	tree.set_column_custom_minimum_width(COLUMN_SIZE, 96)
	tree.set_column_custom_minimum_width(COLUMN_MODIFIED, 156)
	tree.set_column_title_alignment(COLUMN_SIZE, HORIZONTAL_ALIGNMENT_RIGHT)
	tree.item_selected.connect(func() -> void:
		selection_changed.emit(tree.get_selected() != null))
	tree.item_activated.connect(_on_item_activated)
	tree.column_title_clicked.connect(_on_column_title_clicked)
	add_child(tree)


## Push the entry list (resource-index row dictionaries) and the path of the
## currently-open file (its row is tinted); rebuilds immediately.
func set_entries(entries: Array, current_path: String = "") -> void:
	_entries = entries
	_current_path = current_path
	refresh()


func refresh() -> void:
	tree.clear()
	_visible_entries = []
	var needle := search.text.strip_edges().to_lower()
	for entry_value in _entries:
		var entry := entry_value as Dictionary
		var display_name := String(entry.get("display_name", ""))
		var relative_path := String(entry.get("relative_path", ""))
		var haystack := ("%s %s" % [display_name, relative_path]).to_lower()
		if not needle.is_empty() and not haystack.contains(needle):
			continue
		_visible_entries.append(entry)
	_sort_visible_entries()
	_update_column_titles()

	var root_item := tree.create_item()
	var first_item: TreeItem = null
	var open_item: TreeItem = null
	for entry_value in _visible_entries:
		var entry := entry_value as Dictionary
		var item := tree.create_item(root_item)
		_populate_row(item, entry)
		item.set_metadata(COLUMN_NAME, entry)
		if first_item == null:
			first_item = item
		if open_item == null and not _current_path.is_empty() \
				and _same_filesystem_path(String(entry.get("path", "")), _current_path):
			_mark_open_row(item)
			open_item = item

	# Default selection so the keyboard flow works without a click: prefer the
	# currently-open file, else the first row.
	var to_select := open_item if open_item != null else first_item
	if to_select != null:
		to_select.select(COLUMN_NAME)
		tree.scroll_to_item(to_select)
	selection_changed.emit(to_select != null)
	list_changed.emit(_visible_entries.size())


func get_selected_entry() -> Dictionary:
	var item := tree.get_selected()
	if item == null:
		return {}
	var entry := item.get_metadata(COLUMN_NAME) as Dictionary
	return entry if entry != null else {}


## Make rows draggable: the provider receives a row's entry Dictionary and
## returns its drag data (or null for non-draggable rows). Opt-in only - the
## persistent pane calls this; the modal picker never does, so a dialog row
## can't start a system drag out from under its input grab.
func enable_drag_source(payload_provider: Callable) -> void:
	_drag_payload_provider = payload_provider
	tree.set_drag_forwarding(_get_tree_drag_data, Callable(), Callable())


func _get_tree_drag_data(at_position: Vector2) -> Variant:
	if not _drag_payload_provider.is_valid():
		return null
	var item := tree.get_item_at_position(at_position)
	if item == null:
		# A LIVE drag gesture that misses every row (column titles, the blank
		# area below the list) must stay inert - falling back to the selection
		# there would start a drag of a row the user never grabbed. The
		# selected-row fallback only serves headless tests, which call this
		# directly (no GUI drag in flight) with Vector2.ZERO.
		if get_viewport() != null and get_viewport().gui_is_dragging():
			return null
		item = tree.get_selected()
	if item == null:
		return null
	var entry := item.get_metadata(COLUMN_NAME) as Dictionary
	if entry == null or entry.is_empty():
		return null
	var data: Variant = _drag_payload_provider.call(entry)
	if data == null:
		return null
	# Previews only attach during a live GUI drag; tests call this directly.
	if get_viewport() != null and get_viewport().gui_is_dragging():
		var preview := Label.new()
		preview.text = String(entry.get("display_name", ""))
		tree.set_drag_preview(preview)
	return data


func get_visible_count() -> int:
	return _visible_entries.size()


func clear_search() -> void:
	search.text = ""


func focus_search() -> void:
	search.call_deferred("grab_focus")


func _populate_row(item: TreeItem, entry: Dictionary) -> void:
	var display_name := String(entry.get("display_name", ""))
	var relative_path := String(entry.get("relative_path", ""))
	item.set_text(COLUMN_NAME, display_name)
	item.set_tooltip_text(COLUMN_NAME, relative_path)
	item.set_text(COLUMN_TYPE, relative_path.get_extension().to_upper())
	item.set_text(COLUMN_SIZE, _format_size(int(entry.get("size_bytes", 0))))
	item.set_text_alignment(COLUMN_SIZE, HORIZONTAL_ALIGNMENT_RIGHT)
	item.set_text(COLUMN_MODIFIED, _format_modified(int(entry.get("modified_time", 0))))


# The open file's row is tinted and tooltipped rather than carrying a text
# suffix, so the name column reads cleanly.
func _mark_open_row(item: TreeItem) -> void:
	for column in tree.columns:
		item.set_custom_color(column, OPEN_ROW_COLOR)
	item.set_tooltip_text(COLUMN_NAME, "%s  (currently open)" % item.get_tooltip_text(COLUMN_NAME))


func _format_size(bytes: int) -> String:
	if bytes <= 0:
		return ""
	var unit := 1024.0
	if bytes < 1024:
		return "%d B" % bytes
	if bytes < 1024 * 1024:
		return "%0.1f KB" % (bytes / unit)
	if bytes < 1024 * 1024 * 1024:
		return "%0.1f MB" % (bytes / (unit * unit))
	return "%0.1f GB" % (bytes / (unit * unit * unit))


func _format_modified(unix_seconds: int) -> String:
	if unix_seconds <= 0:
		return ""
	var dt := Time.get_datetime_dict_from_unix_time(unix_seconds)
	return "%04d-%02d-%02d %02d:%02d" % [dt["year"], dt["month"], dt["day"], dt["hour"], dt["minute"]]


func _sort_visible_entries() -> void:
	var column := _sort_column
	var ascending := _sort_ascending
	_visible_entries.sort_custom(func(a: Dictionary, b: Dictionary) -> bool:
		var a_key: Variant = _sort_key(a, column)
		var b_key: Variant = _sort_key(b, column)
		if a_key == b_key:
			# Stable tiebreak on name so equal sizes/types keep a predictable order.
			return String(a.get("display_name", "")).to_lower() < String(b.get("display_name", "")).to_lower()
		if ascending:
			return a_key < b_key
		return a_key > b_key
	)


func _sort_key(entry: Dictionary, column: int) -> Variant:
	match column:
		COLUMN_TYPE:
			return String(entry.get("relative_path", "")).get_extension().to_lower()
		COLUMN_SIZE:
			return int(entry.get("size_bytes", 0))
		COLUMN_MODIFIED:
			return int(entry.get("modified_time", 0))
		_:
			return String(entry.get("display_name", "")).to_lower()


func _update_column_titles() -> void:
	for i in COLUMN_TITLES.size():
		var title := COLUMN_TITLES[i]
		if i == _sort_column:
			title += "  ▲" if _sort_ascending else "  ▼"
		tree.set_column_title(i, title)


func _on_column_title_clicked(column: int, mouse_button_index: int) -> void:
	if mouse_button_index != MOUSE_BUTTON_LEFT:
		return
	if _sort_column == column:
		_sort_ascending = not _sort_ascending
	else:
		_sort_column = column
		_sort_ascending = true
	refresh()


func _on_item_activated() -> void:
	_activate_selected()


func _on_search_submitted(_text: String) -> void:
	_activate_selected()


func _activate_selected() -> void:
	var entry := get_selected_entry()
	if not entry.is_empty():
		entry_activated.emit(entry)


func _on_search_gui_input(event: InputEvent) -> void:
	if not (event is InputEventKey):
		return
	var key := event as InputEventKey
	if not key.pressed or key.is_echo() or key.keycode != KEY_DOWN:
		return
	if _visible_entries.is_empty():
		return
	# Down arrow drops focus from the search box into the result list.
	tree.grab_focus()
	if tree.get_selected() == null:
		var root_item := tree.get_root()
		var first := root_item.get_first_child() if root_item != null else null
		if first != null:
			first.select(COLUMN_NAME)
	search.accept_event()


func _same_filesystem_path(a: String, b: String) -> bool:
	if a.is_empty() or b.is_empty():
		return false
	var left := _globalized_path(a).replace("\\", "/").to_lower()
	var right := _globalized_path(b).replace("\\", "/").to_lower()
	return left == right


func _globalized_path(path: String) -> String:
	if path.begins_with("res://") or path.begins_with("user://"):
		return ProjectSettings.globalize_path(path)
	return path
