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

var header: Label
var find_button: Button
var rows: VBoxContainer
var empty_label: Label

var _noun := "file"
var _services: ReferenceServices = null
var _allowed_kinds := PackedStringArray()
var _keys := PackedStringArray()
# True once a referrers query has run (user asked, or the graph pre-existed):
# from then on retargets refresh live.
var _live := false
# True between the Find-uses press and the query: keeps a refresh()/retarget
# landing in that window from repainting the button back to "Find uses".
var _scanning := false


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


## services: the ReferenceServices record (null, or one with invalid
## referrers/is_ready Callables, hides the strip).
##
## allowed_kinds: target_kind values that count as uses of this target. The
## graph indexes referrers by NAME alone, and bare-stem queries share buckets
## with every extensionless namespace (terrain headers, 3di textures, string
## keys) — without the filter a name collision renders wrong-kind rows and
## inflates "(n places)" counts. Empty = accept everything.
func configure(noun: String, services: ReferenceServices = null,
		allowed_kinds: PackedStringArray = PackedStringArray()) -> void:
	_noun = noun
	_services = services
	_allowed_kinds = allowed_kinds
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


func _refresh_state() -> void:
	if _scanning:
		return
	if _services == null or not _services.referrers.is_valid() \
			or not _services.is_ready.is_valid() or _keys.is_empty():
		visible = false
		return
	visible = true
	if _live or bool(_services.is_ready.call()):
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
	if _scanning:
		return
	# A full frame (not call_deferred, which still runs before this frame's
	# draw) so the busy state actually paints before the synchronous
	# whole-root scan blocks.
	_scanning = true
	find_button.disabled = true
	find_button.text = "Scanning…"
	await get_tree().process_frame
	_scanning = false
	_live = true
	find_button.visible = false
	_populate()


func _clear_rows() -> void:
	for child in rows.get_children():
		child.queue_free()


func _populate() -> void:
	_clear_rows()
	var referrers := _services.referrers
	# Merge every key's edges, deduped per source file (lowercase — the VFS is
	# case-insensitive). The graph lowercases queries itself, keys pass raw;
	# duplicate keys (an extensionless document's file == stem) query once or
	# the same sites would double-count.
	var merged: Dictionary = {}
	var seen_keys: Dictionary = {}
	for key in _keys:
		if String(key).is_empty() or seen_keys.has(String(key).to_lower()):
			continue
		seen_keys[String(key).to_lower()] = true
		for edge_value in referrers.call(String(key)):
			var edge := edge_value as ReferenceEdge
			if not _allowed_kinds.is_empty() \
					and not _allowed_kinds.has(edge.target_kind):
				continue
			if edge.source_path.is_empty():
				continue
			var dedupe_key := edge.source_path.to_lower()
			if not merged.has(dedupe_key):
				merged[dedupe_key] = {
					"path": edge.source_path,
					"kind": edge.source_kind,
					# A plain Array: packed arrays are value types, an appended
					# copy would never land back in the dictionary.
					"sites": [],
				}
			var row: Dictionary = merged[dedupe_key]
			if not edge.site.is_empty():
				(row["sites"] as Array).append(edge.site)

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
	if _services != null and _services.jump.is_valid():
		_services.jump.call(ResourceKinds.jump_kind(kind), path)


## Referrer edges carry VFS-logical source names (the graph is built from the
## index's logical listing, never absolute paths) — but several workspaces
## open from disk only. Adopters route their jump service through this to
## translate when the shell can; the name passes through untouched otherwise.
static func resolve_source_path(shell: Object, path: String) -> String:
	if shell == null or not shell.has_method("get_resource_root"):
		return path
	var resources: Variant = shell.get_resource_root()
	if resources == null:
		return path
	var resolved := String(resources.resolve_file(path))
	return resolved if not resolved.is_empty() else path
