class_name SoundPreviewPlayer
extends Node

## Editor-local audition for the Sound workspace. Resolves a member's .wav from
## the shell's NovaResourceRoot (the loose game data), decodes it via
## NovaWavLoader (RIFF -> AudioStreamWAV, 8-bit-unsigned fix), and plays it on a
## single reused 2D AudioStreamPlayer with the member's pitch/volume applied.
## Non-positional and non-looping: this is a quick preview, not the runtime mixer.

var _player: AudioStreamPlayer


func _ensure_player() -> AudioStreamPlayer:
	if _player == null:
		_player = AudioStreamPlayer.new()
		_player.name = "SoundPreviewStream"
		add_child(_player)
	return _player


func preview_member(data: NovaLwfData, resource_root, si: int, li: int, mi: int) -> void:
	if data == null:
		return
	var member := data.get_member(si, li, mi)
	if member.is_empty():
		return
	_play_member(member, resource_root)


func preview_set(data: NovaLwfData, resource_root, si: int) -> void:
	# Editor-local path: audition the first member of the first non-empty layer.
	# (Runtime selection logic lives in Phase 4's nova_sound_bank; the editor only
	# needs a representative sample.)
	if data == null:
		return
	var set_d := data.get_set(si)
	var layers: Array = set_d.get("layers", [])
	for layer_d in layers:
		var members: Array = (layer_d as Dictionary).get("members", [])
		if members.size() > 0:
			_play_member(members[0], resource_root)
			return


func stop_preview() -> void:
	if _player != null:
		_player.stop()


func _play_member(member: Dictionary, resource_root) -> void:
	var wav_path := String(member.get("wav_path", ""))
	if wav_path.is_empty():
		push_warning("SoundPreviewPlayer: member has no .wav reference")
		return
	var name := wav_path.get_file()
	if resource_root == null or not resource_root.has_method("read_file"):
		push_warning("SoundPreviewPlayer: no resource root to resolve %s" % name)
		return
	var bytes: PackedByteArray = resource_root.read_file(name)
	if bytes.is_empty():
		push_warning("SoundPreviewPlayer: .wav not found in resource root: %s" % name)
		return
	var stream := NovaWavLoader.from_bytes(bytes)
	if stream == null:
		push_warning("SoundPreviewPlayer: could not decode .wav: %s" % name)
		return

	var player := _ensure_player()
	player.stream = stream
	var base_pitch := float(member.get("base_pitch", 1.0))
	player.pitch_scale = base_pitch if base_pitch > 0.01 else 1.0
	var volume := int(member.get("volume", 255))
	player.volume_db = linear_to_db(clampf(float(volume) / 255.0, 0.0, 1.0)) if volume > 0 else -80.0
	player.play()
