class_name MissionAudio
extends RefCounted

## Runtime mission audio orchestrator. Loads the mission's co-named .LWF + the
## global banks into a SoundBank, resolves each placed envs-class entity's
## time-of-day slot sets BY NAME (items.def soundloop_1..4 = morning/day/
## evening/night [orig: Entity_UpdateEnvSoundEmitter @ 0x4a8080]; the engine is
## name-keyed — see docs/audio/lwf-dbf-sound-re.md), and retains each layer as
## lightweight candidate data. tick(camera_pos) runs the witnessed
## ambient emitter mix: per-voice two-radius distance volumes, region
## crossfades, and an eight-player physical channel pool [orig:
## SoundEmitter_UpdateAndMixTop8 @ 0x5284a0]. Also exposes the PlayWavList
## action seam and the music/reverb bed.
##
## Bank chain vs the original: Game_StartMission walks six global name slots in
## order [<exp>L.lwf, <exp>.lwf, gamelocl.lwf, game.lwf, game3.lwf, game2.lwf]
## (name table @ 0x82A5B0, walk @ 0x525443; expansion slots filled by
## Expansion_LoadAssets @ 0x4a495e), and the mission co-named .lwf is loaded
## separately as the DIALOG bank (DialogManager_LoadFromFile @ 0x44e7d4, only
## when the .dbf exists, with a .pwf fallback). We load one merged chain with
## the co-named bank first (it carries the dialog voices) then the global
## slots in the engine's order — the slot table lives in engine/runtime/audio
## (audio/bank_chain.h, via AmbientMixer.global_bank_chain).



## One placed ambient marker ("snd:" item): data, not a scene node. The four
## time-of-day slot set names ("" = silent in that region) and, per distinct
## set, the LWF layer descriptors (SoundBank.describe_ambient's Dictionary, the
## documented layer transport edge), each carrying a stable `candidate_id` for
## the mission's lifetime so an incumbent keeps its channel across ranking ticks.
class Marker extends RefCounted:
	var pos: Vector3
	var source_bms_id: int
	var slot_sets: PackedStringArray
	## The marker's pool-slot nibble: the native mixer derives the walk cohort
	## AND the (slot << 11) Q16-hours clock stagger from it
	## [orig: tick & 7 @ 0x4c225a; (poolHandle & 0xF) << 11 @ 0x408158].
	var stagger_slot: int
	var layers_by_set: Dictionary  # set name -> Array of layer descriptors

	func _init(p_pos: Vector3, p_source_bms_id: int, p_slot_sets: PackedStringArray,
			p_stagger_slot: int, p_layers_by_set: Dictionary) -> void:
		pos = p_pos
		source_bms_id = p_source_bms_id
		slot_sets = p_slot_sets
		stagger_slot = p_stagger_slot
		layers_by_set = p_layers_by_set


## One of the MIX_CHANNELS physical channels: a reusable player and the
## candidate bound to it (-1 = free).
class Channel extends RefCounted:
	var player: AudioStreamPlayer3D
	var candidate_id := -1

	func _init(p_player: AudioStreamPlayer3D) -> void:
		player = p_player


## A live candidate's descriptor and bus, by candidate id: marker layers ride
## the Ambient bus, dynamic emitter layers the SFX bus.
class CandidateBinding extends RefCounted:
	var descriptor: Dictionary
	var bus: StringName

	func _init(p_descriptor: Dictionary, p_bus: StringName) -> void:
		descriptor = p_descriptor
		bus = p_bus


## One ranked mix row the native mixer returned this frame, joined to its
## binding; `resolved_stream` is filled for an entrant that reaches the top eight.
class Candidate extends RefCounted:
	var candidate_id: int
	var descriptor: Dictionary
	var bus: StringName
	var pos: Vector3
	var vol: int
	var pitch_q16: int
	var resolved_stream: AudioStreamWAV = null

	func _init(p_candidate_id: int, p_binding: CandidateBinding, p_pos: Vector3, p_vol: int,
			p_pitch_q16: int) -> void:
		candidate_id = p_candidate_id
		descriptor = p_binding.descriptor
		bus = p_binding.bus
		pos = p_pos
		vol = p_vol
		pitch_q16 = p_pitch_q16


