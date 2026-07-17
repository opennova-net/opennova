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
# Cadence: NovaSimulation.step() runs ONE logic tick (the original's 62 Hz engine tick) — the
# engine's dividers gate INSIDE the systems (the WAC VM fires every 62nd tick, the BMS evaluator
# quarter-passes every 16th). Deciding HOW MANY ticks a host frame runs is this driver's job, not
# the sim's: tick_realtime() banks wall-clock and dispatches 0..N of them; tick() dispatches exactly
# one. The game and the editor preview share the sim's default loco_scale. The driver can self-tick
# via _process (editor) or be driven by an explicit tick() call so a host can order it against its
# other passes (game). Stop rewinds the world (World::restore) AND restores the authored node
# transforms captured at setup.

signal effects_drained(effects: Array)
## Emitted once after every authoritative 62.5 Hz logic step, after that
## step's side effects have been delivered. Presentation-only fixed-step
## systems (particles) subscribe here instead of integrating render delta.
signal fixed_tick_completed(logic_tick: int)
## Emitted after Stop rewinds simulation and authored transforms. Host-owned
## presentation systems use this boundary to discard transient runtime state.
signal simulation_restarted()

const MissionEntityRegistry := preload("res://engine/world/mission_entity_registry.gd")
const MissionObjectPlacer := preload("res://engine/mission/mission_object_placer.gd")
const MissionPresentPass := preload("res://engine/world/mission_present_pass.gd")
const WirePresentPass := preload("res://engine/world/wire_present_pass.gd")
const FirePresentPass := preload("res://engine/world/fire_present_pass.gd")
const DestructionPresentPass := preload("res://engine/world/destruction_present_pass.gd")
const MissionSeatDiagnostics := preload("res://engine/world/mission_seat_diagnostics.gd")

# Fixed-timestep accumulator. The original decouples the simulation from rendering: the master
# loop accumulates real elapsed time and dispatches the logic update once per 16 ms (62.5 Hz),
# independently of the variable render rate — multiple ticks on a long frame, zero on a short one.
# [orig: Game_MainLoop @ 0x52b630 -> Game_ProcessMainFrame @ 0x5263f0 (one current_tick++ @ 0x24c1968)]
const TICK_DT := 1.0 / 62.5          # 0.016 s; matches AiEventQueue::kFrameDt (world/ai.h)
const MAX_CATCHUP_TICKS := 31        # spiral-of-death clamp: port of the 500 ms / 16 ms accumulator cap

