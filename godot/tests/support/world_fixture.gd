class_name WorldFixture
extends RefCounted

## Real world fixtures for the GUT suite (ADR 0043 rule 11): a test boots the
## packaged GameWorld scene over the minimal asset pack through the public load
## seams instead of subclassing a production Node. Every helper takes the
## GutTest so what it builds is autofreed with the test.
##
## - stage_minimal_root(): a fresh cache directory holding a copy of `assets/`
##   (the minimal pack: mnml.bms/.trn/.env, the defs, menus, fonts), optionally
##   overlaid with the render-capable `fixtures/terrain/tmap` (Tmap.trn/.cpt),
##   extra files written on top. Tests remove it with TestFs.remove_dir_recursive.
## - boot_minimal(): the packaged game_world.tscn in the tree with mnml.bms
##   loaded from that root (the typed MissionData path: nothing but a real
##   MissionRoot can result).
## - boot_mission_data(): a MissionRoot over an in-memory MissionData
##   (the entity-registry/discovery recipe), no VFS involved.

const MINIMAL_ASSETS_DIR := "res://../assets"
const TMAP_FIXTURE_DIR := "res://../fixtures/terrain/tmap"
const PARTICLE_FIXTURE_DIR := "res://../fixtures/particle"
const LWF_FIXTURE_DIR := "res://../fixtures/lwf"
const MINIMAL_MISSION := "mnml.bms"
const CACHE_ROOT := "world_fixture"


static func write_file(path: String, text: String) -> void:
	var file := FileAccess.open(path, FileAccess.WRITE)
	assert(file != null, "fixture file should be writable: %s" % path)
	file.store_string(text)
	file.close()


static func _copy_dir_files(source_dir: String, root_dir: String) -> void:
	for file_name in DirAccess.get_files_at(source_dir):
		var err := DirAccess.copy_absolute(
				source_dir.path_join(file_name), root_dir.path_join(file_name))
		assert(err == OK, "fixture copy failed: %s" % file_name)


## A fresh root directory under the cache holding the minimal pack. With
## `with_tmap` the synthetic Tmap terrain is copied over it (its `items.def`
## wins, and Tmap.trn names the same mnml_* art); `extra_files` maps a file
## name to its text and is written last.
static func stage_minimal_root(name: String, with_tmap := false,
		extra_files: Dictionary = {}) -> String:
	var root_dir := OS.get_cache_dir().path_join(CACHE_ROOT).path_join(
			"%s_%d" % [name, Time.get_ticks_usec()])
	assert(DirAccess.make_dir_recursive_absolute(root_dir) == OK)
	_copy_dir_files(ProjectSettings.globalize_path(MINIMAL_ASSETS_DIR), root_dir)
	if with_tmap:
		_copy_dir_files(ProjectSettings.globalize_path(TMAP_FIXTURE_DIR), root_dir)
	for file_name in extra_files:
		write_file(root_dir.path_join(String(file_name)), String(extra_files[file_name]))
	return root_dir


## Stage every synthetic particle document (fixtures/particle/*.ptl) into the
## root so a real EffectWorld interns authored definitions (the minimal pack
## ships none). Returns the effect names the documents author, in file order.
static func stage_effects(root_dir: String) -> PackedStringArray:
	var names := PackedStringArray()
	var source_dir := ProjectSettings.globalize_path(PARTICLE_FIXTURE_DIR)
	for file_name in DirAccess.get_files_at(source_dir):
		if file_name.get_extension().to_lower() != "ptl":
			continue
		var err := DirAccess.copy_absolute(
				source_dir.path_join(file_name), root_dir.path_join(file_name))
		assert(err == OK, "particle fixture copy failed: %s" % file_name)
		var doc := ParticleFile.new()
		if doc.load_from_file(root_dir.path_join(file_name)) == OK:
			for effect in doc.get_effects():
				names.append(String((effect as ParticleEffect).get_id()))
	return names


