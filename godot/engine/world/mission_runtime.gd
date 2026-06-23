extends Node

# THE shared mission runtime driver. Owns the sim (NovaSimulation) + the present pass + the entity
# index, and runs the ONE faithful per-tick loop both the game and the editor go through. Consolidates
# the two divergent stacks it replaces: the game's hand-wired NovaSimulation + MissionCommandHost, and
# the editor's separate MissionSimDriver. Both now load + tick + present + restore through this.
#
# Per-tick order (single-sourced here, faithful to the original main loop's server-tick-then-render):
#   advance logic (sim) -> present entity state onto nodes -> drain + emit side effects.
# [orig: sub_4F81A0 runs the logic systems; the client then renders the entities. Terrain/foliage/audio
#  are host render passes the caller composes around this.]
#
# Cadence: both tick modes run ONE logic tick per call (the original's 62 Hz engine tick) — the
# engine's dividers gate INSIDE the systems (the WAC VM fires every 62nd tick, the BMS evaluator
# quarter-passes every 16th). TICK_DIVIDED is the game mode (advance_frame, the faithful host-frame
# entry); TICK_EVERY_PROCESS calls step() directly (tests). The game and the editor preview both run
# DIVIDED with the sim's default loco_scale. The driver can self-tick via _process (editor) or be
# driven by an explicit tick() call so a host can order it against its other passes (game). Stop
# rewinds the world (World::restore) AND restores the authored node transforms captured at setup.

signal effects_drained(effects: Array)

const MissionEntityRegistry := preload("res://engine/world/mission_entity_registry.gd")
const MissionObjectPlacer := preload("res://engine/mission/mission_object_placer.gd")
const MissionPresentPass := preload("res://engine/world/mission_present_pass.gd")
const MissionSeatDiagnostics := preload("res://engine/world/mission_seat_diagnostics.gd")

# Fixed-timestep accumulator. The original decouples the simulation from rendering: the master
# loop accumulates real elapsed time and dispatches the logic update once per 16 ms (62.5 Hz),
# independently of the variable render rate — multiple ticks on a long frame, zero on a short one.
# [orig: Game_MainLoop @ 0x52b630 -> Game_ProcessMainFrame @ 0x5263f0 (one current_tick++ @ 0x24c1968)]
const TICK_DT := 1.0 / 62.5          # 0.016 s; matches AiEventQueue::kFrameDt (world/ai.h)
const MAX_CATCHUP_TICKS := 31        # spiral-of-death clamp: port of the 500 ms / 16 ms accumulator cap

var _sim: NovaSimulation
var _present
var _index
var _self_tick := false              # editor: self-tick via _process while playing; game: host calls tick()
var _playing := false
var _orig_transforms: Dictionary = {} # node -> Transform3D captured at setup, for restore-on-stop
var _perf_tick_us: int = 0
var _perf_sim_us: int = 0
var _perf_present_us: int = 0
var _perf_effects_us: int = 0
var _perf_did_tick := false
var _accum := 0.0                    # banked real time (s) not yet consumed by a logic tick
var _ticks_last_frame := 0           # logic ticks run by the last tick_realtime() call (catch-up signal)


