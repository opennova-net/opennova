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

const FirePresentPass := preload("res://game/world/fire_present_pass.gd")
const DestructionPresentPass := preload("res://game/world/destruction_present_pass.gd")
const ThrowablePresentPass := preload("res://game/world/throwable_present_pass.gd")
const ScarPresentPass := preload("res://game/world/scar_present_pass.gd")

# Fixed-timestep accumulator. The original decouples the simulation from rendering: the master
# loop accumulates real elapsed time and dispatches the logic update once per 16 ms (62.5 Hz),
# independently of the variable render rate — multiple ticks on a long frame, zero on a short one.
# [orig: Game_MainLoop @ 0x52b630 -> Game_ProcessMainFrame @ 0x5263f0 (one current_tick++ @ 0x24c1968)]
# 0.016 s — the engine tick quantum, single-sourced natively as
# world::TickAccumulator::kTickDt (S14); kept here for cadence CONSUMERS
# (avatar preview's menu clock). The accumulator arithmetic itself is native.

var _sim: Simulation
var _present: PresentApplier          # placed nodes on every role or tooling/test preview
var _wire_present: WirePresentPass    # un-placed network entities or SP attachment children
var _fire_present: FirePresentPass    # non-local fire sound + muzzle + tracers; else null
var _fire_listener := Callable()      # -> Vector3 camera listener, stamped into the sim per frame (world/fire_sound.h)
var _destruction_present: DestructionPresentPass  # husk swap + debris + wreck effects (every viewing peer); null without fire_audio
var _throwable_present: ThrowablePresentPass      # flying/placed throwable models
var _scar_present: ScarPresentPass                # impact-scar rings as textured quads (every viewing peer)
var _index: EntityIndex
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
# Stable mission identity for shell-neutral diagnostics such as the F3 overlay.
var _mission_file := ""
var _mission_name := ""
var _setup_error := OK

# The current authoritative present tick (-1 = none yet). Attachment-pose
# lookups are Simulation's generation-bound native index — the one path;
# test sims implement the same compact API.
var _effect_pose_snapshot_tick := -1


