class_name NovaSoundBank
extends RefCounted

## Runtime playback over one or more loaded .lwf banks (NovaLwfData). Indexes
## sound sets by name (case-insensitive across all banks), decodes member .wav
## bytes via NovaWavLoader (cached), and spawns AudioStreamPlayer3D voices. The
## member-selection state machine (engine-faithful: sequential / random-anchor
## cycle / random default, one shared ROL-LCG stream) lives in portable C++
## (libs/audio, via NovaSoundSelector [orig: SoundBank_PlayTriggerEntries @
## 0x75ccd0]); this only feeds it the layer's member count + mode and poses the
## chosen member.
##
## Resolution is NAME-keyed and case-insensitive, matching the engine
## (SoundBank_FindTriggerByName @ 0x75be90 stricmp's set names;
## SoundBank_FindSetByNameAnyBank @ 0x5274f0); the .lwf Multi.target_id is
## never used for NAME resolution — its runtime meaning is the 3D one-shot
## CULL RANGE in whole units [orig: Sound_Play3DPositional @ 0x527cd1 reads
## set+72] (every JOX set carries one; the field rename is a tracked
## follow-up). See docs/audio/lwf-dbf-sound-re.md.

# The engine volume byte ceiling (member/clamp volumes, emitter fire volume)
# [orig: e.g. the full-volume emitter fire path passes 255 @ 0x528e20].
const VOLUME_BYTE_MAX := 255

# Mirrors NovaLwfData / opennova::audio::SelectionMode selection-mode constants.
const SELECTION_FIRST := 0
const SELECTION_RANDOM := 1
const SELECTION_SEQUENTIAL := 2
const SELECTION_RANDOM_SEQ := 3

var _resource_root  # NovaResourceRoot
# Occlusion provider (the NovaSimulation, or null): one-shot fire distances
# inflate through the witnessed two-ray LOS so occluded sources fire quieter /
# cull farther [orig: Sound_ApplyOcclusionDistance @ 0x529970, applied in
# Sound_Play3DPositional @ 0x527d95]. Null (tests/menu) fires unoccluded.
var occlusion_provider: Object = null
var _banks: Array = []  # Array[NovaLwfData]
# name(lower) -> Array[{bank:int, set:int}]
var _index: Dictionary = {}
# The portable member-selection state machine (libs/audio); holds the per-(bank,set,layer) state.
var _selector := NovaSoundSelector.new()
# wav basename(lower) -> AudioStreamWAV (or null if it failed to resolve/decode)
var _wav_cache: Dictionary = {}
# exclusive_key -> the gating voice of an exclusive one-shot (see play_oneshot_3d);
# entries go stale harmlessly (checked with is_instance_valid before use).
var _exclusive: Dictionary = {}


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


## Return lightweight layer descriptors for the mission ambient mixer. No WAV
## is read or decoded here: placed emitters remain data until one of the eight
## physical channels actually needs the layer. The emitter path always uses
## member 0 of each layer [orig: SoundEmitter_UpdateAndMixTop8 @ 0x528649].
func describe_ambient(name: String) -> Array:
	var loc := _find_set(name)
	if loc.is_empty():
		return []
	var lwf = _banks[loc.bank]
	var set_d: Dictionary = lwf.get_set(loc.set)
	var layers: Array = set_d.get("layers", [])
	var out: Array = []
	for li in layers.size():
		var layer_d: Dictionary = layers[li]
		var members: Array = layer_d.get("members", [])
		if members.is_empty():
			continue
		var member: Dictionary = members[0]
		var wav_path := String(member.get("wav_path", ""))
		if wav_path.is_empty():
			continue
		# Match _resolve_stream's basename lookup without paying its read/decode
		# cost during mission setup. Corrupt data is rejected lazily and cached
		# if the candidate first reaches the physical channel budget.
		if _resource_root == null or not _resource_root.has_method("read_file"):
			continue
		if _resource_root.has_method("has_file") and not _resource_root.has_file(wav_path.get_file()):
			continue
		out.append({
			"wav_path": wav_path,
			"falloff_radius": int(layer_d.get("falloff_radius", 0)),
			"min_distance": int(layer_d.get("min_distance", 0)),
			"volume": int(member.get("volume", 255)),
			"clamp_volume": int(member.get("clamp_volume", 255)),
			"base_pitch": float(member.get("base_pitch", 1.0)),
		})
	return out


## Resolve a descriptor returned by describe_ambient(). Description and decode
## are separate so hundreds of candidates can be ranked while only selected
## channel entrants cause VFS reads and WAV decoding.
func resolve_ambient_stream(descriptor: Dictionary) -> AudioStreamWAV:
	return _resolve_stream({"wav_path": String(descriptor.get("wav_path", ""))})


