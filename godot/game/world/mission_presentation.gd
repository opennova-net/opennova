class_name MissionPresentation
extends Node

# The Godot presentation owner for one native inmatch::Session. It owns the
# Simulation adapter, present passes, and entity index; cadence, lifecycle, and
# input retention remain native. GameWorld is its sole live owner.
#
# Per-tick order (single-sourced here, faithful to the original main loop's server-tick-then-render):
#   advance logic (sim) -> present entity state onto nodes -> drain + emit side effects.
# [orig: WacScript_AdvanceTick runs the logic systems; the client then renders the entities. Terrain/foliage/audio
#  are Godot render passes the caller composes around this.]
#
# Cadence: inmatch::Session banks wall clock and dispatches 0..N 62.5 Hz ticks;
# tick() asks that same session for one deterministic local/test step. The
# engine's WAC/BMS dividers remain inside their systems. Stop rewinds both the
# native world baseline and the authored node transforms captured at setup.

signal effects_drained(effects: Array)
## Emitted once after every authoritative 62.5 Hz logic step, after that
## step's side effects have been delivered. Presentation-only fixed-step
## systems (particles) subscribe here instead of integrating render delta.
signal fixed_tick_completed(logic_tick: int)
## Emitted after Stop rewinds simulation and authored transforms. GameWorld-owned
## presentation systems use this boundary to discard transient runtime state.
signal simulation_restarted()

# Fixed-timestep accumulator. The original decouples the simulation from rendering: the master
# loop accumulates real elapsed time and dispatches the logic update once per 16 ms (62.5 Hz),
# independently of the variable render rate — multiple ticks on a long frame, zero on a short one.
# [orig: Game_MainLoop @ 0x52b630 -> Game_ProcessMainFrame @ 0x5263f0 (one current_tick++ @ 0x24c1968)]
# The 0.016 s tick quantum is single-sourced natively as
# world::TickAccumulator::kTickDt (Simulation.tick_dt()); the accumulator
# arithmetic itself is native.

var _sim: Simulation
# THE entity presenter (ADR 0043 d9), the "Entities" child: its placed walk drives the
# authored nodes on every role (or a tooling/test preview); its wire walk, when set up,
# materializes un-placed network entities or SP attachment children; it owns the four
# tick-driven present passes (fire, destruction, throwable, scars) and its "Scars" child.
var _entities: EntityPresenter
# The pass inputs captured at setup for setup_passes: the mission container the
# passes graft into, the item database (husk/round graphics) and the resource
# root (scar strips).
var _container: Node = null
var _item_db: ItemDatabase = null
var _resource_root: ResourceRoot = null
var _index: EntityIndex
var _registry_placer: MissionObjectPlacer = null
var _orig_transforms: Dictionary = {} # node -> Transform3D captured at setup, for restore-on-stop
var _perf_tick_us: int = 0
var _perf_sim_us: int = 0
var _perf_present_us: int = 0
var _perf_effects_us: int = 0
var _perf_did_tick := false
# The shared F3 frame-stats board (null outside the game shell). While its
# Stats tab captures, the sim/net legs and each present pass land their spans
# on it; otherwise all extra clock reads are skipped.
var _frame_stats: FrameStats = null
var _runtime_probe_enabled := false
var _has_trace_stats_sampling := false
var _ticks_last_frame := 0           # logic ticks run by the last session frame
var _presentation_time_ms := -1      # shared render/PANM DWORD; negative = direct-sim fallback
# Stable mission identity for diagnostics (the dev tools, the MCP catalog).
var _mission_file := ""
var _mission_name := ""
var _setup_error := OK

# The current authoritative present tick (-1 = none yet). Attachment-pose
# lookups are Simulation's generation-bound native index — the one path;
# test sims implement the same compact API.
var _effect_pose_snapshot_tick := -1


