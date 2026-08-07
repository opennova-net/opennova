extends Node

# THE shared mission runtime driver. Owns the sim (NovaSimulation), present pass, entity index, and
# one faithful tick pipeline. `GameWorld`, under `MainGame`, is its sole live mission host; ONED has
# no PIE or in-place mission simulation. Tests and non-gameplay tooling previews may still instantiate
# this component directly. It consolidated the historical game's hand-wired NovaSimulation +
# MissionCommandHost stack and the editor's separate MissionSimDriver.
#
# Per-tick order (single-sourced here, faithful to the original main loop's server-tick-then-render):
#   advance logic (sim) -> present entity state onto nodes -> drain + emit side effects.
# [orig: WacScript_AdvanceTick runs the logic systems; the client then renders the entities. Terrain/foliage/audio
#  are host render passes the caller composes around this.]
#
# Cadence: NovaSimulation.step() runs ONE logic tick (the original's 62 Hz engine tick) — the
# engine's dividers gate INSIDE the systems (the WAC VM fires every 62nd tick, the BMS evaluator
# quarter-passes every 16th). Deciding HOW MANY ticks a host frame runs is this driver's job, not
# the sim's: tick_realtime() banks wall-clock and dispatches 0..N of them; tick() dispatches exactly
# one. Live cadence is driven explicitly by GameWorld so it can order the runtime against its other
# passes. `_process` self-tick remains an opt-in seam for isolated tests/tooling previews; ONED does
# not use it for gameplay. Stop rewinds the world (World::restore) AND restores the authored node
# transforms captured at setup.

signal effects_drained(effects: Array)
## Emitted once after every authoritative 62.5 Hz logic step, after that
## step's side effects have been delivered. Presentation-only fixed-step
## systems (particles) subscribe here instead of integrating render delta.
signal fixed_tick_completed(logic_tick: int)
## Emitted after Stop rewinds simulation and authored transforms. Host-owned
## presentation systems use this boundary to discard transient runtime state.
signal simulation_restarted()

const MissionEntityRegistry := preload("res://adapter/world/mission_entity_registry.gd")
const MissionObjectPlacer := preload("res://adapter/mission/mission_object_placer.gd")
const MissionPresentPass := preload("res://adapter/world/mission_present_pass.gd")
const WirePresentPass := preload("res://adapter/world/wire_present_pass.gd")
const FirePresentPass := preload("res://adapter/world/fire_present_pass.gd")
const DestructionPresentPass := preload("res://adapter/world/destruction_present_pass.gd")
const ThrowablePresentPass := preload("res://adapter/world/throwable_present_pass.gd")
const ItemSeatSpecs := preload("res://adapter/world/item_seat_specs.gd")

# Fixed-timestep accumulator. The original decouples the simulation from rendering: the master
# loop accumulates real elapsed time and dispatches the logic update once per 16 ms (62.5 Hz),
# independently of the variable render rate — multiple ticks on a long frame, zero on a short one.
# [orig: Game_MainLoop @ 0x52b630 -> Game_ProcessMainFrame @ 0x5263f0 (one current_tick++ @ 0x24c1968)]
# 0.016 s — the engine tick quantum, single-sourced natively as
# world::TickAccumulator::kTickDt (S14); kept here for cadence CONSUMERS
# (avatar preview's menu clock). The accumulator arithmetic itself is native.
const TICK_DT := 1.0 / 62.5

var _sim: NovaSimulation
var _present                          # MissionPresentPass: placed nodes on every role or tooling/test preview
var _wire_present                     # WirePresentPass: un-placed network entities or SP attachment children
var _fire_present                     # FirePresentPass: non-local fire sound + muzzle + tracers; else null
var _fire_listener := Callable()      # -> Vector3 camera listener, stamped into the sim per frame (world/fire_sound.h)
var _destruction_present              # DestructionPresentPass: husk swap + debris + wreck effects (every viewing peer); null without fire_audio
var _throwable_present                # ThrowablePresentPass: flying/placed throwable models
var _index
var _playing := false
var _orig_transforms: Dictionary = {} # node -> Transform3D captured at setup, for restore-on-stop
var _perf_tick_us: int = 0
var _perf_sim_us: int = 0
var _perf_present_us: int = 0
var _perf_effects_us: int = 0
var _perf_did_tick := false
# The shared F3 frame-stats board (null outside the game shell). While its
# Stats tab captures, the sim/net legs and each present pass land their spans
# on it; otherwise all extra clock reads are skipped.
var _frame_stats: FrameStatsBoard = null
var _runtime_probe_enabled := false
var _has_trace_stats_sampling := false
var _ticks_last_frame := 0           # logic ticks run by the last tick_realtime() call (catch-up signal)
var _presentation_time_ms := -1      # shared render/PANM DWORD; negative = direct-sim fallback
# Stable mission identity for shell-neutral diagnostics such as the F3 overlay.
var _mission_file := ""
var _mission_name := ""
var _setup_error := OK

# Value-only attachment poses for the current authoritative tick. Production
# lookups stay in NovaSimulation's generation-bound native index; these boxed
# maps remain only as a compatibility path for snapshot-source test seams.
var _has_native_present_effect_pose_lookup := false
var _effect_pose_snapshot_tick := -1
var _effect_pose_snapshot_ready := false
var _effect_poses_by_bms_id: Dictionary = {}
var _effect_poses_by_origin: Dictionary = {}
var _effect_poses_by_wire_handle: Dictionary = {}
var _effect_poses_by_ssn: Dictionary = {}


