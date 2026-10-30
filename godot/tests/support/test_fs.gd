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
## The synthetic models staged under the Tmap's two foliage graphics.
# The Tmap's two foliage definitions' models: the synth crate under both names
# (36 LOD0 vertices), as the editor fixture stages them. A foliage model sizes
# its slot's detail pool from its vertex count (foliage::Runtime::
# detail_cache_capacity, TerrainTile_InitBuffers @0x5ff920: 0xFFFF over 36
# vertices a source vertex), so a fixture model must be as light as the game's
# foliage for the pool to hold the cells around the eye: the crate's slot holds
# 50, where the bird's 324 vertices held 5 and the house's 108 held 16, and a
# steady frame then regenerated cells every frame.
const FOLIAGE_MODEL_BUSH1 := "res://../fixtures/threedi/synth/crate.3di"
const FOLIAGE_MODEL_BUSH2 := "res://../fixtures/threedi/synth/crate.3di"


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


## The synthetic Tmap staged as staged_tmap(label) stages it, with the foliage
## its two definitions draw (stage_tmap_foliage); returns the staged Tmap.trn
## path. Released with release_staged_tmap(label).
static func staged_foliage_tmap(label: String) -> String:
	if not _staged_tmaps.has(label):
		var root := stage_terrain_root(label)
		stage_tmap_foliage(root)
		_staged_tmaps[label] = root
	return String(_staged_tmaps[label]).path_join(TMAP_TRN)


## The synthetic models the Tmap's two foliage definitions name and their
## diffuses (bush1.3di and bush2.3di, the synth crate both), written into the staged terrain
## root `root`; with `code` >= 0 the foliage map (Tmap_f.pcx) is painted `code`
## on every texel, so every candidate the frame weighs matches the definition
## naming it (254: bush1) wherever the terrain collects a cell. A
## dispatcher configured over the root (FoliageDispatcher.configure_for_terrain)
## draws the foliage as a mission's load configures it.
static func stage_tmap_foliage(root: String, code := 254) -> void:
	DirAccess.copy_absolute(ProjectSettings.globalize_path(FOLIAGE_MODEL_BUSH1),
			root.path_join("bush1.3di"))
	DirAccess.copy_absolute(ProjectSettings.globalize_path(FOLIAGE_MODEL_BUSH2),
			root.path_join("bush2.3di"))
	for texture in ["crate.tga"]:
		_store(root.path_join(texture), tga_bytes(Vector2i(64, 64), Color(0.25, 0.5, 0.25, 1.0)))
	if code >= 0:
		_store(root.path_join("Tmap_f.pcx"), TestPcx.solid_indexed(256, 256, code))


## A ResourceRoot over the root staged_foliage_tmap(label) (or staged_tmap)
## staged.
static func staged_tmap_root(label: String) -> ResourceRoot:
	var root := ResourceRoot.new()
	root.set_root_dir(String(_staged_tmaps.get(label, "")))
	return root


static func _store(path: String, bytes: PackedByteArray) -> void:
	var file := FileAccess.open(path, FileAccess.WRITE)
	if file != null:
		file.store_buffer(bytes)
		file.close()


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


## Make the folder `path` lands in (and every folder above it), asserting on
## the test that it exists after.
static func make_parent_dir(test: GutTest, path: String) -> void:
	var dir := path.get_base_dir()
	test.assert_eq(DirAccess.make_dir_recursive_absolute(dir), OK, "Fixture folder %s" % dir)


## Write raw bytes to `path`, making its folder first and asserting on the
## test that the file opens.
static func write_bytes(test: GutTest, path: String, bytes: PackedByteArray) -> void:
	make_parent_dir(test, path)
	var file := FileAccess.open(path, FileAccess.WRITE)
	test.assert_not_null(file, "Fixture should be writable: %s" % path)
	if file != null:
		file.store_buffer(bytes)
		file.close()


## `text` with every line ending in CR LF, the last one included: the one break
## the retail .def walk splits at, which drops the last byte of a line left
## without it (File_ParseASCIIFile; engine/base/io/ascii_config.h). A test
## writes its def text with LF and stages it through this.
static func crlf(text: String) -> String:
	var lf := text.replace("\r\n", "\n")
	if not lf.is_empty() and not lf.ends_with("\n"):
		lf += "\n"
	return lf.replace("\n", "\r\n")


## Write text to `path`, making its folder first and asserting on the test
## that the file opens. A `.def` is written with CR LF line ends (crlf above).
static func write_text(test: GutTest, path: String, text: String) -> void:
	make_parent_dir(test, path)
	var file := FileAccess.open(path, FileAccess.WRITE)
	test.assert_not_null(file, "Fixture should be writable: %s" % path)
	if file != null:
		file.store_string(crlf(text) if path.get_extension().to_lower() == "def" else text)
		file.close()


