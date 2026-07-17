class_name NovaMissionAudio
extends RefCounted

## Runtime mission audio orchestrator. Loads the mission's co-named .LWF + the
## global banks into a NovaSoundBank, resolves each placed envs-class entity's
## time-of-day slot sets BY NAME (items.def soundloop_1..4 = morning/day/
## evening/night [orig: Entity_UpdateEnvSoundEmitter @ 0x4a8080]; the engine is
## name-keyed — see docs/audio/lwf-dbf-sound-re.md), and spawns looping
## AudioStreamPlayer3D voices at the marker. tick(camera_pos) runs the witnessed
## ambient emitter mix: per-voice two-radius distance volumes, region
## crossfades, and the loudest-8 channel budget [orig:
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
## name is plumbed into the world — host follow-up).

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
# Time-of-day region cuts, hours: [4,10)=morning, [10,17)=day, [17,21)=evening,
# else night — soundloop_1..4 select by region [orig: Entity_CalcTimeOfDayRegion
# @ 0x408110 boundaries 0x40000/0xA0000/0x110000/0x150000 Q16].
const REGION_CUTS_H: Array[float] = [4.0, 10.0, 17.0, 21.0]
# Crossfade margin at a region edge: 5460/65536 h (~5 game-minutes) [orig: @ 0x408203].
const REGION_BLEND_H := 5460.0 / 65536.0

var _resource_root  # NovaResourceRoot
var _item_db  # NovaItemDatabase
var _simulation: Object = null  # NovaSimulation (occlusion LOS); optional
var _bank: NovaSoundBank
var _dbf  # NovaDbfData (mission co-named dialog bank; null if absent)
var _audio_root: Node3D
# Placed ambient markers ("snd:" items). Each carries the four time-of-day slot
# set names (soundloop_1..4) and one spawned voice group per DISTINCT set; the
# mix tick picks the active slot by region and drives volumes/pauses.
# [{ node, pos:Vector3, slot_sets:PackedStringArray(4), stagger_h:float,
#    voices:{set_name: Array[AudioStreamPlayer3D]} }]
var _markers: Array = []
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


## Load banks, spawn ambient marker voices under `container`, apply the reverb bed.
## `mission_name` is the .bms filename (its basename selects the co-named .LWF).
## Returns a stats dictionary.
func setup(mission, mission_name: String, container: Node3D) -> Dictionary:
	_stats = {"markers_total": 0, "markers_resolved": 0, "banks_loaded": 0, "voices": 0}
	if mission == null or container == null or _resource_root == null:
		return _stats
	var mission_info: Dictionary = mission.get_info()

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
			# Preserve the two explicit host-only fallback strategies on the
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
		# One voice group per distinct time-of-day set; all spawn silent+paused
		# and the mix tick activates the region's slot. The original registers
		# only the CURRENT region's set each tick [orig: Entity_UpdateEnvSoundEmitter
		# @ 0x4a81da]; persistent paused voices are the host equivalent.
		var voices: Dictionary = {}
		var voice_count := 0
		var marker_node: Node3D = null
		for set_name in distinct:
			var node := _bank.spawn_ambient(_audio_root, pos, set_name, AMBIENT_BUS)
			if node == null:
				continue
			if marker_node == null:
				marker_node = node
			var players: Array[AudioStreamPlayer3D] = []
			for child in node.get_children():
				if child is AudioStreamPlayer3D:
					players.append(child)
			voices[set_name] = players
			voice_count += players.size()
		if voices.is_empty():
			continue
		_markers.append({
			"node": marker_node,
			"pos": pos,
			"slot_sets": slot_sets,
			# De-sync marker crossfades like the engine's per-entity clock
			# stagger [orig: @ 0x408158 (poolHandle & 0xF) << 11 Q16 hours].
			"stagger_h": float((_markers.size() & 0xF) << 11) / 65536.0,
			"voices": voices,
		})
		_stats.markers_resolved += 1
		_stats.voices += voice_count

	# Silence here has historically gone unnoticed (a bare stats print) — warn on
	# the two states that mean "no ambience will play" so they surface in logs.
	if int(_stats.banks_loaded) == 0:
		push_warning("NovaMissionAudio: no sound banks loaded (probed %s.LWF, expansion, %s) — mission ambience will be silent" % [
			mission_name.get_file().get_basename(), ", ".join(GLOBAL_LWFS)])
	elif int(_stats.markers_total) > 0 and int(_stats.markers_resolved) == 0:
		push_warning("NovaMissionAudio: 0/%d sound markers resolved (item db %s) — mission ambience will be silent" % [
			int(_stats.markers_total),
			"missing" if _item_db == null else "loaded"])

	_apply_reverb(int(mission_info.get("reverb", 0)))
	_apply_music(int(mission_info.get("music", 0)))
	return _stats