## Create + promote the mission, build the shared index over the placed nodes (`container`), and wire
## the present pass. `options` is the typed MissionSetupOptions record (ADR
## 0017); a net session carries its request as options.host_session
## (HostSessionConfig) or options.join_target (JoinTarget). Returns the AI
## entity count, or 0 on load failure (the orphan sim is
## freed). Inspect get_setup_error() to distinguish a valid empty mission from a setup
## failure. The sim is a RefCounted this driver owns.
func setup(mission: MissionData, container: Node,
		options: MissionSetupOptions = null) -> int:
	if options == null:
		options = MissionSetupOptions.new()
	_clear_present_effect_poses()
	_setup_error = OK
	# A remote join may already own the live socket + NP session while it waits
	# for S2C 0x7B to identify the mission. Keep that exact connection across the
	# wire-header world construction instead of reconnecting after discovery. Standalone host/SP and
	# isolated test/tooling callers do not provide a simulation and retain the fresh-instance path.
	_sim = options.simulation
	if _sim == null:
		_sim = Simulation.new()
		# `--capture-pcap <path>` (LaunchFlags) records this process's datagrams.
		_sim.set_capture_pcap_path(LaunchFlags.capture_pcap())
	_sim.set_frame_stats(_frame_stats)
	_has_trace_stats_sampling = _sim != null
	_sync_runtime_profiling()
	_mission_file = options.mission_file.replace("\\", "/").get_file()
	_mission_name = options.mission_name
	if _mission_name.is_empty() and mission != null:
		_mission_name = String(mission.get_mission_name()).strip_edges()
	if _mission_name.is_empty() and not _mission_file.is_empty():
		_mission_name = _mission_file.get_file().get_basename()

	# The listen host stamps its own type-2 connection during role bring-up, so
	# its two per-side character selections must already be present here. The
	# same profile is what a joiner uploads through ClientAuth.
	# [orig: apply_session_settings_to_globals @0x551500;
	#  Server_PlayerAdd @0x51CBC0 -> packed id @0x51D0B1]
	if options.local_character_profile != null:
		_sim.set_local_character_profile(options.local_character_profile)
	# P7 / ADR 0011: every authoritative live mission is an in-process listen server, stood up BEFORE
	# load; the host player auto-spawns at bring-up (faithful §5.0 mode-3). MainGame/GameWorld is the
	# sole live runtime owner (ADR 0025). Isolated tests may instantiate this same
	# seam, but ONED does not. A co-op LAN host additionally binds a real UDP
	# socket; a joiner is the non-authority client.
	var playable := options.playable
	var join_target: JoinTarget = options.join_target
	var host_session: HostSessionConfig = options.host_session
	var is_joiner := join_target != null or _sim.is_joiner()
	if is_joiner:
		# Co-op LAN JOINER (a non-authority client): dial the host and run the witnessed
		# in-match JOIN. The local player L is spawned on the name-match (inside the sim's
		# joiner poll), NOT here. The player_name rides game ClientAuth.NA. [net-re §5.38b]
		# A preconnected simulation already completed this socket leg while the loading
		# screen was up; do not replace its connection, restart its handshake, or
		# reload charattr after ordered S2C 0x41 mutations have already landed.
		var needs_join_connection := not _sim.is_joiner()
		if needs_join_connection and options.resource_root != null:
			_sim.load_charattr_challenge(options.resource_root)
		if needs_join_connection and options.join_character_profile != null:
			_sim.set_join_character_profile(options.join_character_profile)
		if needs_join_connection and not join_target.integrity_profile.is_empty() and not \
				_sim.set_join_integrity_profile(join_target.integrity_profile):
			push_warning("MissionPresentation: unknown join integrity profile '%s'." % \
					join_target.integrity_profile)
			_setup_error = ERR_INVALID_PARAMETER
			_sim = null
			_has_trace_stats_sampling = false
			return 0
		if needs_join_connection and not _sim.enable_join(
				join_target.host_ip, join_target.port, join_target.player_name,
				join_target.join_role, join_target.spectator_password):
			push_warning("MissionPresentation: could not dial co-op host %s:%d — joiner disabled." % [
				join_target.host_ip, join_target.port])
		# The JOIN VERSIONCRCSTRING checksum reads the loose
		# expansion/<name>/version.txt under the install root (D-NET-166).
		if needs_join_connection and options.resource_root != null:
			_sim.set_join_expansion_version_root(options.resource_root.get_root_dir())
	elif host_session != null:
		# Co-op LAN HOST: project the typed session request to the sim's record and
		# stamp the mission-derived identity the session advertises on top. bind_port
		# defaults to the witnessed retail LAN host port (HostSessionConfig.DEFAULT_LAN_PORT);
		# serve-and-play vs DEDICATED and the lobby player cap ride to_session_options()
		# (net-re §5.2b, host_session_pump step 5).
		var session_options := host_session.to_session_options()
		if host_session.game_type_auto:
			var mission_mode := 0
			if mission != null:
				mission_mode = int(mission.get_game_mode())
			session_options.game_type = HostSessionConfig.game_type_for_mission_mode(mission_mode)
		var mission_name := options.mission_name.strip_edges()
		if mission_name.is_empty() and mission != null:
			mission_name = String(mission.get_mission_name()).strip_edges()
		if mission_name.is_empty() and not options.mission_file.is_empty():
			mission_name = options.mission_file.get_basename()
		if not options.mission_file.is_empty():
			session_options.mission_file = options.mission_file
		if not options.spawn_names.is_empty():
			session_options.spawn_names = options.spawn_names
		if not mission_name.is_empty():
			session_options.mission_name = mission_name
			if session_options.spawn_names.is_empty():
				session_options.spawn_names = PackedStringArray([mission_name])
		# The host's expansion version checksum (retail's g_expansion_checksum) is
		# CRC'd from the loose expansion/<name>/version.txt under the install root;
		# the join gate compares it against each joiner's VERSIONCRCSTRING while an
		# expansion is active (D-NET-166).
		if options.resource_root != null:
			session_options.game_root = options.resource_root.get_root_dir()
		_sim.configure_host_session(session_options)
		# Retail builds the active game-type score table, then overlays the loose
		# VERSION 40 score.ini before answering C2S 0x2D with S2C 0x58. This
		# caller is explicitly loose-first even in a packed runtime: retail opens
		# score.ini from the game directory rather than resolving it from a PFF.
		if options.resource_root != null:
			var score_ini_bytes := options.resource_root.read_file(
					"score.ini", ResourceRoot.LOOKUP_FORCE_LOOSE_FIRST)
			if not score_ini_bytes.is_empty() and not _sim.set_score_config_data(score_ini_bytes):
				push_warning("MissionPresentation: rejected score.ini; session status uses zero score values.")
		if not _sim.enable_host_listen(host_session.bind_port):
			# A requested LAN host that cannot own its UDP endpoint is not a host.
			# Never degrade into the visually-identical socketless SP/listen path:
			# the caller must surface the bind failure and keep the menu active.
			_setup_error = ERR_CANT_CREATE
			_sim = null
			_has_trace_stats_sampling = false
			return 0
	else:
		# Standalone SP (or an isolated tooling/test preview): the in-process listen server. ONED live
		# play reaches this branch only through GameWorld. The host player auto-spawns at bring-up.
		_sim.enable_listen_server(true)
	# S9 (ADR 0028): the ordered mission boot. The sequence, its gates, and the
	# file-resolution policy (mission-text fallback, .aip profile speeds, the
	# adm default) live in engine/runtime/mission runtime_boot; boot_mission
	# supplies the step bodies over the sim's feeds — including the S16 native
	# seat-spec extraction (the GDScript extractor and its Array pass-through
	# are gone). Role bring-up ran above; presentation composition follows.
	var boot_err := int(_sim.boot_mission(
			mission,
			options.resource_root,
			options.item_db,
			options.terrain,
			options.terrain_til,
			options.wac_basename,
			options.infantry_adm,
			_mission_file.get_file().get_basename(),
			playable))
	if boot_err != OK:
		_setup_error = ERR_CANT_OPEN
		_sim = null  # the RefCounted sim drops with its last reference
		_has_trace_stats_sampling = false
		return 0
	# The shared render/PANM presentation DWORD — re-stamped after the boot
	# because the load reset cleared it (an order-free scalar, not a boot step).
	if _presentation_time_ms >= 0:
		_sim.set_panm_time_ms(_presentation_time_ms)
	# The SIM is a RefCounted this driver owns: only this driver advances it; it
	# drops with the last reference in _exit_tree.
	# This MissionPresentation node itself IS in the tree — GameWorld adds it and
	# drives advance_session_frame() explicitly (ADR 0025: the game shell is the only live runtime owner).
	_index = EntityIndex.new()
	var registry_placer: MissionObjectPlacer = options.placer
	_registry_placer = registry_placer
	var placed_models: Array[ObjectModel] = []
	if registry_placer != null:
		placed_models = registry_placer.placed_models
	_index.build(placed_models,
			mission.get_area_triggers() if mission != null else [])
	# The registry present drives whichever authored mission nodes actually exist. A
	# production joiner owns only the 616-byte wire header, so its index is empty: the
	# native sim separately materializes streamed pools 1-3 at exact packed handles for
	# world-side consumers, while the wire walk below renders their decoded live state.
	# Complete-BMS/debug joins still retain authored nodes and the ordinary defer identity.
	# ONE presenter node runs both walks over the same snapshot (ADR 0043 d9); output
	# channels keep its native OUTPUT_ALL default. Visibility ownership is the two bits
	# on each ObjectModel: the placed walk writes the sim's intent
	# (set_present_visible), the occlusion frame its claim (set_occlusion_hidden), and
	# the node's visible flag is their product, so neither writer fights the other.
	_entities = EntityPresenter.new()
	_entities.name = "Entities"
	add_child(_entities)
	_entities.setup(_sim, _index, registry_placer)
	# Co-op renders decoded remote rows WIRE-DIRECT. On a host that principally covers
	# dynamically admitted players; on a header-only joiner it covers every remote row,
	# because none has an authored placed node. Complete-BMS/debug roles still defer any
	# row resolved by _index so the placed and wire walks never double-render.
	var full_wire_present := is_joiner or _sim.is_host_listening()
	var sp_attachment_present := (
			not full_wire_present and options.placer != null)
	if full_wire_present or sp_attachment_present:
		# A retail network join has no local BMS identity table at load: the
		# index is empty until the host's world stream settles, when GameWorld
		# places the streamed statics through the same placer and
		# rebind_placed_entities() fills this SAME index. Rows carrying a placed
		# identity defer with or without a resolvable node, so the wire walk
		# keeps only organics and runtime spawns on every role.
		_entities.setup_wire(_sim, options.placer, container, _index)
		_entities.set_synthetic_origin_only(sp_attachment_present)
	# The Stop -> Play boundary: the one connect resets the wire registry AND
	# the presenter's four passes (destruction, throwable, scars reset their
	# runtime state inside reset_wire_runtime_state).
	simulation_restarted.connect(_entities.reset_wire_runtime_state)
	# The four tick-driven present passes are EntityPresenter's own (ADR 0043
	# d9): the fire pass (AI/remote fire sound + muzzle effect + tracers off
	# the sim's fired/tracer drains; a joiner re-runs decoded S2C tag-2 rounds
	# through the same visual RoundSim, so it drains this queue too), the
	# destruction pass (husk swaps, death-piece debris, wreck fire/smoke,
	# destruction sounds off world/destruction.h; every viewing peer), the
	# throwable pass (item models for flying grenades/satchels and placed
	# devices; world-wac-ai-re §27) and the scar pass (world-wac-ai-re §24.9;
	# every viewing peer). Their typed collaborators — the mission audio, the
	# effect world, the light director, the environment node and the
	# owner-anchor registry — exist only after the later load stages, so the
	# load binds them through setup_passes; the pass inputs known now are
	# captured here.
	_container = container
	_item_db = options.item_db
	_resource_root = options.resource_root
	# ADR 0035: the native session owns lifecycle/cadence and invokes one
	# synchronous per-tick presentation sink. The camera remains a Godot device;
	# a dedicated host has none.
	# Capture the authored node transforms now (pre-tick) so Stop restores them whether the caller
	# played or only stepped. Cheap; the game never Stops but holding the map costs nothing.
	_capture_transforms()
	return _sim.get_entity_count()


