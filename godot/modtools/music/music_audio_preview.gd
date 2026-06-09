class_name MusicAudioPreview
extends Node

# Round-robin pool of AudioStreamPlayer children for in-editor previews. The
# pool is populated on demand so callers may invoke play_stream before _ready
# fires (for example, immediately after add_child during scene setup).

@export var pool_size: int = 2
@export var audio_bus: StringName = &"Master"

var _players: Array[AudioStreamPlayer] = []
var _next: int = 0


func _ready() -> void:
	_ensure_pool()


func play_stream(stream: AudioStream) -> void:
	_ensure_pool()
	if _players.is_empty() or stream == null:
		return
	var p: AudioStreamPlayer = _players[_next % _players.size()]
	_next += 1
	p.stream = stream
	p.play()


func stop_all() -> void:
	for p in _players:
		if p != null:
			p.stop()


func _ensure_pool() -> void:
	if not _players.is_empty():
		return
	for i in range(pool_size):
		var p := AudioStreamPlayer.new()
		p.name = "PreviewPlayer_%d" % i
		p.bus = audio_bus
		add_child(p)
		_players.append(p)