func get_stats() -> Dictionary:
	return _stats


## Read/drive seams (ADR 0018): tests and diagnostics go through these, never
## the private fields. set_markers injects fully-resolved marker entries (the
## shape _markers documents above) so the mix tick can be driven without a
## mission; dialog_voice exposes the active serialized-dialog voice.
func set_markers(markers: Array) -> void:
	_markers = markers


func set_resolution_strategy(strategy: int) -> void:
	_strategy = strategy


func dialog_voice() -> AudioStreamPlayer:
	return _dialog_voice if _dialog_voice != null and is_instance_valid(_dialog_voice) else null


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
## engine resolves a pre-loaded sound_id handle; the action path plays it at full
## emitter volume: ActionSlot_PlaySound @0x4010c0 -> Entity_PlaySound3D_FullVolume
## @0x528e20). One-shot volume snapshots the listener distance at fire time, and
## the set's cull range gates the fire entirely [orig: Sound_Play3DPositional
## @ 0x527cb0; cull @ 0x527cd1].
func fire_soundset(name: String, world_pos: Vector3) -> bool:
	if _bank == null or _audio_root == null:
		return false
	return _bank.play_oneshot_3d(_audio_root, world_pos, name, SFX_BUS, _last_camera_pos)


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


## Host pump for the mission clock; HHMM like NovaEnvironment.time_of_day.
func set_time_of_day_hhmm(hhmm: float) -> void:
	_time_of_day_hhmm = hhmm


## Occlusion provider (the NovaSimulation) — emitter/one-shot distances inflate
## through the witnessed two-ray LOS so occluded sources sound farther [orig:
## Sound_ApplyOcclusionDistance @ 0x529970]. Optional: tests and the menu run
## without a sim and mix unoccluded.
func set_simulation(sim: Object) -> void:
	_simulation = sim
	if _bank != null:
		_bank.occlusion_provider = sim


## The per-frame ambient emitter mix [orig: SoundEmitter_UpdateAndMixTop8
## @ 0x5284a0]: every marker voice computes its witnessed distance volume for
## the CURRENT time-of-day slot, the loudest MIX_CHANNELS play, everything else
## pauses. Volume = member volume x the region crossfade blend through the
## two-radius curve; a voice at or beyond its falloff radius is hard silent
## (which is also the cull [orig: @ 0x5285da]). Persistent paused voices stand
## in for the original's transient re-registered slots —
## docs/audio/lwf-dbf-sound-re.md (D-SND-6, D-SND-8). Occlusion inflates the
## mixed distance through the sim's two-ray LOS [orig: the
## Sound_ApplyOcclusionDistance call @ 0x528659, after the range cull]; rays
## run only for markers whose raw distance already yields audible volume — the
## original culls before raycasting [orig: @ 0x5285da], and inflation only
## ever reduces volume, so a raw-silent marker stays silent either way.
func tick(camera_pos: Vector3) -> void:
	var start := Time.get_ticks_usec()
	_last_camera_pos = camera_pos
	var writes := 0
	var hhmm := _time_of_day_hhmm
	var base_hours := _hhmm_to_hours(hhmm)
	var candidates: Array = []  # [{player, vol}]
	var silent: Array[AudioStreamPlayer3D] = []
	for m in _markers:
		var holder: Node3D = m.node
		if holder == null or not is_instance_valid(holder):
			continue
		var tod := time_of_day_region(base_hours + float(m.stagger_h))
		var region := int(tod.region)
		var slot_sets: PackedStringArray = m.slot_sets
		var active_set := String(slot_sets[region])
		var blend := float(tod.blend)
		# Neighbouring regions sharing the set keep full volume through the
		# crossfade [orig: @ 0x4a819d same-slot check].
		if active_set == String(slot_sets[int(tod.adjacent)]):
			blend = 1.0
		var vol_byte := crossfade_volume_byte(blend)
		var dist_q16 := int((m.pos as Vector3).distance_to(camera_pos) * 65536.0)
		var dist_occluded_q16 := -1  # lazy: at most one two-ray LOS per marker per tick
		var voices: Dictionary = m.voices
		for set_name in voices.keys():
			var players: Array = voices[set_name]
			var is_active: bool = String(set_name) == active_set and not active_set.is_empty()
			for player in players:
				if not (player is AudioStreamPlayer3D) or not is_instance_valid(player):
					continue
				var vol := 0
				if is_active and player.has_meta("layer_params"):
					var lp: Dictionary = player.get_meta("layer_params")
					var falloff := int(lp.get("falloff_radius", 0))
					var min_d := int(lp.get("min_distance", 0))
					var member_vol := int(lp.get("volume", 255))
					var clamp_vol := int(lp.get("clamp_volume", 255))
					vol = NovaSoundBank.emitter_layer_volume(
						dist_q16, falloff, min_d, vol_byte, member_vol, clamp_vol)
					if vol > 0 and _simulation != null:
						if dist_occluded_q16 < 0:
							dist_occluded_q16 = int(_simulation.sound_occlusion_distance_q16(
								camera_pos, m.pos, dist_q16))
						if dist_occluded_q16 != dist_q16:
							vol = NovaSoundBank.emitter_layer_volume(
								dist_occluded_q16, falloff, min_d, vol_byte, member_vol, clamp_vol)
				if vol > 0:
					candidates.append({"player": player, "vol": vol})
				else:
					silent.append(player)
	# Loudest-first; only the top MIX_CHANNELS mix [orig: the top-8 sort
	# @ 0x5287ab and the drop-out channel stop @ 0x528a70]. Writes are gated on
	# volume_db (not stream_paused readback — the headless dummy audio driver
	# always reads stream_paused back as false, which would defeat idempotence).
	candidates.sort_custom(func(a, b): return int(a.vol) > int(b.vol))
	for i in candidates.size():
		var c: Dictionary = candidates[i]
		var player: AudioStreamPlayer3D = c.player
		if i < MIX_CHANNELS:
			var db := NovaSoundBank.volume_db_from_255(int(c.vol))
			if not is_equal_approx(player.volume_db, db):
				player.volume_db = db
				player.stream_paused = false
				writes += 1
		else:
			silent.append(player)
	for player in silent:
		if player.volume_db > SILENT_DB:
			player.volume_db = SILENT_DB
			player.stream_paused = true
			writes += 1
	_perf_markers = _markers.size()
	_perf_voice_writes = writes
	_perf_tick_us = Time.get_ticks_usec() - start