func get_setup_error() -> int:
	return _setup_error


# World position of the entity addressed by a runtime SSN (WAC/BMS addressing),
# or null when no live registry entity carries that net id.
# [orig: WacScript_SpawnEffectAtSsnEntity @ 0x4f23a0].
func entity_position_for_ssn(ssn: int) -> Variant:
	if _sim == null or ssn <= 0:
		return null
	var state: PackedVector3Array = _sim.get_entity_effect_state_for_ssn(ssn)
	if state.size() != Simulation.EFFECT_STATE_COUNT:
		return null
	return state[Simulation.EFFECT_STATE_POSITION]


# Full attached-effect transform for fx2ssn. Simulation owns the LIVE
# registry lookup and frame data; presentation applies the single canonical basis
# conversion shared with the mission present pass.
func entity_effect_transform_for_ssn(ssn: int) -> Variant:
	if _sim == null or ssn <= 0:
		return null
	# Once a logic tick has completed, follow the exact client-view pose that the
	# render pass will present for that tick. Before the first tick there is no
	# such snapshot, so retain the authoritative registry lookup as the seed.
	if has_current_present_effect_snapshot():
		return _effect_transform_from_state(
				_sim.get_present_effect_state_for_ssn(ssn))
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
func presented_entity_effect_transform(entity_ref: EntityRef) -> Variant:
	if entity_ref == null or not has_current_present_effect_snapshot():
		return null
	var wire_handle := entity_ref.wire_handle
	var state := PackedVector3Array()
	if wire_handle >= 0 and wire_handle <= 0xffff:
		state = _sim.get_present_effect_state_for_wire_handle(wire_handle)
	else:
		var native_bms_id := entity_ref.bms_id
		if native_bms_id > 0:
			state = _sim.get_present_effect_state_for_bms_id(native_bms_id)
		else:
			var native_kind := entity_ref.kind if entity_ref.kind >= 0 \
					else entity_ref.origin_kind
			var native_index := entity_ref.index
			if native_kind >= 0 and native_index >= 0:
				state = _sim.get_present_effect_state_for_origin(
						native_kind, native_index)
	return _effect_transform_from_state(state)


