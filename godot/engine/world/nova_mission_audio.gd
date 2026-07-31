class_name NovaMissionAudio
extends RefCounted

## Runtime mission audio orchestrator. Loads the mission's co-named .LWF + the
## global banks into a NovaSoundBank, resolves each placed envs-class entity's
## time-of-day slot sets BY NAME (items.def soundloop_1..4 = morning/day/
## evening/night [orig: Entity_UpdateEnvSoundEmitter @ 0x4a8080]; the engine is
## name-keyed — see docs/audio/lwf-dbf-sound-re.md), and retains each layer as
## lightweight candidate data. tick(camera_pos) runs the witnessed
## ambient emitter mix: per-voice two-radius distance volumes, region
## crossfades, and an eight-player physical channel pool [orig:
## SoundEmitter_UpdateAndMixTop8 @ 0x5284a0]. Also exposes the PlayWavList
## action seam and the music/reverb bed.
##
## Bank chain vs the original: Game_StartMission loads six global slots in order
## [<exp>L.lwf, <exp>.lwf, gamelocl.lwf, game.lwf, game3.lwf, game2.lwf] (name
## table @ 0x82A5B0, loop @ 0x525448; expansion names filled by
## Expansion_LoadAssets @ 0x4a495e), and the mission co-named .lwf is loaded
## separately as the DIALOG bank (DialogManager_LoadFromFile @ 0x44e7d4, only
## when the .dbf exists, with a .pwf fallback). We load one merged chain with
## the co-named bank first (it carries the dialog voices) then the globals in
## the engine's slot order; expansion banks are not loaded yet (no expansion
## name is plumbed into the world — reimpl follow-up).

const MissionObjectPlacer := preload("res://engine/mission/mission_object_placer.gd")


class TimeOfDayRegion extends RefCounted:
	var region: int
	var adjacent: int
	var blend: float

	func _init(p_region: int, p_adjacent: int, p_blend: float) -> void:
		region = p_region
		adjacent = p_adjacent
		blend = p_blend


const AMBIENT_BUS := &"Ambient"
const SFX_BUS := &"SFX"
const VOICE_BUS := &"Voice"
# Global banks in the engine's slot/search order [orig: @ 0x82A5B0 table]:
# gamelocl.LWF (localized voice) before game.lwf (ambient loops / SFX, LPNV_*),
# then the optional game3/game2 overflow banks (absent in JO base assets).
const GLOBAL_LWFS: PackedStringArray = ["gamelocl.LWF", "game.lwf", "game3.lwf", "game2.lwf"]

# Marker -> sound set resolution strategy. The faithful default is the marker
# item's items.def soundloop_1..7 set names (e.g. id 106178 "snd: Lp Flourescent
# Light" -> soundloop_1 LPNV_LIGHT) [orig: ItemDef_ParseProperty @ 0x49fec4];
# the engine is name-keyed (docs/audio/lwf-dbf-sound-re.md). The others stay as seams.
const STRATEGY_ITEM_SOUNDLOOP := 0
const STRATEGY_MARKER_NAME := 1
const STRATEGY_TARGET_ID := 2

# The ambient emitter mix budget: the engine sorts every in-range emitter voice
# by computed volume each frame and keeps the loudest 8 on real channels
# [orig: SoundEmitter_UpdateAndMixTop8 @ 0x5284a0, channel table @ 0x24D6688].
const MIX_CHANNELS := 8
const SILENT_DB := -80.0  # hard-silent floor for out-of-mix voices
# Time-of-day region cuts (4/10/17/21 h) and the ~5-game-minute crossfade margin
# live with the eval in libs/audio (ambient_mixer.cpp time_of_day_region)
# [orig: Entity_CalcTimeOfDayRegion @ 0x408110; margin @ 0x408203].