## One dynamic emitter lane's live registration ((source lifetime, lane) key):
## the set it plays, its stable candidate ids, and the tick it expires.
class DynamicEmitter extends RefCounted:
	var set_name: String
	var candidate_ids: PackedInt32Array
	var expires_tick := 0

	func _init(p_set_name: String, p_candidate_ids: PackedInt32Array) -> void:
		set_name = p_set_name
		candidate_ids = p_candidate_ids


## The setup statistics: a typed record like the sibling present passes' Stats
## (ADR 0017); to_dict() is the probe/MCP JSON edge.
class Stats extends RefCounted:
	var markers_total := 0
	var markers_resolved := 0
	var banks_loaded := 0
	var ambient_candidates := 0
	var ambient_candidates_validated := 0
	var ambient_decode_failures := 0
	var physical_channels := 0
	var channel_budget := MissionAudio.MIX_CHANNELS
	var dialogs := 0

	func to_dict() -> Dictionary:
		return {
			"markers_total": markers_total,
			"markers_resolved": markers_resolved,
			"banks_loaded": banks_loaded,
			"ambient_candidates": ambient_candidates,
			"ambient_candidates_validated": ambient_candidates_validated,
			"ambient_decode_failures": ambient_decode_failures,
			"physical_channels": physical_channels,
			"channel_budget": channel_budget,
			"dialogs": dialogs,
		}



const AMBIENT_BUS := &"Ambient"
const SFX_BUS := &"SFX"
const VOICE_BUS := &"Voice"
# Marker -> sound set resolution is the marker item's items.def soundloop_1..7
# set names (e.g. id 106178 "snd: Lp Flourescent Light" -> soundloop_1
# LPNV_LIGHT) [orig: ItemDef_ParseProperty @ 0x49fec4]; the engine is
# name-keyed (docs/audio/lwf-dbf-sound-re.md) and resolves it natively
# (audio/envs_markers.h via ItemDatabase.resolve_envs_markers).

# The thunder bearing is an 8-bit binary angle (one byte = a full turn;
# world/weather_state.h WeatherSound.bearing).
const BEARING_BAM8_TURN := 256.0
const MINUTES_PER_HOUR := 60.0

# The ambient emitter mix budget: the engine sorts every in-range emitter voice
# by computed volume each frame and keeps the loudest 8 on real channels
# [orig: SoundEmitter_UpdateAndMixTop8 @ 0x5284a0, channel table @ 0x24D6688].
const MIX_CHANNELS := 8
const SILENT_DB := -80.0  # hard-silent floor for out-of-mix voices
# Time-of-day region cuts (4/10/17/21 h) and the ~5-game-minute crossfade margin
# live with the eval in engine/runtime/audio (ambient_mixer.cpp time_of_day_region)
# [orig: Entity_CalcTimeOfDayRegion @ 0x408110; margin @ 0x408203].