## Author a sound bank in the root (`bank_name`, the mission's co-named .LWF)
## whose sets all play the fixture tone at `falloff_radius`, so a real
## MissionAudio fires them (the minimal pack's mnml.lwf is an empty stub).
static func stage_sound_bank(root_dir: String, set_names: PackedStringArray,
		bank_name := "mnml.lwf", falloff_radius := 2000) -> void:
	var tone := FileAccess.get_file_as_bytes(
			ProjectSettings.globalize_path(LWF_FIXTURE_DIR).path_join("tone.wav"))
	assert(not tone.is_empty(), "the LWF tone fixture is available")
	var wav := FileAccess.open(root_dir.path_join("tone.wav"), FileAccess.WRITE)
	wav.store_buffer(tone)
	wav.close()
	var lwf := LwfData.new()
	lwf.create_empty()
	for set_name in set_names:
		var set_i := lwf.add_set()
		lwf.set_set_field(set_i, "name", set_name)
		var layer_i := lwf.add_layer(set_i)
		lwf.set_layer_field(set_i, layer_i, "falloff_radius", falloff_radius)
		var member_i := lwf.add_member(set_i, layer_i)
		lwf.set_member_field(set_i, layer_i, member_i, "wav_path", "tone.wav")
	assert(lwf.save_file(root_dir.path_join(bank_name)) == OK, "the fixture bank saves")


## Re-emit the mission at `source` through MissionData with its authored
## weapon kit cleared, into `dest` (the same path is fine). The committed
## mnml.bms promotes an AK kit that outranks any PLAYER_INFO selection
## offline, so a fixture that wants the profile to win stages this copy.
static func write_mission_without_loadout(test: GutTest, source: String, dest: String) -> void:
	var mission := MissionData.new()
	test.assert_eq(mission.open_file(source), OK,
			"the mission opens for the no-kit fixture: %s" % source)
	test.assert_true(mission.set_weapon_loadout([]),
			"the profile-over-fallback fixture can clear the authored kit")
	test.assert_eq(mission.save_as(dest), OK, "the no-kit mission serializes")


## The packaged world scene, instantiated and parented under the test. The
## world unloads itself (and clears its mounted root's caches) as it leaves
## the tree: a loaded world freed without unload() leaves render state behind
## that faults Godot's exit teardown (the main_game_test after_each rule).
static func make_world(test: GutTest) -> GameWorld:
	var packed := load("res://game/world/game_world.tscn") as PackedScene
	var world := packed.instantiate() as GameWorld
	world.tree_exiting.connect(_unload_on_exit.bind(world))
	test.add_child_autofree(world)
	return world


static func _unload_on_exit(world: GameWorld) -> void:
	if not is_instance_valid(world):
		return
	var root := world.get_resource_root()
	world.unload()
	if root != null:
		root.clear()


## Load `mission_name` from `root_dir` into `world` through the typed path:
## the resource root, MissionData opened from it (mutated by `mutator` when
## given), then load_mission_data. Returns the load error.
static func load_mission(world: GameWorld, root_dir: String,
		mission_name := MINIMAL_MISSION, mutator: Callable = Callable()) -> Error:
	var root := ResourceRoot.new()
	var root_err := root.set_root_dir(root_dir)
	if root_err != OK:
		return root_err
	world.set_resource_root(root)
	var mission := MissionData.new()
	var open_err := mission.open_from_resource_root(root, mission_name)
	if open_err != OK:
		return open_err
	if mutator.is_valid():
		mutator.call(mission)
	return world.load_mission_data(mission, mission_name)


## The packaged world in the tree with the minimal mission loaded from
## `root_dir` (the committed `assets/` directory when empty). Asserts the load.
static func boot_minimal(test: GutTest, root_dir := "",
		mutator: Callable = Callable()) -> GameWorld:
	var world := make_world(test)
	if root_dir.is_empty():
		root_dir = ProjectSettings.globalize_path(MINIMAL_ASSETS_DIR)
	var err := load_mission(world, root_dir, MINIMAL_MISSION, mutator)
	test.assert_eq(err, OK, "the minimal mission loads from %s" % root_dir)
	return world


