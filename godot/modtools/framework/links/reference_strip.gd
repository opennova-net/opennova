class_name ReferenceStrip
extends VBoxContainer
## A read-only "Used by" panel: the files that reference a target, fed by the
## shell's reference index through context-local Callables. Rows are deduped
## per source file and click through to that file's workspace.
##
## The first referrers query triggers the index's one-time whole-root scan,
## which is expensive — so the strip NEVER queries as a side effect of merely
## mounting or retargeting. When the graph is not built yet it offers a
## "Find uses" button instead; once the user asks (or when the graph was
## already built), the strip goes live and re-queries on every retarget —
## incremental rebuilds are memoized per file, so those stay cheap.

## Same workspace-jump translation the link widgets and the browser pane apply.
const _JUMP_KIND := {
	"object_project": "object",
	"object_model": "object",
	"object_scene": "object",
	"sbf": "music",
	"music_script": "music",
}

var header: Label
var find_button: Button
var rows: VBoxContainer
var empty_label: Label

var _noun := "file"
var _services: Dictionary = {}
var _keys := PackedStringArray()
# True once a referrers query has run (user asked, or the graph pre-existed):
# from then on retargets refresh live.
var _live := false


func _init() -> void:
	add_theme_constant_override("separation", 4)
	size_flags_horizontal = Control.SIZE_EXPAND_FILL

	header = Label.new()
	header.name = "UsedByHeader"
	header.text = "Used by"
	header.theme_type_variation = &"Heading"
	add_child(header)

	find_button = Button.new()
	find_button.name = "UsedByFindButton"
	find_button.text = "Find uses"
	find_button.focus_mode = Control.FOCUS_NONE
	find_button.visible = false
	find_button.pressed.connect(_on_find_pressed)
	add_child(find_button)

	rows = VBoxContainer.new()
	rows.name = "UsedByRows"
	rows.add_theme_constant_override("separation", 2)
	rows.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	add_child(rows)

	empty_label = Label.new()
	empty_label.name = "UsedByEmpty"
	empty_label.theme_type_variation = &"Muted"
	empty_label.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	empty_label.visible = false
	add_child(empty_label)


## services (all Callables, any subset; missing referrers/is_ready hides the strip):
##   "referrers": Callable(name: String) -> Array of reference-edge Dictionaries
##                ({source_path, source_kind, site, ...})
##   "is_ready":  Callable() -> bool — whether the whole-root graph is already
##                built (querying then is free, so the strip skips the button)
##   "jump":      Callable(kind: String, path: String) — open a source file
func configure(noun: String, services: Dictionary = {}) -> void:
	_noun = noun
	_services = services
	find_button.tooltip_text = \
			"Looks through the resource folder for files that use this %s." % _noun
	_refresh_state()


## The names this target is referenced as. Fonts, for example, are referenced
## both bare ("arial12b" in credits) and with the extension ("arial12b.fnt" in
## menus) — pass every spelling; results are merged and deduped per source file.
func set_target(keys: PackedStringArray) -> void:
	_keys = keys
	_refresh_state()


func refresh() -> void:
	_refresh_state()


func _service(service_name: String) -> Callable:
	var cb: Variant = _services.get(service_name)
	return cb if cb is Callable else Callable()


func _refresh_state() -> void:
	var referrers := _service("referrers")
	var is_ready := _service("is_ready")
	if not referrers.is_valid() or not is_ready.is_valid() or _keys.is_empty():
		visible = false
		return
	visible = true
	if _live or bool(is_ready.call()):
		_live = true
		find_button.visible = false
		_populate()
	else:
		_clear_rows()
		empty_label.visible = false
		find_button.visible = true
		find_button.disabled = false
		find_button.text = "Find uses"


func _on_find_pressed() -> void:
	# Paint the busy state before the synchronous whole-root scan runs.
	find_button.disabled = true
	find_button.text = "Scanning…"
	call_deferred("_run_first_query")


func _run_first_query() -> void:
	_live = true
	find_button.visible = false
	_populate()


func _clear_rows() -> void:
	for child in rows.get_children():
		child.queue_free()


func _populate() -> void:
	_clear_rows()
	var referrers := _service("referrers")
	# Merge every key's edges, deduped per source file (lowercase — the VFS is
	# case-insensitive). The graph lowercases queries itself, keys pass raw.
	var merged: Dictionary = {}
	for key in _keys:
		if String(key).is_empty():
			continue
		for edge_value in referrers.call(String(key)):
			var edge := edge_value as Dictionary
			var source_path := String(edge.get("source_path", ""))
			if source_path.is_empty():
				continue
			var dedupe_key := source_path.to_lower()
			if not merged.has(dedupe_key):
				merged[dedupe_key] = {
					"path": source_path,
					"kind": String(edge.get("source_kind", "")),
					# A plain Array: packed arrays are value types, an appended
					# copy would never land back in the dictionary.
					"sites": [],
				}
			var row: Dictionary = merged[dedupe_key]
			var site := String(edge.get("site", ""))
			if not site.is_empty():
				(row["sites"] as Array).append(site)

	if merged.is_empty():
		empty_label.text = "Nothing in the resource folder uses this %s." % _noun
		empty_label.visible = true
		return
	empty_label.visible = false

	var paths := merged.keys()
	paths.sort()
	for dedupe_key in paths:
		var row: Dictionary = merged[dedupe_key]
		var sites: Array = row["sites"]
		var button := Button.new()
		button.text = String(row["path"]).get_file() if sites.size() <= 1 \
				else "%s  (%d places)" % [String(row["path"]).get_file(), sites.size()]
		button.tooltip_text = "\n".join(PackedStringArray(sites)) if sites.size() > 0 else String(row["path"])
		button.alignment = HORIZONTAL_ALIGNMENT_LEFT
		button.flat = true
		button.focus_mode = Control.FOCUS_NONE
		button.pressed.connect(_on_row_pressed.bind(String(row["kind"]), String(row["path"])))
		rows.add_child(button)


func _on_row_pressed(kind: String, path: String) -> void:
	var jump := _service("jump")
	if jump.is_valid():
		jump.call(_JUMP_KIND.get(kind, kind), path)