var _sim: NovaSimulation
var _present                          # MissionPresentPass: placed nodes (host/SP/editor); null on a joiner
var _wire_present                     # WirePresentPass: un-placed remote players (co-op host + joiner); else null
var _fire_present                     # FirePresentPass: AI/remote fire sound + muzzle + tracers (host); else null
var _destruction_present              # DestructionPresentPass: husk swap + debris + wreck effects (host); else null
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
# Stable mission identity for host-neutral diagnostics such as the F3 overlay.
var _mission_file := ""
var _mission_name := ""

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
## the present pass. options: { loco_scale, self_tick, present_options }. Returns the AI
## entity count, or 0 on load failure (the orphan sim is freed). The sim is held off-tree by this driver.
func setup(mission, container: Node, options: Dictionary = {}) -> int:
	_clear_present_effect_poses()
	_sim = NovaSimulation.new()
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

	_has_native_present_effect_pose_lookup = (
			_sim.has_method("get_present_effect_state_for_ssn")
			and _sim.has_method("get_present_effect_state_for_wire_handle")
			and _sim.has_method("get_present_effect_state_for_bms_id")
			and _sim.has_method("get_present_effect_state_for_origin"))
	if options.has("loco_scale"):
		_sim.set_loco_scale(int(options["loco_scale"]))
	# P7: EVERY play/preview path is the in-process listen server (ADR 0011) — stood up BEFORE load,
	# the host player auto-spawns at bring-up (faithful §5.0 mode-3). The editor preview goes through
	# it too (the no-net AI-pool present is retired). A co-op LAN host additionally binds a real UDP
	# socket; a joiner is the non-authority client.
	var playable := bool(options.get("playable", false))
	var net_transport := String(options.get("net_transport", ""))
	var is_joiner := net_transport == "lan-join"
	if is_joiner:
		# Co-op LAN JOINER (a non-authority client): dial the host and run the witnessed
		# in-match JOIN. The local player L is spawned on the name-match (inside the sim's
		# joiner poll), NOT here. The player_name rides the ClientHello.co. [net-re §5.38b]
		if not _sim.enable_join(String(options.get("host_ip", "127.0.0.1")),
				int(options.get("port", 32768)), String(options.get("player_name", "Player"))):
			push_warning("MissionRuntime: could not dial co-op host %s:%d — joiner disabled." % [
				String(options.get("host_ip", "127.0.0.1")), int(options.get("port", 32768))])
	elif net_transport == "lan":
		# Default to the witnessed retail LAN host port — the first of the [32768, 32787]
		# range [orig: game.cfg mplanserverportmin/max, JO_SERVER]; matches host_and_join_lan.pcapng.
		var bind_port := int(options.get("bind_port", 32768))
		var session_options := options.duplicate()
		var mission_name := String(session_options.get("mission_name", "")).strip_edges()
		if mission_name.is_empty() and mission != null and mission.has_method("get_mission_name"):
			mission_name = String(mission.get_mission_name()).strip_edges()
		var mission_file := String(session_options.get("mission_file", ""))
		if mission_name.is_empty() and not mission_file.is_empty():
			mission_name = mission_file.get_basename()
		if not mission_name.is_empty():
			session_options["mission_name"] = mission_name
			if not session_options.has("spawn_names") or Array(session_options.get("spawn_names", [])).is_empty():
				session_options["spawn_names"] = [mission_name]
		session_options["bind_port"] = bind_port
		# Server type: serve-and-play (default) spawns + renders the host's own player and folds its
		# loopback view; a DEDICATED host (config "dedicated") runs the listen server with NO local
		# player and discards its loopback (net-re §5.2b, host_session_pump step 5). max_players is the
		# lobby-advertised cap (clamped host-side to the witnessed 1..65).
		session_options["serve_and_play"] = not bool(options.get("dedicated", false))
		session_options["max_players"] = int(options.get("max_players", 16))
		_sim.configure_host_session(session_options)
		if not _sim.enable_host_listen(bind_port):
			push_warning("MissionRuntime: could not bind co-op LAN host port %d — falling back to local listen server." % bind_port)
			_sim.enable_listen_server(true)
	else:
		# SP / editor preview: the in-process listen server. The host player auto-spawns at bring-up.
		_sim.enable_listen_server(true)
	if options.get("resource_root") != null and options.get("item_db") != null:
		_sim.set_item_seat_specs(_build_item_seat_specs(mission, options["resource_root"], options["item_db"]))
	# Feed the mission's raw .til bytes BEFORE load so the host bring-up streams the S2C 0x45 terrain-tile
	# load to joiners (net-re §5.37). Harmless for SP/joiner (only the host bring-up reads it).
	if options.has("terrain_til"):
		_sim.set_terrain_til_data(options["terrain_til"])
	if mission == null or not _sim.load_from_mission_data(mission):
		_sim.free()  # NovaSimulation is a Node (not RefCounted); free the orphan on load failure
		_sim = null
		_has_native_present_effect_pose_lookup = false
		return 0
	# Ground the AI on the host's terrain (editor preview + game share this one call). Entities hug
	# the terrain instead of floating; absent/unloaded terrain leaves their authored Z untouched.
	if options.get("terrain") != null:
		_sim.set_terrain_height_field(options["terrain"])
	# The sound-profile chain: SndProf.def feeds the sim's footstep/foley/landing/
	# scream slot table (retail loads it once at boot; ours rides the mission's
	# resource root — same file either way). The night gate the death scream
	# reads is the BMS attrib dword finish_load already stamps.
	# [orig: SoundProfile_LoadAll @ 0x527490 from Game_InitSubsystems]
	if _sim.has_method("set_sound_profiles") and options.get("resource_root") != null:
		var sound_rr = options["resource_root"]
		if sound_rr.has_file("SndProf.def"):
			_sim.set_sound_profiles(sound_rr.read_file("SndProf.def"))
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
	# The registry present drives placed mission nodes (host listen-server / SP / editor preview);
	# a joiner has none, so it skips it.
	if not is_joiner:
		_present = MissionPresentPass.new()
		_present.setup(_sim, _index, options.get("present_options", {}))
	# Co-op needs remote PLAYERS rendered WIRE-DIRECT: a dynamically-spawned player (an admitted
	# joiner on the host, or — on the joiner — the host + everyone) has no .bms placement, so
	# MissionPresentPass can't resolve it. The host keeps MissionPresentPass for its placed NPCs and
	# adds this pass for the spawned players, deferring any row that resolves to a placed node (via
	# _index) so nothing double-renders. The joiner places nothing (index null -> render every row).
	if is_joiner or _sim.is_host_listening():
		_wire_present = WirePresentPass.new()
		_wire_present.setup(_sim, options.get("placer"), container, options.get("env_node"),
			null if is_joiner else _index)
	# The host fire-presentation pass: AI/remote fire sound + muzzle effect + tracer
	# streaks off the sim's fired/tracer drains — providers come from the host shell
	# (game_world). A joiner's presentation seam is its own decode path (net views).
	if not is_joiner and options.has("fire_audio"):
		_fire_present = FirePresentPass.new()
		_fire_present.setup(_sim, container,
			options.get("fire_audio", Callable()),
			options.get("fire_fx", Callable()),
			options.get("fire_listener", Callable()))
	# The host destruction-presentation pass: husk model swaps, death-piece
	# debris, wreck fire/smoke, destruction sounds — off the sim's destruction
	# drain (world/destruction.h; world-wac-ai-re §24). Shares the fire pass's
	# audio/fx providers.
	if not is_joiner and options.has("fire_audio"):
		_destruction_present = DestructionPresentPass.new()
		_destruction_present.setup(_sim, _index, options.get("placer"),
			options.get("item_db"), options.get("game_world"),
			options.get("fire_audio", Callable()),
			options.get("fire_fx", Callable()))
	# Spawn the host's own player as an authoritative pool-0 entity (ADR 0012 / net-re §5.2b).
	# After load (the spawn needs the AI system wired). The spawn POSE is selected the way the
	# original engine does — by game type, from the mission's player-START marker FARTHEST from the
	# enemy set — NOT from the first NPC's position (net-re §5.2c). The player then runs the infantry
	# motor from input (set_player_input); visible translation needs walk clips.
	# A joiner's local player L is spawned at the host-advertised pose on the name-match (inside
	# the sim's joiner poll), NOT from a local start marker — so skip the host spawn here.
	if (playable or options.get("player", false)) and not is_joiner:
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
	# Stamp each entity's items.def wire traits: AI-capability (0x0D AI-trailer gate, D-NET-97),
	# the §5.10b replication class (0x0A serialize dispatch — an ewep emplacement must not ride
	# the vehicle record), and hp -> health/health_max (vehicles spawn at full health on the wire).
	# Also installs the same class table on the local client view's 0x0A DECODE (the retail
	# client sizes records from its own items.def), so both sides of the in-process wire agree —
	# a mis-sized record desyncs the frame and scatters entities around the player.
	if options.get("item_db") != null:
		_sim.resolve_item_traits(options["item_db"])
	# World-object collision: register each placed graphic's .3di collision block (BVOL
	# volumes + BPLN planes) on the sim and attach per-entity instances — walls push back,
	# roofs carry, hurt/ladder volumes act, blink boxes set indoors. [orig:
	# Entity_ProcessCollisionAndPlatformPhysics @0x4b2bd0 + the query set;
	# docs/world/world-wac-ai-re.md §15; D-INF-3 burn-down]
	if options.get("item_db") != null and options.get("placer") != null:
		_sim.resolve_collision_instances(options["item_db"], options["placer"])
		# Mission-start portal init over the occlusion models just attached:
		# register the exterior window faces, weld coincident opposite pairs of
		# adjacent buildings into cross-building links, stamp the per-building
		# flag bytes. [orig: Terrain_InitBuildingPortals @ 0x5c7480 from
		# Game_StartMission @ 0x525e11]
		if _sim.has_method("occlusion_init_mission"):
			_sim.occlusion_init_mission()
	# Armory table (weapon.def) onto the sim world — the 0x5A ammo resolve + 0x2F filter source
	# and the uplink equipped-weapon gate (D-NET-141/143). Missing root/file leaves the table
	# empty; the loadout reply then degrades to the tracked request-echo fallback.
	if options.get("resource_root") != null:
		if _sim.load_weapon_table(options["resource_root"], "weapon.def") != OK:
			push_warning("MissionRuntime: weapon.def not loaded — 0x5A ammo resolve degraded to echo")
		# Ballistics table (ammo.def) + the round_type resolve — the authoritative round
		# sim's data feed (fire -> flight -> damage -> death; net-re §5.60). After the
		# armory so every adm's fired round binds to its ammo index.
		if _sim.load_ammo_table(options["resource_root"], "ammo.def") != OK:
			push_warning("MissionRuntime: ammo.def not loaded — client fire echoes without authoritative rounds")
		elif options.get("item_db") != null:
			# Seed each NPC's anim-fire weapon: items.def ammo_closeattack + clipsize
			# resolved against the ammo table just loaded (the D-AI-5 host seed).
			# Without it every placed NPC is unarmed — the fire pass skips ammo_primary < 0.
			_sim.resolve_ai_weapons(options["item_db"])
	_log_infantry_debug_mounts()
	# Capture the authored node transforms now (pre-tick) so Stop restores them whether the host
	# played or only stepped. Cheap; the game never Stops but holding the map costs nothing.
	_capture_transforms()
	return _sim.get_entity_count()


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
	var wire_handle := int(entity_ref.get("wire_handle", 0))
	if _has_native_present_effect_pose_lookup:
		var state := PackedVector3Array()
		if wire_handle > 0:
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
	if wire_handle > 0:
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
		if wire_handle > 0:
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


