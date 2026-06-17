extends GutTest

# NovaReferenceIndex (godot/engine/refs): libs/refs bound over a mounted
# NovaResourceRoot. Covers on-demand references_of with resolution annotation,
# the lazy whole-root build behind referrers_of, kind-specific resolve
# candidates (texture fallbacks vs header-ref extensions vs unprobed name-table
# kinds), the explicit items.def probe (the index scan skips kindless files),
# and cache-epoch self-invalidation.

# NovaResourceRoot rejects user:// roots by design, so the fixture root lives
# in the OS cache dir (the same convention as the workstation tests).
static var ROOT_DIR := OS.get_cache_dir().path_join("opennova_test_reference_index")


func before_each() -> void:
	_remove_dir_recursive(ROOT_DIR)
	DirAccess.make_dir_recursive_absolute(ROOT_DIR)
	# A tiny world: an .env naming a texture that exists (clouds.tga via the
	# resolver's fallback list) and celestial models that do not; items.def
	# naming one graphic that exists and one that does not.
	_write_text("desert.env", "sky_map1 clouds.pcx\nsky_map2 missing_b.pcx\n")
	_write_text("clouds.tga", "not a real tga, existence is what resolve probes")
	_write_text("items.def",
		"begin \"Crate\"\n  id 10\n  type object\n  graphic Wcrate5\nend\n" +
		"begin \"Ghost\"\n  id 11\n  type object\n  graphic NoSuchModel\nend\n")
	_write_text("wcrate5.3di", "existence-only stand-in for the crate model")
	# A menu naming: a texture that exists, an action target that exists, a
	# sound bank that does not, a string table, and a string key (unprobed).
	_write_text("main.mnu",
		"<SCREEN><NAME>MAIN</NAME><WINDOW type=\"window\" name=\"ROOT\">" +
		"<TEXT_RSRC>menutxt.BIN</TEXT_RSRC>" +
		"<WINDOW type=\"window\" name=\"BG\">" +
		"<APPEARANCE type=\"image\" state=\"default\">clouds.tga</APPEARANCE></WINDOW>" +
		"<WINDOW type=\"button\" name=\"GO\">" +
		"<ACTION type=\"screen\" file=\"sub.mnu\">SUB</ACTION>" +
		"<SOUND state=\"selected\" trigger=\"CLICK_SELECT\">ui.lwf</SOUND>" +
		"<STRING type=\"id\" justify=\"LEFT\">BTN_GO</STRING>" +
		"</WINDOW></WINDOW></SCREEN>")
	_write_text("sub.mnu", "<SCREEN><NAME>SUB</NAME><WINDOW type=\"window\" name=\"R\"></WINDOW></SCREEN>")


func after_each() -> void:
	_remove_dir_recursive(ROOT_DIR)


func _write_text(name: String, body: String) -> void:
	var f := FileAccess.open(ROOT_DIR.path_join(name), FileAccess.WRITE)
	f.store_string(body)
	f.close()


func _make_index() -> NovaReferenceIndex:
	var root := NovaResourceRoot.new()
	assert_eq(root.set_root_dir(ROOT_DIR), OK)
	var index := NovaReferenceIndex.new()
	index.set_resource_root(root)
	return index


func _edge_with_site(edges: Array, site: String) -> Dictionary:
	for edge in edges:
		if String((edge as Dictionary)["site"]) == site:
			return edge
	return {}


func test_references_of_annotates_resolution() -> void:
	var index := _make_index()
	var edges := index.references_of("desert.env")
	assert_false(index.is_built(), "single-file queries never trigger the whole-root build")

	var sky1 := _edge_with_site(edges, "sky_map1")
	assert_false(sky1.is_empty(), "the env's sky_map1 edge extracts")
	assert_eq(String(sky1["status"]), "found", "clouds.pcx resolves through the texture fallback list (clouds.tga exists)")
	assert_string_contains(String(sky1["target_path"]).to_lower(), "clouds.tga")

	var sky2 := _edge_with_site(edges, "sky_map2")
	assert_eq(String(sky2["status"]), "missing", "a texture with no candidate on disk reports missing")

	# Default celestial models (msun.3di etc.) are effective references; none
	# exist in this root, so they report missing with the object_model probe.
	var sun := _edge_with_site(edges, "sun_3di")
	assert_eq(String(sun["status"]), "missing")