## Create + promote the mission, build the shared index over the placed nodes (`container`), and wire
## the present pass. options: { loco_scale, present_options, and for a net
## session the typed request under "host_session" (HostSessionConfig) or "join_target"
## (JoinTarget) }. Returns the AI entity count, or 0 on load failure (the orphan sim is
## freed). Inspect get_setup_error() to distinguish a valid empty mission from a setup
## failure. The sim is held off-tree by this driver.
func setup(mission, container: Node, options: Dictionary = {}) -> int:
	_clear_present_effect_poses()
	_setup_error = OK
	# A remote join may already own the live socket + NP session while it waits
	# for S2C 0x7B to identify the mission. Keep that exact connection across the
	# wire-header world construction instead of reconnecting after discovery. Standalone host/SP and
	# isolated test/tooling callers do not provide a simulation and retain the fresh-instance path.
	_sim = options.get("simulation", null)
	if _sim == null:
		_sim = NovaSimulation.new()
	_has_trace_stats_sampling = _sim != null
	_sync_runtime_profiling()
	var mission_path := String(options.get(
			"debug_mission_file", options.get("mission_file", "")))
	_mission_file = mission_path.replace("\\", "/").get_file()
	_mission_name = String(options.get(
			"debug_mission_name", options.get("mission_name", "")))
	if _mission_name.is_empty() and mission != null \
			and mission.has_method("get_mission_name"):
		_mission_name = String(mission.get_mission_name()).strip_edges()
	if _mission_name.is_empty() and not _mission_file.is_empty():
		_mission_name = _mission_file.get_file().get_basename()

	_has_native_present_effect_pose_lookup = _sim != null
	if options.has("loco_scale"):
		_sim.set_loco_scale(int(options["loco_scale"]))
	# P7 / ADR 0011: every authoritative live mission is an in-process listen server, stood up BEFORE
	# load; the host player auto-spawns at bring-up (faithful §5.0 mode-3). MainGame/GameWorld is the
	# sole live host (ADR 0025); F5/F6 launch that standalone path. Isolated tests/tooling previews may
	# instantiate this same seam, but ONED does not. A co-op LAN host additionally binds a real UDP
	# socket; a joiner is the non-authority client.
	var playable := bool(options.get("playable", false))
	var join_target: JoinTarget = options.get("join_target")
	var host_session: HostSessionConfig = options.get("host_session")
	var is_joiner := join_target != null or _sim.is_joiner()
	if is_joiner:
		# Co-op LAN JOINER (a non-authority client): dial the host and run the witnessed
		# in-match JOIN. The local player L is spawned on the name-match (inside the sim's
		# joiner poll), NOT here. The player_name rides game ClientAuth.NA. [net-re §5.38b]
		# A preconnected simulation already completed this socket leg while the loading
		# screen was up; do not replace its connection, restart its handshake, or
		# reload charattr after ordered S2C 0x41 mutations have already landed.
		var needs_join_connection := not _sim.is_joiner()
		if needs_join_connection and options.get("resource_root") != null:
			_sim.load_charattr_challenge(options["resource_root"])
		if needs_join_connection and options.has("join_character_profile"):
			_sim.set_join_character_profile(options["join_character_profile"])
		if needs_join_connection and not join_target.integrity_profile.is_empty() and not \
				_sim.set_join_integrity_profile(join_target.integrity_profile):
			push_warning("MissionRuntime: unknown join integrity profile '%s'." % \
					join_target.integrity_profile)
			_setup_error = ERR_INVALID_PARAMETER
			_sim.free()
			_sim = null
			_has_trace_stats_sampling = false
			_has_native_present_effect_pose_lookup = false
			return 0
		if needs_join_connection and not _sim.enable_join(
				join_target.host_ip, join_target.port, join_target.player_name):
			push_warning("MissionRuntime: could not dial co-op host %s:%d — joiner disabled." % [
				join_target.host_ip, join_target.port])
	elif host_session != null:
		# Co-op LAN HOST: encode the typed session request at the FFI boundary (ADR 0017)
		# and stamp the mission-derived identity the session advertises on top. bind_port
		# defaults to the witnessed retail LAN host port (HostSessionConfig.DEFAULT_LAN_PORT);
		# serve-and-play vs DEDICATED and the lobby player cap ride to_session_options()
		# (net-re §5.2b, host_session_pump step 5).
		var session_options := host_session.to_session_options()
		if host_session.game_type_auto:
			var mission_mode := 0
			var mission_data := mission as NovaMissionData
			if mission_data != null:
				mission_mode = int(mission_data.get_game_mode())
			session_options["gametype"] = HostSessionConfig.game_type_for_mission_mode(mission_mode)
		var mission_name := String(options.get("mission_name", "")).strip_edges()
		if mission_name.is_empty() and mission != null and mission.has_method("get_mission_name"):
			mission_name = String(mission.get_mission_name()).strip_edges()
		var mission_file := String(options.get("mission_file", ""))
		if mission_name.is_empty() and not mission_file.is_empty():
			mission_name = mission_file.get_basename()
		if not mission_file.is_empty():
			session_options["mission_file"] = mission_file
		if options.has("spawn_names"):
			session_options["spawn_names"] = options["spawn_names"]
		if not mission_name.is_empty():
			session_options["mission_name"] = mission_name
			if Array(session_options.get("spawn_names", [])).is_empty():
				session_options["spawn_names"] = [mission_name]
		_sim.configure_host_session(session_options)
		# Retail builds the active game-type score table, then overlays the loose
		# VERSION 40 score.ini before answering C2S 0x2D with S2C 0x58. This
		# caller is explicitly loose-first even in a packed runtime: retail opens
		# score.ini from the game directory rather than resolving it from a PFF.
		var session_resource_root: NovaResourceRoot = options.get("resource_root")
		if session_resource_root != null:
			var score_ini_bytes := session_resource_root.read_file(
					"score.ini", NovaResourceRoot.LOOKUP_FORCE_LOOSE_FIRST)
			if not score_ini_bytes.is_empty() and not _sim.set_score_config_data(score_ini_bytes):
				push_warning("MissionRuntime: rejected score.ini; session status uses zero score values.")
		if not _sim.enable_host_listen(host_session.bind_port):
			# A requested LAN host that cannot own its UDP endpoint is not a host.
			# Never degrade into the visually-identical socketless SP/listen path:
			# the caller must surface the bind failure and keep the menu active.
			_setup_error = ERR_CANT_CREATE
			_sim.free()
			_sim = null
			_has_trace_stats_sampling = false
			_has_native_present_effect_pose_lookup = false
			return 0
	else:
		# Standalone SP (or an isolated tooling/test preview): the in-process listen server. ONED live
		# play reaches this branch only through GameWorld. The host player auto-spawns at bring-up.
		_sim.enable_listen_server(true)
	# S9 (ADR 0028): the ordered mission boot. The sequence, its gates, and the
	# file-resolution policy (mission-text fallback, .aip profile speeds, the
	# adm default) live in engine/runtime/mission runtime_boot; boot_mission
	# supplies the step bodies over the sim's feeds. Role bring-up ran above;
	# presentation composition follows below. Seat specs still ride the
	# GDScript extractor until S4b, so the built rows pass through as an
	# argument.
	var seat_specs: Array = []
	if options.get("resource_root") != null and options.get("item_db") != null:
		seat_specs = _build_item_seat_specs(mission, options["resource_root"], options["item_db"])
	var boot_err := int(_sim.boot_mission(
			mission,
			options.get("resource_root"),
			options.get("item_db"),
			options.get("placer"),
			options.get("terrain"),
			seat_specs,
			options.get("terrain_til", PackedByteArray()),
			String(options.get("wac_basename", "")),
			String(options.get("infantry_adm", "")),
			_mission_file.get_file().get_basename(),
			playable or bool(options.get("player", false))))
	if boot_err != OK:
		_setup_error = ERR_CANT_OPEN
		_sim.free()  # NovaSimulation is a Node (not RefCounted); free the orphan on load failure
		_sim = null
		_has_trace_stats_sampling = false
		_has_native_present_effect_pose_lookup = false
		return 0
	# The shared render/PANM presentation DWORD — re-stamped after the boot
	# because the load reset cleared it (an order-free scalar, not a boot step).
	if _presentation_time_ms >= 0:
		_sim.set_panm_time_ms(_presentation_time_ms)
	# The SIM is held off-tree (never add_child'd): only this driver advances it, and an off-tree
	# node never self-ticks via _process; it is freed explicitly in _exit_tree (mirrors the old
	# MissionSimDriver). This MissionRuntime node itself IS in the tree — its host adds it and
	# drives tick_realtime() explicitly (ADR 0025: the game shell is the only live host).
	_index = MissionEntityRegistry.new()
	_index.build(container, mission)
	# The registry present drives whichever authored mission nodes actually exist. A
	# production joiner owns only the 616-byte wire header, so its index is empty: the
	# native sim separately materializes streamed pools 1-3 at exact packed handles for
	# world-side consumers, while the wire pass below renders their decoded live state.
	# Complete-BMS/debug joins still retain authored nodes and the ordinary defer identity.
	_present = MissionPresentPass.new()
	_present.setup(_sim, _index, options.get("present_options", {}))
	# Co-op renders decoded remote rows WIRE-DIRECT. On a host that principally covers
	# dynamically admitted players; on a header-only joiner it covers every remote row,
	# because none has an authored placed node. Complete-BMS/debug roles still defer any
	# row resolved by _index so MissionPresentPass and WirePresentPass never double-render.
	var full_wire_present := is_joiner or _sim.is_host_listening()
	var sp_attachment_present := (
			not full_wire_present and options.get("placer") != null)
	if full_wire_present or sp_attachment_present:
		_wire_present = WirePresentPass.new()
		# A retail network join has no local BMS identity table. Pools 1-3 are
		# materialized separately into the native World at their exact wire handles,
		# but presentation remains wire-direct because there are no authored nodes to
		# drive. Passing the empty index here would incorrectly hide valid decoded
		# buildings/items. Complete/debug missions retain placed-node defer.
		var wire_defer_index = _index
		var mission_data := mission as NovaMissionData
		if mission_data != null and mission_data.is_wire_header_only():
			wire_defer_index = null
		_wire_present.setup(_sim, options.get("placer"), container, options.get("env_node"),
			wire_defer_index, {
				"synthetic_origin_only": sp_attachment_present,
			})
		simulation_restarted.connect(
				Callable(_wire_present, 'reset_runtime_state'))
	# The viewing client's fire-presentation pass: AI/remote fire sound + muzzle
	# effect + tracer streaks off the sim's fired/tracer drains. A joiner re-runs
	# decoded S2C tag-2 rounds through the same visual RoundSim, so it must drain
	# this queue too. FirePresentPass filters the locally predicted round by
	# is_local_player; the first-person action slot remains its sole presenter.
	# [orig: remote tag-2 receive -> RoundData_SpawnRound; net-re §5.60]
	if options.has("fire_audio"):
		_fire_present = FirePresentPass.new()
		# The muzzle anchor for retail's adm-arm fire effect. Bound to the WIRE pass
		# (built just above) because that pass owns the per-handle held-weapon node the
		# effect spawns at; an absent wire pass simply leaves the Callable invalid and
		# the fire pass keeps the wire position, which is the pre-existing behaviour.
		_fire_present.setup(_sim, container,
			options.get("fire_audio", Callable()),
			options.get("fire_fx", Callable()),
			options.get("fire_listener", Callable()),
			Callable(_wire_present, "muzzle_world_for") if _wire_present != null
					else Callable())
		# The sim's fire-sound distance gate reads the camera listener at fire
		# time on the logic clock (world/fire_sound.h) — stamped per frame in
		# tick_realtime. A host with no fire presentation (dedicated) never
		# stamps, which is the witnessed peer gate [orig: @ 0x528e57].
		_fire_listener = options.get("fire_listener", Callable())
	# The destruction-presentation pass: husk model swaps, death-piece debris,
	# wreck fire/smoke, destruction sounds — off the sim's destruction drain
	# (world/destruction.h; world-wac-ai-re §24). Shares the fire pass's
	# audio/fx providers. Joiners run it too: their world raises the same
	# events from the S2C 0x13-driven death chain and the client-side
	# explosion/piece drains (retail's client runs the identical presentation
	# from its own pools).
	# [orig: NapiNPClientMsg_EntityDeath @0x42EB50 -> deathCallback(entity,4,0);
	#  Entity_UpdateAllEntities @0x4c2100 drains unconditionally on every peer]
	if options.has("fire_audio"):
		_destruction_present = DestructionPresentPass.new()
		_destruction_present.setup(_sim, container, _index, options.get("placer"),
			options.get("item_db"), options.get("game_world"),
			options.get("fire_audio", Callable()),
			options.get("fire_fx", Callable()), options.get("env_node"),
			_wire_present)
		simulation_restarted.connect(
				Callable(_destruction_present, 'reset_runtime_state'))
	# The throwable-presentation pass: item models for flying grenades/satchels
	# and placed devices, reconciled from the sim's visual snapshot. Joiners need
	# the flying-round half because decoded S2C tag-2 descriptors run the visual
	# throwable motor locally. Placed-device replication remains the separate
	# unported 0x59/0x12 seam; enabling this read-only pass does not invent it.
	# (world-wac-ai-re §27; the sim stays render-free).
	_throwable_present = ThrowablePresentPass.new()
	_throwable_present.setup(_sim, container, options.get("placer"),
		options.get("item_db"), options.get("env_node"),
		options.get("fire_fx", Callable()), options.get("game_world"))
	simulation_restarted.connect(
		Callable(_throwable_present, 'reset_runtime_state'))
	# Capture the authored node transforms now (pre-tick) so Stop restores them whether the host
	# played or only stepped. Cheap; the game never Stops but holding the map costs nothing.
	_capture_transforms()
	return _sim.get_entity_count()