func teardown() -> void:
	# Dropping _audio_root frees the dialog + wac voice nodes too; just drop our refs
	# so a late `finished` after teardown can't pump a freed queue.
	_apply_reverb(0)
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

static func _hhmm_to_hours(hhmm: float) -> float:
	var wrapped := fposmod(hhmm, 2400.0)
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
	var t := fposmod(hours, 24.0)
	var region := 3
	var low := REGION_CUTS_H[3]
	var high := 0.0
	for i in range(3):
		# Regions are OPEN at the low cut (the original's unsigned range-check
		# idiom starts each interval at cut+1 tick): the exact cut instant
		# falls through to night at full blend [orig: @ 0x408175/0x4081b0
		# (t - (cut+1)) <= (width-2) forms].
		if t > REGION_CUTS_H[i] and t < REGION_CUTS_H[i + 1]:
			region = i
			low = REGION_CUTS_H[i]
			high = REGION_CUTS_H[i + 1]
			break
	var blend := 1.0
	var direction := 1
	var blend_dist := 0.0
	var in_blend := false
	if region == 3:
		# Night wraps 21h -> 4h; only its 21h edge fades [orig: the wrapped
		# region's high-edge test can't fire @ 0x40820f].
		if t > low and t - REGION_BLEND_H < low:
			blend_dist = t - low
			in_blend = true
	else:
		if t - REGION_BLEND_H < low:
			blend_dist = t - low
			in_blend = true
		elif t + REGION_BLEND_H > high:
			direction = -1
			blend_dist = high - t
			in_blend = true
	# A zero blend distance falls through to full volume — the original's
	# `if (blendDistance && ...)` guard [orig: @ 0x408251].
	if in_blend and blend_dist > 0.0:
		blend = blend_dist / REGION_BLEND_H
	var adjacent := region - direction
	if adjacent > 3:
		adjacent = 0
	elif adjacent < 0:
		adjacent = 3
	return TimeOfDayRegion.new(region, adjacent, clampf(blend, 0.0, 1.0))


## The emitter volume byte for a region crossfade blend. The original registers
## the volume word (0xFFFF * blend_q16 + 0x8000) >> 16 — ROUNDED, with 0xFFFF as
## the full-blend sentinel — and the mixer reads its HIGH byte as the emitter
## volume [orig: Entity_UpdateEnvSoundEmitter @ 0x4a81c6 (blendAlpha); the mix
## reads slot byte +25 @ 0x52865e]. Net: byte = (0xFFFF * blend_q16 + 0x8000) >> 24.
static func crossfade_volume_byte(blend: float) -> int:
	var blend_q16 := 0xFFFF if blend >= 1.0 else int(clampf(blend, 0.0, 1.0) * 65536.0)
	return (0xFFFF * blend_q16 + 0x8000) >> 24


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
