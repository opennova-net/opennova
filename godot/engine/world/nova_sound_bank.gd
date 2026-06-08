class_name NovaSoundBank
extends RefCounted

## Runtime playback over one or more loaded .lwf banks (NovaLwfData). Indexes
## sound sets by name (case-insensitive across all banks), decodes member .wav
## bytes via NovaWavLoader (cached), and spawns AudioStreamPlayer3D voices. The
## member-selection state machine (FIRST / RANDOM / SEQUENTIAL / RANDOM_SEQUENTIAL
## with per-layer state, originally the archive's sound_set.cpp) lives in portable
## C++ (libs/audio, via NovaSoundSelector); this only feeds it the layer's member
## count + mode and poses the chosen member.
##
## Resolution is NAME-keyed, matching the engine (SoundProfile_FindLoadedByName @
## Jointops 0x5274f0); the .lwf Multi.target_id is NOT used. See notes/lwf/grill.md.

# Mirrors NovaLwfData / opennova::audio::SelectionMode selection-mode constants.
const SELECTION_FIRST := 0
const SELECTION_RANDOM := 1
const SELECTION_SEQUENTIAL := 2
const SELECTION_RANDOM_SEQ := 3

var _resource_root  # NovaResourceRoot
var _banks: Array = []  # Array[NovaLwfData]
# name(lower) -> Array[{bank:int, set:int}]
var _index: Dictionary = {}
# The portable member-selection state machine (libs/audio); holds the per-(bank,set,layer) state.
var _selector := NovaSoundSelector.new()
# wav basename(lower) -> AudioStreamWAV (or null if it failed to resolve/decode)
var _wav_cache: Dictionary = {}


func _init(resource_root) -> void:
	_resource_root = resource_root


## Index a loaded NovaLwfData bank. Sets already indexed under a name win (banks
## added first take precedence), matching "load mission bank, then global".
func add_bank(lwf) -> void:
	if lwf == null or not lwf.is_loaded():
		return
	var bank_i := _banks.size()
	_banks.append(lwf)
	for si in lwf.get_set_count():
		var name := String(lwf.get_set(si).get("name", "")).to_lower()
		if name.is_empty():
			continue
		if not _index.has(name):
			_index[name] = []
		(_index[name] as Array).append({"bank": bank_i, "set": si})


func has_set(name: String) -> bool:
	return _index.has(name.to_lower())


func get_set_names() -> PackedStringArray:
	var out := PackedStringArray()
	for k in _index.keys():
		out.append(k)
	return out


## Spawn a looping ambient voice for the named sound set at `world_pos`, parented
## under `parent`. One AudioStreamPlayer3D per layer. Returns the holder Node3D, or
## null if the set is unknown or no member resolves to audio.
func spawn_ambient(parent: Node3D, world_pos: Vector3, name: String, bus: StringName) -> Node3D:
	var loc := _find_set(name)
	if loc.is_empty():
		return null
	var lwf = _banks[loc.bank]
	var set_d: Dictionary = lwf.get_set(loc.set)
	var layers: Array = set_d.get("layers", [])
	var holder: Node3D = null
	for li in layers.size():
		var layer_d: Dictionary = layers[li]
		var member := _pick_member(layer_d, loc.bank, loc.set, li)
		if member.is_empty():
			continue
		var stream := _resolve_stream(member)
		if stream == null:
			continue
		if holder == null:
			holder = Node3D.new()
			holder.name = "Ambient_%s" % name
			holder.position = world_pos
			parent.add_child(holder)
		var player := _make_player(stream, layer_d, member, bus, true)
		holder.add_child(player)
		player.play()
	return holder


## Fire a one-shot voice for the named set at a world position (PlayWavList /
## event actions). Auto-frees when finished. Returns true if anything played.
func play_oneshot_3d(parent: Node3D, world_pos: Vector3, name: String, bus: StringName) -> bool:
	var loc := _find_set(name)
	if loc.is_empty():
		return false
	var lwf = _banks[loc.bank]
	var set_d: Dictionary = lwf.get_set(loc.set)
	var layers: Array = set_d.get("layers", [])
	var played := false
	for li in layers.size():
		var layer_d: Dictionary = layers[li]
		var member := _pick_member(layer_d, loc.bank, loc.set, li)
		if member.is_empty():
			continue
		var stream := _resolve_stream(member)
		if stream == null:
			continue
		var player := _make_player(stream, layer_d, member, bus, false)
		player.position = world_pos
		parent.add_child(player)
		player.finished.connect(player.queue_free)
		player.play()
		played = true
	return played