var _resource_root: ResourceRoot
var _item_db: ItemDatabase
var _simulation: Simulation = null  # occlusion LOS; optional
# Test-injection seam (Callable(listener, source, dist_q16, source_id) -> int),
# forwarded to the bank and every fresh mixer; production uses _simulation.
var _occlusion_override: Callable = Callable()
var _bank: SoundBank
var _dbf  # DbfData (mission co-named dialog bank; null if absent)
var _audio_root: Node3D
var _markers: Array[Marker] = []
# The native emitter system (engine/runtime/audio AmbientMixer): staggered tick&7 marker
# eval/registration on the logic-tick clock + the per-frame live-slot ranking
# (docs/audio/lwf-dbf-sound-re.md §driver cadence, D-SND-16). This node keeps the
# per-candidate descriptors for stream resolution and the voice binding below.
var _mixer: AmbientMixer = null
var _candidate_lookup: Dictionary = {}  # candidate_id -> CandidateBinding
# Portable systems emit short-lived registrations before the GameWorld audio
# pass advances the mixer clock. Queue them so catch-up ticks age the previous
# registrations first, then the newest per-tick refresh lands at the current
# clock [orig: SoundEmitter_Register @0x529270 before the render-frame
# SoundEmitter_UpdateAndMixTop8 @0x5284a0].
var _queued_sound_emitters: Array[SoundEmitterRow] = []
# "source_spawn_id:lane" -> DynamicEmitter. Candidate IDs remain stable across
# per-tick refreshes so an incumbent physical channel does not restart; a set
# change or explicit clear retires the old IDs.
var _dynamic_emitter_states: Dictionary = {}
# Latched once a world-driven logic tick arrives (advance_ticks): the world tick owns
# the eval clock; until then tick(delta) free-runs an autonomous 62.5 Hz clock
# (editor-idle owners — the weather world-driven/autonomous split).
var _world_driven_ticks := false
var _world_driven_tick_offset := 0
var _channels: Array[Channel] = []  # at most MIX_CHANNELS
var _next_candidate_id := 1
var _free_candidate_ids: Array[int] = []
var _retired_candidate_ids: Array[int] = []
var _failed_candidate_ids: Dictionary = {}
var _validated_candidate_ids: Dictionary = {}
var _warned_ambient_decode_failure := false
# The "ambience disabled" arm (the dialog-vs-ambient probe): banks and the
# .DBF still load, no marker resolves.
var _ambient_markers_enabled := true
var _stats: Stats = null
var _time_of_day_hhmm: float = 1200.0  # HHMM like MissionEnvironment.time_of_day; noon default
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


func _init(resource_root: ResourceRoot, item_db: ItemDatabase) -> void:
	_resource_root = resource_root
	_item_db = item_db


## Load banks, describe ambient marker candidates under `container`, and apply the reverb bed.
## `mission_name` is the .bms filename (its basename selects the co-named .LWF).
## Returns the setup Stats record.
func setup(mission: MissionData, mission_name: String, container: Node3D) -> Stats:
	_stats = Stats.new()
	if mission == null or container == null or _resource_root == null:
		return _stats
	var mission_info := mission.get_info()
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

	_bank = SoundBank.new(_resource_root)
	_bank.occlusion_provider = _simulation
	_bank.occlusion_override = _occlusion_override
	_load_bank(mission_name.get_file().get_basename() + ".LWF")
	# The global slots in the engine's order — expansion pair (when one is
	# mounted) ahead of the statics [orig: slot table @ 0x82A5B0, walk
	# @ 0x525443; expansion fill @ 0x4a4989 / @ 0x4a495e]. The witnessed table
	# lives native (audio/bank_chain.h); missing files skip like retail's
	# SoundBank_LoadIfExists (D-SND-2 closed).
	var exp_name := String(_resource_root.get_expansion())
	var global_chain: PackedStringArray = AmbientMixer.global_bank_chain(exp_name)
	for global_name in global_chain:
		_load_bank(global_name)

	# The mission's co-named .DBF maps a PlayWavList dialog id (dlg001) to the LWF
	# set name(s) it plays; loaded only if present.
	var dbf_name := mission_name.get_file().get_basename() + ".DBF"
	if _resource_root.has_file(dbf_name):
		var dbf := DbfData.new()
		if dbf.open_from_resource_root(_resource_root, dbf_name) == OK:
			_dbf = dbf
			_stats.dialogs = dbf.get_dialog_count()

	_audio_root = Node3D.new()
	_audio_root.name = "MissionAudio"
	container.add_child(_audio_root)

	var marker_rows: Array[EnvsMarkerRow] = []
	# S13 (ADR 0028): the faithful envs dispatch + the four soundloop slot
	# names resolve natively over the retained items.def and the mission's
	# bms document (audio/envs_markers.h). The bank-presence filter below
	# stays a shell stream-resolution concern (the original has no such
	# gate — a missing set is simply silent).
	if _ambient_markers_enabled and _item_db != null:
		marker_rows = _item_db.resolve_envs_markers(mission)
	for row in marker_rows:
		_stats.markers_total += 1
		# Authored slot names -> playable slots: only sets the loaded bank chain
		# actually carries participate; an empty slot stays SILENT in its region.
		var authored := row.slot_sets
		var slot_sets: PackedStringArray = ["", "", "", ""]
		for i in range(4):
			if i < authored.size():
				var n := String(authored[i])
				if not n.is_empty() and _bank.has_set(n):
					slot_sets[i] = n
		var distinct: PackedStringArray = []
		for s in slot_sets:
			if not String(s).is_empty() and not distinct.has(s):
				distinct.append(s)
		if distinct.is_empty():
			continue
		var pos: Vector3 = MissionObjectPlacer.bms_to_godot_position(row.position)
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
		_markers.append(Marker.new(pos, row.bms_id, slot_sets,
				_markers.size() & 0xF, layers_by_set))
		_stats.markers_resolved += 1
		_stats.ambient_candidates += candidate_count

	# Silence here has historically gone unnoticed (a bare stats print) — warn on
	# the two states that mean "no ambience will play" so they surface in logs.
	if int(_stats.banks_loaded) == 0:
		push_warning("MissionAudio: no sound banks loaded (probed %s.LWF, %s) — mission ambience will be silent" % [
			mission_name.get_file().get_basename(), ", ".join(global_chain)])
	elif int(_stats.markers_total) > 0 and int(_stats.markers_resolved) == 0:
		push_warning("MissionAudio: 0/%d sound markers resolved (item db %s) — mission ambience will be silent" % [
			int(_stats.markers_total),
			"missing" if _item_db == null else "loaded"])

	_feed_mixer()
	_apply_reverb(mission_info.reverb)
	_apply_music(mission_info.music)
	return _stats