## Create + promote the mission, build the shared index over the placed nodes (`container`), and wire
## the present pass. options: { loco_scale, present_options, and for a net
## session the typed request under "host_session" (HostSessionConfig) or "join_target"
## (JoinTarget) }. Returns the AI entity count, or 0 on load failure (the orphan sim is
## freed). Inspect get_setup_error() to distinguish a valid empty mission from a setup
## failure. The sim is held off-tree by this driver.
func setup(mission: MissionData, container: Node, options: Dictionary = {}) -> int:
	_clear_present_effect_poses()
	_setup_error = OK
	# A remote join may already own the live socket + NP session while it waits
	# for S2C 0x7B to identify the mission. Keep that exact connection across the
	# wire-header world construction instead of reconnecting after discovery. Standalone host/SP and
	# isolated test/tooling callers do not provide a simulation and retain the fresh-instance path.
	_sim = options.get("simulation", null)
	if _sim == null:
		_sim = Simulation.new()
	_sim.set_frame_stats(_frame_stats)
	_has_trace_stats_sampling = _sim != null
	_sync_runtime_profiling()
	var mission_path := String(options.get(
			"debug_mission_file", options.get("mission_file", "")))
	_mission_file = mission_path.replace("\\", "/").get_file()
	_mission_name = String(options.get(
			"debug_mission_name", options.get("mission_name", "")))
	if _mission_name.is_empty() and mission != null:
		_mission_name = String(mission.get_mission_name()).strip_edges()
	if _mission_name.is_empty() and not _mission_file.is_empty():
		_mission_name = _mission_file.get_file().get_basename()

	if options.has("loco_scale"):
		_sim.set_loco_scale(int(options["loco_scale"]))
	# The listen host stamps its own type-2 connection during role bring-up, so
	# its two per-side character selections must already be present here. The
	# same profile is what a joiner uploads through ClientAuth.
	# [orig: apply_session_settings_to_globals @0x551500;
	#  Server_PlayerAdd @0x51CBC0 -> packed id @0x51D0B1]
	if options.has("local_character_profile"):
		_sim.set_local_character_profile(options["local_character_profile"])
	# P7 / ADR 0011: every authoritative live mission is an in-process listen server, stood up BEFORE
	# load; the host player auto-spawns at bring-up (faithful §5.0 mode-3). MainGame/GameWorld is the
	# sole live runtime owner (ADR 0025). Isolated tests may instantiate this same
	# seam, but ONED does not. A co-op LAN host additionally binds a real UDP
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
			push_warning("MissionPresentation: unknown join integrity profile '%s'." % \
					join_target.integrity_profile)
			_setup_error = ERR_INVALID_PARAMETER
			_sim.free()
			_sim = null
			_has_trace_stats_sampling = false
			return 0
		if needs_join_connection and not _sim.enable_join(
				join_target.host_ip, join_target.port, join_target.player_name):
			push_warning("MissionPresentation: could not dial co-op host %s:%d — joiner disabled." % [
				join_target.host_ip, join_target.port])
		# The JOIN VERSIONCRCSTRING checksum reads the loose
		# expansion/<name>/version.txt under the install root (D-NET-166).
		var join_resource_root: ResourceRoot = options.get("resource_root")
		if needs_join_connection and join_resource_root != null:
			_sim.set_join_expansion_version_root(join_resource_root.get_root_dir())
	elif host_session != null:
		# Co-op LAN HOST: encode the typed session request at the FFI boundary (ADR 0017)
		# and stamp the mission-derived identity the session advertises on top. bind_port
		# defaults to the witnessed retail LAN host port (HostSessionConfig.DEFAULT_LAN_PORT);
		# serve-and-play vs DEDICATED and the lobby player cap ride to_session_options()
		# (net-re §5.2b, host_session_pump step 5).
		var session_options := host_session.to_session_options()
		if host_session.game_type_auto:
			var mission_mode := 0
			if mission != null:
				mission_mode = int(mission.get_game_mode())
			session_options["gametype"] = HostSessionConfig.game_type_for_mission_mode(mission_mode)
		var mission_name := String(options.get("mission_name", "")).strip_edges()
		if mission_name.is_empty() and mission != null:
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
		# The host's expansion version checksum (retail's g_expansion_checksum) is
		# CRC'd from the loose expansion/<name>/version.txt under the install root;
		# the join gate compares it against each joiner's VERSIONCRCSTRING while an
		# expansion is active (D-NET-166).
		var session_resource_root: ResourceRoot = options.get("resource_root")
		if session_resource_root != null:
			session_options["game_root"] = session_resource_root.get_root_dir()
		_sim.configure_host_session(session_options)
		# Retail builds the active game-type score table, then overlays the loose
		# VERSION 40 score.ini before answering C2S 0x2D with S2C 0x58. This
		# caller is explicitly loose-first even in a packed runtime: retail opens
		# score.ini from the game directory rather than resolving it from a PFF.
		if session_resource_root != null:
			var score_ini_bytes := session_resource_root.read_file(
					"score.ini", ResourceRoot.LOOKUP_FORCE_LOOSE_FIRST)
			if not score_ini_bytes.is_empty() and not _sim.set_score_config_data(score_ini_bytes):
				push_warning("MissionPresentation: rejected score.ini; session status uses zero score values.")
		if not _sim.enable_host_listen(host_session.bind_port):
			# A requested LAN host that cannot own its UDP endpoint is not a host.
			# Never degrade into the visually-identical socketless SP/listen path:
			# the caller must surface the bind failure and keep the menu active.
			_setup_error = ERR_CANT_CREATE
			_sim.free()
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
			options.get("resource_root"),
			options.get("item_db"),
			options.get("terrain"),
			options.get("terrain_til", PackedByteArray()),
			String(options.get("wac_basename", "")),
			String(options.get("infantry_adm", "")),
			_mission_file.get_file().get_basename(),
			playable or bool(options.get("player", false))))
	if boot_err != OK:
		_setup_error = ERR_CANT_OPEN
		_sim.free()  # Simulation is a Node (not RefCounted); free the orphan on load failure
		_sim = null
		_has_trace_stats_sampling = false
		return 0
	# The shared render/PANM presentation DWORD — re-stamped after the boot
	# because the load reset cleared it (an order-free scalar, not a boot step).
	if _presentation_time_ms >= 0:
		_sim.set_panm_time_ms(_presentation_time_ms)
	# The SIM is held off-tree (never add_child'd): only this driver advances it, and an off-tree
	# node never self-ticks via _process; it is freed explicitly in _exit_tree (mirrors the old
	# MissionSimDriver). This MissionPresentation node itself IS in the tree — GameWorld adds it and
	# drives advance_session_frame() explicitly (ADR 0025: the game shell is the only live runtime owner).
	_index = EntityIndex.new()
	var registry_placer: MissionObjectPlacer = options.get("placer")
	_index.build(registry_placer.placed_entity_records if registry_placer != null else [],
			mission.get_area_triggers() if mission != null else [])
	# The registry present drives whichever authored mission nodes actually exist. A
	# production joiner owns only the 616-byte wire header, so its index is empty: the
	# native sim separately materializes streamed pools 1-3 at exact packed handles for
	# world-side consumers, while the wire pass below renders their decoded live state.
	# Complete-BMS/debug joins still retain authored nodes and the ordinary defer identity.
	_present = PresentApplier.new()
	_present.setup(_sim, _index, registry_placer)
	_apply_present_options(_present, options.get("present_options", {}))
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
		var wire_defer_index: EntityIndex = _index
		if mission != null and mission.is_wire_header_only():
			wire_defer_index = null
		_wire_present.setup(_sim, options.get("placer"), container,
				wire_defer_index)
		_wire_present.set_synthetic_origin_only(sp_attachment_present)
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
					else Callable(),
			options.get("muzzle_light", Callable()))
		# The sim's fire-sound distance gate reads the camera listener at fire
		# time on the logic clock (world/fire_sound.h) — stamped in each typed
		# session frame. A host with no fire presentation (dedicated) never
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
			options.get("item_db"), options.get("effect_anchors"),
			options.get("fire_audio", Callable()),
			options.get("fire_fx", Callable()),
			_wire_present,
			options.get("death_light", Callable()))
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
		options.get("item_db"),
		options.get("fire_fx", Callable()), options.get("effect_anchors"))
	simulation_restarted.connect(
		Callable(_throwable_present, 'reset_runtime_state'))
	# The impact-scar presentation pass (world-wac-ai-re §24.9): the sim's scar
	# rings as textured quads — the shared ring as one world mesh, each entity
	# ring under its carrier's struck section. Every viewing peer runs it: retail
	# draws its own caches on every client from the same impact processor
	# [orig: Scar_RenderAllCaches @0x5CDF70 from Terrain_CollectVisibleEntities
	#  @0x5c91b7]. The camera + environment providers are the device inputs the
	# renderer's cull and vertex colour read (fire_listener IS the camera).
	_scar_present = ScarPresentPass.new()
	_scar_present.setup(_sim, container, _index, _wire_present,
		options.get("resource_root"),
		options.get("fire_listener", Callable()),
		options.get("environment_node", Callable()))
	simulation_restarted.connect(
		Callable(_scar_present, 'reset_runtime_state'))
	# ADR 0035: the native session owns lifecycle/cadence and invokes one
	# synchronous per-tick presentation sink. The camera remains a Godot device;
	# a dedicated host has none.
	# Capture the authored node transforms now (pre-tick) so Stop restores them whether the caller
	# played or only stepped. Cheap; the game never Stops but holding the map costs nothing.
	_capture_transforms()
	return _sim.get_entity_count()