func _effect_transform_from_state(state: PackedVector3Array) -> Variant:
	if state.size() != Simulation.EFFECT_STATE_COUNT:
		return null
	return Transform3D(
			MissionObjectPlacer.bms_to_godot_basis(
					state[Simulation.EFFECT_STATE_ROTATION_DEG]),
			state[Simulation.EFFECT_STATE_POSITION])


func get_mission_file() -> String:
	return _mission_file


func get_mission_name() -> String:
	return _mission_name


func _clear_present_effect_poses() -> void:
	_effect_pose_snapshot_tick = -1


func _begin_present_effect_tick(logic_tick: int) -> void:
	_effect_pose_snapshot_tick = logic_tick


# --- the local player (Phase 2; ADR 0012). W4-1 removed the pass-through
# delegates: consumers reach the sim natively via get_sim(). What remains here
# either composes (has_player), decodes (aim overlay, ADR 0017), or feeds
# GameWorld's own _music_var_pump. ---
func has_player() -> bool:
	return _sim != null and _sim.has_local_player()

# Decoded at the Simulation transport edge (ADR 0017); null when absent/invalid.
func local_player_aim_overlay() -> PlayerAimOverlay:
	return _sim.get_local_player_aim_overlay() if _sim != null else null

func local_player_team() -> int:
	return int(_sim.get_local_player_team()) if _sim != null else 0