## Copy `source` (a res:// or absolute path) to `target`, asserting on the test
## that the source held bytes and the target opened.
static func copy(test: GutTest, source: String, target: String) -> void:
	var bytes := FileAccess.get_file_as_bytes(source)
	test.assert_gt(bytes.size(), 0, "Fixture should be readable: %s" % source)
	write_bytes(test, target, bytes)


# The scratch directories this process made, so two made in the same
# microsecond never share a name.
static var _scratch_count := 0


## A fresh scratch directory under the cache dir (ResourceRoot rejects
## user:// roots), named after `label`; callers remove it with
## remove_dir_recursive, typically from after_each.
static func cache_dir(test: GutTest, label: String) -> String:
	_scratch_count += 1
	var dir := OS.get_cache_dir().path_join(
			"opennova_%s_%d_%d" % [label, Time.get_ticks_usec(), _scratch_count])
	test.assert_eq(DirAccess.make_dir_recursive_absolute(dir), OK, "scratch dir %s" % dir)
	return dir


## The smallest texture the overlay's real VFS/texture path loads: an
## uncompressed 32-bit BGRA TGA (image type 2) of `size`, every texel
## `texel`, its image descriptor (byte 17) `descriptor`. The default 0x28 is
## the top-left origin with 8 alpha bits; 0x08 stores the rows bottom-up.
static func tga_bytes(size: Vector2i, texel := Color.WHITE, descriptor := 0x28) -> PackedByteArray:
	var bytes := PackedByteArray()
	bytes.resize(18 + size.x * size.y * 4)
	bytes[2] = 2
	bytes.encode_u16(12, size.x)
	bytes.encode_u16(14, size.y)
	bytes[16] = 32
	bytes[17] = descriptor
	for i in range(size.x * size.y):
		var at := 18 + i * 4
		bytes[at] = texel.b8
		bytes[at + 1] = texel.g8
		bytes[at + 2] = texel.r8
		bytes[at + 3] = texel.a8
	return bytes


## A 16-bit mono PCM RIFF/WAVE at 22050 Hz holding `seconds` of a sine
## (sample i is sin(i * 0.1) * 8000): the 44-byte RIFF, "fmt " and "data"
## header, then int(22050 * seconds) samples.
static func wave_bytes(seconds: float) -> PackedByteArray:
	var samples := int(22050 * seconds)
	var data := PackedByteArray()
	data.resize(samples * 2)
	for i in samples:
		data.encode_s16(i * 2, int(sin(i * 0.1) * 8000.0))
	var head := PackedByteArray()
	head.resize(44)
	head.encode_u32(0, 0x46464952)  # RIFF
	head.encode_u32(4, 36 + data.size())
	head.encode_u32(8, 0x45564157)  # WAVE
	head.encode_u32(12, 0x20746d66)  # "fmt "
	head.encode_u32(16, 16)
	head.encode_u16(20, 1)  # PCM
	head.encode_u16(22, 1)  # mono
	head.encode_u32(24, 22050)
	head.encode_u32(28, 44100)  # byte rate
	head.encode_u16(32, 2)  # block align
	head.encode_u16(34, 16)  # bits per sample
	head.encode_u32(36, 0x61746164)  # data
	head.encode_u32(40, data.size())
	head.append_array(data)
	return head


## One file's contents before a test touches it (typically a user:// config):
## restore() rewrites the bytes when the file existed, else removes whatever
## the test left behind.
class Snapshot:
	extends RefCounted
	var path: String
	var existed: bool
	var bytes: PackedByteArray

	func restore() -> void:
		if existed:
			var file := FileAccess.open(path, FileAccess.WRITE)
			if file != null:
				file.store_buffer(bytes)
				file.close()
		elif FileAccess.file_exists(path):
			DirAccess.remove_absolute(ProjectSettings.globalize_path(path))


## Snapshot `path` in before_each; after_each calls restore() on it.
static func snapshot(path: String) -> Snapshot:
	var snap := Snapshot.new()
	snap.path = path
	snap.existed = FileAccess.file_exists(path)
	snap.bytes = FileAccess.get_file_as_bytes(path) if snap.existed else PackedByteArray()
	return snap


## Snapshot `path` and remove it, so the test starts with no such file: a direct
## gut_cmdln.gd run reads the developer's own user:// (only scripts/test_godot.sh
## isolates it), whose keys must not reach the test. after_each calls restore().
static func isolate(path: String) -> Snapshot:
	var snap := snapshot(path)
	if snap.existed:
		DirAccess.remove_absolute(ProjectSettings.globalize_path(path))
	return snap


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
