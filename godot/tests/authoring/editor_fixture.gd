extends RefCounted

## The editor as the GUT tests boot it, and the projects they mint for it. boot makes the OpenNova
## Editor (editor_root.tscn, the editor-enabled variant) in the test's tree with its settings in a
## scratch folder of its own, and answers its typed seam (editor_seam.gd; its `app` the editor);
## teardown removes the folders a test made. new_project makes a project in a scratch folder;
## mint_tmap_mission writes the minted mission on the synthetic Tmap terrain into it and scans it.
## Test-only, preloaded by path (`preload("res://tests/authoring/editor_fixture.gd")`), as the seam
## is, so no class cache stands between a fresh worktree and its tests.

const EDITOR_SCENE := "res://editor/editor_root.tscn"
const EditorSeam := preload("res://tests/authoring/editor_seam.gd")
const FIXTURES := "res://../fixtures/"
## Where the minted mission lies in a project mint_tmap_mission writes.
const MISSION := "missions/synth_logic.bms"
## The terrain's files (fixtures/terrain/tmap, minted by tests/fixtures/minimal_terrain_gen.cpp).
const TERRAIN_FILES := ["Tmap.trn", "Tmap.cpt", "Tmap_m.pcx", "Tmap_f.pcx"]
## The texture slots Tmap.trn names, minted as the runtime fixture mints them
## (support/runtime_fixture.gd): grey, the colormap and the blend map 2048 a side, the rest 32.
const TERRAIN_TEXTURES := [
	"mnml_c", "mnml_dm", "mnml_dc1", "mnml_dc2", "mnml_dc3", "mnml_dmd", "mnml_d1", "mnml_t"
]
const LARGE_TEXTURES := ["mnml_c", "mnml_d1"]
## One effect, Puff: a burst of dots of the fixture's graphic emitting for a minute (DI-14's
## recipe).
const PUFF_PTL := (
	"[effectdef]\n{\n\tid = Puff;\n\tpdefs = PuffDot;\n}\n\n[particledef]\n{\n\tid = PuffDot;\n"
	+ "\temit_dur = 60;\n\temit_rate = 40;\n\temit_burst = 1;\n\tage = 1.0;\n\tscale = 1.0;\n"
	+ "\tspeed = 1.5;\n\tspread = 40;\n\tgraphic1 = particle_dot.tga, blend;\n\tg1_alpha = 1;\n"
	+ "\tg1_scale = 1;\n}\n\n"
)

## The grey textures' bytes by their side, made once a run.
static var _greys: Dictionary = {}


## The editor booted in `test`'s tree, its settings in a scratch folder named after `label` (kept
## in `dirs` for teardown): its seam, null when the editor scene does not load (the test's failure
## says so). `frames` (a windowed run's) run first, the ImGui context past its first frames before
## the editor attaches to it (a context whose first NewFrame ran without docking asserts when
## docking is enabled exactly at its second: imgui.cpp's frame sanity check), and then one more
## once the editor is in the tree.
static func boot(test: GutTest, label: String, dirs: Array[String], frames := 0) -> RefCounted:
	for _frame in frames:
		await test.get_tree().process_frame
	var packed := load(EDITOR_SCENE) as PackedScene
	test.assert_not_null(packed, "the editor scene loads (the editor-enabled variant is loaded)")
	if packed == null:
		return null
	var app := packed.instantiate()
	var settings_dir := OS.get_cache_dir().path_join(
		"opennova editor %s %d" % [label, Time.get_ticks_usec()]
	)
	test.assert_eq(DirAccess.make_dir_recursive_absolute(settings_dir), OK)
	dirs.append(settings_dir)
	app.set("settings_path", settings_dir.path_join("editor_settings.json"))
	test.add_child_autofree(app)
	if frames > 0:
		await test.get_tree().process_frame
	return EditorSeam.new(app)


## Every folder in `dirs` removed, then a frame: a device's rebuild queues its old nodes for
## deletion (ObjectModel's clear), which the frame frees before GUT counts orphans.
static func teardown(test: GutTest, dirs: Array[String]) -> void:
	for dir in dirs:
		TestFs.remove_dir_recursive(dir)
	dirs.clear()
	await test.get_tree().process_frame


## A new project titled `title` in a scratch folder (kept in `dirs`), its required files made: its
## root.
static func new_project(
	test: GutTest, seam: RefCounted, dirs: Array[String], title: String
) -> String:
	var dir := OS.get_cache_dir().path_join(
		"opennova editor project %s %d" % [title.to_lower(), Time.get_ticks_usec()]
	)
	dirs.append(dir)
	test.assert_true(seam.new_project(dir, title), "the project %s" % title)
	test.assert_eq(seam.create_missing_files(), 0, "its required files made")
	return seam.get_project_root()