func get_stats() -> Stats:
	return _stats


## The candidate ids the top-eight mixer holds a physical channel for, sorted.
func active_ambient_candidate_ids() -> Array[int]:
	var out: Array[int] = []
	for channel in _channels:
		if channel.candidate_id >= 0:
			out.append(channel.candidate_id)
	out.sort()
	return out


## The physical channel playing `candidate_id`, or null when the mixer holds
## none for it.
func ambient_player_for_candidate(candidate_id: int) -> AudioStreamPlayer3D:
	if candidate_id < 0:
		return null
	for channel in _channels:
		if channel.candidate_id == candidate_id \
				and is_instance_valid(channel.player):
			return channel.player
	return null


## Read/drive seams (ADR 0018): tests and diagnostics go through these, never
## the private fields. set_markers injects fully-described Marker records so the
## mix tick can be driven without a mission. `container` supplies a SceneTree
## home for the physical test channels.
func set_markers(markers: Array, container: Node3D = null) -> void:
	_stop_all_ambient_channels()
	_markers.assign(markers)
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
	for marker in _markers:
		for set_name in marker.layers_by_set:
			var layers: Array = marker.layers_by_set[set_name]
			for layer_value in layers:
				var layer: Dictionary = layer_value
				if not layer.has("candidate_id"):
					layer["candidate_id"] = _next_candidate_id
				_next_candidate_id = maxi(
					_next_candidate_id, int(layer.get("candidate_id", 0)) + 1)
	_feed_mixer()


## The "ambience disabled" arm: banks and the mission .DBF still load, no
## ambient marker resolves (the dialog-vs-ambient probe's silent control).
func set_ambient_markers_enabled(enabled: bool) -> void:
	_ambient_markers_enabled = enabled


func dialog_voice() -> AudioStreamPlayer:
	return _dialog_voice if _dialog_voice != null and is_instance_valid(_dialog_voice) else null


func get_perf_counters() -> Dictionary:
	var active_channels := 0
	for channel in _channels:
		if channel.candidate_id >= 0:
			active_channels += 1
	return {
		"tick_us": _perf_tick_us,
		"markers": _perf_markers,
		"voice_writes": _perf_voice_writes,
		"physical_channels": _channels.size(),
		"active_channels": active_channels,
		"ambient_decode_failures": _failed_candidate_ids.size(),
	}


func get_bank() -> SoundBank:
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
		var event := event_value as SoundEmitterRow
		if event != null:
			_queued_sound_emitters.append(event)