## GameWorld hands the shared FrameStats here (game shell -> GameWorld ->
## each runtime it creates). The sim folds its native phase spans onto the
## same board inside advance_session_frame.
func set_frame_stats(board: FrameStats) -> void:
	if board == _frame_stats:
		return
	if _sim != null:
		_sim.set_frame_stats(board)
	if _frame_stats != null:
		var old_capture_changed := _on_frame_stats_capture_changed
		if _frame_stats.capture_changed.is_connected(old_capture_changed):
			_frame_stats.capture_changed.disconnect(old_capture_changed)
	_frame_stats = board
	if _frame_stats != null:
		var capture_changed := _on_frame_stats_capture_changed
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
	return _entities.get_stats_record() \
			if _entities != null else MissionPresentStats.new()


## Bind the present passes' typed collaborators (ADR 0043 d9), after setup():
## `audio` (nullable) gates the fire/destruction sound legs — a dedicated serve
## presents no sounds while the effect legs still run; `fx` (nullable) the
## effect legs; `lights` (nullable) the MF_Light muzzle glow + the death
## flash; `environment` (nullable) the scar pass's fog distance + combined
## terrain light; `anchors` (nullable) the owner-anchor registry the
## wreck/piece/move effect groups anchor through. The load calls this once
## the audio and effect stages have run; isolated tests pass what they have.
func setup_passes(audio: MissionAudio, fx: EffectWorld, lights: EffectLightDirector,
		environment: MissionEnvironment, anchors: ItemEffectDirector) -> void:
	if _entities == null:
		return
	var container: Node3D = _container as Node3D \
			if _container != null and is_instance_valid(_container) else null
	_entities.setup_passes(container, _item_db, _resource_root, audio, fx, lights,
			environment, anchors)


# Fire-presentation counters (FirePresentStats, typed per ADR 0017; an empty
# record before setup).
func get_fire_present_stats() -> FirePresentStats:
	return _entities.get_fire_present_stats() \
			if _entities != null else FirePresentStats.new()


# Destruction-presentation counters (DestructionPresentStats, typed per ADR 0017).
func get_destruction_present_stats() -> DestructionPresentStats:
	return _entities.get_destruction_present_stats() \
			if _entities != null else DestructionPresentStats.new()


# Wire-presentation counters (spawned/unresolved/live; empty before setup).
func get_wire_present_stats() -> WirePresentStats:
	return _entities.get_wire_stats_record() \
			if _entities != null else WirePresentStats.new()


## Cold wire rows the presentation budget still owes. Zero before setup;
## NetSessionDrive keys the join-admission edge on this drain.
func join_wire_present_pending() -> int:
	return _entities.pending_spawn_count() if _entities != null else 0


## Load-time warm hook: compile the fire-presentation pipelines (the tracer
## ribbon materials) behind the loading screen; see GameWorld's effect warm.
func warm_present_pipelines(at_position: Vector3) -> void:
	if _entities != null:
		_entities.warm_fire_pipelines(at_position)


# Throwable-presentation counters (ThrowablePresentStats, typed per ADR 0017;
# built per call).
func get_throwable_present_stats() -> ThrowablePresentStats:
	return _entities.get_throwable_present_stats() \
			if _entities != null else ThrowablePresentStats.new()


# Scar-presentation counters (ScarPresentStats, typed per ADR 0017).
func get_scar_present_stats() -> ScarPresentStats:
	return _entities.get_scar_present_stats() \
			if _entities != null else ScarPresentStats.new()


func get_sim() -> Simulation:
	return _sim


func set_presentation_time_ms(value_ms: int) -> void:
	_presentation_time_ms = -1 if value_ms < 0 else value_ms & 0xffffffff
	if _sim != null:
		_sim.set_panm_time_ms(_presentation_time_ms)


## The placed-node registry (bms_id/kind/group -> live node). The render-occlusion
## frame resolves building masks and entity render gates through it.
func get_registry() -> EntityIndex:
	return _index


## The entity presenter (the "Entities" child): the render-occlusion frame's
## wire render gates + lighting contexts, the wire-handle resolver the
## destruction/scar passes read, the `wire_node_spawned` signal the world's
## effect directors subscribe to, and the perf probes' output-channel A/B
## seam. Null before setup.
func get_entity_presenter() -> EntityPresenter:
	return _entities


## Re-key the placed-node index after a late placement (a joiner's streamed
## statics, placed once the host's world stream settles). The mission and wire
## presenters share this one index: the rebuild bumps its generation, so the
## wire pass re-plans and releases any node a now-placed row had claimed.
func rebind_placed_entities(placer: MissionObjectPlacer) -> void:
	if _index == null or placer == null:
		return
	_registry_placer = placer
	_index.build(placer.placed_models, [])


