class_name ResourceBrowserPane
extends VBoxContainer

## The persistent Resource Browser: the shared ResourceTable over the indexed
## resource folder, docked in the shell's right lane. A kind dropdown filters
## the listing and double-click (or Enter) opens the entry in its workspace
## through the same cross-jump path the link widgets use — no private open
## flow. Capabilities arrive as Callables in setup(), so the pane never
## reaches into the shell's internals and headless tests drive it directly.

const ResourceTableScript := preload("res://modtools/framework/resource_table.gd")

# Label -> index filter. "object"/"music" are umbrella filters the resource
# index expands natively.
const _KIND_FILTERS := [
	["All", ""],
	["Terrains", "terrain"],
	["Missions", "mission"],
	["Environments", "environment"],
	["Objects", "object"],
	["Fonts", "font"],
	["Credits", "credits"],
	["Strings", "strings"],
	["Menus", "menu"],
	["Menu styles", "menu_style"],
	["Music", "music"],
	["Sounds", "sound"],
	["Particle effects", "particle"],
]

var kind_option: OptionButton
var table: ResourceTable
var hint: Label

var _get_index: Callable
var _get_root_dir: Callable
var _scan_root: Callable
var _current_resource_path: Callable
var _open_entry: Callable
# The lazy _scan_root call below re-enters refresh() through the shell's
# post-scan hook; the guard makes that inner call a no-op (the outer pass
# finishes against the freshly scanned index).
var _refreshing := false


func _init() -> void:
	add_theme_constant_override("separation", 8)
	size_flags_horizontal = Control.SIZE_EXPAND_FILL
	size_flags_vertical = Control.SIZE_EXPAND_FILL

	var header := HBoxContainer.new()
	header.name = "BrowserPaneHeader"
	header.add_theme_constant_override("separation", 8)
	add_child(header)

	var title := Label.new()
	title.text = "Resources"
	title.theme_type_variation = &"Heading"
	title.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	header.add_child(title)

	kind_option = OptionButton.new()
	kind_option.name = "BrowserPaneKind"
	kind_option.focus_mode = Control.FOCUS_NONE
	for pair in _KIND_FILTERS:
		kind_option.add_item(String(pair[0]))
	kind_option.item_selected.connect(func(_index: int) -> void: refresh())
	header.add_child(kind_option)

	hint = Label.new()
	hint.name = "BrowserPaneHint"
	hint.theme_type_variation = &"Muted"
	hint.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	hint.visible = false
	add_child(hint)

	table = ResourceTableScript.new()
	table.name = "BrowserPaneTable"
	# The table's default child names are the MODAL browser's pinned lookups;
	# the pane renames its copies so dialog-scoped find_child stays unambiguous.
	table.tree.name = "BrowserPaneList"
	table.search.name = "BrowserPaneSearch"
	table.entry_activated.connect(_on_entry_activated)
	table.list_changed.connect(func(_visible_count: int) -> void: _refresh_hint())
	table.enable_drag_source(_drag_payload_for_entry)
	add_child(table)


## Capabilities: get_index() -> NovaResourceIndex wrapper, get_root_dir() ->
## String, scan_root() (index the configured root), current_resource_path(kind)
## -> String (the active workspace's open file, for the row tint), and
## open_entry(kind, path) (the shell's open_in_workspace).
func setup(get_index: Callable, get_root_dir: Callable, scan_root: Callable,
		current_resource_path: Callable, open_entry: Callable) -> void:
	_get_index = get_index
	_get_root_dir = get_root_dir
	_scan_root = scan_root
	_current_resource_path = current_resource_path
	_open_entry = open_entry


func current_kind() -> String:
	var index := kind_option.selected
	if index < 0 or index >= _KIND_FILTERS.size():
		return ""
	return String(_KIND_FILTERS[index][1])


## Re-pull the index listing for the active kind. Called on show, on kind
## change, and by the shell after a root change/rescan.
func refresh() -> void:
	if not _get_index.is_valid() or _refreshing:
		return
	_refreshing = true
	var entries: Array = []
	var index = _get_index.call()
	var root := String(_get_root_dir.call())
	if not root.is_empty():
		if String(index.get_root_dir()).is_empty() and _scan_root.is_valid():
			_scan_root.call()
		if not String(index.get_root_dir()).is_empty():
			entries = index.get_resource_files(current_kind())
	var current := ""
	if _current_resource_path.is_valid():
		current = String(_current_resource_path.call(current_kind()))
	table.set_entries(entries, current)
	_refreshing = false


func _refresh_hint() -> void:
	var root := String(_get_root_dir.call()) if _get_root_dir.is_valid() else ""
	if root.strip_edges().is_empty():
		hint.text = "No resource directory selected."
		hint.visible = true
	elif table.get_visible_count() == 0:
		hint.text = "No matching resources."
		hint.visible = true
	else:
		hint.visible = false


func _on_entry_activated(entry: Dictionary) -> void:
	if not _open_entry.is_valid():
		return
	var path := String(entry.get("path", ""))
	if path.is_empty():
		path = String(entry.get("logical_name", ""))
	if path.is_empty():
		return
	var kind := String(entry.get("kind", ""))
	_open_entry.call(ResourceKinds.jump_kind(kind), path)


# Rows drag as LinkPayloads. The name keeps its extension (extension-keeping
# widgets commit the file name verbatim), so a row without a relative_path is
# vetoed rather than falling back to the stem-stripped display_name; the kind
# stays the index's own vocabulary - ResourceKinds.jump_kind is workspace-jump
# vocabulary, drop targets match on reference kinds.
func _drag_payload_for_entry(entry: Dictionary) -> Variant:
	var file_name := String(entry.get("relative_path", "")).get_file()
	if file_name.is_empty():
		return null
	var path := String(entry.get("path", ""))
	if path.is_empty():
		path = String(entry.get("logical_name", ""))
	return LinkPayload.make(String(entry.get("kind", "")), file_name, path).to_drag_data()