## A MissionRoot in the tree over an in-memory mission: the container
## and the runtime are parented under the test; `options` reach setup() as is.
static func boot_mission_data(test: GutTest, mission: MissionData,
		options: MissionSetupOptions = null) -> MissionRoot:
	var container := Node3D.new()
	test.add_child_autofree(container)
	var runtime := MissionRoot.new()
	test.add_child_autofree(runtime)
	if options != null:
		runtime.setup(mission, container, options)
	else:
		runtime.setup(mission, container)
	return runtime


## An in-memory default mission with `organics` organic entities placed along
## -Z (the discovery/registry recipe).
static func default_mission(organics := 2) -> MissionData:
	var mission := MissionData.new()
	assert(mission.create_default() == OK)
	for i in range(organics):
		mission.add_entity(MissionData.KIND_ORGANIC, 0,
				Vector3(10 * (i + 1), 0, -30 - 10 * i), Vector3.ZERO)
	return mission


# --- The packed runtime shell: main_game.tscn over retail-shaped archives ------
#
# The lifecycle recipe: three boot-table PFFs written into a fresh cache dir
# from the minimal pack (+ the baked Tmap terrain), the persisted resource dir
# pointed at it, the packaged shell instantiated under the test and given one
# frame to boot its main menu. The shell is NOT autofreed: release_shell()
# unloads the world and clears the mounted roots before the archive files go
# (the after_each order the lifecycle test established).

const MAIN_GAME_SCENE_PATH := "res://game/main_game.tscn"
# Witnessed retail placement (assets/README.md): strings plus the
# mission .bin/.pcx/.lwf family live in language; menus/defs/.bms/.dbf in
# localres; environment, terrain, and terrain art in resource.
const SHELL_LANGUAGE_FILES := [
	"gameerr.bin", "gametext.bin", "vmacros.bin", "keyhelp.bin",
	"menutxt.bin", "mnml.bin", "mnml.pcx", "mnml.lwf",
]
const SHELL_LOCALRES_FILES := [
	"items.def", "weapon.def", "ammo.def", "main.mnu", "mp.mnu",
	"game.mnu", "weapon.mnu",
	"mnml.bms", "menu_style.mns", "newarow1.tga", "mnml.dbf",
]
const SHELL_RESOURCE_FILES := [
	"mnml.env", "mnml.trn", "mnml_c.tga", "mnml_dm.tga",
	"mnml_dc1.tga", "mnml_dc2.tga", "mnml_dc3.tga", "mnml_dmd.tga", "mnml_d1.tga",
	"mnml_t.tga", "mnml_m.pcx", "mnml_f.pcx",
	# The set's own model (items.def 108001, placed in mnml.bms) + its swatches.
	"house.3di", "wall.tga", "roof.tga", "wood.tga",
]
# The synthetic Tmap terrain (its .trn names the mnml art packed above).
const SHELL_BAKED_TERRAIN_FILES := ["Tmap.cpt", "Tmap_f.pcx", "Tmap_m.pcx"]
# This lifecycle-only armory deliberately resolves the engine fallback as well
# as the selected profile weapon. The regression must fail if GameWorld mistakes
# a nonempty WPN_M4AUTO fallback inventory for a mission-authored kit.
const SHELL_WEAPON_DEF := """
ammoclass_max_carry CLASS_556MM 1000

weapon "WPN_AK47AUTO"
	category 1
	rank 0
	statid 102
	ammo AM_556MM
	clip 30
	maxclips 7
	gfx1 AK_TEST_FIRST
end

weapon "WPN_M4AUTO"
	category 1
	rank 0
	statid 101
	ammo AM_556MM
	clip 30
	maxclips 7
	gfx1 M4AUTO_TEST_FIRST
end

weapon "WPN_M4"
	category 1
	rank 0
	statid 100
	ammo AM_556MM
	clip 30
	maxclips 7
	gfx1 M4_TEST_FIRST
end
"""