## A joiner's stamped slot vanished or was re-typed: its placed representation
## (an individual node or a batched static instance) must stop drawing, since
## the wire walk now owns whatever occupies that slot.
func _retire_placed_rows() -> void:
	if _sim == null or not _sim.is_joiner():
		return
	var retired: PackedInt32Array = _sim.take_retired_placement_ids()
	for bms_id in retired:
		if _index != null:
			var node: ObjectModel = _index.resolve_single(int(bms_id))
			if node != null:
				node.set_present_visible(false)
		if _registry_placer != null:
			_registry_placer.hide_static_instance(int(bms_id))


func entity_count() -> int:
	return _sim.get_entity_count() if _sim != null else 0


func is_playing() -> bool:
	return _sim != null and _sim.is_playing()


func _present_entity_rows(stats_on := false) -> void:
	if _sim == null or _entities == null:
		return
	var stride := int(_sim.get_present_stride())
	if stride <= 0:
		return
	# Both walks consume the same immutable row buffer. Fetching it once also
	# makes their topology revision refer to exactly the same layout.
	var snapshot: PackedFloat32Array = _sim.get_present_snapshot()
	if stats_on:
		# The native buffer build the fetch above just paid for.
		_frame_stats.add(FrameStats.PRESENT_SNAPSHOT,
				int(_sim.get_last_present_snapshot_us()))
	var layout_revision := int(_sim.get_present_layout_revision())
	var mission_start := Time.get_ticks_usec() if stats_on else 0
	if stats_on:
		var profile: PackedInt64Array = _entities.profile_present_snapshot(
				snapshot, stride, layout_revision)
		if profile.size() >= EntityPresenter.MISSION_PROFILE_SLOT_COUNT:
			_frame_stats.add(FrameStats.PRESENT_MISSION_CORE,
					profile[EntityPresenter.MISSION_PROFILE_CORE_US])
			_frame_stats.add(FrameStats.PRESENT_MISSION_AIM,
					profile[EntityPresenter.MISSION_PROFILE_AIM_US])
			_frame_stats.add(FrameStats.PRESENT_MISSION_CONTROLS,
					profile[EntityPresenter.MISSION_PROFILE_CONTROLS_US])
			_frame_stats.add(FrameStats.PRESENT_MISSION_VISIBILITY,
					profile[EntityPresenter.MISSION_PROFILE_VISIBILITY_US])
			_frame_stats.add(FrameStats.PRESENT_MISSION_BODY,
					profile[EntityPresenter.MISSION_PROFILE_BODY_US])
			_frame_stats.add(FrameStats.PRESENT_MISSION_ROWS,
					profile[EntityPresenter.MISSION_PROFILE_ROWS])
			_frame_stats.add(FrameStats.PRESENT_MISSION_SUBMITTED_ROWS,
					profile[EntityPresenter.MISSION_PROFILE_SUBMITTED_ROWS])
			_frame_stats.add(FrameStats.PRESENT_MISSION_BODY_ROWS,
					profile[EntityPresenter.MISSION_PROFILE_BODY_ROWS])
		_frame_stats.add(FrameStats.PRESENT_MISSION,
				Time.get_ticks_usec() - mission_start)
	else:
		_entities.present_snapshot(snapshot, stride, layout_revision)
	# The wire walk is a no-op until setup_wire ran (a placer-less preview).
	var wire_start := Time.get_ticks_usec() if stats_on else 0
	_entities.present_wire_snapshot(snapshot, stride, layout_revision)
	if stats_on:
		_frame_stats.add(FrameStats.PRESENT_WIRE,
				Time.get_ticks_usec() - wire_start)
		var wire_stats: WirePresentStats = _entities.get_wire_stats_record()
		_frame_stats.add(FrameStats.PRESENT_WIRE_LIVE, wire_stats.live)
		_frame_stats.add(FrameStats.PRESENT_WIRE_PENDING, wire_stats.pending)


# One whole present frame: the entity rows plus the tick-driven passes, each
# pass timed onto the stats board while the Stats tab captures. Owns the
# bundled _perf_present_us the probe scripts read.
func _present_frame(stats_on: bool) -> void:
	var present_start := Time.get_ticks_usec()
	_retire_placed_rows()
	_present_entity_rows(stats_on)
	# After the entity rows, the presenter's four passes in drive order (fire ->
	# destruction -> throwable -> scars: the entity-ring meshes parent under
	# section nodes the row walks may have just built).
	if _entities != null:
		if stats_on:
			var spans: PackedInt64Array = _entities.profile_present_passes()
			if spans.size() >= EntityPresenter.PASS_PROFILE_SLOT_COUNT:
				_frame_stats.add(FrameStats.PRESENT_FIRE,
						spans[EntityPresenter.PASS_PROFILE_FIRE_US])
				_frame_stats.add(FrameStats.PRESENT_DESTRUCTION,
						spans[EntityPresenter.PASS_PROFILE_DESTRUCTION_US])
				_frame_stats.add(FrameStats.PRESENT_THROWABLE,
						spans[EntityPresenter.PASS_PROFILE_THROWABLE_US])
				_frame_stats.add(FrameStats.PRESENT_SCARS,
						spans[EntityPresenter.PASS_PROFILE_SCARS_US])
		else:
			_entities.present_passes()
	_perf_present_us = Time.get_ticks_usec() - present_start