var _resource_root  # NovaResourceRoot
var _item_db  # NovaItemDatabase
var _simulation: Object = null  # NovaSimulation (occlusion LOS); optional
var _bank: NovaSoundBank
var _dbf  # NovaDbfData (mission co-named dialog bank; null if absent)
var _audio_root: Node3D
# Placed ambient markers ("snd:" items) are data, not scene nodes. Each carries
# the four time-of-day slot set names and lightweight layer descriptors for each
# distinct set. A candidate_id identifies one marker/set/layer for the lifetime
# of the mission, allowing incumbents to retain playback across ranking ticks.
# [{ pos:Vector3, slot_sets:PackedStringArray(4), stagger_h:float,
#    layers_by_set:{set_name: Array[Dictionary]} }]
var _markers: Array = []
# The native emitter system (libs/audio AmbientMixer): staggered tick&7 marker
# eval/registration on the logic-tick clock + the per-frame live-slot ranking
# (docs/audio/lwf-dbf-sound-re.md §driver cadence, D-SND-16). This node keeps the
# per-candidate descriptors for stream resolution and the voice binding below.
var _mixer: NovaAmbientMixer = null
var _candidate_lookup: Dictionary = {}  # candidate_id -> {descriptor[, bus]}
# Portable systems emit short-lived registrations before the GameWorld audio
# pass advances the mixer clock. Queue them so catch-up ticks age the previous
# registrations first, then the newest per-tick refresh lands at the current
# clock [orig: SoundEmitter_Register @0x529270 before the render-frame
# SoundEmitter_UpdateAndMixTop8 @0x5284a0].
var _queued_sound_emitters: Array = []
# (source registry lifetime, lane) -> {set_name, candidate_ids}. Candidate IDs
# remain stable across per-tick refreshes so an incumbent physical channel does
# not restart; a set change or explicit clear retires the old IDs.
var _dynamic_emitter_states: Dictionary = {}
# Latched once a world-driven logic tick arrives (advance_ticks): the world tick owns
# the eval clock; until then tick(delta) free-runs an autonomous 62.5 Hz clock
# (editor-idle owners — the nova_weather world-driven/autonomous split).
var _world_driven_ticks := false
var _world_driven_tick_offset := 0
# At most MIX_CHANNELS entries: [{player:AudioStreamPlayer3D, candidate_id:int}].
var _channels: Array = []
var _next_candidate_id := 1
var _free_candidate_ids: Array[int] = []
var _retired_candidate_ids: Array[int] = []
var _failed_candidate_ids: Dictionary = {}
var _validated_candidate_ids: Dictionary = {}
var _warned_ambient_decode_failure := false
var _strategy: int = STRATEGY_ITEM_SOUNDLOOP
var _stats: Dictionary = {}
var _time_of_day_hhmm: float = 1200.0  # HHMM like NovaEnvironment.time_of_day; noon default
var _last_camera_pos := Vector3.INF  # listener at the last tick; INF until first tick
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


## Load banks, describe ambient marker candidates under `container`, and apply the reverb bed.
## `mission_name` is the .bms filename (its basename selects the co-named .LWF).
## Returns a stats dictionary.
func setup(mission, mission_name: String, container: Node3D) -> Dictionary:
	_stats = {
		"markers_total": 0,
		"markers_resolved": 0,
		"banks_loaded": 0,
		"ambient_candidates": 0,
		"ambient_candidates_validated": 0,
		"ambient_decode_failures": 0,
		"physical_channels": 0,
		"channel_budget": MIX_CHANNELS,
	}
	if mission == null or container == null or _resource_root == null:
		return _stats
	var mission_info: Dictionary = mission.get_info()
	# A repeated setup is not the normal owner lifecycle, but it must not orphan
	# an earlier physical pool or carry dialog state into the next mission.
	_stop_all_ambient_channels()
	_reset_mission_playback_state()
	if _audio_root != null and is_instance_valid(_audio_root):
		_audio_root.queue_free()
	_audio_root = null
	_markers.clear()
	_channels.clear()
	_failed_candidate_ids.clear()
	_validated_candidate_ids.clear()
	_warned_ambient_decode_failure = false
	_next_candidate_id = 1
	_free_candidate_ids.clear()
	_retired_candidate_ids.clear()
	_queued_sound_emitters.clear()
	_dynamic_emitter_states.clear()

	_bank = NovaSoundBank.new(_resource_root)
	_bank.occlusion_provider = _simulation
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
		var item_id := int(entity.get("item_id", 0))
		if _strategy == STRATEGY_ITEM_SOUNDLOOP:
			if not _is_envs_item(item_id):
				continue
		elif int(entity.get("kind", -1)) != NovaMissionData.KIND_MARKER:
			# Preserve the two explicit reimpl-only fallback strategies on the
			# marker pool; only the faithful item dispatch crosses BMS kinds.
			continue
		_stats.markers_total += 1
		var slot_sets := _resolve_slot_sets(entity)
		var distinct: PackedStringArray = []
		for s in slot_sets:
			if not String(s).is_empty() and not distinct.has(s):
				distinct.append(s)
		if distinct.is_empty():
			continue
		var pos: Vector3 = MissionObjectPlacer.bms_to_godot_position(entity.get("position", Vector3.ZERO))
		# Keep layer candidates as data. The original registers only the current
		# region's set and has eight physical channels; it does not materialize a
		# player for every marker/time-of-day layer [orig: @ 0x4a81da].
		var layers_by_set: Dictionary = {}
		var candidate_count := 0
		for set_name in distinct:
			var described: Array = _bank.describe_ambient(set_name)
			if described.is_empty():
				continue
			var layers: Array = []
			for layer_value in described:
				var layer: Dictionary = (layer_value as Dictionary).duplicate()
				layer["candidate_id"] = _next_candidate_id
				_next_candidate_id += 1
				layers.append(layer)
			layers_by_set[set_name] = layers
			candidate_count += layers.size()
		if layers_by_set.is_empty():
			continue
		_markers.append({
			"pos": pos,
			"source_bms_id": int(entity.get("bms_id", 0)),
			"slot_sets": slot_sets,
			# De-sync marker crossfades like the engine's per-entity clock
			# stagger [orig: @ 0x408158 (poolHandle & 0xF) << 11 Q16 hours].
			"stagger_h": float((_markers.size() & 0xF) << 11) / 65536.0,
			"layers_by_set": layers_by_set,
		})
		_stats.markers_resolved += 1
		_stats.ambient_candidates += candidate_count

	# Silence here has historically gone unnoticed (a bare stats print) — warn on
	# the two states that mean "no ambience will play" so they surface in logs.
	if int(_stats.banks_loaded) == 0:
		push_warning("NovaMissionAudio: no sound banks loaded (probed %s.LWF, expansion, %s) — mission ambience will be silent" % [
			mission_name.get_file().get_basename(), ", ".join(GLOBAL_LWFS)])
	elif int(_stats.markers_total) > 0 and int(_stats.markers_resolved) == 0:
		push_warning("NovaMissionAudio: 0/%d sound markers resolved (item db %s) — mission ambience will be silent" % [
			int(_stats.markers_total),
			"missing" if _item_db == null else "loaded"])

	_feed_mixer()
	_apply_reverb(int(mission_info.get("reverb", 0)))
	_apply_music(int(mission_info.get("music", 0)))
	return _stats


