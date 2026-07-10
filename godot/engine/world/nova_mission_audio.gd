class_name NovaMissionAudio
extends RefCounted

## Runtime mission audio orchestrator. Loads the mission's co-named .LWF + the
## global banks into a NovaSoundBank, resolves each placed sound marker to a
## sound set BY NAME (items.def soundloop_1..7 -> Multi.name; the engine is
## name-keyed, not target_id — see docs/audio/lwf-dbf-sound-re.md), and spawns a looping
## AudioStreamPlayer3D voice at the marker. Also exposes the PlayWavList action
## seam and the music/reverb bed. Voice culling runs from tick(camera_pos).
##
## Bank chain vs the original: Game_StartMission loads six global slots in order
## [<exp>L.lwf, <exp>.lwf, gamelocl.lwf, game.lwf, game3.lwf, game2.lwf] (name
## table @ 0x82A5B0, loop @ 0x525448; expansion names filled by
## Expansion_LoadAssets @ 0x4a495e), and the mission co-named .lwf is loaded
## separately as the DIALOG bank (DialogManager_LoadFromFile @ 0x44e7d4, only
## when the .dbf exists, with a .pwf fallback). We load one merged chain with
## the co-named bank first (it carries the dialog voices) then the globals in
## the engine's slot order; expansion banks are not loaded yet (no expansion
## name is plumbed into the world — host follow-up).

const MissionObjectPlacer := preload("res://engine/mission/mission_object_placer.gd")

const AMBIENT_BUS := &"Ambient"
const SFX_BUS := &"SFX"
const VOICE_BUS := &"Voice"
# Global banks in the engine's slot/search order [orig: @ 0x82A5B0 table]:
# gamelocl.LWF (localized voice) before game.lwf (ambient loops / SFX, LPNV_*),
# then the optional game3/game2 overflow banks (absent in JO base assets).
const GLOBAL_LWFS: PackedStringArray = ["gamelocl.LWF", "game.lwf", "game3.lwf", "game2.lwf"]
const CULL_RADIUS := 240.0  # mission units (== Godot units); pause beyond this

# Marker -> sound set resolution strategy. The faithful default is the marker
# item's items.def soundloop_1..7 set names (e.g. id 106178 "snd: Lp Flourescent
# Light" -> soundloop_1 LPNV_LIGHT) [orig: ItemDef_ParseProperty @ 0x49fec4];
# the engine is name-keyed (docs/audio/lwf-dbf-sound-re.md). The others stay as seams.
const STRATEGY_ITEM_SOUNDLOOP := 0
const STRATEGY_MARKER_NAME := 1
const STRATEGY_TARGET_ID := 2

var _resource_root  # NovaResourceRoot
var _item_db  # NovaItemDatabase
var _bank: NovaSoundBank
var _dbf  # NovaDbfData (mission co-named dialog bank; null if absent)
var _audio_root: Node3D
var _markers: Array = []  # [{ node:Node3D, pos:Vector3, players:Array, paused:bool }]
var _strategy: int = STRATEGY_ITEM_SOUNDLOOP
var _stats: Dictionary = {}
# Serialized dialog playback. The engine plays one dialog audio channel at a time
# (Dialog_Register @ 0x44d980 queues, Dialog_UpdatePlayback @ 0x44e470 only loads
# the next clip once the active channel frees), so we queue resolved line
# set-names and play them one after another instead of firing every PlayWavList
# at once.
var _dialog_queue: Array = []  # pending set names (resolved group lines), FIFO
var _dialog_voice: AudioStreamPlayer = null  # currently-playing dialog voice, or null
# WAC wave/pwave scripted voice: a single dedicated channel the engine RESETS
# before each play (a new wave interrupts the previous one), independent of the
# .DBF dialog queue [orig: wave/pwave @ 0x4ed610, channel dword_C6EC30].
var _wac_voice: AudioStreamPlayer = null
var _wac_wav_cache: Dictionary = {}  # filename(lower) -> AudioStreamWAV (or null)
var _perf_tick_us: int = 0
var _perf_markers: int = 0
var _perf_voice_writes: int = 0


func _init(resource_root, item_db) -> void:
	_resource_root = resource_root
	_item_db = item_db