## The weather tick's thunder: the THUNDER trigger set played at a distance
## from the listener along a bearing (a 0..255 turn; 128 = behind the camera)
## (retail Sound_PlayTriggerSetScaled @ 0x527b90 — the 24-byte emitter
## {0x10000, bearing, g_SoundVolumeOption, 0, distance, 0} into
## SoundBank_PlayTriggerEntries @ 0x75ccd0 on dword_24E0914; sequencer A at
## 1 m centred @ 0x57ecfb, B at 10 m from behind @ 0x57edc4).
func play_weather_sounds(events: Array, camera_xform: Transform3D) -> void:
	if events.is_empty() or _bank == null or _audio_root == null:
		return
	var forward := -camera_xform.basis.z
	for event_value in events:
		var event := event_value as WeatherSoundRow
		if event == null:
			continue
		var distance := event.distance
		var bearing := event.bearing
		var dir := forward.rotated(Vector3.UP, float(bearing) * TAU / BEARING_BAM8_TURN)
		var pos := camera_xform.origin + dir * distance
		_bank.play_oneshot_3d(_audio_root, pos, "THUNDER", SFX_BUS, camera_xform.origin)


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


## A non-positional interface one-shot: the engine's zero-position play used by
## the weapon-switch/equip deny click — a 24-byte emitter with header 0x10000,
## zeroed position, and the interface volume option, routed straight into the
## trigger-set player [orig: Sound_PlayInterfaceTriggerSet @ 0x527be0 ->
## SoundBank_PlayTriggerEntries @ 0x75ccd0]. The set NAME comes from the
## mission-load resolver walking the 36-B {name[32], slot*} table @ 0x82F590
## across every loaded bank (DialogSystem_Init @ 0x527687/@ 0x5276e6, two
## passes of SoundBank_FindTriggerByName @ 0x75be90).
func ui_soundset(name: String) -> bool:
	if _bank == null or _audio_root == null:
		return false
	return _bank.spawn_oneshot_2d(_audio_root, name, SFX_BUS) != null


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
		push_warning("MissionAudio: unresolved dialog id %d" % wav_id)
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
		push_warning("MissionAudio: WAC wave '%s' did not resolve" % filename)
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
		stream = WavLoader.from_bytes(bytes)
	_wac_wav_cache[key] = stream
	return stream


## Pump for the mission clock; HHMM like MissionEnvironment.time_of_day.
func set_time_of_day_hhmm(hhmm: float) -> void:
	_time_of_day_hhmm = hhmm
	if _mixer != null:
		_mixer.set_time_of_day_hours(_hhmm_to_hours(hhmm))


## World-driven eval clock: the world tick pushes the sim's logic tick after each
## session tick batch, and the native mixer runs the witnessed staggered cohort
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


## Occlusion provider (the Simulation) — emitter/one-shot distances inflate
## through the witnessed two-ray LOS so occluded sources sound farther [orig:
## Sound_ApplyOcclusionDistance @ 0x529970]. Optional: tests and the menu run
## without a sim and mix unoccluded.
func set_simulation(sim: Simulation) -> void:
	_simulation = sim
	if _bank != null:
		_bank.occlusion_provider = sim
	if _mixer != null:
		_mixer.set_occlusion_provider(sim)


## Test-injection seam: a Callable(listener, source, dist_q16, source_id) -> int
## occlusion override, consulted by the bank and the mixer only when no
## Simulation is set. Replaces the deleted duck-typed provider stubs.
func set_occlusion_override(override: Callable) -> void:
	_occlusion_override = override
	if _bank != null:
		_bank.occlusion_override = override
	if _mixer != null:
		_mixer.set_occlusion_override(override)