func get_stats() -> Dictionary:
	return _stats


## Read/drive seams (ADR 0018): tests and diagnostics go through these, never
## the private fields. set_markers injects fully-described marker entries (the
## shape _markers documents above) so the mix tick can be driven without a
## mission. `container` supplies a SceneTree home for the physical test channels.
func set_markers(markers: Array, container: Node3D = null) -> void:
	_stop_all_ambient_channels()
	_markers = markers
	_failed_candidate_ids.clear()
	_validated_candidate_ids.clear()
	_warned_ambient_decode_failure = false
	_next_candidate_id = 1
	_free_candidate_ids.clear()
	_retired_candidate_ids.clear()
	_queued_sound_emitters.clear()
	_dynamic_emitter_states.clear()
	if container != null and (_audio_root == null or not is_instance_valid(_audio_root)):
		_audio_root = Node3D.new()
		_audio_root.name = "MissionAudio"
		container.add_child(_audio_root)
	for marker_value in _markers:
		var marker: Dictionary = marker_value
		var layers_by_set: Dictionary = marker.get("layers_by_set", {})
		for set_name in layers_by_set:
			var layers: Array = layers_by_set[set_name]
			for layer_value in layers:
				var layer: Dictionary = layer_value
				if not layer.has("candidate_id"):
					layer["candidate_id"] = _next_candidate_id
				_next_candidate_id = maxi(
					_next_candidate_id, int(layer.get("candidate_id", 0)) + 1)
	_feed_mixer()


func set_resolution_strategy(strategy: int) -> void:
	_strategy = strategy


func dialog_voice() -> AudioStreamPlayer:
	return _dialog_voice if _dialog_voice != null and is_instance_valid(_dialog_voice) else null


func get_perf_counters() -> Dictionary:
	var active_channels := 0
	for state_value in _channels:
		var state: Dictionary = state_value
		if int(state.get("candidate_id", -1)) >= 0:
			active_channels += 1
	return {
		"tick_us": _perf_tick_us,
		"markers": _perf_markers,
		"voice_writes": _perf_voice_writes,
		"physical_channels": _channels.size(),
		"active_channels": active_channels,
		"ambient_decode_failures": _failed_candidate_ids.size(),
	}


func get_bank() -> NovaSoundBank:
	return _bank


## Apply the portable entity-attached emitter drain. These are keep-alive
## registrations, not one-shots: the same (source registry lifetime, lane,
## layer) refreshes in place and competes with placed ambience in retail's one
## loudest-eight table. Pitch/volume zero is the common keyed clear.
## [orig: SoundEmitter_RegisterSetLayers @0x528340;
## SoundEmitter_ClearByEntityAndSlot @0x527a50]
func apply_sound_emitters(events: Array) -> void:
	if _mixer == null or _bank == null:
		return
	for event_value in events:
		if event_value is Dictionary:
			_queued_sound_emitters.append(
				(event_value as Dictionary).duplicate())


## PlayWavList / event-action seam: fire a one-shot sound set by name at a world
## position. The .bms action param -> set-name decode is left to the caller (the
## engine resolves a pre-loaded sound_id handle; the action path plays it at full
## emitter volume: ActionSlot_PlaySound @0x4010c0 -> Entity_PlaySound3D_FullVolume
## @0x528e20). One-shot volume snapshots the listener distance at fire time, and
## the set's cull range gates the fire entirely [orig: Sound_Play3DPositional
## @ 0x527cb0; cull @ 0x527cd1].
func fire_soundset(name: String, world_pos: Vector3, source_bms_id: int = 0) -> bool:
	if _bank == null or _audio_root == null:
		return false
	return _bank.play_oneshot_3d(
		_audio_root, world_pos, name, SFX_BUS, _last_camera_pos, source_bms_id)