## Advance EXACTLY ONE cadence step and present. Returns true when a logic tick fired (and effects
## were drained). The deterministic single-tick primitive: standalone F3/MCP Step and isolated
## tests/tooling previews use this. GameWorld is the sole live real-time owner and
## advances the native session; ONED has no self-ticking mission runtime.
func tick() -> bool:
	_perf_effects_us = 0
	if _sim == null:
		_perf_tick_us = 0
		_perf_sim_us = 0
		_perf_present_us = 0
		_perf_effects_us = 0
		_perf_did_tick = false
		_ticks_last_frame = 0
		return false
	var input := MissionFrameInput.new()
	input.delta_seconds = Simulation.tick_dt()
	var outcome: MissionFrameOutcome = _sim.step_session_frame(
			input, _consume_session_tick)
	_read_frame_perf(outcome)
	if outcome != null and not outcome.is_terminal() and outcome.did_tick():
		_present_frame(_stats_capture_on())
	return outcome != null and outcome.did_tick()


# --- Per-tick presentation sink (ADR 0035) -----------------------------------


func _consume_session_tick(outcome: MissionTickOutcome) -> bool:
	if _sim == null or outcome == null or outcome.is_terminal():
		return false
	_begin_present_effect_tick(outcome.logic_tick)
	# The throwable pass's fixed-tick half: the round-bound move groups
	# reconcile BEFORE the effect world advances on this tick.
	if _entities != null:
		_entities.sync_fixed_tick_effects()
	var effects_start := Time.get_ticks_usec()
	var effects: Array = _sim.drain_effects()
	_perf_effects_us += Time.get_ticks_usec() - effects_start
	if not effects.is_empty():
		effects_drained.emit(effects)
	_feed_projectile_trace_stats()
	fixed_tick_completed.emit(outcome.logic_tick)
	return true


func _stats_capture_on() -> bool:
	return _frame_stats != null and _frame_stats.is_capture_active()


# Pull the frame's tick accounting into the probe counters from the typed
# outcome. The session phase spans (SIM_STEP and the SIM_* attribution) land
# on the board natively inside Simulation.advance_session_frame while the
# profiling clocks run; only the shell-measured legs are fed here (the
# per-pass present spans land inside the present legs themselves).
func _read_frame_perf(outcome: MissionFrameOutcome) -> void:
	_ticks_last_frame = outcome.get_ticks_run() if outcome != null else 0
	_perf_did_tick = _ticks_last_frame > 0
	_perf_tick_us = int(outcome.get_tick_us()) if outcome != null else 0
	_perf_sim_us = 0
	if not _sim.is_runtime_profiling_enabled():
		return
	_perf_sim_us = int(_sim.get_last_session_sim_us())
	if _ticks_last_frame > 0 and _stats_capture_on():
		_frame_stats.add(FrameStats.EFFECTS_DRAIN, _perf_effects_us)
		_frame_stats.add(FrameStats.SIM_TICKS, _ticks_last_frame)
		# The sim row's counter cells and the Net row's peer count, folded from
		# the sim this presentation already owns (frame_stats_slots.h).
		_frame_stats.add(FrameStats.SIM_ENTITY_COUNT, _sim.get_entity_count())
		_frame_stats.add(FrameStats.SIM_ROLE, _stats_role_value())
		_frame_stats.add(FrameStats.NET_PEER_COUNT, _sim.get_host_peer_count())


# The Stats window's SIM_ROLE contract: the three roles the window names.
const STATS_ROLE_SINGLE_PLAYER := 0
const STATS_ROLE_HOST := 1
const STATS_ROLE_JOINER := 2


func _stats_role_value() -> int:
	match _sim.session_role():
		Simulation.ROLE_JOINER:
			return STATS_ROLE_JOINER
		Simulation.ROLE_LISTEN_HOST, Simulation.ROLE_DEDICATED_HOST:
			return STATS_ROLE_HOST
		_:
			return STATS_ROLE_SINGLE_PLAYER


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
	_frame_stats.add(FrameStats.TRACE_TERRAIN,
			times.x)
	_frame_stats.add(FrameStats.TRACE_STATIC,
			times.y)
	_frame_stats.add(FrameStats.TRACE_DYNAMIC,
			times.z)
	_frame_stats.add(FrameStats.TRACE_PERSON,
			times.w)
	_frame_stats.add(FrameStats.TRACE_CALLS, counts.x)
	_frame_stats.add(FrameStats.TRACE_STATIC_SURVIVORS,
			counts.y)
	_frame_stats.add(FrameStats.TRACE_DYNAMIC_SURVIVORS,
			counts.z)
	_frame_stats.add(FrameStats.TRACE_PERSON_SURVIVORS,
			counts.w)
	_frame_stats.add(FrameStats.TRACE_STATIC_FACES,
			faces.x)
	_frame_stats.add(FrameStats.TRACE_DYNAMIC_FACES,
			faces.y)