## Create + promote the mission, build the shared index over the placed nodes (`container`), and wire
## the present pass. options: { tick_mode, loco_scale, self_tick, present_options }. Returns the AI
## entity count, or 0 on load failure (the orphan sim is freed). The sim is held off-tree by this driver.
func setup(mission, container: Node, options: Dictionary = {}) -> int:
	_sim = NovaSimulation.new()
	_sim.set_tick_mode(int(options.get("tick_mode", NovaSimulation.TICK_DIVIDED)))
	if options.has("loco_scale"):
		_sim.set_loco_scale(int(options["loco_scale"]))
	# Playable hosts turn the sim into the SP in-process listen server BEFORE load
	# (NetSystem registers ahead of WAC, ADR 0011), then spawn the host player after load.
	var playable := bool(options.get("playable", false))
	if playable or options.get("listen_server", false):
		_sim.enable_listen_server(true)
	if options.get("resource_root") != null and options.get("item_db") != null:
		_sim.set_item_seat_specs(_build_item_seat_specs(mission, options["resource_root"], options["item_db"]))
	if mission == null or not _sim.load_from_mission_data(mission):
		_sim.free()  # NovaSimulation is a Node (not RefCounted); free the orphan on load failure
		_sim = null
		return 0
	# Ground the AI on the host's terrain (editor preview + game share this one call). Entities hug
	# the terrain instead of floating; absent/unloaded terrain leaves their authored Z untouched.
	if options.get("terrain") != null:
		_sim.set_terrain_height_field(options["terrain"])
	# Anim-driven soldiers: resolve the infantry clip set (.adm -> .bad root-motion tracks) through
	# the host's resource root. Without it soldiers stand still — their motion comes from clips.
	if options.get("resource_root") != null:
		var adm_name := String(options.get("infantry_adm", "E_STAND.adm"))
		if int(_sim.set_infantry_anim_map(options["resource_root"], adm_name)) <= 0:
			push_warning("MissionRuntime: no infantry clips from '%s' — AI soldiers will stand still." % adm_name)
	# Mission WAC scripts: compile game.wac/server.wac/<mission>.wac through the host's
	# resource root and install on the sim [orig: WacScript_InitAndLoad]. Absent files skip
	# silently — a BMS-only mission leaves the VM unloaded and the script system early-outs.
	# The VM self-gates to every 62nd tick inside the system [orig: dword_C6EAD4 / cmp 0x3E].
	if options.get("resource_root") != null and options.has("wac_basename"):
		var wac := NovaWacProgram.new()
		var wac_err := int(wac.compile_from_resource_root(options["resource_root"], String(options["wac_basename"])))
		if wac_err == OK:
			_sim.set_wac_program(wac)
		elif wac_err != ERR_DOES_NOT_EXIST:
			push_warning("MissionRuntime: WAC for '%s' failed to compile (%d error(s)) — scripts disabled." % [
				options["wac_basename"], wac.get_error_count()])
	# The SIM is held off-tree (never add_child'd): only this driver advances it, and an off-tree
	# node never self-ticks via _process; it is freed explicitly in _exit_tree (mirrors the old
	# MissionSimDriver). This MissionRuntime node itself IS in the tree — its host adds it, and
	# self_tick only decides whether _process here calls tick() or the host does.
	_self_tick = bool(options.get("self_tick", false))
	_index = MissionEntityRegistry.new()
	_index.build(container, mission)
	_present = MissionPresentPass.new()
	_present.setup(_sim, _index, options.get("present_options", {}))
	# Spawn the host's own player as an authoritative pool-0 entity (ADR 0012 / net-re §5.2b).
	# After load (the spawn needs the AI system wired). The spawn POSE is selected the way the
	# original engine does — by game type, from the mission's player-START marker FARTHEST from the
	# enemy set — NOT from the first NPC's position (net-re §5.2c). The player then runs the infantry
	# motor from input (set_player_input); visible translation needs walk clips.
	if playable or options.get("player", false):
		var spawn_status := int(_sim.spawn_local_player_at_start())
		if spawn_status < 0:
			push_warning("MissionRuntime: spawn_local_player failed (pool 0 full / no AI?)")
		elif spawn_status == 0:
			push_warning("MissionRuntime: no player-start marker (60xx start family) in this mission — spawned at fallback origin.")
	# Per-entity grounding: resolve each infantry soldier's OWN model .adm so it grounds + locomotes
	# off its own clip's capsule_bottom (crouch/sit/jump plant correctly), not the shared default
	# set. Runs after the NPC promote AND the player spawn so both are covered. [D-INF-6]
	if options.get("resource_root") != null and options.get("item_db") != null:
		_sim.resolve_infantry_adm_ids(options["resource_root"], options["item_db"])
	_log_infantry_debug_mounts()
	# Capture the authored node transforms now (pre-tick) so Stop restores them whether the host
	# played or only stepped. Cheap; the game never Stops but holding the map costs nothing.
	_capture_transforms()
	return _sim.get_entity_count()


# --- the local player (Phase 2; ADR 0012). Thin delegates to the sim for the host. ---
func has_player() -> bool:
	return _sim != null and _sim.has_local_player()

func local_player_position() -> Vector3:
	return _sim.get_local_player_position() if _sim != null else Vector3.ZERO

func local_player_yaw_deg() -> float:
	return _sim.get_local_player_yaw_deg() if _sim != null else 0.0

func local_player_pitch_deg() -> float:
	return _sim.get_local_player_pitch_deg() if _sim != null else 0.0

func local_player_anim_slot() -> int:
	return _sim.get_local_player_anim_slot() if _sim != null else -1

func local_player_anim_key() -> String:
	return String(_sim.get_local_player_anim_key()) if _sim != null else ""

func local_player_anim_phase_ticks() -> int:
	return int(_sim.get_local_player_anim_phase_ticks()) if _sim != null else 0

func set_player_input(forward: bool, back: bool, left: bool, right: bool, run: bool, crouch: bool, prone: bool, jump: bool, look_yaw_deg: float, look_pitch_deg: float) -> void:
	if _sim != null:
		_sim.set_player_input(forward, back, left, right, run, crouch, prone, jump, look_yaw_deg, look_pitch_deg)


func get_sim() -> NovaSimulation:
	return _sim