# Translate the composition's present_options bundle onto the native applier:
# the four drive_* channel flags plus the shell's shared occlusion/visibility
# maps (shared BY REFERENCE: GameWorld mutates the hidden set in place; the
# release lands on the sim's current intent so neither visibility writer
# fights the other).
static func _apply_present_options(applier: PresentApplier,
		options: Dictionary) -> void:
	var channels := int(PresentApplier.OUTPUT_ALL)
	if not bool(options.get("drive_transform", true)):
		channels &= ~PresentApplier.OUTPUT_TRANSFORM
	if not bool(options.get("drive_part_anim", true)):
		channels &= ~PresentApplier.OUTPUT_PART_ANIM
	if not bool(options.get("drive_visibility", true)):
		channels &= ~PresentApplier.OUTPUT_VISIBILITY
	if not bool(options.get("drive_body_anim", true)):
		channels &= ~PresentApplier.OUTPUT_BODY_ANIM
	applier.set_output_channels(channels)
	applier.set_shared_visibility_maps(
			options.get("occlusion_hidden_ids", {}),
			options.get("present_visibility", {}))


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
func presented_entity_effect_transform(entity_ref: Dictionary) -> Variant:
	if not has_current_present_effect_snapshot():
		return null
	var has_wire_handle := entity_ref.has("wire_handle")
	var wire_handle := int(entity_ref.get("wire_handle", -1))
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
	return PlayerAimOverlay.from_sim_dict(_sim.get_local_player_aim_overlay()) if _sim != null else null