## Bind a resolved stream to a reusable physical ambient channel. Playback is
## owned by NovaMissionAudio: incumbents continue while new bindings restart.
static func configure_ambient_player(
		player: AudioStreamPlayer3D, stream: AudioStreamWAV,
		descriptor: Dictionary, bus: StringName) -> void:
	var loop_stream: AudioStreamWAV = stream.duplicate()
	loop_stream.loop_mode = AudioStreamWAV.LOOP_FORWARD
	loop_stream.loop_begin = 0
	loop_stream.loop_end = _stream_frames(loop_stream)
	player.stream = loop_stream
	player.attenuation_model = AudioStreamPlayer3D.ATTENUATION_DISABLED
	if bus != StringName() and AudioServer.get_bus_index(bus) >= 0:
		player.bus = bus
	var base_pitch := float(descriptor.get("base_pitch", 1.0))
	player.pitch_scale = base_pitch if base_pitch > 0.01 else 1.0


## Spawn the looping ambient voices for the named sound set at `world_pos`,
## parented under `parent`. One AudioStreamPlayer3D per layer, playing the
## layer's FIRST member — the emitter path does not run the selection machine
## [orig: SoundEmitter_UpdateAndMixTop8 @ 0x528649 reads layer+16 = member 0].
## Voices spawn SILENT and paused; the caller's mix tick (NovaMissionAudio)
## owns audibility via the witnessed distance model, reading each voice's
## layer/member params from its "layer_params" meta. Returns the holder Node3D,
## or null if the set is unknown or no member resolves to audio.
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
		var members: Array = layer_d.get("members", [])
		if members.is_empty():
			continue
		var member: Dictionary = members[0]
		var stream := _resolve_stream(member)
		if stream == null:
			continue
		if holder == null:
			holder = Node3D.new()
			holder.name = "Ambient_%s" % name
			holder.position = world_pos
			parent.add_child(holder)
		var player := _make_player(stream, member, bus, true, 0)
		player.set_meta("layer_params", {
			"falloff_radius": int(layer_d.get("falloff_radius", 0)),
			"min_distance": int(layer_d.get("min_distance", 0)),
			"volume": int(member.get("volume", 255)),
			"clamp_volume": int(member.get("clamp_volume", 255)),
		})
		holder.add_child(player)
		player.play()
		# Ambient candidates are data until the top-eight mixer selects them.
		# play() clears stream_paused, so pause only after starting the looping
		# playback, then remove the silent player from SceneTree processing. An
		# inherited AudioStreamPlayer3D keeps an internal physics callback alive
		# even at -80 dB; dense missions otherwise revisit hundreds of silent
		# candidates on every physics catch-up step.
		player.stream_paused = true
		player.process_mode = Node.PROCESS_MODE_DISABLED
	return holder


## Fire a one-shot voice for the named set at a world position (PlayWavList /
## event actions). Volume is computed ONCE at fire time from the witnessed
## distance model when `listener_pos` is known [orig: Sound_Play3DPositional
## @ 0x527cb0 -> SoundBank_PlayTriggerEntries @ 0x75ccd0 compute vol/pan at
## play, no per-frame update]; pass Vector3.INF to play distance-flat (menu /
## tests). Auto-frees when finished. Returns true if anything played.
func play_oneshot_3d(parent: Node3D, world_pos: Vector3, name: String, bus: StringName,
		listener_pos: Vector3 = Vector3.INF, source_bms_id: int = 0,
		exclusive_key: String = "") -> bool:
	var loc := _find_set(name)
	if loc.is_empty():
		return false
	# Exclusive one-shots: a non-empty key declines to RESTART the set while its
	# previous voice still plays. Reimpl stand-in for the engine folding every-tick
	# refires (chute flap / freefall retrigger each body tick) into its finite
	# channel pool — audibly one continuous sound either way (audio doc D-SND-10).
	if not exclusive_key.is_empty():
		var prev = _exclusive.get(exclusive_key)
		if prev != null and is_instance_valid(prev) and prev.playing:
			return false
	var lwf = _banks[loc.bank]
	var set_d: Dictionary = lwf.get_set(loc.set)
	var layers: Array = set_d.get("layers", [])
	var has_listener := listener_pos != Vector3.INF and listener_pos.is_finite()
	var dist_q16 := 0
	if has_listener:
		dist_q16 = int(world_pos.distance_to(listener_pos) * 65536.0)
		# The set-level 3D cull: beyond the set's range (Multi dword 18,
		# in-memory set+72) the one-shot does not fire at all — axis checks,
		# then euclidean, all <= range<<16 (equality passes); the euclidean
		# test subsumes the axis ones [orig: Sound_Play3DPositional
		# @ 0x527cd1-0x527d83].
		var cull_q16 := maxi(int(set_d.get("target_id", 0)), 0) << 16
		if dist_q16 > cull_q16:
			return false
		# Occlusion inflates the fire distance between the euclidean cull and
		# the recheck, and the INFLATED distance feeds the once-at-fire volume
		# snapshot [orig: the Sound_ApplyOcclusionDistance call @ 0x527d95 and
		# the <= range recheck @ 0x527da1].
		if occlusion_provider != null:
			dist_q16 = int(occlusion_provider.sound_occlusion_distance_q16(
				listener_pos, world_pos, dist_q16, source_bms_id))
			if dist_q16 > cull_q16:
				return false
	var played := false
	for li in layers.size():
		var layer_d: Dictionary = layers[li]
		var member := _pick_member(layer_d, loc.bank, loc.set, li)
		if member.is_empty():
			continue
		var stream := _resolve_stream(member)
		if stream == null:
			continue
		var vol255 := int(member.get("volume", 255))
		if has_listener:
			vol255 = oneshot_distance_volume(dist_q16, layer_d, member)
			if vol255 <= 0:
				continue
		var player := _make_player(stream, member, bus, false, vol255)
		player.position = world_pos
		parent.add_child(player)
		player.finished.connect(player.queue_free)
		player.play()
		if not exclusive_key.is_empty() and not played:
			_exclusive[exclusive_key] = player  # first layer's voice gates the refire
		played = true
	return played