func get_setup_error() -> int:
	return _setup_error


# World position of the entity addressed by a runtime SSN (WAC/BMS addressing),
# or null when no live registry entity carries that net id.
# [orig: WacScript_SpawnSoundAtEntity @ 0x4f23a0].
func entity_position_for_ssn(ssn: int) -> Variant:
	if _sim == null or ssn <= 0:
		return null
	var state: PackedVector3Array = _sim.get_entity_effect_state_for_ssn(ssn)
	if state.size() != NovaSimulation.EFFECT_STATE_COUNT:
		return null
	return state[NovaSimulation.EFFECT_STATE_POSITION]


# Full attached-effect transform for fx2ssn. NovaSimulation owns the LIVE
# registry lookup and frame data; the host applies the single canonical basis
# conversion shared with the mission present pass.
func entity_effect_transform_for_ssn(ssn: int) -> Variant:
	if _sim == null or ssn <= 0:
		return null
	# Once a logic tick has completed, follow the exact client-view pose that the
	# render pass will present for that tick. Before the first tick there is no
	# such snapshot, so retain the authoritative registry lookup as the seed.
	if has_current_present_effect_snapshot():
		if _has_native_present_effect_pose_lookup:
			return _effect_transform_from_state(
					_sim.get_present_effect_state_for_ssn(ssn))
		_ensure_present_effect_poses()
		return _effect_poses_by_ssn.get(ssn)
	var state: PackedVector3Array = _sim.get_entity_effect_state_for_ssn(ssn)
	return _effect_transform_from_state(state)


