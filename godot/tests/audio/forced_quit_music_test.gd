extends GutTest

## A quit with the music streaming (C-164, the D-7 reproduction): a child launch
## whose MusicService streams the synthetic fixture bank and quits from code
## mid-stream, bypassing the shell's orderly drain (support/
## forced_quit_music_child.gd), exits 0. Before the SBF stream handed the
## AudioServer a built-in AudioStreamWAV playback, the server's teardown, which
## Godot runs after unregistering every extension class, freed the extension
## playback it still listed through its unregistered class: signal 11 at exit
## (docs/audio/mus-sbf-re.md "Godot playback shutdown"). The child streams
## fixtures/ alone, so the test does not depend on the base game's art.

const CHILD := "res://tests/support/forced_quit_music_child.gd"


func test_forced_quit_with_the_music_streaming_exits_cleanly() -> void:
	var output: Array = []
	var command := PackedStringArray([
		"--headless", "--path", ProjectSettings.globalize_path("res://"),
		"--disable-crash-handler", "--quit-after", "6000", "-s", CHILD,
	])
	var status := OS.execute(OS.get_executable_path(), command, output, true)
	var log_text := "\n".join(output)
	assert_true(log_text.contains("forced_quit_music_child: streaming"),
			"the child's MusicDirector reached real streaming playback before the quit")
	assert_eq(status, 0, "the quit with the music streaming exits cleanly")
	if status != 0:
		# The tail only, for the crash's last lines.
		gut.p(log_text.right(2000))