# --- the local player (Phase 2; ADR 0012). Thin delegates to the sim for the host. ---
func has_player() -> bool:
	return _sim != null and _sim.has_local_player()

func local_player_position() -> Vector3:
	return _sim.get_local_player_position() if _sim != null else Vector3.ZERO

func local_player_yaw_deg() -> float:
	return _sim.get_local_player_yaw_deg() if _sim != null else 0.0

func local_player_pitch_deg() -> float:
	return _sim.get_local_player_pitch_deg() if _sim != null else 0.0

func local_player_body_anim_slot() -> int:
	return _sim.get_local_player_body_anim_slot() if _sim != null else -1

func local_player_anim_key() -> String:
	return String(_sim.get_local_player_anim_key()) if _sim != null else ""

func local_player_anim_phase_ticks() -> int:
	return int(_sim.get_local_player_anim_phase_ticks()) if _sim != null else 0

# Decoded at the NovaSimulation transport edge (ADR 0017); null when absent/invalid.
func local_player_aim_overlay() -> PlayerAimOverlay:
	return PlayerAimOverlay.from_sim_dict(_sim.get_local_player_aim_overlay()) if _sim != null else null

func local_player_health() -> int:
	return int(_sim.get_local_player_health()) if _sim != null else 0

func local_player_max_health() -> int:
	return int(_sim.get_local_player_max_health()) if _sim != null else 100