## The per-frame ambient mix pass [orig: SoundEmitter_UpdateAndMixTop8 @ 0x5284a0,
## called once per render frame from the Game Loop render callback @ 0x521341]:
## the native mixer (engine/runtime/audio AmbientMixer) ranks the LIVE emitter slots —
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
	var rows: PackedFloat32Array = _mixer.mix(camera_pos)
	const row_stride := 6
	# Ranked loudest-first (candidate-id tie-break) by the native mixer; a
	# physical incumbent is never rebound merely because its rank within the
	# selected eight changed.
	var candidates: Array[Candidate] = []
	for base in range(0, rows.size(), row_stride):
		var row_id := int(rows[base])
		var binding: CandidateBinding = _candidate_lookup.get(row_id)
		if binding == null:
			continue
		var pos_base := base + 3
		candidates.append(Candidate.new(row_id, binding,
				Vector3(rows[pos_base], rows[pos_base + 1], rows[pos_base + 2]),
				int(rows[base + 1]), int(rows[base + 2])))
	var incumbent_by_id: Dictionary = {}  # candidate_id -> Channel
	for channel in _channels:
		if channel.candidate_id >= 0:
			incumbent_by_id[channel.candidate_id] = channel

	# Resolve streams only for new candidates that would enter the top eight.
	# Failed/corrupt descriptors are cached out and the next-ranked candidate
	# gets the channel, matching the old eager path's "unresolvable = absent".
	var selected: Array[Candidate] = []
	for candidate in candidates:
		if selected.size() >= MIX_CHANNELS:
			break
		if _failed_candidate_ids.has(candidate.candidate_id):
			continue
		if not incumbent_by_id.has(candidate.candidate_id):
			var stream := _validate_candidate_stream(
				candidate.candidate_id, candidate.descriptor)
			if stream == null:
				_failed_candidate_ids[candidate.candidate_id] = true
				continue
			candidate.resolved_stream = stream
		selected.append(candidate)

	var selected_ids: Dictionary = {}
	for candidate in selected:
		selected_ids[candidate.candidate_id] = true

	# Dropouts release their physical slot. If the same virtual candidate later
	# re-enters it is rebound and play() starts it from the beginning, like the
	# original transient channel registration.
	for channel in _channels:
		if channel.candidate_id < 0 or selected_ids.has(channel.candidate_id):
			continue
		var player := channel.player
		player.stop()
		player.stream = null
		player.volume_db = SILENT_DB
		player.process_mode = Node.PROCESS_MODE_DISABLED
		channel.candidate_id = -1
		writes += 1

	for candidate in selected:
		var channel: Channel = incumbent_by_id.get(candidate.candidate_id)
		if channel == null:
			channel = _free_or_new_channel()
			if channel == null:
				continue
			var player := channel.player
			SoundBank.configure_ambient_player(
				player, candidate.resolved_stream, candidate.descriptor, candidate.bus)
			player.position = candidate.pos
			player.volume_db = SoundBank.volume_db_from_255(candidate.vol)
			player.pitch_scale = _candidate_pitch_scale(candidate)
			player.process_mode = Node.PROCESS_MODE_INHERIT
			channel.candidate_id = candidate.candidate_id
			player.play()
			writes += 1
			continue
		var incumbent := channel.player
		var changed := false
		if incumbent.position != candidate.pos:
			incumbent.position = candidate.pos
			changed = true
		var db := SoundBank.volume_db_from_255(candidate.vol)
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
	if _stats != null:
		_stats.physical_channels = _channels.size()


func _resolve_candidate_stream(descriptor: Dictionary) -> AudioStreamWAV:
	var injected: Variant = descriptor.get("stream")
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
		if _stats != null:
			_stats.ambient_candidates_validated = _validated_candidate_ids.size()
	if stream != null:
		return stream
	if _stats != null:
		_stats.ambient_decode_failures = _failed_candidate_ids.size() + 1
	if not _warned_ambient_decode_failure:
		_warned_ambient_decode_failure = true
		push_warning(
			"MissionAudio: ambient WAV '%s' failed to decode; excluding failed candidates from the eight-channel mix" %
			String(descriptor.get("wav_path", "<injected>")))
	return null


