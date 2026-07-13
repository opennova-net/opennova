extends GutTest

# ReferenceStrip: the read-only "Used by" panel over the reference index.
# Fake services pin the load-bearing contract — mounting/retargeting NEVER
# triggers the expensive whole-root build; the explicit "Find uses" button
# does, once; a pre-built index populates immediately; rows dedupe per source
# file and jump with the workspace-kind alias. One test runs the real
# NovaReferenceIndex over a cache-dir fixture to pin the file+stem key merge.

const ReferenceStripScript := preload("res://modtools/framework/links/reference_strip.gd")

static var ROOT_DIR := OS.get_cache_dir().path_join("opennova_test_reference_strip")


func _make_strip(services: ReferenceServices, noun := "font") -> ReferenceStrip:
	var strip: ReferenceStrip = ReferenceStripScript.new()
	add_child_autofree(strip)
	strip.configure(noun, services)
	return strip


# edges_by_key: key -> Array[ReferenceEdge]; log["queries"] counts referrers calls.
func _services(edges_by_key: Dictionary, log: Dictionary, ready := false) -> ReferenceServices:
	return ReferenceServices.make(
		func(name: String) -> Array:
			log["queries"] = log.get("queries", []) + [name]
			return edges_by_key.get(name, []),
		func() -> bool:
			return ready or bool(log.get("force_ready", false)),
		func(kind: String, path: String) -> void:
			log["jump"] = [kind, path])


func _edge(source_path: String, source_kind: String, site: String,
		target_kind := "font") -> ReferenceEdge:
	var edge := ReferenceEdge.new()
	edge.source_path = source_path
	edge.source_kind = source_kind
	edge.site = site
	edge.target_kind = target_kind
	return edge


# The find press defers its query by one frame (so the busy state actually
# paints); two awaited frames make the resume order test-independent.
func _await_scan() -> void:
	await get_tree().process_frame
	await get_tree().process_frame


func _row_buttons(strip: ReferenceStrip) -> Array:
	return strip.rows.get_children().filter(func(c): return not c.is_queued_for_deletion())


func test_mounting_never_queries_until_built() -> void:
	var log := {}
	var strip := _make_strip(_services({"arial12b": [_edge("a.kda", "credits", "e")]}, log))
	strip.set_target(PackedStringArray(["arial12b"]))
	strip.refresh()
	assert_eq(log.get("queries", []), [],
		"mount + retarget + refresh must not trigger the whole-root build")
	assert_true(strip.find_button.visible, "an unbuilt index offers the explicit button instead")


func test_find_uses_queries_all_keys_once_and_renders_deduped_rows() -> void:
	var log := {}
	var strip := _make_strip(_services({
		"arial12b.fnt": [_edge("ui/main.mnu", "menu", "screen[MAIN].window[A].font"),
			_edge("ui/main.mnu", "menu", "screen[MAIN].window[B].font")],
		"arial12b": [_edge("nlist.kda", "credits", "entry[3]")],
	}, log))
	strip.set_target(PackedStringArray(["arial12b.fnt", "arial12b"]))
	strip.find_button.pressed.emit()
	assert_eq(strip.find_button.text, "Scanning…", "the busy state paints before the scan")
	assert_true(strip.find_button.disabled, "...with the button disabled against double-presses")
	assert_eq(log.get("queries", []), [],
		"the scan defers a full frame so the busy state can draw first")
	strip.refresh()
	assert_eq(strip.find_button.text, "Scanning…",
		"a refresh landing mid-scan must not repaint the button back to Find uses")
	await _await_scan()

	assert_eq(log["queries"], ["arial12b.fnt", "arial12b"], "every key spelling queries exactly once")
	assert_false(strip.find_button.visible, "the button retires after the first query")
	var buttons := _row_buttons(strip)
	assert_eq(buttons.size(), 2, "two source files, deduped (main.mnu's two sites merge)")
	var texts := buttons.map(func(b): return String(b.text))
	assert_has(texts, "main.mnu  (2 places)", "a multi-site source shows its count")
	assert_has(texts, "nlist.kda")


