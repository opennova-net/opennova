class_name TestFs
extends RefCounted

## Scratch-filesystem helpers the GUT suite shares, plus the staged synthetic
## terrain most terrain tests render over.

## The synthetic terrain map (fixtures/terrain/tmap, minted by
## tests/fixtures/minimal_terrain_gen.cpp): Tmap.trn names the minimal set's
## terrain art, so a runtime root is the synthetic boot fixtures plus the tmap files.
const TMAP_FIXTURE_DIR := "res://../fixtures/terrain/tmap"
static var BOOT_FIXTURE_DIR := RuntimeFixture.directory()
const TMAP_TRN := "Tmap.trn"


## Stage a runtime root under the cache dir: every tmap fixture file, then
## every minimal asset over it (the minimal items.def wins). Returns the
## absolute directory; callers remove it with remove_dir_recursive.
static func stage_terrain_root(name: String) -> String:
	var root := OS.get_cache_dir().path_join(
			"opennova_terrain_%s_%d" % [name, Time.get_ticks_usec()])
	DirAccess.make_dir_recursive_absolute(root)
	for source in [TMAP_FIXTURE_DIR, BOOT_FIXTURE_DIR]:
		var source_dir := ProjectSettings.globalize_path(source)
		for file_name in DirAccess.get_files_at(source_dir):
			var target := root.path_join(file_name)
			if FileAccess.file_exists(target):
				DirAccess.remove_absolute(target)
			DirAccess.copy_absolute(source_dir.path_join(file_name), target)
	return root


# label -> the root staged_tmap(label) staged; one per test file.
static var _staged_tmaps: Dictionary = {}


## The synthetic Tmap terrain (fixtures/terrain/tmap) staged over the minimal
## assets it names, once per `label` (one root per test file); returns the
## staged Tmap.trn path. The file releases it in after_all with
## release_staged_tmap(label).
static func staged_tmap(label: String) -> String:
	if not _staged_tmaps.has(label):
		_staged_tmaps[label] = stage_terrain_root(label)
	return String(_staged_tmaps[label]).path_join(TMAP_TRN)


## Remove the root staged_tmap(label) staged (a no-op when it never staged).
static func release_staged_tmap(label: String) -> void:
	if _staged_tmaps.has(label):
		remove_dir_recursive(String(_staged_tmaps[label]))
		_staged_tmaps.erase(label)


## Render frames until the terrain's tile cache has published every visible
## page (no pending job, every request a ready hit), and return the settled
## diagnostics. Exact material animation can make the two CPU workers
## rasterize every authored alpha surface instead of skipping an unsupported
## draw, so the loop is frame-bounded but leaves enough headless Debug frames
## for all 61 visible pages to publish on slower Windows CI runners.
static func settle_tile_cache(test: GutTest, terrain: Terrain) -> Dictionary:
	var diagnostics: Dictionary = {}
	for _attempt in range(2048):
		terrain.render_frame()
		diagnostics = terrain.get_tile_cache_diagnostics()
		if int(diagnostics.get("pending_jobs", -1)) == 0 \
				and int(diagnostics.get("frame_requests", 0)) > 0 \
				and int(diagnostics.get("frame_ready_hits", -1)) \
						== int(diagnostics.get("frame_requests", 0)):
			return diagnostics
		await test.get_tree().process_frame
	test.assert_true(false, "the bounded terrain compiler must settle visible pages")
	return diagnostics


## Write raw bytes to `path`, asserting on the test that the file opens.
static func write_bytes(test: GutTest, path: String, bytes: PackedByteArray) -> void:
	var file := FileAccess.open(path, FileAccess.WRITE)
	test.assert_not_null(file, "Fixture should be writable: %s" % path)
	if file != null:
		file.store_buffer(bytes)
		file.close()


## Write text to `path`, asserting on the test that the file opens.
static func write_text(test: GutTest, path: String, text: String) -> void:
	var file := FileAccess.open(path, FileAccess.WRITE)
	test.assert_not_null(file, "Fixture should be writable: %s" % path)
	if file != null:
		file.store_string(text)
		file.close()


## Put a snapshotted config file back: rewrite `bytes` when it existed before
## the test, else remove whatever the test left behind.
static func restore_file(path: String, existed: bool, bytes: PackedByteArray) -> void:
	if existed:
		var file := FileAccess.open(path, FileAccess.WRITE)
		if file != null:
			file.store_buffer(bytes)
			file.close()
	elif FileAccess.file_exists(path):
		DirAccess.remove_absolute(ProjectSettings.globalize_path(path))


## Delete a directory tree (a missing path is a no-op).
static func remove_dir_recursive(path: String) -> void:
	var dir := DirAccess.open(path)
	if dir == null:
		return
	dir.list_dir_begin()
	var entry := dir.get_next()
	while entry != "":
		var child := path.path_join(entry)
		if dir.current_is_dir():
			remove_dir_recursive(child)
		else:
			DirAccess.remove_absolute(child)
		entry = dir.get_next()
	dir.list_dir_end()
	DirAccess.remove_absolute(path)