## A free channel, a new one under the MIX_CHANNELS budget, else null.
func _free_or_new_channel() -> Channel:
	for channel in _channels:
		if channel.candidate_id < 0:
			return channel
	if _channels.size() >= MIX_CHANNELS or _audio_root == null:
		return null
	var player := AudioStreamPlayer3D.new()
	player.name = "AmbientChannel%d" % _channels.size()
	player.volume_db = SILENT_DB
	player.process_mode = Node.PROCESS_MODE_DISABLED
	_audio_root.add_child(player)
	var channel := Channel.new(player)
	_channels.append(channel)
	return channel


func _stop_all_ambient_channels() -> void:
	for channel in _channels:
		var player := channel.player
		if player != null and is_instance_valid(player):
			player.stop()
			player.stream = null
			player.volume_db = SILENT_DB
			player.process_mode = Node.PROCESS_MODE_DISABLED
		channel.candidate_id = -1


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
	_mixer = AmbientMixer.new()
	_mixer.set_occlusion_provider(_simulation)
	_mixer.set_occlusion_override(_occlusion_override)
	_mixer.set_time_of_day_hours(_hhmm_to_hours(_time_of_day_hhmm))
	_candidate_lookup.clear()
	_free_candidate_ids.clear()
	_retired_candidate_ids.clear()
	_queued_sound_emitters.clear()
	_dynamic_emitter_states.clear()
	_world_driven_ticks = false
	_world_driven_tick_offset = 0
	for marker in _markers:
		var set_names: Array = []
		var sets: Array = []
		for set_name in marker.layers_by_set:
			var packed := PackedInt32Array()
			for layer_value in marker.layers_by_set[set_name]:
				var layer: Dictionary = layer_value
				var cid := int(layer.get("candidate_id", 0))
				_candidate_lookup[cid] = CandidateBinding.new(layer, AMBIENT_BUS)
				packed.append_array(PackedInt32Array([
					cid,
					int(layer.get("falloff_radius", 0)),
					int(layer.get("min_distance", 0)),
					int(layer.get("volume", SoundBank.VOLUME_BYTE_MAX)),
					int(layer.get("clamp_volume", SoundBank.VOLUME_BYTE_MAX)),
				]))
			if packed.is_empty():
				continue
			set_names.append(String(set_name))
			sets.append(packed)
		var slot_keys := PackedInt32Array([-1, -1, -1, -1])
		for r in range(4):
			if r < marker.slot_sets.size():
				slot_keys[r] = set_names.find(String(marker.slot_sets[r]))
		_mixer.add_marker(marker.pos, marker.source_bms_id, marker.stagger_slot, 0,
				slot_keys, sets)


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
	for event in pending:
		var event_tick := int(event.emitted_tick)
		if _world_driven_ticks:
			event_tick += _world_driven_tick_offset
		event_tick = mini(event_tick, final_tick)
		if event_tick > int(_mixer.clock_tick()):
			_mixer.advance_to_tick(event_tick)
		var source_spawn_id := event.source_spawn_id
		var lane := event.lane
		var key := "%d:%d" % [source_spawn_id, lane]
		var pos := event.pos
		var source_bms_id := event.source_bms_id
		var lifetime := maxi(1, event.lifetime)
		var pitch_q16 := event.pitch_q16
		var volume_q8_8 := event.volume_q8_8
		if event.source_only:
			_mixer.update_emitter_source(source_spawn_id, pos, source_bms_id)
			continue
		if pitch_q16 == 0 or volume_q8_8 == 0:
			_mixer.register_emitter(source_spawn_id, lane, pos, source_bms_id,
					lifetime, pitch_q16, volume_q8_8, PackedInt32Array())
			_forget_dynamic_emitter(key)
			continue

		var set_name := event.soundset
		if set_name.is_empty():
			continue
		var described: Array = _bank.describe_ambient(set_name)
		if described.is_empty():
			continue
		var emitter: DynamicEmitter = _dynamic_emitter_states.get(key)
		if emitter == null or emitter.set_name != set_name \
				or emitter.candidate_ids.size() != described.size():
			if emitter != null:
				_mixer.register_emitter(source_spawn_id, lane, pos,
						source_bms_id, lifetime, 0, 0, PackedInt32Array())
				_forget_dynamic_emitter(key)
			var candidate_ids := PackedInt32Array()
			candidate_ids.resize(described.size())
			for i in described.size():
				candidate_ids[i] = _allocate_dynamic_candidate_id()
			emitter = DynamicEmitter.new(set_name, candidate_ids)
			_dynamic_emitter_states[key] = emitter
		emitter.expires_tick = int(_mixer.clock_tick()) + lifetime

		var layers := PackedInt32Array()
		for i in described.size():
			var descriptor: Dictionary = (
					described[i] as Dictionary).duplicate()
			var candidate_id := int(emitter.candidate_ids[i])
			descriptor["candidate_id"] = candidate_id
			_candidate_lookup[candidate_id] = CandidateBinding.new(descriptor, SFX_BUS)
			layers.append_array(PackedInt32Array([
				candidate_id,
				int(descriptor.get("falloff_radius", 0)),
				int(descriptor.get("min_distance", 0)),
				int(descriptor.get(
					"volume", SoundBank.VOLUME_BYTE_MAX)),
				int(descriptor.get(
					"clamp_volume", SoundBank.VOLUME_BYTE_MAX)),
			]))
		_mixer.register_emitter(source_spawn_id, lane, pos, source_bms_id,
				lifetime, pitch_q16, volume_q8_8, layers)


