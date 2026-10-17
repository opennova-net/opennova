extends SceneTree

## The child launch of forced_quit_music_test.gd: the MusicService autoload streams
## the synthetic fixture bank (fixtures/, which every test job pulls) through its
## MusicDirector, then the child quits from code with the stream still playing,
## bypassing the shell's orderly drain (MainGame.request_quit) as a forced quit
## does. Before the SBF stream handed the AudioServer a built-in playback, that
## exit crashed in the server's teardown (docs/audio/mus-sbf-re.md "Godot
## playback shutdown").

const SCRIPT_FIXTURE := "res://../fixtures/mus/synth_gamemus.bin"
const BANK_FIXTURE := "res://../fixtures/sbf/synth_gamemus.sbf"
## The frames the stream keeps playing past its start before the quit.
const FRAMES_STREAMING := 30
## The bound on the stream's start; past it the child reports and exits 2.
const START_TIMEOUT_MSEC := 5000

var _director: MusicDirector = null
var _started_msec := 0
var _streaming_frames := -1


func _process(_delta: float) -> bool:
	if _director == null:
		_start()
		return false
	if _streaming_frames < 0:
		if _director.has_pending_playback():
			print("forced_quit_music_child: streaming")
			_streaming_frames = 0
		elif Time.get_ticks_msec() - _started_msec > START_TIMEOUT_MSEC:
			print("forced_quit_music_child: no stream")
			quit(2)
		return false
	_streaming_frames += 1
	if _streaming_frames >= FRAMES_STREAMING:
		quit()
	return false


func _start() -> void:
	var service := root.get_node_or_null("MusicService")
	if service == null:
		print("forced_quit_music_child: no MusicService")
		quit(3)
		return
	_director = service.director()
	var bank := SbfBank.new()
	bank.load_from_path(BANK_FIXTURE)
	var script := MusicScript.new()
	script.load_from_path(SCRIPT_FIXTURE)
	_director.bank = bank
	_director.load_mus_script(script)
	_director.start()
	_started_msec = Time.get_ticks_msec()