## Load banks, spawn ambient marker voices under `container`, apply the reverb bed.
## `mission_name` is the .bms filename (its basename selects the co-named .LWF).
## Returns a stats dictionary.
func setup(mission, mission_name: String, container: Node3D) -> Dictionary:
	_stats = {"markers_total": 0, "markers_resolved": 0, "banks_loaded": 0, "voices": 0}
	if mission == null or container == null or _resource_root == null:
		return _stats

	_bank = NovaSoundBank.new(_resource_root)
	_load_bank(mission_name.get_file().get_basename() + ".LWF")
	# Expansion bank slots 0/1 ahead of the static banks, engine slot order
	# [orig: Expansion_LoadAssets @ 0x4a4989 (<exp>L.lwf) / @ 0x4a495e
	# (<exp>.lwf); slot table @ 0x82A5B0]. Missing files skip like retail's
	# SoundBank_LoadIfExists (D-SND-2 closed).
	if _resource_root.has_method("get_expansion"):
		var exp_name: String = _resource_root.get_expansion()
		if exp_name != "":
			_load_bank(exp_name + "L.lwf")
			_load_bank(exp_name + ".lwf")
	for global_name in GLOBAL_LWFS:
		_load_bank(global_name)

	# The mission's co-named .DBF maps a PlayWavList dialog id (dlg001) to the LWF
	# set name(s) it plays; loaded only if present.
	var dbf_name := mission_name.get_file().get_basename() + ".DBF"
	if _resource_root.has_file(dbf_name):
		var dbf = NovaDbfData.new()
		if dbf.open_from_resource_root(_resource_root, dbf_name) == OK:
			_dbf = dbf
			_stats["dialogs"] = dbf.get_dialog_count()

	_audio_root = Node3D.new()
	_audio_root.name = "MissionAudio"
	container.add_child(_audio_root)

	for e in mission.get_all_entities():
		var entity: Dictionary = e
		if int(entity.get("kind", -1)) != NovaMissionData.KIND_MARKER:
			continue
		_stats.markers_total += 1
		var name := _resolve_name(entity)
		if name.is_empty() or not _bank.has_set(name):
			continue
		var pos: Vector3 = MissionObjectPlacer.bms_to_godot_position(entity.get("position", Vector3.ZERO))
		var node := _bank.spawn_ambient(_audio_root, pos, name, AMBIENT_BUS)
		if node != null:
			var players: Array[AudioStreamPlayer3D] = []
			for child in node.get_children():
				if child is AudioStreamPlayer3D:
					players.append(child)
			_markers.append({"node": node, "pos": pos, "players": players, "paused": false})
			_stats.markers_resolved += 1
			_stats.voices += players.size()

	# Silence here has historically gone unnoticed (a bare stats print) — warn on
	# the two states that mean "no ambience will play" so they surface in logs.
	if int(_stats.banks_loaded) == 0:
		push_warning("NovaMissionAudio: no sound banks loaded (probed %s.LWF, expansion, %s) — mission ambience will be silent" % [
			mission_name.get_file().get_basename(), ", ".join(GLOBAL_LWFS)])
	elif int(_stats.markers_total) > 0 and int(_stats.markers_resolved) == 0:
		push_warning("NovaMissionAudio: 0/%d sound markers resolved (item db %s) — mission ambience will be silent" % [
			int(_stats.markers_total),
			"missing" if _item_db == null else "loaded"])

	_apply_reverb(int(mission.get_info().get("reverb", 0)))
	_apply_music(int(mission.get_info().get("music", 0)))
	return _stats


func get_stats() -> Dictionary:
	return _stats


func get_perf_counters() -> Dictionary:
	return {
		"tick_us": _perf_tick_us,
		"markers": _perf_markers,
		"voice_writes": _perf_voice_writes,
	}


func get_bank() -> NovaSoundBank:
	return _bank


## PlayWavList / event-action seam: fire a one-shot sound set by name at a world
## position. The .bms action param -> set-name decode is left to the caller (the
## engine resolves a pre-loaded sound_id handle; see docs/audio/lwf-dbf-sound-re.md
## ActionSlot_PlaySound @0x4010c0).
func fire_soundset(name: String, world_pos: Vector3) -> bool:
	if _bank == null or _audio_root == null:
		return false
	return _bank.play_oneshot_3d(_audio_root, world_pos, name, SFX_BUS)