## The minted mission (fixtures/bms/synth_logic.bms) as MISSION in the project at `root`, on its
## terrain (Tmap: its .trn, its heights, its two maps and the textures it names) under its
## environment (synth_full.env, its clouds and the overcast table every mission's environment load
## reads, empty: no keyframe, as the load with none; the game notes it missing where a project
## lacks it), then scanned (a Rescan steps across pumps, S13 A3). `opts`:
## - "skip": the terrain's files (of TERRAIN_FILES) left out;
## - "small": every texture 32 a side; "textures": a texture's bytes by its name, in place of the
##   grey one;
## - "foliage": the terrain's two foliage definitions' models, bush1 and bush2 (the synth crate
##   under both names);
## - "items": the item table, defs/items.def (true: the fixture's, a String: that text);
## - "models": the synth models (fixtures/threedi/synth) the items draw, copied into models/;
## - "puff": the particle file particles/fx.ptl of PUFF_PTL, with its graphic.
static func mint_tmap_mission(test: GutTest, seam: RefCounted, root: String, opts := {}) -> void:
	copy_fixture(test, "bms/synth_logic.bms", root.path_join(MISSION))
	var skip: PackedStringArray = opts.get("skip", PackedStringArray())
	for name: String in TERRAIN_FILES:
		if not skip.has(name):
			copy_fixture(test, "terrain/tmap/" + name, root.path_join("terrain").path_join(name))
	var textures: Dictionary = opts.get("textures", {})
	for name: String in TERRAIN_TEXTURES:
		var large := LARGE_TEXTURES.has(name) and not bool(opts.get("small", false))
		var bytes: PackedByteArray = textures.get(name, grey_tga(2048 if large else 32))
		write(test, root.path_join("terrain").path_join(name + ".tga"), bytes)
	for name in ["synth_full.env", "cloud01.pcx", "cloud01b.pcx"]:
		copy_fixture(test, "env/" + name, root.path_join("env").path_join(name))
	write(test, root.path_join("env").path_join("overcast.def"), PackedByteArray())
	if bool(opts.get("foliage", false)):
		for name in ["bush1.3di", "bush2.3di"]:
			copy_fixture(test, "threedi/synth/crate.3di", root.path_join("models").path_join(name))
	var items: Variant = opts.get("items", false)
	var table := root.path_join("defs").path_join("items.def")
	if items is String:
		write(test, table, (items as String).to_utf8_buffer())
	elif bool(items):
		copy_fixture(test, "def/items.def", table)
	for name: String in opts.get("models", []):
		copy_fixture(test, "threedi/synth/" + name, root.path_join("models").path_join(name))
	if bool(opts.get("puff", false)):
		var particles := root.path_join("particles")
		write(test, particles.path_join("fx.ptl"), PUFF_PTL.to_utf8_buffer())
		copy_fixture(test, "cbin/particle_dot.tga", particles.path_join("particle_dot.tga"))
	seam.request({"kind": "rescan"})
	test.assert_true(seam.settled, "a Rescan steps across pumps (S13 A3)")


## The fixture's item table (fixtures/def/items.def) as text, its lines ended CR LF (the one break
## the retail def walk splits at).
static func items_def() -> String:
	return FileAccess.get_file_as_string(ProjectSettings.globalize_path(FIXTURES + "def/items.def"))


## A texture `size` a side, grey, as an uncompressed true-colour TGA (all channels alike, so RGBA
## bytes are BGRA TGA bytes; top origin, eight alpha bits): the runtime fixture's minting.
static func grey_tga(size: int) -> PackedByteArray:
	if not _greys.has(size):
		var image := Image.create(size, size, false, Image.FORMAT_RGBA8)
		image.fill(Color(0.5, 0.5, 0.5, 1.0))
		var header := PackedByteArray()
		header.resize(18)
		header[2] = 2
		header.encode_u16(12, size)
		header.encode_u16(14, size)
		header[16] = 32
		header[17] = 0x28
		_greys[size] = header + image.get_data()
	return _greys[size]


## `bytes` written to `path`, its folder made.
static func write(test: GutTest, path: String, bytes: PackedByteArray) -> void:
	test.assert_eq(DirAccess.make_dir_recursive_absolute(path.get_base_dir()), OK)
	TestFs.write_bytes(test, path, bytes)


## The fixture `source` (under fixtures/) copied to `target`.
static func copy_fixture(test: GutTest, source: String, target: String) -> void:
	var bytes := FileAccess.get_file_as_bytes(ProjectSettings.globalize_path(FIXTURES + source))
	test.assert_false(bytes.is_empty(), "the fixture is available: " + source)
	write(test, target, bytes)