## Spawn a one-shot, NON-positional voice for the named set (mission dialog/voice
## is centered and full-volume, not 3D-attenuated) and RETURN its
## AudioStreamPlayer (the first resolvable layer's voice) without auto-freeing it
## — the caller owns its lifetime and listens for `finished`. Used by the
## serialized dialog queue (the engine plays one dialog audio channel at a time:
## Dialog_UpdatePlayback @ 0x44e470 only advances when the active channel frees).
## Returns null if the set is unknown or no member resolves to audio.
func spawn_oneshot_2d(parent: Node, name: String, bus: StringName) -> AudioStreamPlayer:
	var loc := _find_set(name)
	if loc.is_empty():
		return null
	var lwf = _banks[loc.bank]
	var set_d: Dictionary = lwf.get_set(loc.set)
	var layers: Array = set_d.get("layers", [])
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
		player.play()
		return player
	return null


# --- Internals ---

func _find_set(name: String) -> Dictionary:
	var key := name.to_lower()
	if not _index.has(key):
		return {}
	var hits: Array = _index[key]
	return hits[0] if hits.size() > 0 else {}


func _make_player(stream: AudioStreamWAV, member: Dictionary, bus: StringName, loop: bool, vol255: int) -> AudioStreamPlayer3D:
	var player := AudioStreamPlayer3D.new()
	# Loop the stream copy (not the cached one's loop flag for one-shots): duplicate
	# so the looping ambient flag never leaks into a shared cached one-shot.
	var s := stream
	if loop:
		s = stream.duplicate()
		s.loop_mode = AudioStreamWAV.LOOP_FORWARD
		s.loop_begin = 0
		# loop_end is an absolute frame index and playback wraps the moment it is
		# reached — 0 does NOT mean "whole stream", it pins the voice at sample 0
		# forever (constant DC = silence). Loop the full decoded buffer.
		s.loop_end = _stream_frames(s)
	player.stream = s
	# The witnessed distance model owns volume (the engine computes a 0..255
	# channel volume from the two layer radii; Godot must not attenuate on top
	# of it — its inverse-distance curve AMPLIFIES inside unit_size, which is
	# how a marker bed drowned the mission dialog). Positional panning stays
	# (host approximation of the bearing-byte pan [orig: @ 0x5289c2]).
	player.attenuation_model = AudioStreamPlayer3D.ATTENUATION_DISABLED
	# Only route to a bus that actually exists; otherwise keep the default (Master)
	# so a missing/renamed bus can never silence the voice.
	if bus != StringName() and AudioServer.get_bus_index(bus) >= 0:
		player.bus = bus
	var base_pitch := float(member.get("base_pitch", 1.0))
	player.pitch_scale = base_pitch if base_pitch > 0.01 else 1.0
	player.volume_db = volume_db_from_255(vol255)
	return player


## dB for a 0..255 engine channel volume; 0 -> hard silent.
static func volume_db_from_255(vol255: int) -> float:
	return linear_to_db(clampf(float(vol255) / 255.0, 0.0001, 1.0)) if vol255 > 0 else -80.0