## Enqueue a mission dialog by its PlayWavList id (param1). Resolution, faithful
## first: dialog id "dlg%03d" -> co-named .DBF -> def_id set name(s); then direct
## set-name fallbacks. Playback is SERIALIZED: the original plays one dialog audio
## channel at a time [orig: Dialog_PlayByIndex @ 0x527ae0 -> Dialog_PlayByName
## @ 0x44d9f0 -> Dialog_Register @ 0x44d980 queue; Dialog_UpdatePlayback @ 0x44e470
## advances only when the active channel frees], so the resolved line set-names are
## queued and played one after another instead of all at mission start. Returns true
## if the id resolved to at least one playable set.
func play_dialog(wav_id: int) -> bool:
	if _bank == null or _audio_root == null:
		return false
	var sets := _resolve_dialog_sets(wav_id)
	if sets.is_empty():
		push_warning("NovaMissionAudio: unresolved dialog id %d" % wav_id)
		return false
	for s in sets:
		_dialog_queue.append(s)
	_pump_dialog_queue()
	return true


## Resolve-only (no playback) for tests/diagnostics: the first set name a dialog id
## maps to that the loaded banks actually contain, or "" if none.
func resolve_dialog_set(wav_id: int) -> String:
	var sets := _resolve_dialog_sets(wav_id)
	return String(sets[0]) if not sets.is_empty() else ""


# Resolve a PlayWavList dialog id to the ordered set name(s) it should play. With a
# co-named .DBF, that is the dialog group's line set-names (played in sequence, one
# per dialog "line" as the engine advances entry index in Dialog_UpdatePlayback);
# without a .DBF, the first direct set-name form that the banks contain.
func _resolve_dialog_sets(wav_id: int) -> Array:
	if _bank == null:
		return []
	var out: Array = []
	var dlg_id := "dlg%03d" % wav_id
	if _dbf != null and _dbf.is_loaded():
		for def_id in _dbf.resolve_dialog(dlg_id):
			var n := String(def_id)
			if not n.is_empty() and _bank.has_set(n):
				out.append(n)
	if not out.is_empty():
		return out
	for n in ["DLG%03d" % wav_id, dlg_id, str(wav_id)]:
		if _bank.has_set(n):
			return [n]
	return out


# Start the next queued dialog line if nothing is currently playing. A line that
# fails to actually spawn is skipped so the queue never stalls.
func _pump_dialog_queue() -> void:
	if _dialog_voice != null and is_instance_valid(_dialog_voice):
		return  # a line is still playing; _on_dialog_finished pumps the next
	_dialog_voice = null
	while not _dialog_queue.is_empty():
		var name := String(_dialog_queue.pop_front())
		var voice := _bank.spawn_oneshot_2d(_audio_root, name, VOICE_BUS)
		if voice != null:
			_dialog_voice = voice
			voice.finished.connect(_on_dialog_finished)
			return


func _on_dialog_finished() -> void:
	if _dialog_voice != null and is_instance_valid(_dialog_voice):
		_dialog_voice.queue_free()
	_dialog_voice = null
	_pump_dialog_queue()


## Play a WAC-scripted voice .wav by filename [orig: wave/pwave @ 0x4ed610]. Loads it
## from the VFS and plays it non-positional on a single dedicated channel that
## REPLACES any currently-playing wave (the engine resets the channel before each
## play [orig: AudioChannel_ResetByHandle(dword_C6EC30) @ 0x4ed625]), so a new
## scripted line interrupts the previous one. Independent of the .DBF dialog queue
## (they may overlap). Returns true if the wav resolved and played.
func play_wac_wave(filename: String) -> bool:
	if _audio_root == null or _resource_root == null or filename.is_empty():
		return false
	var stream := _resolve_wav(filename)
	if stream == null:
		push_warning("NovaMissionAudio: WAC wave '%s' did not resolve" % filename)
		return false
	if _wac_voice == null or not is_instance_valid(_wac_voice):
		_wac_voice = AudioStreamPlayer.new()
		if AudioServer.get_bus_index(VOICE_BUS) >= 0:
			_wac_voice.bus = VOICE_BUS
		_audio_root.add_child(_wac_voice)
	_wac_voice.stream = stream
	_wac_voice.play()  # play() on an active player restarts it -> interrupts the previous wave
	return true


# Resolve + cache a .wav by filename through the VFS (tolerates a missing .wav
# extension). Returns the decoded AudioStreamWAV, or null.
func _resolve_wav(filename: String) -> AudioStreamWAV:
	var key := filename.to_lower()
	if _wac_wav_cache.has(key):
		return _wac_wav_cache[key]
	# Explicit type: _resource_root is untyped, so := cannot infer read_file's return.
	var bytes: PackedByteArray = _resource_root.read_file(filename)
	if bytes.is_empty() and not key.ends_with(".wav"):
		bytes = _resource_root.read_file(filename + ".wav")
	var stream: AudioStreamWAV = null
	if not bytes.is_empty():
		stream = NovaWavLoader.from_bytes(bytes)
	_wac_wav_cache[key] = stream
	return stream