func local_player_health() -> int:
	return int(_sim.get_local_player_health()) if _sim != null else 0

func local_player_max_health() -> int:
	return int(_sim.get_local_player_max_health()) if _sim != null else 100

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


func get_fire_present_stats() -> RefCounted:
	# FirePresentPass.Stats (typed counters, ADR 0017); null until the
	# presentation pass exists.
	return _fire_present.get_stats() if _fire_present != null else null


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
	if _fire_present != null:
		_fire_present.warm_pipelines(at_position)


func get_throwable_present_stats() -> RefCounted:
	# ThrowablePresentPass.Stats (typed counters, ADR 0017); null until the
	# presentation pass exists.
	return _throwable_present.get_stats() if _throwable_present != null else null


func get_destruction_present_stats() -> RefCounted:
	# DestructionPresentPass.Stats (typed counters, ADR 0017); null until a
	# loaded mission runs with the pass.
	return _destruction_present.get_stats() if _destruction_present != null else null


func get_scar_present_stats() -> RefCounted:
	# ScarPresentPass.Stats (typed counters, ADR 0017); null until the
	# presentation pass exists.
	return _scar_present.get_stats() if _scar_present != null else null

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


## Register a render-side consumer for models materialized from the replicated
## wire stream. The pass replays already-live nodes when the callback is set.
func set_wire_node_spawned_callback(callback: Callable) -> void:
	if _wire_present != null:
		_wire_present.set_node_spawned_callback(callback)


## The active wire-direct presenter, exposed for lifecycle integrations and
## diagnostics. Null when this mission has no replicated/synthetic rows.
func get_wire_presenter() -> WirePresentPass:
	return _wire_present


func entity_count() -> int:
	return _sim.get_entity_count() if _sim != null else 0


