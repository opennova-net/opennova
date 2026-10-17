extends GutTest

## A forced quit with the menu music streaming (C-164, the D-7 reproduction): a
## child launch of the bundled menu whose --quit-after lands well after the
## menu's music has started exits 0. Before the SBF stream handed the
## AudioServer a built-in AudioStreamWAV playback, the server's teardown, which
## Godot runs after unregistering every extension class, freed the extension
## playback it still listed through its unregistered class: signal 11 at exit
## (3 of 3 runs with --verbose; docs/audio/mus-sbf-re.md "Godot playback
## shutdown").

var _dirs: Array[String] = []
var _config: TestFs.Snapshot


func before_each() -> void:
	_config = TestFs.snapshot(ResourceDirSettings.CONFIG_PATH)
	ResourceDirSettings.set_game("jo")


func after_each() -> void:
	for dir in _dirs:
		TestFs.remove_dir_recursive(dir)
	_dirs.clear()
	_config.restore()


func test_forced_quit_with_the_menu_music_streaming_exits_cleanly() -> void:
	var dir := OS.get_cache_dir().path_join("opennova_forced_quit_%d" % Time.get_ticks_usec())
	assert_eq(DirAccess.make_dir_recursive_absolute(dir), OK)
	_dirs.append(dir)
	var output: Array = []
	var command := PackedStringArray([
		"--headless", "--path", ProjectSettings.globalize_path("res://"),
		"--disable-render-loop", "--disable-crash-handler", "--quit-after", "900", "--verbose",
		"--", "--working-dir", dir,
	])
	var status := OS.execute(OS.get_executable_path(), command, output, true)
	var log_text := "\n".join(output)
	assert_string_contains(log_text, "OpenNova: bundled menu up from")
	# MusicDirector's verbose line as a stream starts: the quit lands with the
	# music streaming.
	assert_string_contains(log_text, "MusicDirector: playing stream")
	assert_eq(status, 0, log_text)