## A body slot sound (footstep/foley/landing/scream) from the sim's per-tick
## drain: the same full-volume positional one-shot as fire_soundset [orig:
## Entity_PlaySound3D_FullVolume @ 0x528e20 — emitter volume 255], with an
## optional exclusive key for the every-tick refire slots (chute flap/freefall).
func slot_soundset(name: String, world_pos: Vector3, exclusive_key: String = "") -> bool:
	if _bank == null or _audio_root == null:
		return false
	return _bank.play_oneshot_3d(
		_audio_root, world_pos, name, SFX_BUS, _last_camera_pos, 0, exclusive_key)


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


## Pump for the mission clock; HHMM like NovaEnvironment.time_of_day.
func set_time_of_day_hhmm(hhmm: float) -> void:
	_time_of_day_hhmm = hhmm
	if _mixer != null:
		_mixer.set_time_of_day_hours(_hhmm_to_hours(hhmm))


## World-driven eval clock: the world tick pushes the sim's logic tick after each
## tick_realtime batch, and the native mixer runs the witnessed staggered cohort
## walk for the elapsed ticks — each placed marker re-evaluates every 8th 62.5 Hz
## tick [orig: Entity_UpdateAllEntities @ 0x4c225a pool-2 walk;
## Entity_UpdateEnvSoundEmitter @ 0x4a8080]. The first world-driven tick rebases the
## clock so an editor session that free-ran before Play keeps its slot lifetimes.
func advance_ticks(logic_tick: int) -> void:
	if _mixer == null:
		return
	if not _world_driven_ticks:
		_world_driven_ticks = true
		_world_driven_tick_offset = maxi(0, int(_mixer.clock_tick()) - logic_tick)
	var target_tick := logic_tick + _world_driven_tick_offset
	# Apply each intent at its producer tick before advancing to the end of a
	# catch-up batch. A lane last refreshed early in the batch therefore spends
	# the elapsed ticks from its real 30-tick lifetime.
	_flush_sound_emitters(target_tick)
	_mixer.advance_to_tick(target_tick)


## Occlusion provider (the NovaSimulation) — emitter/one-shot distances inflate
## through the witnessed two-ray LOS so occluded sources sound farther [orig:
## Sound_ApplyOcclusionDistance @ 0x529970]. Optional: tests and the menu run
## without a sim and mix unoccluded.
func set_simulation(sim: Object) -> void:
	_simulation = sim
	if _bank != null:
		_bank.occlusion_provider = sim
	if _mixer != null:
		_mixer.set_occlusion_provider(sim)


