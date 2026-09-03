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
##   MissionPresentation can result).
## - boot_mission_data(): a MissionPresentation over an in-memory MissionData
##   (the entity-registry/discovery recipe), no VFS involved.

const MINIMAL_ASSETS_DIR := "res://../assets"
const TMAP_FIXTURE_DIR := "res://../fixtures/terrain/tmap"
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


## A MissionPresentation in the tree over an in-memory mission: the container
## and the runtime are parented under the test; `options` reach setup() as is.
static func boot_mission_data(test: GutTest, mission: MissionData,
		options: MissionSetupOptions = null) -> MissionPresentation:
	var container := Node3D.new()
	test.add_child_autofree(container)
	var runtime := MissionPresentation.new()
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
