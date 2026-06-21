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
const MissionPresentPass := preload("res://engine/world/mission_present_pass.gd")

var _sim: NovaSimulation
var _present
var _index
var _self_tick := false              # editor: self-tick via _process while playing; game: host calls tick()
var _playing := false
var _orig_transforms: Dictionary = {} # node -> Transform3D captured at setup, for restore-on-stop


## Create + promote the mission, build the shared index over the placed nodes (`container`), and wire
## the present pass. options: { tick_mode, loco_scale, self_tick, present_options }. Returns the AI
## entity count, or 0 on load failure (the orphan sim is freed). The sim is held off-tree by this driver.
func setup(mission, container: Node, options: Dictionary = {}) -> int:
	_sim = NovaSimulation.new()
	_sim.set_tick_mode(int(options.get("tick_mode", NovaSimulation.TICK_DIVIDED)))
	if options.has("loco_scale"):
		_sim.set_loco_scale(int(options["loco_scale"]))
	# Phase 2 (the moving player): turn the sim into the SP in-process listen server BEFORE
	# load (NetSystem registers ahead of WAC, ADR 0011). Gated by the NOVA_PLAYER launch flag,
	# threaded into options by the host. Default off -> the editor/preview path is unchanged.
	if options.get("listen_server", false):
		_sim.enable_listen_server(true)
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
	# After load (the spawn needs the AI system wired). Spawn near the first placed entity so
	# the player lands on the terrain; the terrain clamp grounds it. The player then runs the
	# infantry motor from input (set_player_input) — visible translation needs walk clips.
	if options.get("player", false):
		var spawn_pos := Vector3.ZERO
		if _sim.get_entity_count() > 0:
			spawn_pos = _sim.get_entity_position(0)
		if not _sim.spawn_local_player(spawn_pos, 0.0, 1):
			push_warning("MissionRuntime: spawn_local_player failed (pool 0 full / no AI?)")
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

func set_player_input(forward: bool, back: bool, left: bool, right: bool, run: bool, look_yaw_deg: float, look_pitch_deg: float) -> void:
	if _sim != null:
		_sim.set_player_input(forward, back, left, right, run, look_yaw_deg, look_pitch_deg)


func get_sim() -> NovaSimulation:
	return _sim


func get_present_index():
	return _index


func entity_count() -> int:
	return _sim.get_entity_count() if _sim != null else 0


func is_playing() -> bool:
	return _playing


## Advance one cadence step and present. Returns true when a logic tick fired (and effects were drained).
## The host calls this in its own per-frame order (game), or _process self-ticks it (editor).
func tick() -> bool:
	if _sim == null:
		return false
	var did_tick: bool
	if _sim.get_tick_mode() == NovaSimulation.TICK_EVERY_PROCESS:
		_sim.step()
		did_tick = true
	else:
		did_tick = _sim.advance_frame()  # one frame = one 62 Hz logic tick (WAC self-gates inside)
	if did_tick:
		if _present != null:
			_present.present()
		var effects := _sim.drain_effects()
		if not effects.is_empty():
			effects_drained.emit(effects)
	return did_tick


func _process(_delta: float) -> void:
	if _self_tick and _playing:
		tick()


# --- Editor transport (Play / Step / Stop) ------------------------------------

func play() -> void:
	_playing = true


func pause() -> void:
	_playing = false


## One manual tick (editor Step): one logic tick + present, without running the self-tick loop.
## Both tick modes advance one logic tick per call, so Step behaves identically under DIVIDED.
func step_once() -> void:
	_playing = false
	tick()


## Stop: rewind the world to the play-start baseline AND restore the authored node transforms, so the
## placed world is left exactly as it was. Safe to call when never played.
func stop() -> void:
	_playing = false
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