static var _last_shell_dir := ""


## The cache directory the most recent boot_shell() staged its archives in
## ("" before the first boot). Callers remove it in after_each with
## TestFs.remove_dir_recursive once release_shell() has run.
static func last_shell_dir() -> String:
	return _last_shell_dir


## The shared PFF3 fixture writer (TestPff.write), asserted on the test.
static func write_pff(test: GutTest, path: String, entries: Array) -> void:
	test.assert_eq(TestPff.write(path, entries), OK,
			"PFF fixture should be writable: %s" % path)


## One archive's rows for `filenames` out of the minimal pack, with the
## lifecycle substitutions: the render-capable Tmap.trn under mnml.trn's
## name, the shipped JO in-world menus (when the reference fixture set is
## present) under the game.mnu / weapon.mnu archive names, and
## SHELL_WEAPON_DEF as weapon.def. With `without_mission_loadout` the
## committed mnml.bms is re-emitted with its authored kit cleared
## (write_mission_without_loadout, saved beside the archives in
## `staging_dir`), so a shell test can watch the PLAYER_INFO selection win
## over the engine fallback.
static func shell_archive_entries(test: GutTest, filenames: Array,
		without_mission_loadout := false, staging_dir := "") -> Array:
	var entries: Array = []
	for filename in filenames:
		var source := MINIMAL_ASSETS_DIR.path_join(filename)
		# mnml.bms names mnml.trn. Substitute a committed render-capable TRN
		# while retaining that logical archive name.
		if filename == "mnml.trn":
			source = TMAP_FIXTURE_DIR.path_join("Tmap.trn")
		# The in-world screens (ESC pause overlay + armory) pack the shipped JO
		# menus from the reference fixture set under their retail archive names;
		# without the set the boot packs the minted main menu under those names
		# (a valid menu; the screen-verb test that opens them pends).
		elif filename == "game.mnu":
			source = RetailData.fixture("mnu/jo_game.mnu")
			if source.is_empty():
				source = MINIMAL_ASSETS_DIR.path_join("main.mnu")
		elif filename == "weapon.mnu":
			source = RetailData.fixture("mnu/jo_weapon.mnu")
			if source.is_empty():
				source = MINIMAL_ASSETS_DIR.path_join("main.mnu")
		var bytes := FileAccess.get_file_as_bytes(source)
		if filename == "mnml.bms" and without_mission_loadout:
			var no_kit_path := staging_dir.path_join("mnml_no_kit.bms")
			write_mission_without_loadout(test, ProjectSettings.globalize_path(source), no_kit_path)
			bytes = FileAccess.get_file_as_bytes(no_kit_path)
		if filename == "weapon.def":
			bytes = SHELL_WEAPON_DEF.to_utf8_buffer()
		test.assert_false(bytes.is_empty(),
				"%s is available in the committed fixture" % filename)
		entries.append({"name": filename, "bytes": bytes})
	return entries