func _forget_dynamic_emitter(key: String) -> void:
	var emitter: DynamicEmitter = _dynamic_emitter_states.get(key)
	if emitter == null:
		return
	for candidate_id in emitter.candidate_ids:
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
		var emitter: DynamicEmitter = _dynamic_emitter_states[key]
		if now_tick > emitter.expires_tick:
			expired_keys.append(key)
	for key in expired_keys:
		_forget_dynamic_emitter(key)


func _release_retired_candidate_ids() -> void:
	if _retired_candidate_ids.is_empty():
		return
	var bound_ids: Dictionary = {}
	for channel in _channels:
		if channel.candidate_id >= 0:
			bound_ids[channel.candidate_id] = true
	var still_retired: Array[int] = []
	for candidate_id in _retired_candidate_ids:
		if bound_ids.has(candidate_id):
			still_retired.append(candidate_id)
		else:
			_free_candidate_ids.append(candidate_id)
	_retired_candidate_ids = still_retired


static func _sound_emitter_event_before(a: SoundEmitterRow, b: SoundEmitterRow) -> bool:
	return a.emitted_tick < b.emitted_tick


## The player's pitch: the layer's authored base pitch times the emitter's
## 16.16 pitch word (the native mix row's pitch_q16).
static func _candidate_pitch_scale(candidate: Candidate) -> float:
	return SoundBank.effective_base_pitch(candidate.descriptor) \
			* maxf(AmbientMixer.q16_to_float(candidate.pitch_q16), 0.0001)


## HHMM (MissionEnvironment.time_of_day) -> hours, through the engine's
## conversion (environment_state.h hhmm_to_minute_of_day).
static func _hhmm_to_hours(hhmm: float) -> float:
	return MissionEnvironment.hhmm_to_minute_of_day(hhmm) / MINUTES_PER_HOUR


func _load_bank(lwf_name: String) -> void:
	if not _resource_root.has_file(lwf_name):
		return
	var lwf := LwfData.new()
	if lwf.open_from_resource_root(_resource_root, lwf_name) == OK:
		_bank.add_bank(lwf)
		_stats.banks_loaded += 1


# Reverb id -> an AudioEffectReverb preset on the Ambient bus. NOT a port: retail's
# reverb is a software DSP in the mixer driven by a 20-row coefficient table baked
# into the image (0x7BF400; every row identical in stock JO, which ships no
# reverb.def for Audio_LoadReverbDefs @0x766d80 / the parser @0x7bf5e4 to
# override) and indexed by the per-tick reverb id the player body sets
# (@0x4b633f: userpoint, else the occupied building's def `reverb`, else the
# mission default). Witness record: docs/audio/lwf-dbf-sound-re.md "The reverb
# bed". This room-size stand-in scales with an id retail's rows do not vary with;
# its disposition (port the DSP or ledger a D-SND row) is pending the maintainer.
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