func is_playing() -> bool:
	return _sim != null and _sim.is_playing()


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
		_frame_stats.add(FrameStats.PRESENT_SNAPSHOT,
				int(_sim.get_last_present_snapshot_us()))
	var layout_revision := int(_sim.get_present_layout_revision())
	if _present != null:
		var mission_start := Time.get_ticks_usec() if stats_on else 0
		if stats_on:
			var profile: PackedInt64Array = _present.profile_present_snapshot(
					snapshot, stride, layout_revision)
			if profile.size() >= PresentApplier.MISSION_PROFILE_SLOT_COUNT:
				_frame_stats.add(FrameStats.PRESENT_MISSION_CORE,
						profile[PresentApplier.MISSION_PROFILE_CORE_US])
				_frame_stats.add(FrameStats.PRESENT_MISSION_AIM,
						profile[PresentApplier.MISSION_PROFILE_AIM_US])
				_frame_stats.add(FrameStats.PRESENT_MISSION_CONTROLS,
						profile[PresentApplier.MISSION_PROFILE_CONTROLS_US])
				_frame_stats.add(FrameStats.PRESENT_MISSION_VISIBILITY,
						profile[PresentApplier.MISSION_PROFILE_VISIBILITY_US])
				_frame_stats.add(FrameStats.PRESENT_MISSION_BODY,
						profile[PresentApplier.MISSION_PROFILE_BODY_US])
				_frame_stats.add(FrameStats.PRESENT_MISSION_ROWS,
						profile[PresentApplier.MISSION_PROFILE_ROWS])
				_frame_stats.add(FrameStats.PRESENT_MISSION_SUBMITTED_ROWS,
						profile[PresentApplier.MISSION_PROFILE_SUBMITTED_ROWS])
				_frame_stats.add(FrameStats.PRESENT_MISSION_BODY_ROWS,
						profile[PresentApplier.MISSION_PROFILE_BODY_ROWS])
			_frame_stats.add(FrameStats.PRESENT_MISSION,
					Time.get_ticks_usec() - mission_start)
		else:
			_present.present_snapshot(snapshot, stride, layout_revision)
	if _wire_present != null:
		var wire_start := Time.get_ticks_usec() if stats_on else 0
		_wire_present.present_snapshot(snapshot, stride, layout_revision)
		if stats_on:
			_frame_stats.add(FrameStats.PRESENT_WIRE,
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
			_frame_stats.add(FrameStats.PRESENT_FIRE,
					Time.get_ticks_usec() - fire_start)
	if _destruction_present != null:
		var destruction_start := Time.get_ticks_usec() if stats_on else 0
		_destruction_present.present()
		if stats_on:
			_frame_stats.add(FrameStats.PRESENT_DESTRUCTION,
					Time.get_ticks_usec() - destruction_start)
	if _throwable_present != null:
		var throwable_start := Time.get_ticks_usec() if stats_on else 0
		_throwable_present.present()
		if stats_on:
			_frame_stats.add(FrameStats.PRESENT_THROWABLE,
					Time.get_ticks_usec() - throwable_start)
	# After the entity rows: the entity-ring meshes parent under section nodes
	# the row passes may have just built.
	if _scar_present != null:
		var scar_start := Time.get_ticks_usec() if stats_on else 0
		_scar_present.present()
		if stats_on:
			_frame_stats.add(FrameStats.PRESENT_SCARS,
					Time.get_ticks_usec() - scar_start)
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
	if _throwable_present != null:
		_throwable_present.sync_fixed_tick_effects()
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


func present_entity_rows() -> void:
	_present_entity_rows(_stats_capture_on())


func present_frame() -> void:
	_present_frame(_stats_capture_on())


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


func play() -> void:
	if _sim != null:
		_sim.resume_session()


func pause() -> void:
	if is_transport_locked():
		return
	if _sim != null:
		_sim.pause_session()


## One manual debug/tooling tick: one logic tick + present, outside the real-time loop.
## The standalone game's F3/MCP Step and isolated tests/tooling previews share this primitive.
func step_once() -> void:
	# Stepping pauses real-time cadence (and any opt-in tooling self-tick), which starves
	# the socket between steps just as pause() does. See is_transport_locked().
	if is_transport_locked():
		return
	if _sim != null:
		_sim.pause_session()
	tick()


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
			int(snap[base + Simulation.PF_BMS_ID]),
			int(snap[base + Simulation.PF_KIND]),
			int(snap[base + Simulation.PF_INDEX]))
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
		_sim.close_session()
	_clear_present_effect_poses()
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
	if _scar_present != null:
		var reset_scars := Callable(_scar_present, 'reset_runtime_state')
		if simulation_restarted.is_connected(reset_scars):
			simulation_restarted.disconnect(reset_scars)
		_scar_present.teardown()  # frees the scar meshes under the owner models
		_scar_present = null
	if _sim != null and is_instance_valid(_sim):
		_sim.free()
		_sim = null