func local_player_team() -> int:
	return int(_sim.get_local_player_team()) if _sim != null else 0


# Fire-presentation counters (probe/diagnostic seam; empty when the pass is absent).
func get_fire_present_stats() -> Dictionary:
	return _fire_present.get_stats() if _fire_present != null else {}


func get_destruction_present_stats() -> RefCounted:
	# DestructionPresentPass.Stats (typed counters, ADR 0017); null until a
	# host mission runs with the pass.
	return _destruction_present.get_stats() if _destruction_present != null else null

func set_player_input(forward: bool, back: bool, left: bool, right: bool, lean_left: bool, lean_right: bool, jump: bool) -> void:
	if _sim != null:
		_sim.set_player_input(forward, back, left, right, lean_left, lean_right, jump)


## One frame of raw mouse pixels onto the sim-owned look angles (the witnessed
## integer pipeline: sensitivity, scoped zoom reduction, prone pitch clamp).
## [orig: Input_ProcessMouseAxisBindings @0x499680; cases 166/164]
func add_player_look(dx_px: float, dy_px: float) -> void:
	if _sim != null:
		_sim.add_local_player_look(dx_px, dy_px)


## Stance SELECT request: 0 stand / 1 crouch / 2 prone (the 3-key semantics,
## refused while the equipped weapon has ForceCrouch). [orig: cases 169/170/172
## -> C2S 0x1D -> NapiNPServerMsg_HandleStanceChange @0x501c60]
func request_player_stance(stance: int) -> bool:
	return _sim.request_local_player_stance(stance) if _sim != null else false