func test_row_click_jumps_with_kind_alias() -> void:
	var log := {}
	var strip := _make_strip(_services({
		"wcrate5": [_edge("wcrate5_scene.3di", "object_model", "material[0]")],
	}, log, true))
	strip.set_target(PackedStringArray(["wcrate5"]))
	var buttons := _row_buttons(strip)
	assert_eq(buttons.size(), 1)
	(buttons[0] as Button).pressed.emit()
	assert_eq(log["jump"], ["object", "wcrate5_scene.3di"],
		"the jump translates index kinds to workspace kinds")


func test_ready_index_populates_without_the_button() -> void:
	var log := {}
	var strip := _make_strip(_services({"menutxt.bin": [_edge("main.mnu", "menu", "text_rsrc")]}, log, true))
	strip.set_target(PackedStringArray(["menutxt.bin"]))
	assert_false(strip.find_button.visible, "a pre-built graph needs no button (querying is free)")
	assert_eq(_row_buttons(strip).size(), 1, "rows render immediately")


func test_live_strip_requeries_on_set_target() -> void:
	var log := {}
	var strip := _make_strip(_services({
		"alpha": [_edge("one.mnu", "menu", "s")],
		"bravo": [_edge("two.mnu", "menu", "s")],
	}, log))
	strip.set_target(PackedStringArray(["alpha"]))
	strip.find_button.pressed.emit()
	await _await_scan()
	assert_eq(log["queries"], ["alpha"])

	strip.set_target(PackedStringArray(["bravo"]))
	assert_eq(log["queries"], ["alpha", "bravo"],
		"after the first explicit query the strip stays live across retargets")
	assert_false(strip.find_button.visible)


func test_empty_results_show_empty_state_copy() -> void:
	var log := {}
	var strip := _make_strip(_services({}, log, true), "text table")
	strip.set_target(PackedStringArray(["orphan.bin"]))
	assert_true(strip.empty_label.visible)
	assert_string_contains(strip.empty_label.text, "text table",
		"the empty state names the artist-facing noun")


func test_hides_without_services_or_target() -> void:
	var bare: ReferenceStrip = ReferenceStripScript.new()
	add_child_autofree(bare)
	bare.configure("font")
	assert_false(bare.visible, "no services - no strip")

	var log := {}
	var strip := _make_strip(_services({}, log, true))
	strip.set_target(PackedStringArray())
	assert_false(strip.visible, "no target (unsaved document) - no strip")
	strip.set_target(PackedStringArray(["arial12b"]))
	assert_true(strip.visible, "a target brings it back")


func test_allowed_kinds_filter_drops_wrong_kind_rows() -> void:
	# Referrer buckets are name-keyed: a bare stem like "arial12b" is shared
	# with every extensionless namespace in the graph (terrain headers, 3di
	# textures, string keys). The filter keeps collisions out of the rows AND
	# out of the "(n places)" counts.
	var log := {}
	var strip: ReferenceStrip = ReferenceStripScript.new()
	add_child_autofree(strip)
	strip.configure("font", _services({
		"arial12b": [
			_edge("nlist.kda", "credits", "entry[3]", "font"),
			_edge("alpha.bms", "mission", "header.terrain", "terrain"),
			_edge("nlist.kda", "credits", "entry[9]", "datasource"),
		],
	}, log, true), PackedStringArray(["font"]))
	strip.set_target(PackedStringArray(["arial12b"]))
	var buttons := _row_buttons(strip)
	assert_eq(buttons.size(), 1, "only the font edge renders a row")
	assert_eq(String((buttons[0] as Button).text), "nlist.kda",
		"the same-file wrong-kind edge must not inflate the place count")


func test_duplicate_keys_query_once() -> void:
	# An extensionless document has file == stem; querying the same bucket
	# twice would double-count every site.
	var log := {}
	var strip := _make_strip(_services({
		"menutxt": [_edge("main.mnu", "menu", "text_rsrc", "strings")],
	}, log, true))
	strip.set_target(PackedStringArray(["menutxt", "menutxt"]))
	assert_eq(log["queries"], ["menutxt"], "duplicate key spellings collapse to one query")
	var buttons := _row_buttons(strip)
	assert_eq(buttons.size(), 1)
	assert_eq(String((buttons[0] as Button).text), "main.mnu",
		"one site stays one site, never (2 places)")