## True after at least one authoritative logic tick has produced a client-view
## snapshot. Hosts can use this to distinguish "not presented yet" (fall back to
## an authored Node seed) from "not present in the current tick" (detach).
func has_current_present_effect_snapshot() -> bool:
	return _sim != null and _effect_pose_snapshot_tick >= 0


## Resolve one presented entity by its stable value identity. Placed nodes use
## bms_id first and (kind,index) as the zero-id fallback; wire-spawned nodes use
## their wire handle. Returns null when the entity is absent this tick.
func presented_entity_effect_transform(entity_ref: Dictionary) -> Variant:
	if not has_current_present_effect_snapshot():
		return null
	var has_wire_handle := entity_ref.has("wire_handle")
	var wire_handle := int(entity_ref.get("wire_handle", -1))
	if _has_native_present_effect_pose_lookup:
		var state := PackedVector3Array()
		if has_wire_handle and wire_handle >= 0 and wire_handle <= 0xffff:
			state = _sim.get_present_effect_state_for_wire_handle(wire_handle)
		else:
			var native_bms_id := int(entity_ref.get("bms_id", 0))
			if native_bms_id > 0:
				state = _sim.get_present_effect_state_for_bms_id(native_bms_id)
			else:
				var native_kind := int(entity_ref.get(
						"kind", entity_ref.get("origin_kind", -1)))
				var native_index := int(entity_ref.get("index", -1))
				if native_kind >= 0 and native_index >= 0:
					state = _sim.get_present_effect_state_for_origin(
							native_kind, native_index)
		return _effect_transform_from_state(state)

	# Compatibility path for a snapshot-source test seam without the compact API.
	_ensure_present_effect_poses()
	if has_wire_handle and wire_handle >= 0 and wire_handle <= 0xffff:
		return _effect_poses_by_wire_handle.get(wire_handle)
	var bms_id := int(entity_ref.get("bms_id", 0))
	if bms_id > 0:
		return _effect_poses_by_bms_id.get(bms_id)
	var kind := int(entity_ref.get("kind", entity_ref.get("origin_kind", -1)))
	var index := int(entity_ref.get("index", -1))
	if kind >= 0 and index >= 0:
		return _effect_poses_by_origin.get(_effect_origin_key(kind, index))
	return null


func _effect_transform_from_state(state: PackedVector3Array) -> Variant:
	if state.size() != NovaSimulation.EFFECT_STATE_COUNT:
		return null
	return Transform3D(
			MissionObjectPlacer.bms_to_godot_basis(
					state[NovaSimulation.EFFECT_STATE_ROTATION_DEG]),
			state[NovaSimulation.EFFECT_STATE_POSITION])