## Fire a one-shot, NON-positional voice for the named set (mission dialog/voice is
## centered and full-volume, not 3D-attenuated). Auto-frees on finish. Returns true
## if anything played.
func play_oneshot_2d(parent: Node, name: String, bus: StringName) -> bool:
	var loc := _find_set(name)
	if loc.is_empty():
		return false
	var lwf = _banks[loc.bank]
	var set_d: Dictionary = lwf.get_set(loc.set)
	var layers: Array = set_d.get("layers", [])
	var played := false
	for li in layers.size():
		var layer_d: Dictionary = layers[li]
		var member := _pick_member(layer_d, loc.bank, loc.set, li)
		if member.is_empty():
			continue
		var stream := _resolve_stream(member)
		if stream == null:
			continue
		var player := AudioStreamPlayer.new()
		if bus != StringName() and AudioServer.get_bus_index(bus) >= 0:
			player.bus = bus
		var base_pitch := float(member.get("base_pitch", 1.0))
		player.pitch_scale = base_pitch if base_pitch > 0.01 else 1.0
		var volume := int(member.get("volume", 255))
		player.volume_db = linear_to_db(clampf(float(volume) / 255.0, 0.0001, 1.0))
		player.stream = stream
		parent.add_child(player)
		player.finished.connect(player.queue_free)
		player.play()
		played = true
	return played


# --- Internals ---

func _find_set(name: String) -> Dictionary:
	var key := name.to_lower()
	if not _index.has(key):
		return {}
	var hits: Array = _index[key]
	return hits[0] if hits.size() > 0 else {}


func _make_player(stream: AudioStreamWAV, layer_d: Dictionary, member: Dictionary, bus: StringName, loop: bool) -> AudioStreamPlayer3D:
	var player := AudioStreamPlayer3D.new()
	# Loop the stream copy (not the cached one's loop flag for one-shots): duplicate
	# so the looping ambient flag never leaks into a shared cached one-shot.
	var s := stream
	if loop:
		s = stream.duplicate()
		s.loop_mode = AudioStreamWAV.LOOP_FORWARD
		s.loop_begin = 0
		s.loop_end = 0
	player.stream = s
	# Only route to a bus that actually exists; otherwise keep the default (Master)
	# so a missing/renamed bus can never silence the voice.
	if bus != StringName() and AudioServer.get_bus_index(bus) >= 0:
		player.bus = bus
	var base_pitch := float(member.get("base_pitch", 1.0))
	player.pitch_scale = base_pitch if base_pitch > 0.01 else 1.0
	var volume := int(member.get("volume", 255))
	player.volume_db = linear_to_db(clampf(float(volume) / 255.0, 0.0001, 1.0))
	# Mission units map 1:1 to Godot units (mission_object_placer does no scaling).
	# min_distance -> the reference distance for nominal volume; falloff -> cull range.
	var min_distance := float(layer_d.get("min_distance", 0))
	var falloff := float(layer_d.get("falloff", 0))
	if min_distance > 0.0:
		player.unit_size = min_distance
	if falloff > 0.0:
		player.max_distance = falloff
	return player


# Pick the member to play for one layer. The selection STATE MACHINE (mode + per-layer cursor/bag)
# lives in libs/audio (NovaSoundSelector); here we only feed it the member count + mode and return
# the chosen member dictionary. Faithful to the engine's per-layer member selection.
func _pick_member(layer_d: Dictionary, bank: int, set_i: int, layer_i: int) -> Dictionary:
	var members: Array = layer_d.get("members", [])
	if members.is_empty():
		return {}
	var mode := int(layer_d.get("selection_mode", SELECTION_FIRST))
	var idx := int(_selector.select_member(bank, set_i, layer_i, members.size(), mode))
	if idx < 0 or idx >= members.size():
		return {}
	return members[idx]


func _resolve_stream(member: Dictionary) -> AudioStreamWAV:
	var wav_path := String(member.get("wav_path", ""))
	if wav_path.is_empty():
		return null
	var name := wav_path.get_file().to_lower()
	if _wav_cache.has(name):
		return _wav_cache[name]
	var stream: AudioStreamWAV = null
	if _resource_root != null and _resource_root.has_method("read_file"):
		var bytes: PackedByteArray = _resource_root.read_file(wav_path.get_file())
		if not bytes.is_empty():
			stream = NovaWavLoader.from_bytes(bytes)
	_wav_cache[name] = stream
	return stream