func get_present_index():
	return _index


func entity_count() -> int:
	return _sim.get_entity_count() if _sim != null else 0


func _build_item_seat_specs(mission, resource_root, item_db) -> Array:
	return MissionSeatDiagnostics.build_item_seat_specs(mission, resource_root, item_db)


func _model_name_for_graphic(graphic: String) -> String:
	return MissionSeatDiagnostics.model_name_for_graphic(graphic)


func _seat_specs_from_model(data: NovaObjectData) -> Array:
	return MissionSeatDiagnostics.seat_specs_from_model(data)


func _seat_local_from_user_point_position(pos: Vector3) -> Vector3:
	return MissionSeatDiagnostics.seat_local_from_user_point_position(pos)


func _seat_yaw_offset_from_user_point_rotation(direction: Vector3) -> int:
	return MissionSeatDiagnostics.seat_yaw_offset_from_user_point_rotation(direction)


func _seat_type_for_user_point(name: String) -> int:
	return MissionSeatDiagnostics.seat_type_for_user_point(name)


func _seat_pose_index_for_user_point(name: String) -> int:
	return MissionSeatDiagnostics.seat_pose_index_for_user_point(name)


func _log_infantry_debug_mounts() -> void:
	if _sim == null or OS.get_environment("NOVA_INF_DEBUG").is_empty():
		return
	for i in range(_sim.get_entity_count()):
		var card: Dictionary = _sim.get_entity_debug(i)
		if card.is_empty():
			continue
		var waypoint_id := int(card.get("waypoint_id", 0))
		if not bool(card.get("mounted", false)) and (waypoint_id < 123 or waypoint_id > 125):
			continue
		print("NOVA_INF_DEBUG entity=%d ssn=%d wp=%d:%d mounted=%s target=%d seat=%d source=%s type=%d bone=%d pose=%d local=%s yaw_offset=%d seats=%d anim=%s(%d)" % [
			i,
			int(card.get("net_id", 0)),
			waypoint_id,
			int(card.get("wp_number", 0)),
			str(bool(card.get("mounted", false))),
			int(card.get("mount_target_net_id", 0)),
			int(card.get("mount_seat", -1)),
			String(card.get("mount_seat_source_name", "")),
			int(card.get("mount_type", 0)),
			int(card.get("mount_seat_bone", 0)),
			int(card.get("mount_seat_pose_index", 0)),
			str(card.get("mount_seat_local", Vector3.ZERO)),
			int(card.get("mount_seat_yaw_offset", 0)),
			int(card.get("mount_target_seat_count", 0)),
			String(card.get("anim_key", "")),
			int(card.get("anim_state", -1)),
		])


func is_playing() -> bool:
	return _playing


## Advance EXACTLY ONE cadence step and present. Returns true when a logic tick fired (and effects
## were drained). The deterministic single-tick primitive: editor Step, the MCP, and tests use this.
## Real-time hosts (game + editor preview) use tick_realtime() instead, which accumulates wall-clock.
func tick() -> bool:
	if _sim == null:
		_perf_tick_us = 0
		_perf_sim_us = 0
		_perf_present_us = 0
		_perf_effects_us = 0
		_perf_did_tick = false
		_ticks_last_frame = 0
		return false
	var tick_start := Time.get_ticks_usec()
	var did_tick := _advance_one_tick_no_present()
	_perf_present_us = 0
	if did_tick and _present != null:
		var present_start := Time.get_ticks_usec()
		_present.present()
		_perf_present_us = Time.get_ticks_usec() - present_start
	_perf_tick_us = Time.get_ticks_usec() - tick_start
	_perf_did_tick = did_tick
	_ticks_last_frame = 1 if did_tick else 0
	return did_tick


# One logic tick + drain/emit effects, WITHOUT presenting. Shared by tick() (which presents once
# after) and tick_realtime() (which presents once after the whole catch-up batch). Updates the
# sim/effects perf counters. Returns true when a logic tick fired.
func _advance_one_tick_no_present() -> bool:
	var sim_start := Time.get_ticks_usec()
	var did_tick: bool
	if _sim.get_tick_mode() == NovaSimulation.TICK_EVERY_PROCESS:
		_sim.step()
		did_tick = true
	else:
		did_tick = _sim.advance_frame()  # one frame = one 62 Hz logic tick (WAC self-gates inside)
	_perf_sim_us = Time.get_ticks_usec() - sim_start
	_perf_effects_us = 0
	if did_tick:
		var effects_start := Time.get_ticks_usec()
		var effects := _sim.drain_effects()
		_perf_effects_us = Time.get_ticks_usec() - effects_start
		if not effects.is_empty():
			effects_drained.emit(effects)
	return did_tick