func get_mission_file() -> String:
	return _mission_file


func get_mission_name() -> String:
	return _mission_name


func _effect_origin_key(kind: int, index: int) -> String:
	return "%d:%d" % [kind, index]


func _clear_present_effect_poses() -> void:
	_effect_pose_snapshot_tick = -1
	_effect_pose_snapshot_ready = false
	_effect_poses_by_bms_id.clear()
	_effect_poses_by_origin.clear()
	_effect_poses_by_wire_handle.clear()
	_effect_poses_by_ssn.clear()


func _begin_present_effect_tick(logic_tick: int) -> void:
	_effect_pose_snapshot_tick = logic_tick
	_effect_pose_snapshot_ready = false
	_effect_poses_by_bms_id.clear()
	_effect_poses_by_origin.clear()
	_effect_poses_by_wire_handle.clear()
	_effect_poses_by_ssn.clear()


func _ensure_present_effect_poses() -> void:
	if _effect_pose_snapshot_ready or _sim == null or _effect_pose_snapshot_tick < 0:
		return
	_effect_pose_snapshot_ready = true
	var stride := int(_sim.get_present_stride())
	if stride <= 0:
		return
	var snapshot: PackedFloat32Array = _sim.get_present_snapshot()
	var count: int = snapshot.size() / stride
	for i in range(count):
		var base := i * stride
		var transform := Transform3D(
				MissionObjectPlacer.bms_to_godot_basis(Vector3(
					snapshot[base + NovaSimulation.PF_PITCH_DEG],
					snapshot[base + NovaSimulation.PF_YAW_DEG],
					snapshot[base + NovaSimulation.PF_ROLL_DEG])),
				Vector3(
					snapshot[base + NovaSimulation.PF_POS_X],
					snapshot[base + NovaSimulation.PF_POS_Y],
					snapshot[base + NovaSimulation.PF_POS_Z]))
		var wire_handle := int(snapshot[base + NovaSimulation.PF_WIRE_HANDLE])
		var type_id := int(snapshot[base + NovaSimulation.PF_TYPE_ID])
		if type_id != 0 and wire_handle >= 0 and wire_handle <= 0xffff:
			_effect_poses_by_wire_handle[wire_handle] = transform
		var bms_id := int(snapshot[base + NovaSimulation.PF_BMS_ID])
		if bms_id > 0:
			_effect_poses_by_bms_id[bms_id] = transform
		var kind := int(snapshot[base + NovaSimulation.PF_KIND])
		var index := int(snapshot[base + NovaSimulation.PF_INDEX])
		if kind >= 0 and index >= 0:
			_effect_poses_by_origin[_effect_origin_key(kind, index)] = transform
		var ssn := int(snapshot[base + NovaSimulation.PF_NET_ID])
		if ssn > 0:
			_effect_poses_by_ssn[ssn] = transform


# --- the local player (Phase 2; ADR 0012). W4-1 removed the pass-through
# delegates: consumers reach the sim natively via get_sim(). What remains here
# either composes (has_player), decodes (aim overlay, ADR 0017), or feeds
# GameWorld's own _music_var_pump. ---
func has_player() -> bool:
	return _sim != null and _sim.has_local_player()

# Decoded at the NovaSimulation transport edge (ADR 0017); null when absent/invalid.
func local_player_aim_overlay() -> PlayerAimOverlay:
	return PlayerAimOverlay.from_sim_dict(_sim.get_local_player_aim_overlay()) if _sim != null else null

func local_player_health() -> int:
	return int(_sim.get_local_player_health()) if _sim != null else 0

func local_player_max_health() -> int:
	return int(_sim.get_local_player_max_health()) if _sim != null else 100

func local_player_team() -> int:
	return int(_sim.get_local_player_team()) if _sim != null else 0


## The host hands the shared FrameStatsBoard here (game shell -> GameWorld ->
## each runtime it creates).
func set_frame_stats_board(board: FrameStatsBoard) -> void:
	if board == _frame_stats:
		return
	if _frame_stats != null:
		var old_capture_changed := Callable(self, "_on_frame_stats_capture_changed")
		if _frame_stats.capture_changed.is_connected(old_capture_changed):
			_frame_stats.capture_changed.disconnect(old_capture_changed)
	_frame_stats = board
	if _frame_stats != null:
		var capture_changed := Callable(self, "_on_frame_stats_capture_changed")
		if not _frame_stats.capture_changed.is_connected(capture_changed):
			_frame_stats.capture_changed.connect(capture_changed)
	_sync_runtime_profiling()


## Manual probe ownership is independent of F3 capture; the native timer stays
## enabled while either consumer is active.
func set_runtime_profiling_enabled(enabled: bool) -> void:
	_runtime_probe_enabled = enabled
	_sync_runtime_profiling()


func _on_frame_stats_capture_changed(_active: bool) -> void:
	_sync_runtime_profiling()


func _sync_runtime_profiling() -> void:
	if _sim == null:
		return
	var stats_active := _frame_stats != null \
			and _frame_stats.is_capture_active()
	_sim.set_runtime_profiling_enabled(
			_runtime_probe_enabled or stats_active)


# Mission-presentation counters (probe/diagnostic seam).
func get_mission_present_stats() -> MissionPresentStats:
	return _present.get_stats_record() \
			if _present != null else MissionPresentStats.new()


# Fire-presentation counters (probe/diagnostic seam; empty when the pass is absent).
func get_fire_present_stats() -> Dictionary:
	return _fire_present.get_stats() if _fire_present != null else {}


# Wire-presentation counters (spawned/unresolved/live; empty when the pass is absent).
func get_wire_present_stats() -> WirePresentStats:
	return _wire_present.get_stats_record() \
			if _wire_present != null else WirePresentStats.new()


