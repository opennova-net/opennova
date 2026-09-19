class_name TestFs
extends RefCounted

## Scratch-filesystem helpers the GUT suite shares.

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
