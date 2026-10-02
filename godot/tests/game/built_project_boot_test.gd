extends GutTest

## The build's done-when (ADR 0046 d8): `opennova-project new -> create-missing ->
## build` yields a directory the runtime boots to the blank startup menu and leaves
## with exit 0, through real child processes like cli_startup_test.gd. Pending when
## the CLI is not built beside the project (the GUT CI job builds only the
## GDExtension; scripts/build.sh builds it).

const CLI_CANDIDATES := [
	"build/apps/project/Release/opennova-project.exe",
	"build/apps/project/opennova-project.exe",
	"build/apps/project/Debug/opennova-project.exe",
	"build/apps/project/opennova-project",
]
const PLAY_BUILDS := ".opennova/build/play"

var _dirs: Array[String] = []
var _saved_config := PackedByteArray()
var _had_config := false


func before_each() -> void:
	_had_config = FileAccess.file_exists(ResourceDirSettings.CONFIG_PATH)
	if _had_config:
		_saved_config = FileAccess.get_file_as_bytes(ResourceDirSettings.CONFIG_PATH)
	ResourceDirSettings.set_expansion("")
	ResourceDirSettings.set_game("jo")


func after_each() -> void:
	for dir in _dirs:
		TestFs.remove_dir_recursive(dir)
	_dirs.clear()
	if _had_config:
		var config := FileAccess.open(ResourceDirSettings.CONFIG_PATH, FileAccess.WRITE)
		config.store_buffer(_saved_config)
		config.close()
	elif FileAccess.file_exists(ResourceDirSettings.CONFIG_PATH):
		DirAccess.remove_absolute(ProjectSettings.globalize_path(ResourceDirSettings.CONFIG_PATH))


static func _find_cli() -> String:
	var repo_root := ProjectSettings.globalize_path("res://").trim_suffix("/").get_base_dir()
	for candidate in CLI_CANDIDATES:
		var path := repo_root.path_join(candidate)
		if FileAccess.file_exists(path):
			return path
	return ""


func _project(cli: String, args: PackedStringArray, expected_exit: int) -> String:
	var output: Array = []
	var status := OS.execute(cli, args, output, true)
	var text := "\n".join(output)
	assert_eq(status, expected_exit, "opennova-project %s:\n%s" % [" ".join(args), text])
	return text


func _launch_runtime(args: PackedStringArray, expected_exit: int) -> String:
	var output: Array = []
	var command := PackedStringArray([
		"--headless", "--path", ProjectSettings.globalize_path("res://"),
		"--disable-render-loop", "--disable-crash-handler", "--quit-after", "10", "--",
	])
	command.append_array(args)
	var status := OS.execute(OS.get_executable_path(), command, output, true)
	var log_text := "\n".join(output)
	assert_eq(status, expected_exit, log_text)
	assert_false(log_text.contains("SCRIPT ERROR"), log_text)
	assert_false(log_text.contains("GDExtension dynamic library not found"), log_text)
	return log_text


func test_built_project_boots_the_runtime() -> void:
	var cli := _find_cli()
	if cli.is_empty():
		pending("opennova-project is not built under build/apps/project (scripts/build.sh)")
		return
	var dir := OS.get_cache_dir().path_join("opennova built project %d" % Time.get_ticks_usec())
	assert_eq(DirAccess.make_dir_recursive_absolute(dir), OK)
	_dirs.append(dir)
	var project := dir.path_join("Game")
	_project(cli, ["new", project, "--title", "Boot Smoke", "--game", "jo"], 0)
	_project(cli, ["create-missing", project], 0)
	_project(cli, ["validate", project], 0)
	_project(cli, ["build", project], 0)

	var record_path := project.path_join(PLAY_BUILDS).path_join("last_good.json")
	assert_true(FileAccess.file_exists(record_path), "the build recorded its last good build")
	var record: Dictionary = JSON.parse_string(FileAccess.get_file_as_string(record_path))
	var build_dir := project.path_join(PLAY_BUILDS).path_join(String(record.get("build_id", "")))
	for archive in ["language.pff", "localres.pff", "resource.pff"]:
		assert_true(FileAccess.file_exists(build_dir.path_join(archive)),
			"%s is in the build directory %s" % [archive, build_dir])

	# The packed build boots to the blank startup menu without loose overrides.
	var log_text := _launch_runtime(["--resource-dir", build_dir], 0)
	assert_false(log_text.contains(ResourceRoot.boot_resource_missing_marker()), log_text)