## Real-time frame entry: bank `delta`, drain it in fixed tick_dt quanta, run that many single logic
## ticks (clamped to the native kMaxCatchupTicks), and present ONCE after the batch — the faithful
## fixed-62.5 Hz accumulator, with a zero-tick frame still presenting current render-only entity
## rows (camera and local attach/detach change between fixed ticks). The native
## session owns bank/clamp, input retention, and per-tick order; GameFramePipeline
## owns the concrete Godot device order [orig: Game_MainLoop @ 0x52b630].
func advance_session_frame(input: MissionFrameInput) -> MissionFrameOutcome:
	_perf_effects_us = 0
	if _sim == null:
		_ticks_last_frame = 0
		return null
	# The presenting shell's listener (the camera sample the pipeline stamped
	# into this frame's input; the sim's fire-sound gate reads the same
	# sample): the fire pass's ribbon camera and the scar pass's fog cull. A
	# frame with no camera (a dedicated serve, an isolated test) pushes
	# nothing and the passes keep their last listener.
	if _entities != null and input != null and input.is_listener_valid():
		_entities.set_listener_position(input.get_camera_position())
	var outcome: MissionFrameOutcome = _sim.advance_session_frame(
			input, _consume_session_tick)
	_read_frame_perf(outcome)
	if outcome == null or outcome.is_terminal():
		return outcome
	if outcome.did_tick():
		_present_frame(_stats_capture_on())
	else:
		_present_entity_rows(_stats_capture_on())
	return outcome


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
## ONLY pump for the session socket (`Simulation::step` is the sole caller of
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


## Resume the engine session. False when nothing is loaded or the transition
## is refused — Simulation.resume_session reports the inmatch::Session verdict.
func play() -> bool:
	if _sim == null:
		return false
	return bool(_sim.resume_session())


## Pause the engine session. False when the session refuses — a net role
## (inmatch::Session::pause is SinglePlayer-only) or a non-Running state — so
## callers report the engine verdict instead of re-deriving the rule.
func pause() -> bool:
	if is_transport_locked():
		return false
	if _sim == null:
		return false
	return bool(_sim.pause_session())


## One manual debug/tooling tick: one logic tick + present, outside the real-time loop.
## The standalone game's F3/MCP Step and isolated tests/tooling previews share this
## primitive. False when the session refused the step (a net role, nothing loaded).
func step_once() -> bool:
	# Stepping pauses real-time cadence (and any opt-in tooling self-tick), which starves
	# the socket between steps just as pause() does. See is_transport_locked().
	if is_transport_locked():
		return false
	if _sim == null:
		return false
	_sim.pause_session()
	return tick()


## Stop: rewind the world to the play-start baseline AND restore the authored node transforms, so the
## placed world is left exactly as it was. Safe to call when never played.
func stop() -> void:
	# Worse than pause on a live session: the restart below rewinds the world under
	# peers that are still streaming against it. See is_transport_locked().
	if is_transport_locked():
		return
	if _sim != null:
		_sim.reset_session()
	_clear_present_effect_poses()
	_restore_transforms()
	simulation_restarted.emit()


func _capture_transforms() -> void:
	_orig_transforms.clear()
	_for_each_present_node(func(node: ObjectModel) -> void:
		_orig_transforms[node] = node.transform)


func _restore_transforms() -> void:
	for node in _orig_transforms.keys():
		if is_instance_valid(node):
			node.transform = _orig_transforms[node]
			node.set_present_visible(true)  # undo any visibility intent the placed walk wrote
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
		var node: ObjectModel = _index.resolve(
			int(snap[base + Simulation.PF_BMS_ID]),
			int(snap[base + Simulation.PF_KIND]),
			int(snap[base + Simulation.PF_INDEX]))
		if node != null and is_instance_valid(node):
			fn.call(node)


# The sim drops with this driver's reference when it leaves the tree (a reload / Stop
# queue_free()s the driver).
func _exit_tree() -> void:
	if _frame_stats != null:
		var capture_changed := _on_frame_stats_capture_changed
		if _frame_stats.capture_changed.is_connected(capture_changed):
			_frame_stats.capture_changed.disconnect(capture_changed)
	_runtime_probe_enabled = false
	_has_trace_stats_sampling = false
	if _sim != null:
		_sim.set_runtime_profiling_enabled(false)
		_sim.close_session()
	_clear_present_effect_poses()
	if _entities != null:
		var reset_wire := _entities.reset_wire_runtime_state
		if simulation_restarted.is_connected(reset_wire):
			simulation_restarted.disconnect(reset_wire)
		# Frees the wire bodies + held weapons and the tracer mesh under the
		# container, the husk models + effect anchors, the throwable models and
		# the scar meshes under the owner models.
		_entities.teardown()
		_entities = null  # the "Entities" child itself dies with this node
	_container = null
	_item_db = null
	_resource_root = null
	_sim = null