## The witnessed distance volume curve [orig: SoundBank_CalcDistanceVolPan
## @ 0x75ca20]: at or beyond `radius` the voice is HARD SILENT; inside it the
## volume runs vol * (1 - d/r)^2, ceilinged by `clamp_vol`. The master-fade
## global (g_SoundMasterFadeQ24, steady state 0xFF0000) folds in as
## (vol * 255) >> 8; the underwater halving flag and the pan half of the packed
## return are reimpl territory (bus volume / Godot's panner). Distances are Q16.16
## like the original's; both arms of the engine pass a consistent scale so only
## the ratio matters.
static func calc_distance_volume(dist_q16: int, radius_q16: int, vol255: int, clamp_vol: int) -> int:
	# The integer form lives in libs/audio (ambient_mixer.cpp) — one implementation
	# for the native mix, this one-shot path, and the GUT pins.
	return NovaAmbientMixer.calc_distance_volume(dist_q16, radius_q16, vol255, clamp_vol)


## Layer volume for the looping ambient-emitter path. `dist_q16` is the
## listener distance in Q16.16 units — the arms subtract in Q16 FIRST and
## truncate to whole units at the curve call, exactly like the original
## (HIWORD(dist - min) is floor(d - m), NOT min - floor(d): the proximity arm
## differs by a unit for fractional d) [orig: SoundEmitter_UpdateAndMixTop8
## @ 0x528667..0x5286df]. `vol_byte` is the emitter volume 0..255 (the
## time-of-day crossfade blend for placed markers); member volume and clamp
## scale by it before the curve [orig: @ 0x5286b9]. With a min_distance the
## falloff REBASES to run min..falloff; inside min_distance the volume RISES
## as (d/min)^2 (the proximity fade); a bare falloff runs 0..falloff.
static func emitter_layer_volume(dist_q16: int, falloff_u: int, min_u: int, vol_byte: int, member_vol: int, clamp_vol: int) -> int:
	# The arm forms live in libs/audio (ambient_mixer.cpp) beside the mix that
	# consumes them natively; this seam stays for the one-shot path and the pins.
	return NovaAmbientMixer.emitter_layer_volume(
			dist_q16, falloff_u, min_u, vol_byte, member_vol, clamp_vol)


## One-shot volume at fire time [orig: SoundBank_PlayTriggerEntries @ 0x75cf14..
## 0x75cf8b]: the proximity stage ((d/min)^2, only under min_distance) feeds the
## falloff stage ((1 - d/falloff)^2, NOT rebased — the one-shot path differs
## from the emitter path here). A layer with NO falloff radius plays at the RAW
## emitter volume — member volume is not consulted, and a min-only layer's
## proximity result is discarded with it [orig: @ 0x75cf88 the no-falloff branch
## stores emitter_info[2]]. Reimpl emitter volume is full (255): the engine's
## fire-time (vol * g_SoundVolumeOption) >> 8 folds the options slider we map to
## bus volume (docs/audio/lwf-dbf-sound-re.md D-SND-8).
func oneshot_distance_volume(dist_q16: int, layer_d: Dictionary, member: Dictionary) -> int:
	var vol := int(member.get("volume", 255))
	var clamp_vol := int(member.get("clamp_volume", 255))
	var min_q16 := int(layer_d.get("min_distance", 0)) << 16
	var falloff_q16 := int(layer_d.get("falloff_radius", 0)) << 16
	if falloff_q16 <= 0:
		return 255
	if min_q16 > 0 and dist_q16 < min_q16:
		vol = calc_distance_volume(min_q16 - dist_q16, min_q16, vol, clamp_vol)
	return calc_distance_volume(dist_q16, falloff_q16, vol, clamp_vol)


# Frame count of a decoded stream, exact from the byte size (get_length() *
# mix_rate re-derives it through a float). NovaWavLoader always emits 16-bit
# PCM; the 8-bit branch is for completeness — IMA-ADPCM never reaches here
# (the loader decodes it to 16-bit).
static func _stream_frames(s: AudioStreamWAV) -> int:
	var bytes_per_sample := 2 if s.format == AudioStreamWAV.FORMAT_16_BITS else 1
	var bytes_per_frame := bytes_per_sample * (2 if s.stereo else 1)
	return s.data.size() / bytes_per_frame


# Pick the member to play for one layer. The selection STATE MACHINE (mode + per-layer cursor/bag)
# lives in libs/audio (NovaSoundSelector); here we only feed it the member count + mode and return
# the chosen member dictionary. Faithful to the engine's per-layer member selection.
func _pick_member(layer_d: Dictionary, bank: int, set_i: int, layer_i: int) -> Dictionary:
	var members: Array = layer_d.get("members", [])
	if members.is_empty():
		return {}
	var mode := int(layer_d.get("selection_mode", SELECTION_RANDOM))
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