## The per-frame ambient mix pass [orig: SoundEmitter_UpdateAndMixTop8 @ 0x5284a0,
## called once per render frame from the Game Loop render callback @ 0x521341]:
## the native mixer (libs/audio AmbientMixer) ranks the LIVE emitter slots —
## registered at the witnessed staggered tick cadence via advance_ticks — through
## the two-radius member-0 curve with occlusion, and this node binds the loudest
## MIX_CHANNELS to reusable players (D-SND-6/D-SND-8 reimpl territory). Stable
## candidate IDs let selected incumbents continue while an entrant restarts,
## matching transient registration. `delta` free-runs the autonomous 62.5 Hz eval
## clock only for owners that never push logic ticks (editor idle) — see
## docs/audio/lwf-dbf-sound-re.md §driver cadence (D-SND-16, ported).
func tick(camera_pos: Vector3, delta: float = 0.0) -> void:
	var start := Time.get_ticks_usec()
	_last_camera_pos = camera_pos
	var writes := 0
	if _mixer == null:
		_perf_markers = 0
		_perf_voice_writes = 0
		_perf_tick_us = Time.get_ticks_usec() - start
		return
	# Autonomous owners register at the current clock before consuming this
	# render frame's elapsed time. World-driven callers normally flush chronologically
	# from advance_ticks above; the fallback handles a late same-tick delivery.
	_flush_sound_emitters(int(_mixer.clock_tick()))
	if not _world_driven_ticks and delta > 0.0:
		_mixer.advance_seconds(delta)
	var rows: PackedFloat32Array = _mixer.mix_v2(camera_pos)
	const row_stride := 6
	# Ranked loudest-first (candidate-id tie-break) by the native mixer; a
	# physical incumbent is never rebound merely because its rank within the
	# selected eight changed.
	var candidates: Array = []  # [{candidate_id, descriptor, pos, vol, pitch_q16, bus}]
	for base in range(0, rows.size(), row_stride):
		var row_id := int(rows[base])
		var entry: Dictionary = _candidate_lookup.get(row_id, {})
		if entry.is_empty():
			continue
		var pitch_q16 := int(rows[base + 2])
		var pos_base := base + 3
		candidates.append({
			"candidate_id": row_id,
			"descriptor": entry.descriptor,
			"pos": Vector3(
				rows[pos_base], rows[pos_base + 1], rows[pos_base + 2]),
			"vol": int(rows[base + 1]),
			"pitch_q16": pitch_q16,
			"bus": entry.get("bus", AMBIENT_BUS),
		})
	var incumbent_by_id: Dictionary = {}
	for state_value in _channels:
		var state: Dictionary = state_value
		var incumbent_id := int(state.get("candidate_id", -1))
		if incumbent_id >= 0:
			incumbent_by_id[incumbent_id] = state

	# Resolve streams only for new candidates that would enter the top eight.
	# Failed/corrupt descriptors are cached out and the next-ranked candidate
	# gets the channel, matching the old eager path's "unresolvable = absent".
	var selected: Array = []
	for candidate_value in candidates:
		if selected.size() >= MIX_CHANNELS:
			break
		var candidate: Dictionary = candidate_value
		var candidate_id := int(candidate.candidate_id)
		if _failed_candidate_ids.has(candidate_id):
			continue
		if not incumbent_by_id.has(candidate_id):
			var stream := _validate_candidate_stream(
				candidate_id, candidate.descriptor)
			if stream == null:
				_failed_candidate_ids[candidate_id] = true
				continue
			candidate["resolved_stream"] = stream
		selected.append(candidate)

	var selected_ids: Dictionary = {}
	for candidate_value in selected:
		var candidate: Dictionary = candidate_value
		selected_ids[int(candidate.candidate_id)] = true

	# Dropouts release their physical slot. If the same virtual candidate later
	# re-enters it is rebound and play() starts it from the beginning, like the
	# original transient channel registration.
	for state_value in _channels:
		var state: Dictionary = state_value
		var candidate_id := int(state.get("candidate_id", -1))
		if candidate_id < 0 or selected_ids.has(candidate_id):
			continue
		var player: AudioStreamPlayer3D = state.player
		player.stop()
		player.stream = null
		player.volume_db = SILENT_DB
		player.process_mode = Node.PROCESS_MODE_DISABLED
		player.remove_meta("ambient_candidate_id")
		state["candidate_id"] = -1
		writes += 1

	for candidate_value in selected:
		var candidate: Dictionary = candidate_value
		var candidate_id := int(candidate.candidate_id)
		var state: Dictionary = incumbent_by_id.get(candidate_id, {})
		if state.is_empty():
			state = _free_or_new_channel()
			if state.is_empty():
				continue
			var player: AudioStreamPlayer3D = state.player
			var stream: AudioStreamWAV = candidate.get("resolved_stream")
			NovaSoundBank.configure_ambient_player(
				player, stream, candidate.descriptor,
				StringName(candidate.get("bus", AMBIENT_BUS)))
			player.position = candidate.pos
			player.volume_db = NovaSoundBank.volume_db_from_255(int(candidate.vol))
			player.pitch_scale = _candidate_pitch_scale(candidate)
			player.process_mode = Node.PROCESS_MODE_INHERIT
			player.set_meta("ambient_candidate_id", candidate_id)
			state["candidate_id"] = candidate_id
			player.play()
			writes += 1
			continue
		var incumbent: AudioStreamPlayer3D = state.player
		var changed := false
		if incumbent.position != candidate.pos:
			incumbent.position = candidate.pos
			changed = true
		var db := NovaSoundBank.volume_db_from_255(int(candidate.vol))
		if not is_equal_approx(incumbent.volume_db, db):
			incumbent.volume_db = db
			changed = true
		var pitch_scale := _candidate_pitch_scale(candidate)
		if not is_equal_approx(incumbent.pitch_scale, pitch_scale):
			incumbent.pitch_scale = pitch_scale
			changed = true
		if changed:
			writes += 1
	_prune_dynamic_emitter_states()
	_release_retired_candidate_ids()
	_perf_markers = _markers.size()
	_perf_voice_writes = writes
	_perf_tick_us = Time.get_ticks_usec() - start
	if not _stats.is_empty():
		_stats["physical_channels"] = _channels.size()


func _resolve_candidate_stream(descriptor: Dictionary) -> AudioStreamWAV:
	var injected = descriptor.get("stream")
	if injected is AudioStreamWAV:
		return injected
	if _bank == null:
		return null
	return _bank.resolve_ambient_stream(descriptor)


func _validate_candidate_stream(
		candidate_id: int, descriptor: Dictionary) -> AudioStreamWAV:
	var first_validation := not _validated_candidate_ids.has(candidate_id)
	var stream := _resolve_candidate_stream(descriptor)
	if first_validation:
		_validated_candidate_ids[candidate_id] = true
		if not _stats.is_empty():
			_stats["ambient_candidates_validated"] = _validated_candidate_ids.size()
	if stream != null:
		return stream
	if not _stats.is_empty():
		_stats["ambient_decode_failures"] = _failed_candidate_ids.size() + 1
	if not _warned_ambient_decode_failure:
		_warned_ambient_decode_failure = true
		push_warning(
			"NovaMissionAudio: ambient WAV '%s' failed to decode; excluding failed candidates from the eight-channel mix" %
			String(descriptor.get("wav_path", "<injected>")))
	return null


func _free_or_new_channel() -> Dictionary:
	for state_value in _channels:
		var state: Dictionary = state_value
		if int(state.get("candidate_id", -1)) < 0:
			return state
	if _channels.size() >= MIX_CHANNELS or _audio_root == null:
		return {}
	var player := AudioStreamPlayer3D.new()
	player.name = "AmbientChannel%d" % _channels.size()
	player.volume_db = SILENT_DB
	player.process_mode = Node.PROCESS_MODE_DISABLED
	_audio_root.add_child(player)
	var state := {"player": player, "candidate_id": -1}
	_channels.append(state)
	return state