func test_referrers_lazily_build_and_cover_items_def() -> void:
	var index := _make_index()
	var referrers := index.referrers_of("Wcrate5")
	assert_true(index.is_built(), "the first referrers query builds the whole-root graph")
	assert_eq(referrers.size(), 1, "the crate graphic is referenced once")
	var edge: Dictionary = referrers[0]
	assert_eq(String(edge["source_path"]), "items.def",
		"items.def is swept despite carrying no resource-index kind (explicit probe)")
	assert_eq(String(edge["status"]), "found", "wcrate5.3di exists")
	# Case-insensitive both directions.
	assert_eq(index.referrers_of("WCRATE5").size(), 1)
	# The ghost graphic's referrer edge reports its missing target.
	var ghost := index.referrers_of("NoSuchModel")
	assert_eq(ghost.size(), 1)
	assert_eq(String((ghost[0] as Dictionary)["status"]), "missing")


func test_mnu_edges_resolve_per_kind() -> void:
	var index := _make_index()
	var edges := index.references_of("main.mnu")
	assert_false(edges.is_empty(), "the menu extracts edges")

	var by_kind := {}
	for edge in edges:
		by_kind[String((edge as Dictionary)["target_kind"]) + "|" + String((edge as Dictionary)["target_name"])] = edge

	var texture: Dictionary = by_kind.get("texture|clouds.tga", {})
	assert_eq(String(texture.get("status", "")), "found", "the appearance image resolves")

	var action: Dictionary = by_kind.get("menu|sub.mnu", {})
	assert_eq(String(action.get("status", "")), "found", "the action's target menu exists")

	var sound: Dictionary = by_kind.get("sound|ui.lwf", {})
	assert_eq(String(sound.get("status", "")), "missing", "a sound bank probes name+.lwf and misses")

	var strings: Dictionary = by_kind.get("strings|menutxt.BIN", {})
	assert_eq(String(strings.get("status", "")), "missing", "the string table probes verbatim")

	var key: Dictionary = by_kind.get("string_id|BTN_GO", {})
	assert_eq(String(key.get("status", "")), "unprobed",
		"string keys resolve against a TABLE, not the root - never a fake miss (A9 builds on this)")


func test_mnu_referrers_through_the_whole_root_build() -> void:
	var index := _make_index()
	var referrers := index.referrers_of("sub.mnu")
	assert_true(index.is_built(), "menus are swept by the whole-root build (resource-index kind 'menu')")
	assert_eq(referrers.size(), 1, "the action edge lands in the inverse index")
	assert_eq(String((referrers[0] as Dictionary)["source_path"]), "main.mnu")


func test_resolve_kinds_and_unprobed() -> void:
	var index := _make_index()
	assert_eq(String(index.resolve("terrain", "dvxi5")["status"]), "missing",
		"header refs probe with their engine extension appended")
	assert_eq(String(index.resolve("item_defs", "items.def")["status"]), "found")
	assert_eq(String(index.resolve("sound_profile", "SP_Buggy")["status"]), "unprobed",
		"name-table kinds are not files; never report a fake miss")
	assert_eq(String(index.resolve("texture", "")["status"]), "unprobed", "empty names are unprobed")


func test_cache_epoch_invalidates_the_graph() -> void:
	var index := _make_index()
	assert_eq(index.referrers_of("Wcrate5").size(), 1)
	# Re-point the only item at a different graphic, as an external edit + epoch bump.
	_write_text("items.def", "begin \"Crate\"\n  id 10\n  type object\n  graphic OtherModel\nend\n")
	NovaResourceRoot.bump_cache_epoch()
	assert_false(index.is_built(), "an epoch bump invalidates on the next query")
	assert_eq(index.referrers_of("Wcrate5").size(), 0, "stale edges are gone after the rebuild")
	assert_eq(index.referrers_of("OtherModel").size(), 1, "the fresh edge is served")


func test_build_stats_report_the_sweep() -> void:
	var index := _make_index()
	var stats := index.get_build_stats()
	assert_true(index.is_built())
	# desert.env + items.def extract; clouds.tga / wcrate5.3di are listed but
	# wcrate5.3di is junk bytes -> a fail-soft error, never a crash.
	assert_gte(int(stats["recognized"]), 2)
	assert_gte(int(stats["extracted"]), 2)


func _remove_dir_recursive(path: String) -> void:
	var dir := DirAccess.open(path)
	if dir == null:
		return
	dir.list_dir_begin()
	var entry := dir.get_next()
	while entry != "":
		var child := path.path_join(entry)
		if dir.current_is_dir():
			_remove_dir_recursive(child)
		else:
			DirAccess.remove_absolute(child)
		entry = dir.get_next()
	dir.list_dir_end()
	DirAccess.remove_absolute(path)