class RootShell:
	extends RefCounted
	var root: NovaResourceRoot
	func get_resource_root() -> NovaResourceRoot:
		return root


func test_resolve_source_path_translates_logical_names() -> void:
	# Referrer edges carry VFS-logical names, but several workspaces open from
	# disk only - the adopters' jump services translate through the shell root.
	_remove_dir_recursive(ROOT_DIR)
	DirAccess.make_dir_recursive_absolute(ROOT_DIR)
	var f := FileAccess.open(ROOT_DIR.path_join("main.mnu"), FileAccess.WRITE)
	f.store_string("<SCREEN><NAME>M</NAME><WINDOW type=\"window\" name=\"R\"></WINDOW></SCREEN>")
	f.close()
	var shell := RootShell.new()
	shell.root = NovaResourceRoot.new()
	assert_eq(shell.root.set_root_dir(ROOT_DIR), OK)

	var resolved: String = ReferenceStripScript.resolve_source_path(shell, "main.mnu")
	assert_ne(resolved, "main.mnu", "a mounted logical name resolves to its disk path")
	assert_true(resolved.replace("\\", "/").to_lower().ends_with("main.mnu"))
	assert_true(FileAccess.file_exists(resolved), "...and that path actually opens")

	assert_eq(ReferenceStripScript.resolve_source_path(shell, "ghost.mnu"), "ghost.mnu",
		"unknown names pass through untouched")
	assert_eq(ReferenceStripScript.resolve_source_path(RefCounted.new(), "main.mnu"), "main.mnu",
		"a shell without a resource root passes through")
	_remove_dir_recursive(ROOT_DIR)


func test_real_index_first_query_builds_and_merges_extension_and_stem_keys() -> void:
	_remove_dir_recursive(ROOT_DIR)
	DirAccess.make_dir_recursive_absolute(ROOT_DIR)
	var f := FileAccess.open(ROOT_DIR.path_join("main.mnu"), FileAccess.WRITE)
	f.store_string("<SCREEN><NAME>MAIN</NAME><WINDOW type=\"window\" name=\"ROOT\">" +
		"<FONT><NAME>gunpl27b.fnt</NAME></FONT></WINDOW></SCREEN>")
	f.close()
	var root := NovaResourceRoot.new()
	assert_eq(root.set_root_dir(ROOT_DIR), OK)
	var index := NovaReferenceIndex.new()
	index.set_resource_root(root)

	var strip := _make_strip(ReferenceServices.make(
		func(name: String) -> Array[ReferenceEdge]:
			return ReferenceEdge.from_dict_rows(index.referrers_of(name)),
		func() -> bool: return index.is_built(),
		func(_kind: String, _path: String) -> void: pass))
	# Fonts are referenced both with the extension (menus) and bare (credits).
	strip.set_target(PackedStringArray(["gunpl27b.fnt", "gunpl27b"]))
	assert_false(index.is_built(), "retargeting must not build the whole-root graph")
	assert_true(strip.find_button.visible)

	strip.find_button.pressed.emit()
	await _await_scan()
	assert_true(index.is_built(), "the explicit ask builds the graph")
	assert_eq(_row_buttons(strip).size(), 1,
		"the .fnt and stem spellings merge into one main.mnu row")

	_remove_dir_recursive(ROOT_DIR)


func _remove_dir_recursive(path: String) -> void:
	if not DirAccess.dir_exists_absolute(path):
		return
	var dir := DirAccess.open(path)
	if dir == null:
		return
	dir.list_dir_begin()
	var entry := dir.get_next()
	while not entry.is_empty():
		var child := path.path_join(entry)
		if dir.current_is_dir():
			_remove_dir_recursive(child)
		else:
			DirAccess.remove_absolute(child)
		entry = dir.get_next()
	dir.list_dir_end()
	DirAccess.remove_absolute(path)