func _stop_all_ambient_channels() -> void:
	for state_value in _channels:
		var state: Dictionary = state_value
		var player: AudioStreamPlayer3D = state.player
		if player != null and is_instance_valid(player):
			player.stop()
			player.stream = null
			player.volume_db = SILENT_DB
			player.process_mode = Node.PROCESS_MODE_DISABLED
			player.remove_meta("ambient_candidate_id")
		state["candidate_id"] = -1


func _reset_mission_playback_state() -> void:
	# Dialog and WAC voices are mission-owned even though they use separate
	# physical players from ambience. Stop them before replacing/queuing their
	# audio root so neither playback nor a queued dialog can cross missions.
	if _dialog_voice != null and is_instance_valid(_dialog_voice):
		_dialog_voice.stop()
	if _wac_voice != null and is_instance_valid(_wac_voice):
		_wac_voice.stop()
	_dialog_queue.clear()
	_dialog_voice = null
	_wac_voice = null
	_wac_wav_cache.clear()
	_dbf = null


func teardown() -> void:
	# Dropping _audio_root frees the dialog + wac voice nodes too; just drop our refs
	# so a late `finished` after teardown can't pump a freed queue.
	_apply_reverb(0)
	_stop_all_ambient_channels()
	_reset_mission_playback_state()
	if _audio_root != null and is_instance_valid(_audio_root):
		_audio_root.queue_free()
	_audio_root = null
	_markers.clear()
	_channels.clear()
	_failed_candidate_ids.clear()
	_validated_candidate_ids.clear()
	_warned_ambient_decode_failure = false
	_mixer = null
	_candidate_lookup.clear()
	_free_candidate_ids.clear()
	_retired_candidate_ids.clear()
	_queued_sound_emitters.clear()
	_dynamic_emitter_states.clear()
	_world_driven_ticks = false
	_world_driven_tick_offset = 0
	_bank = null


# --- Internals ---

# Push the resolved marker/layer data into a fresh native mixer. The mixer owns
# the witnessed cadence (staggered eval, tick-unit slot lifetimes) and the ranked
# mix; this node keeps each candidate's descriptor for stream resolution by id.
func _feed_mixer() -> void:
	_mixer = NovaAmbientMixer.new()
	_mixer.set_occlusion_provider(_simulation)
	_mixer.set_time_of_day_hours(_hhmm_to_hours(_time_of_day_hhmm))
	_candidate_lookup.clear()
	_free_candidate_ids.clear()
	_retired_candidate_ids.clear()
	_queued_sound_emitters.clear()
	_dynamic_emitter_states.clear()
	_world_driven_ticks = false
	_world_driven_tick_offset = 0
	for mi in _markers.size():
		var marker: Dictionary = _markers[mi]
		var pos: Vector3 = marker.get("pos", Vector3.ZERO)
		var layers_by_set: Dictionary = marker.get("layers_by_set", {})
		var slot_sets: PackedStringArray = marker.get("slot_sets", PackedStringArray())
		var set_names: Array = []
		var sets: Array = []
		for set_name in layers_by_set:
			var packed := PackedInt32Array()
			for layer_value in layers_by_set[set_name]:
				var layer: Dictionary = layer_value
				var cid := int(layer.get("candidate_id", 0))
				_candidate_lookup[cid] = {"descriptor": layer, "pos": pos}
				packed.append_array(PackedInt32Array([
					cid,
					int(layer.get("falloff_radius", 0)),
					int(layer.get("min_distance", 0)),
					int(layer.get("volume", NovaSoundBank.VOLUME_BYTE_MAX)),
					int(layer.get("clamp_volume", NovaSoundBank.VOLUME_BYTE_MAX)),
				]))
			if packed.is_empty():
				continue
			set_names.append(String(set_name))
			sets.append(packed)
		var slot_keys := PackedInt32Array([-1, -1, -1, -1])
		for r in range(4):
			if r < slot_sets.size():
				slot_keys[r] = set_names.find(String(slot_sets[r]))
		# The walk cohort and the clock stagger both ride the marker's pool-slot
		# nibble [orig: tick & 7 @ 0x4c225a; (poolHandle & 0xF) << 11 @ 0x408158];
		# the stored stagger_h is that nibble in hours, inverted here.
		var stagger_slot := int(roundf(float(marker.get("stagger_h", 0.0)) * 65536.0)) >> 11
		_mixer.add_marker(pos, int(marker.get("source_bms_id", 0)), stagger_slot,
				0, slot_keys, sets)