## Cold wire rows the presentation budget still owes. Zero when there is no
## wire presenter; NetSessionDrive keys the join-admission edge on this drain.
func join_wire_present_pending() -> int:
	return _wire_present.pending_spawn_count() if _wire_present != null else 0


## Load-time warm hook: compile the fire-presentation pipelines (the tracer
## ribbon materials) behind the loading screen; see GameWorld's effect warm.
func warm_present_pipelines(at_position: Vector3) -> void:
	if _fire_present != null and _fire_present.has_method("warm_pipelines"):
		_fire_present.warm_pipelines(at_position)


func get_throwable_present_stats() -> RefCounted:
	# ThrowablePresentPass.Stats (typed counters, ADR 0017); null until the
	# presentation pass exists.
	return _throwable_present.get_stats() if _throwable_present != null else null


func get_destruction_present_stats() -> RefCounted:
	# DestructionPresentPass.Stats (typed counters, ADR 0017); null until a
	# host mission runs with the pass.
	return _destruction_present.get_stats() if _destruction_present != null else null

func get_sim() -> NovaSimulation:
	return _sim


func set_presentation_time_ms(value_ms: int) -> void:
	_presentation_time_ms = -1 if value_ms < 0 else value_ms & 0xffffffff
	if _sim != null:
		_sim.set_panm_time_ms(_presentation_time_ms)


## The placed-node registry (bms_id/kind/group -> live node). The render-occlusion
## frame resolves building masks and entity render gates through it.
func get_registry():
	return _index


## Register a render-side consumer for models materialized from the replicated
## wire stream. The pass replays already-live nodes when the callback is set.
func set_wire_node_spawned_callback(callback: Callable) -> void:
	if _wire_present != null:
		_wire_present.set_node_spawned_callback(callback)


## The active wire-direct presenter, exposed for lifecycle integrations and
## diagnostics. Null when this mission has no replicated/synthetic rows.
func get_wire_presenter() -> RefCounted:
	return _wire_present


func entity_count() -> int:
	return _sim.get_entity_count() if _sim != null else 0


func _build_item_seat_specs(mission, resource_root, item_db) -> Array:
	return ItemSeatSpecs.build_item_seat_specs(mission, resource_root, item_db)


func _model_name_for_graphic(graphic: String) -> String:
	return ItemSeatSpecs.model_name_for_graphic(graphic)


func _seat_specs_from_model(data: NovaObjectData) -> Array:
	return ItemSeatSpecs.seat_specs_from_model(data)


func _seat_local_from_user_point_position(pos: Vector3) -> Vector3:
	return ItemSeatSpecs.seat_local_from_user_point_position(pos)


func _seat_yaw_offset_from_user_point_rotation(direction: Vector3) -> int:
	return ItemSeatSpecs.seat_yaw_offset_from_user_point_rotation(direction)


func _seat_type_for_user_point(name: String) -> int:
	return ItemSeatSpecs.seat_type_for_user_point(name)


func _seat_pose_index_for_user_point(name: String) -> int:
	return ItemSeatSpecs.seat_pose_index_for_user_point(name)


func is_playing() -> bool:
	return _playing


func _present_entity_rows(stats_on := false) -> void:
	if _sim == null or (_present == null and _wire_present == null):
		return
	var stride := int(_sim.get_present_stride())
	if stride <= 0:
		return
	# Both entity presenters consume the same immutable row buffer. Fetching it
	# once also makes their topology revision refer to exactly the same layout.
	var snapshot: PackedFloat32Array = _sim.get_present_snapshot()
	if stats_on:
		# The native buffer build the fetch above just paid for.
		_frame_stats.add(FrameStatsBoard.PRESENT_SNAPSHOT,
				int(_sim.get_last_present_snapshot_us()))
	var layout_revision := int(_sim.get_present_layout_revision())
	if _present != null:
		var mission_start := Time.get_ticks_usec() if stats_on else 0
		if _present.has_method("present_snapshot"):
			_present.present_snapshot(snapshot, stride, layout_revision)
		else:
			_present.present()
		if stats_on:
			_frame_stats.add(FrameStatsBoard.PRESENT_MISSION,
					Time.get_ticks_usec() - mission_start)
	if _wire_present != null:
		var wire_start := Time.get_ticks_usec() if stats_on else 0
		if _wire_present.has_method("present_snapshot"):
			_wire_present.present_snapshot(snapshot, stride, layout_revision)
		else:
			_wire_present.present()
		if stats_on:
			_frame_stats.add(FrameStatsBoard.PRESENT_WIRE,
					Time.get_ticks_usec() - wire_start)


# One whole present frame: the entity rows plus the tick-driven passes, each
# pass timed onto the stats board while the Stats tab captures. Owns the
# bundled _perf_present_us the probe scripts read.
func _present_frame(stats_on: bool) -> void:
	var present_start := Time.get_ticks_usec()
	_present_entity_rows(stats_on)
	if _fire_present != null:
		var fire_start := Time.get_ticks_usec() if stats_on else 0
		_fire_present.present()
		if stats_on:
			_frame_stats.add(FrameStatsBoard.PRESENT_FIRE,
					Time.get_ticks_usec() - fire_start)
	if _destruction_present != null:
		var destruction_start := Time.get_ticks_usec() if stats_on else 0
		_destruction_present.present()
		if stats_on:
			_frame_stats.add(FrameStatsBoard.PRESENT_DESTRUCTION,
					Time.get_ticks_usec() - destruction_start)
	if _throwable_present != null:
		var throwable_start := Time.get_ticks_usec() if stats_on else 0
		_throwable_present.present()
		if stats_on:
			_frame_stats.add(FrameStatsBoard.PRESENT_THROWABLE,
					Time.get_ticks_usec() - throwable_start)
	_perf_present_us = Time.get_ticks_usec() - present_start