## Pause ambient voices outside the cull radius around the listener; resume inside.
## Godot's max_distance already silences far voices; this also frees their mixing.
func tick(camera_pos: Vector3) -> void:
	var start := Time.get_ticks_usec()
	var cull_sq := CULL_RADIUS * CULL_RADIUS
	var writes := 0
	for m in _markers:
		var holder: Node3D = m.node
		if holder == null or not is_instance_valid(holder):
			continue
		var paused: bool = (m.pos as Vector3).distance_squared_to(camera_pos) > cull_sq
		if bool(m.get("paused", false)) == paused:
			continue
		m["paused"] = paused
		var players: Array = m.get("players", [])
		for player in players:
			if player is AudioStreamPlayer3D and is_instance_valid(player):
				if player.stream_paused != paused:
					player.stream_paused = paused
					writes += 1
	_perf_markers = _markers.size()
	_perf_voice_writes = writes
	_perf_tick_us = Time.get_ticks_usec() - start


func teardown() -> void:
	# Dropping _audio_root frees the dialog + wac voice nodes too; just drop our refs
	# so a late `finished` after teardown can't pump a freed queue.
	_dialog_queue.clear()
	_dialog_voice = null
	_wac_voice = null
	_wac_wav_cache.clear()
	if _audio_root != null and is_instance_valid(_audio_root):
		_audio_root.queue_free()
	_audio_root = null
	_markers.clear()
	_bank = null


# --- Internals ---

func _load_bank(lwf_name: String) -> void:
	if not _resource_root.has_file(lwf_name):
		return
	var lwf := NovaLwfData.new()
	if lwf.open_from_resource_root(_resource_root, lwf_name) == OK:
		_bank.add_bank(lwf)
		_stats.banks_loaded += 1


func _resolve_name(entity: Dictionary) -> String:
	match _strategy:
		STRATEGY_ITEM_SOUNDLOOP:
			if _item_db == null:
				return ""
			var item_id := int(entity.get("item_id", 0))
			# A "snd:" marker carries its looping ambient set(s) in soundloop_1..7;
			# play the first non-empty slot that resolves in the loaded bank(s).
			for loop_name in _item_db.get_sound_loops(item_id):
				var n := String(loop_name)
				if not n.is_empty() and _bank.has_set(n):
					return n
			# Fall back to the entity-attached sound_profile if no loop resolves.
			var sp := String(_item_db.get_sound_profile(item_id))
			return sp if (not sp.is_empty() and _bank.has_set(sp)) else ""
		STRATEGY_MARKER_NAME:
			return String(entity.get("name", ""))
		_:
			return ""


# Reverb id -> an AudioEffectReverb preset on the Ambient bus. The exact JO preset
# table (Audio_LoadReverbDefs @0x766d80) is not yet ported; this is a coarse,
# audible approximation gated on a non-zero mission reverb id.
func _apply_reverb(reverb_id: int) -> void:
	var bus_idx := AudioServer.get_bus_index(AMBIENT_BUS)
	if bus_idx < 0:
		return
	# Clear any reverb left by a previous mission.
	for i in range(AudioServer.get_bus_effect_count(bus_idx) - 1, -1, -1):
		if AudioServer.get_bus_effect(bus_idx, i) is AudioEffectReverb:
			AudioServer.remove_bus_effect(bus_idx, i)
	if reverb_id <= 0:
		return
	var reverb := AudioEffectReverb.new()
	reverb.room_size = clampf(0.4 + 0.08 * float(reverb_id), 0.0, 1.0)
	reverb.wet = 0.25
	reverb.dry = 0.9
	AudioServer.add_bus_effect(bus_idx, reverb)


# Witnessed no-op: the .bms header `music` field has NO live consumer in retail
# JO — the only per-entry music starter (Sbf_StartEntry @ 0x4ed910, the WAC
# `music` handler) targets a stream whose opener (Sbf_OpenFile_Gamemus
# @ 0x4ed6c0) is unreferenced, so nothing ever plays it. The field is vestigial
# data the mission editor round-trips (docs/audio/mus-sbf-re.md §Game music
# driving). Kept as the seam in case a sibling title turns out to consume it.
func _apply_music(_music_id: int) -> void:
	pass