# Resolve queued name-keyed registrations into LWF layer descriptors at their
# producer ticks. Each layer keeps a stable candidate ID while its keyed lane is
# alive; retired IDs are recycled only after no physical channel still carries
# them, keeping the float-packed native row identity exact for long missions.
func _flush_sound_emitters(final_tick: int) -> void:
	if _mixer == null or _bank == null or _queued_sound_emitters.is_empty():
		return
	var pending := _queued_sound_emitters
	_queued_sound_emitters = []
	pending.sort_custom(_sound_emitter_event_before)
	for event_value in pending:
		var event: Dictionary = event_value
		var event_tick := int(event.get(
				"emitted_tick", int(_mixer.clock_tick())))
		if _world_driven_ticks:
			event_tick += _world_driven_tick_offset
		event_tick = mini(event_tick, final_tick)
		if event_tick > int(_mixer.clock_tick()):
			_mixer.advance_to_tick(event_tick)
		var source_spawn_id := int(event.get("source_spawn_id", 0))
		var lane := int(event.get("lane", 0))
		var key := "%d:%d" % [source_spawn_id, lane]
		var pos: Vector3 = event.get("pos", Vector3.ZERO)
		var source_bms_id := int(event.get("source_bms_id", 0))
		var lifetime := maxi(1, int(event.get("lifetime", 30)))
		var pitch_q16 := int(event.get("pitch_q16", 0))
		var volume_q8_8 := int(event.get("volume_q8_8", 0))
		if bool(event.get("source_only", false)):
			_mixer.update_emitter_source(source_spawn_id, pos, source_bms_id)
			continue
		if pitch_q16 == 0 or volume_q8_8 == 0:
			_mixer.register_emitter(source_spawn_id, lane, pos, source_bms_id,
					lifetime, pitch_q16, volume_q8_8, PackedInt32Array())
			_forget_dynamic_emitter(key)
			continue

		var set_name := String(event.get("set", ""))
		if set_name.is_empty():
			continue
		var described: Array = _bank.describe_ambient(set_name)
		if described.is_empty():
			continue
		var state: Dictionary = _dynamic_emitter_states.get(key, {})
		var candidate_ids: PackedInt32Array = state.get(
				"candidate_ids", PackedInt32Array())
		if String(state.get("set_name", "")) != set_name \
				or candidate_ids.size() != described.size():
			if not state.is_empty():
				_mixer.register_emitter(source_spawn_id, lane, pos,
						source_bms_id, lifetime, 0, 0, PackedInt32Array())
				_forget_dynamic_emitter(key)
			candidate_ids = PackedInt32Array()
			candidate_ids.resize(described.size())
			for i in described.size():
				candidate_ids[i] = _allocate_dynamic_candidate_id()
			_dynamic_emitter_states[key] = {
				"set_name": set_name,
				"candidate_ids": candidate_ids,
			}
		var state_tick := int(_mixer.clock_tick())
		var live_state: Dictionary = _dynamic_emitter_states[key]
		live_state["expires_tick"] = state_tick + lifetime
		_dynamic_emitter_states[key] = live_state

		var layers := PackedInt32Array()
		for i in described.size():
			var descriptor: Dictionary = (
					described[i] as Dictionary).duplicate()
			var candidate_id := int(candidate_ids[i])
			descriptor["candidate_id"] = candidate_id
			_candidate_lookup[candidate_id] = {
				"descriptor": descriptor,
				"bus": SFX_BUS,
			}
			layers.append_array(PackedInt32Array([
				candidate_id,
				int(descriptor.get("falloff_radius", 0)),
				int(descriptor.get("min_distance", 0)),
				int(descriptor.get(
					"volume", NovaSoundBank.VOLUME_BYTE_MAX)),
				int(descriptor.get(
					"clamp_volume", NovaSoundBank.VOLUME_BYTE_MAX)),
			]))
		_mixer.register_emitter(source_spawn_id, lane, pos, source_bms_id,
				lifetime, pitch_q16, volume_q8_8, layers)


func _forget_dynamic_emitter(key: String) -> void:
	var state: Dictionary = _dynamic_emitter_states.get(key, {})
	var ids: PackedInt32Array = state.get(
			"candidate_ids", PackedInt32Array())
	for candidate_id in ids:
		var id := int(candidate_id)
		_candidate_lookup.erase(id)
		_failed_candidate_ids.erase(id)
		_validated_candidate_ids.erase(id)
		_retired_candidate_ids.append(id)
	_dynamic_emitter_states.erase(key)


func _allocate_dynamic_candidate_id() -> int:
	if not _free_candidate_ids.is_empty():
		return _free_candidate_ids.pop_back()
	var candidate_id := _next_candidate_id
	_next_candidate_id += 1
	return candidate_id


func _prune_dynamic_emitter_states() -> void:
	if _mixer == null:
		return
	var now_tick := int(_mixer.clock_tick())
	var expired_keys: Array[String] = []
	for key_value in _dynamic_emitter_states:
		var key := String(key_value)
		var state: Dictionary = _dynamic_emitter_states[key]
		if now_tick > int(state.get("expires_tick", now_tick)):
			expired_keys.append(key)
	for key in expired_keys:
		_forget_dynamic_emitter(key)