## Advance EXACTLY ONE cadence step and present. Returns true when a logic tick fired (and effects
## were drained). The deterministic single-tick primitive: standalone F3/MCP Step and isolated
## tests/tooling previews use this. GameWorld is the sole live real-time host and calls
## tick_realtime(), which accumulates wall-clock; ONED has no self-ticking mission host.
func tick() -> bool:
	if _sim == null:
		_perf_tick_us = 0
		_perf_sim_us = 0
		_perf_present_us = 0
		_perf_effects_us = 0
		_perf_did_tick = false
		_ticks_last_frame = 0
		return false
	var stats_on := _frame_stats != null and _frame_stats.is_capture_active()
	var tick_start := Time.get_ticks_usec()
	_stamp_sound_listener()
	var did_tick := _advance_one_tick_no_present()
	_perf_present_us = 0
	if did_tick:
		if stats_on:
			_frame_stats.add(FrameStatsBoard.SIM_STEP, _perf_sim_us)
			_frame_stats.add(FrameStatsBoard.SIM_NET, int(_sim.get_last_net_tick_us()))
			_frame_stats.add(FrameStatsBoard.EFFECTS_DRAIN, _perf_effects_us)
			_frame_stats.add(FrameStatsBoard.SIM_TICKS, 1)
		_present_frame(stats_on)
	_perf_tick_us = Time.get_ticks_usec() - tick_start
	_perf_did_tick = did_tick
	_ticks_last_frame = 1 if did_tick else 0
	return did_tick


# Stamp the camera listener before a tick batch: fires this batch gate their
# propagation delay against the current camera, the retail frame order (the
# listener global updates before the entity/fire processing). Invalid or
# non-finite listeners leave the sim unstamped — a host with no fire
# presentation (dedicated) runs no sound leg, the witnessed peer gate.
# [orig: listener_pos @ 0x24D6630, the @ 0x528e57 gate; world/fire_sound.h]
func _stamp_sound_listener() -> void:
	if not _fire_listener.is_valid():
		return
	var listener_v: Variant = _fire_listener.call()
	if listener_v is Vector3 and (listener_v as Vector3).is_finite():
		_sim.set_sound_listener(listener_v)


# One logic tick + drain/emit effects, WITHOUT presenting. Shared by tick() (which presents once
# after) and tick_realtime() (which presents once after the whole catch-up batch). Updates the
# sim/effects perf counters. Returns true when a logic tick fired.
func _advance_one_tick_no_present() -> bool:
	var sim_start := Time.get_ticks_usec()
	var did_tick := _sim.step()  # one 62 Hz logic tick (the WAC VM self-gates inside)
	_perf_sim_us = Time.get_ticks_usec() - sim_start
	_perf_effects_us = 0
	if did_tick:
		_feed_projectile_trace_stats()
		var logic_tick := int(_sim.get_logic_tick())
		# Invalidate before delivering effects: any owned spawn seeded during
		# this tick and the fixed-tick particle advance both observe this exact
		# client-view pose, even inside a multi-tick catch-up batch.
		_begin_present_effect_tick(logic_tick)
		# Round-bound ammo move groups are particle-simulation state, even though
		# their item-model Nodes stay render-batched. Reconcile them before the
		# fixed_tick_completed consumer advances EffectWorld so birth, motion,
		# and release all happen on the exact owning round tick.
		if _throwable_present != null:
			_throwable_present.sync_fixed_tick_effects()
		var effects_start := Time.get_ticks_usec()
		var effects := _sim.drain_effects()
		_perf_effects_us = Time.get_ticks_usec() - effects_start
		if not effects.is_empty():
			effects_drained.emit(effects)
		fixed_tick_completed.emit(logic_tick)
	return did_tick


func _feed_projectile_trace_stats() -> void:
	if _frame_stats == null or not _frame_stats.is_capture_active() \
			or not _has_trace_stats_sampling:
		return
	# Three value-type native reads preserve each logic tick's snapshot without
	# allocating a Dictionary on the capture hot path. In a catch-up render
	# frame the board folds each tick into one complete-frame peak.
	var counts: Vector4i = _sim.get_last_projectile_trace_counts()
	if counts.x <= 0:
		return
	var times: Vector4i = _sim.get_last_projectile_trace_times_us()
	var faces: Vector2i = _sim.get_last_projectile_trace_faces()
	_frame_stats.add(FrameStatsBoard.TRACE_TERRAIN,
			times.x)
	_frame_stats.add(FrameStatsBoard.TRACE_STATIC,
			times.y)
	_frame_stats.add(FrameStatsBoard.TRACE_DYNAMIC,
			times.z)
	_frame_stats.add(FrameStatsBoard.TRACE_PERSON,
			times.w)
	_frame_stats.add(FrameStatsBoard.TRACE_CALLS, counts.x)
	_frame_stats.add(FrameStatsBoard.TRACE_STATIC_SURVIVORS,
			counts.y)
	_frame_stats.add(FrameStatsBoard.TRACE_DYNAMIC_SURVIVORS,
			counts.z)
	_frame_stats.add(FrameStatsBoard.TRACE_PERSON_SURVIVORS,
			counts.w)
	_frame_stats.add(FrameStatsBoard.TRACE_STATIC_FACES,
			faces.x)
	_frame_stats.add(FrameStatsBoard.TRACE_DYNAMIC_FACES,
			faces.y)