## The three boot-table archives (language / localres / resource) written into
## `dir` from the minimal pack; `with_baked_terrain` packs the Tmap payload +
## textures into resource.pff (the shell boot needs them: the minimal TRN is
## CPT-less and cannot create render RIDs); `without_mission_loadout` packs
## the kit-less mnml.bms (shell_archive_entries).
static func stage_shell_archives(test: GutTest, dir: String,
		with_baked_terrain := true, without_mission_loadout := false) -> void:
	var language_entries := shell_archive_entries(test, SHELL_LANGUAGE_FILES)
	var localres_entries := shell_archive_entries(test, SHELL_LOCALRES_FILES,
			without_mission_loadout, dir)
	var resource_entries := shell_archive_entries(test, SHELL_RESOURCE_FILES)
	if with_baked_terrain:
		# The minimal TRN is intentionally CPT-less and cannot create render RIDs.
		# Pack the substituted TRN's baked payload + textures so this regression
		# reaches the exact raw-patch visibility leak.
		for filename in SHELL_BAKED_TERRAIN_FILES:
			var bytes := FileAccess.get_file_as_bytes(TMAP_FIXTURE_DIR.path_join(filename))
			test.assert_false(bytes.is_empty(),
					"%s is available in the baked fixture" % filename)
			resource_entries.append({"name": filename, "bytes": bytes})
	write_pff(test, dir.path_join("language.pff"), language_entries)
	write_pff(test, dir.path_join("localres.pff"), localres_entries)
	write_pff(test, dir.path_join("resource.pff"), resource_entries)


## The packed runtime shell booted into its main menu under the test (one
## process frame), or null when the scene did not instantiate. The persisted
## resource dir / expansion / game are pointed at the staged archives (the
## caller saves and restores the settings file around the test). The shell is
## a plain child: pair with release_shell() in after_each. `without_mission_loadout`
## stages the kit-less mnml.bms (stage_shell_archives).
static func boot_shell(test: GutTest, without_mission_loadout := false) -> MainGame:
	_last_shell_dir = OS.get_cache_dir().path_join(
			"opennova_main_game_lifecycle_%d" % Time.get_ticks_usec())
	test.assert_eq(DirAccess.make_dir_recursive_absolute(_last_shell_dir), OK)
	test.assert_eq(SHELL_LANGUAGE_FILES.size() + SHELL_LOCALRES_FILES.size()
			+ SHELL_RESOURCE_FILES.size(), 35,
			"the retail-shaped archives contain every minimal fixture resource")
	stage_shell_archives(test, _last_shell_dir, true, without_mission_loadout)

	ResourceDirSettings.set_resource_dir(_last_shell_dir)
	ResourceDirSettings.set_expansion("")
	ResourceDirSettings.set_game("jo")
	var packed := load(MAIN_GAME_SCENE_PATH) as PackedScene
	var shell := packed.instantiate() as MainGame
	test.assert_not_null(shell)
	if shell == null:
		return null
	test.add_child(shell)
	await test.get_tree().process_frame
	var menu_shell := shell.get_menu_shell()
	test.assert_eq(menu_shell.get_current_menu_file().to_lower(),
			"main.mnu", "the packed fixture boots through the real menu shell")
	return shell


## Start `mission_name` through the real front end (the same public intent the
## mission-list ACCEPT command emits) and wait for the world to load and the
## shell's loading gate to close. Returns whether it did within `frame_limit`.
static func start_shell_mission(test: GutTest, shell: MainGame,
		mission_name := MINIMAL_MISSION, frame_limit := 240) -> bool:
	shell.get_menu_shell().start_requested.emit(mission_name)
	var world := shell.get_world()
	for _frame in range(frame_limit):
		if world.is_loaded() and not shell.is_world_loading():
			return true
		await test.get_tree().process_frame
	return world.is_loaded() and not shell.is_world_loading()


## Release a boot_shell() shell: the mounted archive handles go before the
## caller deletes the staged directory (unload the world, clear both roots,
## free the shell, one frame, stop the menu music). Null is fine.
static func release_shell(test: GutTest, shell: MainGame) -> void:
	if shell != null and is_instance_valid(shell):
		var world := shell.get_world()
		if world != null:
			world.unload()
			var world_root := world.get_resource_root()
			if world_root != null:
				world_root.clear()
		var menu_shell := shell.get_menu_shell()
		if menu_shell != null:
			var menu_root := menu_shell.get_resource_root()
			if menu_root != null:
				menu_root.clear()
		shell.queue_free()
	await test.get_tree().process_frame
	MusicService.stop_context()