func _release_retired_candidate_ids() -> void:
	if _retired_candidate_ids.is_empty():
		return
	var bound_ids: Dictionary = {}
	for state_value in _channels:
		var state: Dictionary = state_value
		var candidate_id := int(state.get("candidate_id", -1))
		if candidate_id >= 0:
			bound_ids[candidate_id] = true
	var still_retired: Array[int] = []
	for candidate_id in _retired_candidate_ids:
		if bound_ids.has(candidate_id):
			still_retired.append(candidate_id)
		else:
			_free_candidate_ids.append(candidate_id)
	_retired_candidate_ids = still_retired


static func _sound_emitter_event_before(a: Variant, b: Variant) -> bool:
	var event_a: Dictionary = a
	var event_b: Dictionary = b
	return int(event_a.get("emitted_tick", 0)) < int(
			event_b.get("emitted_tick", 0))


static func _candidate_pitch_scale(candidate: Dictionary) -> float:
	var descriptor: Dictionary = candidate.get("descriptor", {})
	var base_pitch := float(descriptor.get("base_pitch", 1.0))
	if base_pitch <= 0.01:
		base_pitch = 1.0
	return base_pitch * maxf(
			float(candidate.get("pitch_q16", 0x10000)) / 65536.0, 0.0001)


static func _hhmm_to_hours(hhmm: float) -> float:
	var wrapped := fposmod(hhmm, NovaEnvironment.HHMM_DAY)
	var hour := floorf(wrapped / 100.0)
	var minute := clampf(fmod(wrapped, 100.0), 0.0, 59.999999)
	return hour + minute / 60.0


func _is_envs_item(item_id: int) -> bool:
	if _item_db == null:
		return false
	# The env sound updater is selected by the item's dispatch tag, not by the
	# BMS record pool. Most authored `snd:` entries are marker records, but JOX
	# also places envs decorations in the building pool (oil pumps/flares).
	# [orig: the `envs` entry in the class dispatch table @ 0x82ABD4 routes to
	# Entity_UpdateEnvSoundEmitter @ 0x4a8080].
	var ai := String(_item_db.get_ai_function(item_id))
	var move := String(_item_db.get_move_function(item_id))
	return ai.to_lower() == "envs" or move.to_lower() == "envs"


func _load_bank(lwf_name: String) -> void:
	if not _resource_root.has_file(lwf_name):
		return
	var lwf := NovaLwfData.new()
	if lwf.open_from_resource_root(_resource_root, lwf_name) == OK:
		_bank.add_bank(lwf)
		_stats.banks_loaded += 1


# The four time-of-day slot set names for a marker: soundloop_1..4 select by
# region morning/day/evening/night [orig: Entity_UpdateEnvSoundEmitter @ 0x4a8080
# indexes itemDef.soundLoopId[region]]. Unresolvable/empty slots stay "" — a
# marker whose current region has no set is SILENT, like the original's null
# soundLoopId (the original has NO fallback; a sound_profile fallback we once
# carried was unwitnessed and never fires with JO data — zero envs-class items
# ship a sound_profile key).
func _resolve_slot_sets(entity: Dictionary) -> PackedStringArray:
	var slots: PackedStringArray = ["", "", "", ""]
	match _strategy:
		STRATEGY_ITEM_SOUNDLOOP:
			if _item_db == null:
				return slots
			var item_id := int(entity.get("item_id", 0))
			var loops: Array = _item_db.get_sound_loops(item_id)
			for i in range(4):
				if i < loops.size():
					var n := String(loops[i])
					if not n.is_empty() and _bank.has_set(n):
						slots[i] = n
		STRATEGY_MARKER_NAME:
			var n := String(entity.get("name", ""))
			if not n.is_empty() and _bank.has_set(n):
				for i in range(4):
					slots[i] = n
		_:
			pass
	return slots


## Time-of-day region + crossfade for env sound markers [orig:
## Entity_CalcTimeOfDayRegion @ 0x408110]. The typed result carries region
## 0..3, its adjacent region, and blend 0..1; each region fades IN over the first
## ~5 game-minutes after its low cut and fades OUT over the last ~5 before the
## next cut; `adjacent` is the neighbouring region at that edge (same-set
## neighbours suppress the dip [orig: @ 0x4a819d]).
static func time_of_day_region(hours: float) -> TimeOfDayRegion:
	# The open-low cuts, edge blends, and night-wrap forms live in libs/audio
	# (ambient_mixer.cpp time_of_day_region) beside the eval that consumes them.
	var d: Dictionary = NovaAmbientMixer.time_of_day_region(hours)
	return TimeOfDayRegion.new(int(d.region), int(d.adjacent), float(d.blend))


## The emitter volume byte for a region crossfade blend. The original registers
## the volume word (0xFFFF * blend_q16 + 0x8000) >> 16 — ROUNDED, with 0xFFFF as
## the full-blend sentinel — and the mixer reads its HIGH byte as the emitter
## volume [orig: Entity_UpdateEnvSoundEmitter @ 0x4a81c6 (blendAlpha); the mix
## reads slot byte +25 @ 0x52865e]. Net: byte = (0xFFFF * blend_q16 + 0x8000) >> 24.
static func crossfade_volume_byte(blend: float) -> int:
	return NovaAmbientMixer.crossfade_volume_byte(blend)


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