## Real-time host entry: bank `delta`, drain it in fixed TICK_DT quanta, run that many single logic
## ticks (clamped to MAX_CATCHUP_TICKS), and present ONCE after the batch. This is the faithful
## fixed-62.5 Hz accumulator — the sim runs at a constant rate while rendering stays decoupled at the
## host frame rate, with no inter-tick interpolation (present reads current sim state). A long frame
## runs several ticks, a short frame runs none. Effects drain per tick (the original's per-tick
## emission). Returns the number of logic ticks run this call. [orig: Game_MainLoop @ 0x52b630]
func tick_realtime(delta: float) -> int:
	if _sim == null or not _playing:
		_ticks_last_frame = 0
		return 0
	var tick_start := Time.get_ticks_usec()
	_accum += delta
	var n := int(_accum / TICK_DT)
	if n <= 0:
		_ticks_last_frame = 0
		_perf_tick_us = 0
		_perf_did_tick = false
		return 0
	_accum -= float(n) * TICK_DT
	if n > MAX_CATCHUP_TICKS:
		n = MAX_CATCHUP_TICKS
		_accum = 0.0  # drop the backlog so a load hitch doesn't spiral into the next frames
	var sim_us := 0
	var effects_us := 0
	for _i in range(n):
		_advance_one_tick_no_present()
		sim_us += _perf_sim_us
		effects_us += _perf_effects_us
	_perf_sim_us = sim_us
	_perf_effects_us = effects_us
	_perf_present_us = 0
	if _present != null:
		var present_start := Time.get_ticks_usec()
		_present.present()
		_perf_present_us = Time.get_ticks_usec() - present_start
	_perf_tick_us = Time.get_ticks_usec() - tick_start
	_perf_did_tick = true
	_ticks_last_frame = n
	return n


func get_perf_counters() -> Dictionary:
	return {
		"tick_us": _perf_tick_us,
		"sim_us": _perf_sim_us,
		"present_us": _perf_present_us,
		"effects_us": _perf_effects_us,
		"did_tick": _perf_did_tick,
		"ticks": _ticks_last_frame,
		"sim": _sim.get_runtime_perf_counters() if _sim != null and _sim.has_method("get_runtime_perf_counters") else {},
	}


func _process(delta: float) -> void:
	if _self_tick and _playing:
		tick_realtime(delta)


# --- Editor transport (Play / Step / Stop) ------------------------------------

func play() -> void:
	_playing = true
	_accum = 0.0  # discard wall-clock banked while paused / loading, so Play doesn't burst-catch-up


func pause() -> void:
	_playing = false
	_accum = 0.0


## One manual tick (editor Step): one logic tick + present, without running the self-tick loop.
## Both tick modes advance one logic tick per call, so Step behaves identically under DIVIDED.
func step_once() -> void:
	_playing = false
	_accum = 0.0  # manual stepping is fully decoupled from wall-clock
	tick()


## Stop: rewind the world to the play-start baseline AND restore the authored node transforms, so the
## placed world is left exactly as it was. Safe to call when never played.
func stop() -> void:
	_playing = false
	_accum = 0.0  # a Stop -> Play cycle must not replay banked time
	if _sim != null:
		_sim.restart()  # World::restore baseline (registry/vars/env/clock) + AI re-seed
	_restore_transforms()


func _capture_transforms() -> void:
	_orig_transforms.clear()
	_for_each_present_node(func(node): _orig_transforms[node] = node.transform)


func _restore_transforms() -> void:
	for node in _orig_transforms.keys():
		if is_instance_valid(node):
			node.transform = _orig_transforms[node]
			node.visible = true  # undo any visibility the present pass changed
	_orig_transforms.clear()


# Walk the nodes the present pass drives (resolved from the current snapshot through the shared index).
func _for_each_present_node(fn: Callable) -> void:
	if _sim == null or _index == null:
		return
	var stride: int = _sim.get_present_stride()
	if stride <= 0:
		return
	var snap: PackedFloat32Array = _sim.get_present_snapshot()
	var count: int = snap.size() / stride
	for i in range(count):
		var base := i * stride
		var node = _index.resolve(
			int(snap[base + NovaSimulation.PF_BMS_ID]),
			int(snap[base + NovaSimulation.PF_KIND]),
			int(snap[base + NovaSimulation.PF_INDEX]))
		if node != null and is_instance_valid(node):
			fn.call(node)


# The sim is held off-tree, so free it explicitly when this driver leaves the tree (a reload / Stop
# queue_free()s the driver). [mirrors the old MissionSimDriver._exit_tree.]
func _exit_tree() -> void:
	if _sim != null and is_instance_valid(_sim):
		_sim.free()
		_sim = null