## Real-time host entry: bank `delta`, drain it in fixed TICK_DT quanta, run that many single logic
## ticks (clamped to the native kMaxCatchupTicks), and present ONCE after the batch. This is the faithful
## fixed-62.5 Hz accumulator — the sim runs at a constant rate while rendering stays decoupled at the
## host frame rate, with no inter-tick interpolation (present reads current sim state). A long frame
## runs several ticks; a short frame runs none but still presents current render-only state (camera,
## local UseGun suppression, and node ownership). Effects drain per tick (the original's per-tick
## emission). Returns the number of logic ticks run this call. [orig: Game_MainLoop @ 0x52b630]
func tick_realtime(delta: float) -> int:
	if _sim == null or not _playing:
		_ticks_last_frame = 0
		return 0
	var stats_on := _frame_stats != null and _frame_stats.is_capture_active()
	var tick_start := Time.get_ticks_usec()
	# The bank/clamp arithmetic is the native world::TickAccumulator (S14,
	# [orig: Game_MainLoop @ 0x52b630]); the loop + present-once-after-batch
	# orchestration stays here in the shell.
	_stamp_sound_listener()
	var n := int(_sim.bank_realtime(delta))
	if n <= 0:
		# Retail evaluates entity submission every render frame. Camera mode and
		# local attach/detach can change between fixed ticks, so the scene passes
		# must not wait for the next 62.5 Hz quantum. Fire and destruction
		# presentation remain tick-driven because no gameplay state advanced here.
		_perf_present_us = 0
		if _present != null or _wire_present != null:
			var present_start := Time.get_ticks_usec()
			_present_entity_rows(stats_on)
			_perf_present_us = Time.get_ticks_usec() - present_start
		_ticks_last_frame = 0
		_perf_tick_us = Time.get_ticks_usec() - tick_start
		_perf_did_tick = false
		return 0
	var sim_us := 0
	var effects_us := 0
	var net_us := 0
	for _i in range(n):
		_advance_one_tick_no_present()
		sim_us += _perf_sim_us
		effects_us += _perf_effects_us
		if stats_on:
			net_us += int(_sim.get_last_net_tick_us())
	_perf_sim_us = sim_us
	_perf_effects_us = effects_us
	if stats_on:
		_frame_stats.add(FrameStatsBoard.SIM_STEP, sim_us)
		_frame_stats.add(FrameStatsBoard.SIM_NET, net_us)
		_frame_stats.add(FrameStatsBoard.EFFECTS_DRAIN, effects_us)
		_frame_stats.add(FrameStatsBoard.SIM_TICKS, n)
	_present_frame(stats_on)
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
		"sim": _sim.get_runtime_perf_counters() if _sim != null else {},
	}


# --- Debug/tooling transport (Play / Step / Stop) -----------------------------

## True while a live net session owns this runtime, i.e. the Play/Step/Stop
## transport is locked out. The world tick is the
## ONLY pump for the session socket (`NovaSimulation::step` is the sole caller of
## host_pump/joiner_pump), so halting it stops the C2S uplink, the keepalive and
## the empty-send interval, and the peer reaps us at `cs_dir0.timeout_ms = 120000`
## [orig: CNapiNetwork_Init @0x4ca4a0]. Retail multiplayer has no pause at all —
## its in-game menu overlays a running match — and on a listen host a stopped
## world would do this to every joiner at once. The ESC pause honours the same
## rule in the shell; this is the single home so the F3 transport, the perf probe
## and any future caller cannot bypass it.
func is_transport_locked() -> bool:
	# Delegates to the native predicate (S14) — one home for the rule.
	if _sim == null:
		return false
	return bool(_sim.is_transport_locked())


func play() -> void:
	_playing = true
	if _sim != null:
		# Discard wall-clock banked while paused / loading, so Play doesn't burst-catch-up.
		_sim.reset_tick_bank()


func pause() -> void:
	if is_transport_locked():
		return
	_playing = false
	if _sim != null:
		_sim.reset_tick_bank()


## One manual debug/tooling tick: one logic tick + present, outside the real-time loop.
## The standalone game's F3/MCP Step and isolated tests/tooling previews share this primitive.
func step_once() -> void:
	# Stepping pauses real-time cadence (and any opt-in tooling self-tick), which starves
	# the socket between steps just as pause() does. See is_transport_locked().
	if is_transport_locked():
		return
	_playing = false
	if _sim != null:
		_sim.reset_tick_bank()  # manual stepping is fully decoupled from wall-clock
	tick()


## Stop: rewind the world to the play-start baseline AND restore the authored node transforms, so the
## placed world is left exactly as it was. Safe to call when never played.
func stop() -> void:
	# Worse than pause on a live session: the restart below rewinds the world under
	# peers that are still streaming against it. See is_transport_locked().
	if is_transport_locked():
		return
	_playing = false
	if _sim != null:
		_sim.reset_tick_bank()  # a Stop -> Play cycle must not replay banked time
		_sim.restart()  # World::restore baseline (registry/vars/env/clock) + AI re-seed
	_clear_present_effect_poses()
	_restore_transforms()
	simulation_restarted.emit()


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
	if _frame_stats != null:
		var capture_changed := Callable(self, "_on_frame_stats_capture_changed")
		if _frame_stats.capture_changed.is_connected(capture_changed):
			_frame_stats.capture_changed.disconnect(capture_changed)
	_runtime_probe_enabled = false
	_has_trace_stats_sampling = false
	if _sim != null:
		_sim.set_runtime_profiling_enabled(false)
	_clear_present_effect_poses()
	_has_native_present_effect_pose_lookup = false
	if _fire_present != null:
		_fire_present.teardown()  # frees the tracer mesh instance under the container
		_fire_present = null
	if _wire_present != null:
		var reset_wire := Callable(_wire_present, 'reset_runtime_state')
		if simulation_restarted.is_connected(reset_wire):
			simulation_restarted.disconnect(reset_wire)
		_wire_present.teardown()
		_wire_present = null
	if _destruction_present != null:
		var reset_destruction := Callable(_destruction_present, 'reset_runtime_state')
		if simulation_restarted.is_connected(reset_destruction):
			simulation_restarted.disconnect(reset_destruction)
		_destruction_present.teardown()  # frees husk models + effect anchors
		_destruction_present = null
	if _throwable_present != null:
		var reset_throwable := Callable(_throwable_present, 'reset_runtime_state')
		if simulation_restarted.is_connected(reset_throwable):
			simulation_restarted.disconnect(reset_throwable)
		_throwable_present.teardown()
		_throwable_present = null
	if _sim != null and is_instance_valid(_sim):
		_sim.free()
		_sim = null