func get_sim() -> NovaSimulation:
	return _sim


## The placed-node registry (bms_id/kind/group -> live node). The render-occlusion
## frame resolves building masks and entity render gates through it.
func get_registry():
	return _index


## Register a render-side consumer for models materialized from the replicated
## wire stream. The pass replays already-live nodes when the callback is set.
func set_wire_node_spawned_callback(callback: Callable) -> void:
	if _wire_present != null:
		_wire_present.set_node_spawned_callback(callback)


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
	if did_tick:
		var present_start := Time.get_ticks_usec()
		if _present != null:
			_present.present()
		if _wire_present != null:
			_wire_present.present()
		if _fire_present != null:
			_fire_present.present(1)
		if _destruction_present != null:
			_destruction_present.present()
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
	var did_tick := _sim.step()  # one 62 Hz logic tick (the WAC VM self-gates inside)
	_perf_sim_us = Time.get_ticks_usec() - sim_start
	_perf_effects_us = 0
	if did_tick:
		var logic_tick := int(_sim.get_logic_tick())
		# Invalidate before delivering effects: any owned spawn seeded during
		# this tick and the fixed-tick particle advance both observe this exact
		# client-view pose, even inside a multi-tick catch-up batch.
		_begin_present_effect_tick(logic_tick)
		var effects_start := Time.get_ticks_usec()
		var effects := _sim.drain_effects()
		_perf_effects_us = Time.get_ticks_usec() - effects_start
		if not effects.is_empty():
			effects_drained.emit(effects)
		fixed_tick_completed.emit(logic_tick)
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
	if _present != null or _wire_present != null or _fire_present != null or _destruction_present != null:
		var present_start := Time.get_ticks_usec()
		if _present != null:
			_present.present()
		if _wire_present != null:
			_wire_present.present()
		if _fire_present != null:
			_fire_present.present(n)
		if _destruction_present != null:
			_destruction_present.present()
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
	_clear_present_effect_poses()
	_has_native_present_effect_pose_lookup = false
	if _fire_present != null:
		_fire_present.teardown()  # frees the tracer mesh instance under the container
		_fire_present = null
	if _destruction_present != null:
		_destruction_present.teardown()  # frees husk models + effect anchors
		_destruction_present = null
	if _sim != null and is_instance_valid(_sim):
		_sim.free()
		_sim = null
