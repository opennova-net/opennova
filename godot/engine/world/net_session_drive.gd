class_name NetSessionDrive
extends Node

# The drive between a typed session request and an admitted world — the engine
# side of a net-session load, owned by GameWorld as an internal child node:
# request staging (HostSessionConfig / JoinTarget, ADR 0017), the
# authenticated-before-load joiner preload sim, the S2C 0x7B promote, the
# expansion reconcile (D-NET-178), the post-load admission watchdog + per-frame
# admission/deploy/loss observer, the session-loss latch, the ESC aborts, and
# NovaWorld gate registration for a browsable listen host.
#
# Engine-side and shell-neutral: no menu/env knowledge lives here (the shell's
# NetSessionController owns the entries; MainGame owns presentation). The
# session signals stay on GameWorld — the shell contract pins them there — so
# this drive emits them THROUGH its world reference
# (world.join_session_identified.emit(...)); there is no connect-and-re-emit
# hop. Its two waits are coroutines that await process_frame, so this node must
# be in the tree before load_as_joiner runs: GameWorld constructs and adds it
# in _init, which puts it in the tree with the world itself.

const ResourceDirSettings := preload("res://engine/resource_index/resource_dir_settings.gd")

# The retail ConnectOrHost wait window, shared by the joiner's pre-load admission
# drive and the post-load deployment watchdog [orig: 0xEA60 = 60000 ms].
const JOIN_CONNECT_TIMEOUT_MS := 60000

# The GameWorld this drive loads through — its PUBLIC surface only
# (load_mission / mission_file / get_runtime) plus the session signals emitted
# through it; the world's private internals arrive as the setup() Callables
# below. Untyped: the world script owns (preloads) this one.
var _world
var _load_mission_internal_cb := Callable()  # (mission, bms_name, resource_root) -> int
var _resolve_root_cb := Callable()  # (dir) -> NovaResourceRoot (or null after load_failed)
var _spawn_loadout_cb := Callable()  # () -> Dictionary (the staged PLAYER_INFO snapshot)

# NovaWorldHost: registers a LAN/co-op listen host with the NovaWorld gate so a
# retail client can browse + join it (F1). Only created when a gate was supplied
# (via _host_config["nw_gate_host"]); absent for pure-LAN play. Fed the live
# player count from observe_tick(), torn down in reset().
var _nw_host
var _host_config: Dictionary = {}  # internal staging derived from the typed request; consumed once by stage_runtime_options
# The typed session request at the shell seam (ADR 0017): exactly one is non-null
# during a net load — the host screen's HostSessionConfig or the joiner's dial
# JoinTarget — threaded to MissionRuntime as opts["host_session"]/opts["join_target"].
var _pending_host: HostSessionConfig = null
var _pending_join: JoinTarget = null
# A retail LAN join authenticates before the local mission load. This off-tree
# simulation owns that one live socket/session while S2C 0x7B supplies map_file;
# stage_runtime_options surrenders it so the connection is never restarted.
var _join_preload_sim: NovaSimulation
var _join_preload_root: NovaResourceRoot
var _join_preload_request_id := 0
# Admission and deploy notifications are edges, not per-frame state reports.
# The deploy latch releases when pending clears so a later death can reopen DEATH.
var _join_admission_ready_emitted := false
var _join_deploy_signal_active := false
# One session-loss notification per session (the reason stays true afterwards).
var _session_lost_emitted := false
# The post-load admission wait is an async coroutine that awaits process_frame
# every iteration, so unlike the synchronous host load it IS interruptible.
var _join_admission_watch_active := false
var _join_admission_abort := false


## One-time wiring from the owning GameWorld: the world reference the public
## calls + signal emissions go through, and the three private internals it
## lends as Callables (its _load_mission_internal, its _resolve_root, and a
## reader for its staged _local_player_spawn_loadout).
func setup(world, internal_load: Callable, resolve_root: Callable,
		spawn_loadout: Callable) -> void:
	_world = world
	_load_mission_internal_cb = internal_load
	_resolve_root_cb = resolve_root
	_spawn_loadout_cb = spawn_loadout


## Load a mission as a LAN co-op HOST. Same load path as the world's load_mission, but the
## runtime starts the in-process listen server (ADR 0011) bound to a real socket transport and,
## on the NovaWorld channel, registered with the gate. `config` is the typed session
## request every host producer builds (ADR 0017): the mp.mnu host screen, the NovaWorld
## panel, and the NW_LAN_HOST env hook. Returns the same codes as load_mission.
func load_as_host(config: HostSessionConfig) -> int:
	if config == null:
		_world.load_failed.emit("host start: no host configuration")
		return ERR_INVALID_PARAMETER
	_pending_host = config
	# Internal staging for the option spread + the NovaWorld gate registration;
	# the runtime consumes the typed record itself via opts["host_session"].
	_host_config = config.to_session_options()
	_host_config["net_transport"] = "lan"
	_host_config["dedicated"] = config.dedicated
	_host_config["channel"] = config.channel
	if config.channel == HostSessionConfig.CHANNEL_NOVAWORLD:
		_host_config["nw_gate_host"] = config.nw_gate_host
		_host_config["nw_gate_port"] = config.nw_gate_port
		_host_config["region"] = config.region
		if not config.advertise.is_empty():
			_host_config["advertise"] = config.advertise
	var bms := config.mission
	if bms.is_empty() and config.missions.size() > 0:
		bms = config.missions[0]
	if bms.is_empty():
		_clear_pending_session()
		_world.load_failed.emit("host start: no mission selected")
		return ERR_INVALID_PARAMETER
	var err: int = _world.load_mission(bms, config.dir)
	if err != OK:
		_clear_pending_session()
	return err


# One reset for the typed request + its derived staging, used by every session
# load-failure leg and reset().
func _clear_pending_session() -> void:
	_pending_host = null
	_pending_join = null
	_host_config = {}


# Retail's ClientAuth does not invent a network-only player id: it uploads the
# two profile character selections packed from Avatars.def. The packed value is
# [nat:5 | division:4 | combo:6 | alignment:1], and the companion avatar byte is
# the selected combo's head voice unless the profile has an explicit override.
# [orig: PlayerProfile_InitDefaults @0x54BB40,
#  lookup_entity_slot_and_pack_entry @0x57AD40,
#  sub_57AE60 @0x57AE60, CNapiServerInfo_SerializeToSession @0x4C3650]
static func _join_character_selection(
		db: NovaAvatarDatabase, nat_index: int, div_index: int,
		combo_index: int, expected_alignment: int) -> Dictionary:
	if db == null or nat_index < 0 or nat_index >= db.get_nationality_count():
		return {}
	var nat: Dictionary = db.get_nationality(nat_index)
	if int(nat.get("alignment", -1)) != expected_alignment:
		return {}
	if div_index < 0 or div_index >= db.get_division_count(nat_index):
		return {}
	if combo_index < 0 or combo_index >= db.get_combo_count(nat_index, div_index):
		return {}
	var div: Dictionary = db.get_division(nat_index, div_index)
	var combo: Dictionary = db.get_combo(nat_index, div_index, combo_index)
	if nat.is_empty() or div.is_empty() or combo.is_empty():
		return {}
	var packed_id := (
			(int(nat.get("id", 0)) & 0x1F)
			| ((int(div.get("id", 0)) & 0x0F) << 5)
			| ((int(combo.get("id", 0)) & 0x3F) << 9)
			| ((1 if expected_alignment != 0 else 0) << 15))
	var head: Dictionary = combo.get("head", {})
	return {
		"character_id": packed_id,
		"avatar": int(head.get("voice", 1)),
	}


static func _first_join_character_selection(
		db: NovaAvatarDatabase, alignment: int) -> Dictionary:
	if db == null:
		return {}
	for nat_index in db.get_nationality_count():
		var nat: Dictionary = db.get_nationality(nat_index)
		if int(nat.get("alignment", -1)) != alignment:
			continue
		for div_index in db.get_division_count(nat_index):
			if db.get_combo_count(nat_index, div_index) > 0:
				return _join_character_selection(
						db, nat_index, div_index, 0, alignment)
	return {}


# Public test seam over the exact profile-to-wire projection. `selection` is the
# PLAYER_INFO snapshot; its chosen side replaces that side's retail default.
static func character_join_profile_from_database(
		db: NovaAvatarDatabase, selection: Dictionary = {}) -> Dictionary:
	var side_selections: Array[Dictionary] = [
		_first_join_character_selection(db, 0),
		_first_join_character_selection(db, 1),
	]
	var selected_side := int(selection.get("team", -1))
	if selected_side == 0 or selected_side == 1:
		var chosen := _join_character_selection(
				db,
				int(selection.get("nationality", -1)),
				int(selection.get("division", -1)),
				int(selection.get("combo", -1)),
				selected_side)
		if not chosen.is_empty():
			side_selections[selected_side] = chosen

	var player_class := int(selection.get("player_class", 8))
	if player_class < 5 or player_class > 9:
		player_class = 8
	return {
		"character_ids": [
			int(side_selections[0].get("character_id", 0)),
			int(side_selections[1].get("character_id", 0)),
		],
		"player_classes": [player_class, player_class],
		"avatars": [
			int(side_selections[0].get("avatar", 1)),
			int(side_selections[1].get("avatar", 1)),
		],
		"team_request": -1,
	}


func _build_join_character_profile(
		resource_root: NovaResourceRoot, selection: Dictionary) -> Dictionary:
	if resource_root == null:
		return {}
	var db := NovaAvatarDatabase.new()
	if db.load_from_resource_root(resource_root, "Avatars.def") != OK \
			or not db.is_loaded():
		push_warning("NetSessionDrive: Avatars.def not loaded for LAN join profile (%s)"
				% db.get_last_error())
		return {}
	return character_join_profile_from_database(db, selection)


## Load as a LAN co-op JOINER (a non-authority client). Retail LAN enumeration supplies an
## endpoint, not a map name: authenticate first, learn map_file from the normal S2C 0x7B
## post-handshake message, load that local .bms, then resume the SAME socket/session into the
## spawn drive. `target.mission` remains an explicit debug/online-row override. Dynamic
## entities render WIRE-DIRECT (no local .bms placement). `target.player_name` rides the game
## ClientAuth and is echoed in our organic-spawn record for self-identification.
func load_as_joiner(target: JoinTarget) -> int:
	if target == null:
		_world.load_failed.emit("join: no join target")
		return ERR_INVALID_PARAMETER
	_cancel_join_preload()
	_join_admission_ready_emitted = false
	_join_deploy_signal_active = false
	_session_lost_emitted = false
	_pending_join = target
	# Internal staging for the option spread; the 0x7B promote below refreshes it
	# with the authoritative session record before the runtime consumes it.
	_host_config = {
		"net_transport": "lan-join",
		"host_ip": target.host_ip,
		"port": target.port,
		"player_name": target.player_name,
	}
	var bms := target.mission
	if bms.is_empty():
		bms = _world.mission_file
	if bms.is_empty():
		var resource_root: NovaResourceRoot = _resolve_root_cb.call(target.dir)
		if resource_root == null:
			_clear_pending_session()
			return ERR_CANT_OPEN
		_join_preload_sim = NovaSimulation.new()
		# Retail builds g_CharAttr from the boot-soft charattr.def before any
		# network receive can deliver the 0x41 property clears or 0x39 challenge.
		# A missing file deliberately leaves the inactive all-zero table.
		_join_preload_sim.load_charattr_challenge(resource_root)
		_join_preload_sim.set_join_character_profile(
				_build_join_character_profile(
						resource_root, _spawn_loadout_cb.call()))
		if not _join_preload_sim.enable_join(
				target.host_ip, target.port, target.player_name):
			_join_preload_sim.free()
			_join_preload_sim = null
			_clear_pending_session()
			_world.load_failed.emit("join: could not open the LAN session socket")
			return ERR_CANT_CONNECT
		_join_preload_sim.set_join_world_ready(false)
		_join_preload_root = resource_root
		_join_preload_request_id += 1
		call_deferred("_drive_join_preload", _join_preload_request_id)
		return OK
	# NovaWorld's host row carries the retail basename (e.g. ASH_I5A), while the
	# VFS load requires the resource filename. LAN callers that already supply the
	# extension pass through unchanged.
	if not bms.to_lower().ends_with(".bms"):
		bms += ".bms"
	var err: int = _world.load_mission(bms, target.dir)
	if err != OK:
		_clear_pending_session()
		return err
	# An explicit-mission joiner (the NovaWorld panel row, NW_LAN_MISSION) skips the
	# preload drive, but its post-load admission is identical to the preload path's:
	# arm the same watchdog so the player-paced deployment pick emits
	# join_deploy_pick_required (the sim parks at AwaitDeployPick for EVERY joiner)
	# and a stalled host still aborts with the stage-named reason instead of
	# holding the loading screen forever.
	_watch_join_admission(_world.get_runtime())
	return OK


# Drive the witnessed pre-world connect/session exchange while the loading
# screen is visible. The 60-second deadline is the retail ConnectOrHost timeout
# (0xEA60).
func _drive_join_preload(request_id: int) -> void:
	var deadline_ms := Time.get_ticks_msec() + JOIN_CONNECT_TIMEOUT_MS
	while request_id == _join_preload_request_id and _join_preload_sim != null \
			and not _join_preload_sim.is_join_preload_ready():
		_join_preload_sim.poll_join_preload()
		var join_error := String(_join_preload_sim.get_join_error())
		if not join_error.is_empty():
			_fail_join_preload("join failed: %s" % join_error)
			return
		if Time.get_ticks_msec() >= deadline_ms:
			_fail_join_preload("join timed out before the host completed preload admission")
			return
		await get_tree().process_frame
	if request_id != _join_preload_request_id or _join_preload_sim == null:
		return
	# Reconcile the mount with the host's data set BEFORE anything is resolved through it:
	# the host's mission itself may exist only inside the expansion, so this precedes the
	# .bms lookup as well as weapon.def/items.def (D-NET-178).
	if not _reconcile_join_expansion():
		return

	var bms := String(_join_preload_sim.get_join_mission_file()).strip_edges()
	if bms.is_empty():
		_fail_join_preload("join: host sent an empty map_file in S2C 0x7B")
		return
	if not bms.to_lower().ends_with(".bms"):
		bms += ".bms"
	var resource_root := _join_preload_root
	if resource_root == null or not resource_root.has_file(
			bms, NovaResourceRoot.LOOKUP_FORCE_ARCHIVE_ONLY):
		_fail_join_preload("join: host mission %s is not installed locally" % bms)
		return
	var mission := NovaMissionData.new()
	if mission.open_from_resource_root(
			resource_root, bms, NovaResourceRoot.LOOKUP_FORCE_ARCHIVE_ONLY) != OK:
		_fail_join_preload("join: failed to parse host mission %s: %s" % [
			bms, mission.get_last_error()])
		return

	# Promote the authoritative session variables before the runtime consumes
	# _host_config. None came from discovery; every value here came from 0x7B.
	_host_config["server_name"] = _join_preload_sim.get_join_server_name()
	_host_config["mission_name"] = _join_preload_sim.get_join_mission_name()
	_host_config["mission_file"] = bms
	_host_config["gametype"] = _join_preload_sim.get_join_game_type()
	# The MOUNTED expansion, which _reconcile_join_expansion has just proven equal to the
	# host's (case aside) or aborted the join over. Reporting the mount rather than the wire
	# claim keeps this value evidence of what our data set actually is.
	_host_config["expansion"] = resource_root.get_expansion()
	_join_preload_root = null
	_world.join_session_identified.emit({
		"server_name": String(_host_config["server_name"]),
		"mission_name": String(_host_config["mission_name"]),
		"mission_file": bms,
		"game_type": int(_host_config["gametype"]),
	})
	var err: int = _load_mission_internal_cb.call(mission, bms, resource_root)
	if err != OK:
		# _load_mission_internal emitted the specific resource/load failure.
		_cancel_join_preload()
		_clear_pending_session()
		return
	_watch_join_admission(_world.get_runtime())


# Point the joiner's resource root at the HOST's expansion (S2C 0x7B field 7, net-re
# §5.32) before any of the host's data is resolved through it. The ADM weapon index space
# is expansion-scoped, so a joiner mounted on a different expansion than the host misreads
# every wire ADM index from the first diverging weapon.def row on — in BOTH directions, and
# in both its own C2S 0x2F kit and the host's S2C 0x5A grant / round events (D-NET-178).
# Retail switches THE ONE global mount in place on this same leg — it copies the session
# record's expansion over the pending name, switches, and only then connects
# [orig: UI_JoinSelectedSession @ 0x5699d0 (expansion copy @ 0x569afa, switch @ 0x569b02,
# connect @ 0x569ded) -> Expansion_SwitchTo @ 0x5688c0 -> PFF_CloseAllOpenArchives @ 0x4a4380
# / PFF_OpenAllArchives @ 0x4a4310]. There is no second mount object, and the switch is
# sticky: the shell keeps running on the host's expansion after the session.
# Returns false when the join has been failed and the driver must stop.
func _reconcile_join_expansion() -> bool:
	var resource_root := _join_preload_root
	if resource_root == null:
		return true
	var plan := JoinExpansionPlan.decide(
		String(_join_preload_sim.get_join_expansion()),
		String(resource_root.get_expansion()),
		resource_root.list_expansions(resource_root.get_root_dir()))
	if plan.action == JoinExpansionPlan.ACTION_KEEP:
		return true
	# Only a runtime mount layers expansion archives at all. A loose authoring root
	# (an editor-managed --loose-root run mounts the loose authoring tree, ADR 0025;
	# tests hand fixtures over) has no expansion
	# to switch AND reports an empty installed set by construction, so it can neither honour
	# the host's expansion nor prove it missing — every decision below is meaningless there.
	# Report the mismatch and let the authored data stand. This precedes the abort: policing
	# an install we do not own would fail every editor/fixture join against an expansion host.
	# The shipping game always arrives here on a runtime mount (main_game hands GameWorld its
	# live menu mount), so D-NET-178's protection is unaffected.
	if not resource_root.is_runtime_mount():
		push_warning("NetSessionDrive: host expansion '%s' differs from the loose root's '%s'; the authoring mount stands"
			% [String(_join_preload_sim.get_join_expansion()), String(resource_root.get_expansion())])
		return true
	# A runtime mount that cannot supply the host's expansion aborts the join. Retail's switch
	# is a no-op when expansion\<name>\<name>.pff is missing and it connects on its own data set
	# anyway [orig: Expansion_SwitchTo @ 0x5688c0, missing-.pff gate @ 0x568914] — that is
	# precisely the ADM index-space corruption D-NET-178 records, so we refuse the join instead
	# (tracked divergence).
	if plan.action == JoinExpansionPlan.ACTION_FAIL:
		_fail_join_preload(plan.error)
		return false
	var dir := resource_root.get_root_dir()
	var previous := String(resource_root.get_expansion())
	# Switch THIS root rather than swapping in a second one, the same in-place remount
	# NovaMenuShell._apply_expansion does for the Mods screen: every holder (the menu shell, the
	# loading screen) is meant to move with it, and mount_runtime rebuilds the index and bumps
	# the cache epoch, so their caches self-clear. The persisted expansion setting is NOT
	# written — the host owns this session's data set, not the local menu choice. Same layering
	# as GameWorld._mount_runtime_root (see it for the flag rules); only the expansion differs.
	if resource_root.mount_runtime(dir, plan.expansion, NovaLaunchFlags.loose_override_enabled(),
			NovaLaunchFlags.game(ResourceDirSettings.get_game())) != OK:
		# A hard mount failure clears the root, and the shell shares this object, so put the
		# previous expansion back before aborting to the menu (NovaMenuShell._apply_expansion rolls
		# back the same way). The failure surfaces through the preload's abort leg rather than a
		# bare load_failed, so the live session is torn down too.
		var mount_error := String(resource_root.get_last_error())
		resource_root.mount_runtime(dir, previous, NovaLaunchFlags.loose_override_enabled(),
			NovaLaunchFlags.game(ResourceDirSettings.get_game()))
		_fail_join_preload("join: could not mount host expansion '%s' from %s: %s" % [
			plan.expansion, dir, mount_error])
		return false
	# mount_runtime succeeds even when the expansion never layered (opennova::Vfs::mount_game
	# falls back to base game silently), so read back what ACTUALLY mounted. Without this the
	# abort leg above would be bypassed by a root that is quietly base game again. No rollback
	# here: unlike the hard failure above, the root holds a valid mount of whatever DID layer,
	# so the shell survives the abort on it.
	if String(resource_root.get_expansion()).to_lower() != plan.expansion.to_lower():
		_fail_join_preload("join: host runs expansion '%s' but %s mounted '%s' (installed: %s)" % [
			plan.expansion, dir, String(resource_root.get_expansion()),
			JoinExpansionPlan.describe_installed(resource_root.list_expansions(dir))])
		return false
	return true


# Post-load joiner watchdog: the admission tail (C2S 0x0A -> world stream -> loadout
# grants) is server-driven with no protocol-level timeout, so a stalled or incompatible
# host would leave the player loaded but hidden forever with no feedback. Reuse the
# retail ConnectOrHost window (0xEA60) from world-ready and surface a stage-named
# failure through the shell's abort-to-menu leg — the reachable analog of retail's
# post-load network-wait failure returns [orig: NapiClient_WaitForGameStart @ 0x42cc10
# failure legs -> "Mission loading aborted"]. The deadline covers only SERVER-owed
# transitions: once the join reaches the player-paced deployment pick (the DEATH deploy
# screen), the watchdog ends — retail has no in-world join timeout there, the screen
# simply waits (a rejected pick stays up for a re-pick; net-re 5.61).
func _watch_join_admission(runtime) -> void:  # MissionRuntime, untyped like the world's _runtime
	var deadline_ms := Time.get_ticks_msec() + JOIN_CONNECT_TIMEOUT_MS
	_join_admission_watch_active = true
	_join_admission_abort = false
	while is_instance_valid(runtime) and runtime == _world.get_runtime():
		var sim: NovaSimulation = runtime.get_sim()
		if sim == null or not sim.is_joiner():
			_join_admission_watch_active = false
			return
		if _join_admission_abort:
			_join_admission_watch_active = false
			_join_admission_abort = false
			_world.load_failed.emit("Mission loading aborted")
			return
		if sim.has_method("is_join_deploy_pick_pending") \
				and bool(sim.is_join_deploy_pick_pending()):
			_join_admission_watch_active = false
			_emit_join_deploy_pick_required()
			return
		if sim.is_joined_in_match():
			_join_admission_watch_active = false
			_emit_join_admission_ready()
			return
		var join_error := String(sim.get_join_error())
		if not join_error.is_empty():
			_join_admission_watch_active = false
			_world.load_failed.emit("join failed: %s" % join_error)
			return
		if Time.get_ticks_msec() >= deadline_ms:
			_join_admission_watch_active = false
			_world.load_failed.emit("join stalled waiting for the host (%s)"
					% String(sim.get_join_admission_stage()))
			return
		await get_tree().process_frame
	_join_admission_watch_active = false


func _emit_join_admission_ready() -> void:
	if _join_admission_ready_emitted:
		return
	_join_admission_ready_emitted = true
	_world.join_admission_ready.emit()


func _emit_join_deploy_pick_required() -> void:
	if _join_deploy_signal_active:
		return
	_join_deploy_signal_active = true
	_world.join_deploy_pick_required.emit()


# The initial watchdog stops at admission or the player-paced deployment screen.
# Continue observing deploy state afterward: death can create another pending edge
# in the same session.
func _update_joiner_admission_signals() -> void:
	# Render/occlusion tests install deliberately narrow runtime doubles. This
	# observer is optional outside a real MissionRuntime, so keep the seam
	# duck-typed instead of forcing every render-only double to model networking.
	var runtime: Variant = _world.get_runtime()
	if runtime == null or not runtime.has_method("get_sim"):
		return
	var sim: Variant = runtime.get_sim()
	if sim == null or not sim.has_method("is_joiner") or not bool(sim.is_joiner()):
		return
	var deploy_pending: bool = sim.has_method("is_join_deploy_pick_pending") \
			and bool(sim.is_join_deploy_pick_pending())
	if deploy_pending:
		_emit_join_deploy_pick_required()
	else:
		_join_deploy_signal_active = false
	if sim.has_method("is_joined_in_match") and bool(sim.is_joined_in_match()):
		_emit_join_admission_ready()
	_update_session_loss_signal(sim)


## Per-frame in-match session-loss observer, read once per frame off the same seam
## as the admission signals. The reason latches true inside the runtime, so this
## emits exactly once per session. [orig: the reap @ 0x4ca4a0 -> @ 0x4c63d0]
func _update_session_loss_signal(sim: Variant) -> void:
	if _session_lost_emitted or not sim.has_method("get_session_loss_reason"):
		return
	var reason := String(sim.get_session_loss_reason())
	if reason.is_empty():
		return
	_session_lost_emitted = true
	_world.session_lost.emit(reason)


# ESC/abort for the only interruptible load leg: the joiner's pre-load
# connect/session wait (the SP/host load remains one synchronous call the
# SceneTree cannot interrupt). Returns true when an in-flight preload was
# aborted; the ordinary load-failure leg reports it to the shell [orig: the
# "Mission loading aborted" early return of Client_CheckDisconnectOrEscDuringLoad
# @ 0x520270] (docs/interface/loading-screen-re.md D-LOADSCR-7).
func cancel_preload() -> bool:
	if _join_preload_sim == null:
		return false
	_fail_join_preload("Mission loading aborted")
	return true


## ESC/abort for the SECOND interruptible joiner wait: the post-load admission
## tail, where the map is loaded but the world stays hidden until the host drives
## the join to its deploy pick or in-match edge. D-LOADSCR-7's "single synchronous
## operation.call()" reasoning covers the host/SP map load, NOT this one --
## _watch_join_admission awaits process_frame every iteration, so the ESC window
## is as reachable here as it is in the pre-load connect wait. Without this a
## player who joins a host that stalls after the local load has no way out for the
## full JOIN_CONNECT_TIMEOUT_MS. Returns true when a live admission wait was told
## to abort; the watchdog reports it through the ordinary load-failure leg.
func cancel_admission_wait() -> bool:
	if not _join_admission_watch_active:
		return false
	_join_admission_abort = true
	return true


func _fail_join_preload(reason: String) -> void:
	_cancel_join_preload()
	_clear_pending_session()
	_world.load_failed.emit(reason)


func _cancel_join_preload() -> void:
	_join_preload_request_id += 1
	if _join_preload_sim != null:
		_join_preload_sim.free()
	_join_preload_sim = null
	_join_preload_root = null


# True between load_as_joiner and stage_runtime_options' config consume: this load is a
# co-op joiner, so dynamic objects render from the wire rather than from local placement.
func is_join_pending() -> bool:
	return _pending_join != null


## True while the staged host request is a DEDICATED serve (no local-player
## spawn, ADR 0015 serve mode); read by the world's playable gate before
## stage_runtime_options consumes the request.
func pending_dedicated() -> bool:
	return _pending_host != null and _pending_host.dedicated


## Spread the staged session request into the runtime's option dictionary, then
## clear the staging: the typed record (host_session/join_target), the derived
## _host_config keys, and the preload-sim surrender — the sim reference moves
## into opts["simulation"] and is cleared here so MissionRuntime remains the one
## adopter (ADR 0011/0012 ownership stays singular). Called once per load by the
## world's _start_runtime; opts must already carry "resource_root".
func stage_runtime_options(opts: Dictionary) -> void:
	# A LAN host start threads its typed session request (HostSessionConfig) through to the
	# listen server. A LAN JOINER threads its typed dial target (JoinTarget) and is NOT a
	# listen server (ADR 0017). Both are consumed once per load; absent for a normal
	# single-player start, which keeps the in-process (socketless) listen server.
	if _pending_host != null:
		opts["host_session"] = _pending_host
	elif _pending_join != null:
		opts["join_target"] = _pending_join
		opts["join_character_profile"] = _build_join_character_profile(
				opts.get("resource_root"), _spawn_loadout_cb.call())
	_pending_host = null
	_pending_join = null
	if not _host_config.is_empty():
		for k in ["server_name", "max_players", "game_type", "gametype", "net_transport", "bind_port",
				"advertise", "host_ip", "port", "player_name", "expansion",
				"nw_gate_host", "nw_gate_port", "region", "dedicated", "channel"]:
			if _host_config.has(k):
				opts[k] = _host_config[k]
		_host_config = {}
	# Consume the already-authenticated joiner. MissionRuntime adopts and frees
	# this off-tree Node like its usual freshly-created simulation; clearing our
	# reference before setup makes ownership singular even on a setup failure.
	if _join_preload_sim != null:
		opts["simulation"] = _join_preload_sim
		_join_preload_sim = null


## Post-runtime-start hook, called by the world once its runtime is live: the
## NovaWorld gate registration for a browsable listen host. (The joiner's
## admission watchdog arms inside this drive's own load entries, not here.)
func on_runtime_started(opts: Dictionary, bms_name: String) -> void:
	_maybe_start_nw_host(opts, bms_name)


## Per-frame observer, called from the world's tick after the runtime ticked:
## the admission/deploy/session-loss edges plus the gate's advertised occupancy.
func observe_tick(runtime) -> void:
	_update_joiner_admission_signals()
	# Keep the gate's advertised occupancy current (host + admitted joiners).
	# set_player_count self-dedupes, so this is a no-op until the count changes.
	if _nw_host != null and runtime.has_method("get_sim"):
		var sim = runtime.get_sim()
		if sim != null and sim.has_method("get_host_peer_count"):
			_nw_host.set_player_count(1 + sim.get_host_peer_count())


## One teardown for everything this drive staged or stood up, called from the
## world's unload(): the preload sim/root, the notification latches, the typed
## request staging, and the gate registration.
func reset() -> void:
	_cancel_join_preload()
	_join_admission_ready_emitted = false
	_join_deploy_signal_active = false
	_session_lost_emitted = false
	_clear_pending_session()
	# Gate registration teardown: tells the gate to drop the host row (ClientStopHosting).
	if _nw_host != null:
		_nw_host.stop()
		_nw_host.queue_free()
		_nw_host = null


# Register a browsable listen host with the NovaWorld gate (F1, ADR 0010). The host-direction
# sibling of the joiner's NovaWorldClient: it runs the NWU lobby handshake to the gate, then
# ClientHostRequest + ClientHostUpdate heartbeats so the host shows in /api/hosts + the retail
# server browser. Gated so it only fires for a real LAN listen server WITH a gate configured —
# single-player, joiners, and pure-LAN play (no nw_gate_host) all skip it, unchanged.
func _maybe_start_nw_host(opts: Dictionary, bms_name: String) -> void:
	if not bool(opts.get("listen_server", false)):
		return
	if String(opts.get("net_transport", "")) != "lan":
		return
	# Register only when the explicit NovaWorld host flow supplied a gate. The in-match wire is
	# shared, but the LAN menu path never reads or manufactures service configuration.
	var channel := String(opts.get("channel", "LAN"))
	var gate_host := String(opts.get("nw_gate_host", ""))
	if gate_host.is_empty():
		if channel == "NovaWorld":
			push_warning("NetSessionDrive: NovaWorld host requested but no gate address (nw_gate_host) — gate registration skipped; host is LAN-reachable only")
		return  # no gate configured -> pure LAN, nothing to register with
	if not ClassDB.class_exists("NovaWorldHost"):
		push_warning("NetSessionDrive: NovaWorldHost unavailable; host is LAN-only (not browsable)")
		return
	var runtime: Variant = _world.get_runtime()
	var sim = runtime.get_sim() if runtime != null and runtime.has_method("get_sim") else null
	if sim == null or not sim.has_method("is_host_listening") or not sim.is_host_listening():
		return  # the listen socket never came up; nothing reachable to advertise
	_nw_host = ClassDB.instantiate("NovaWorldHost")
	add_child(_nw_host)
	_nw_host.host = gate_host
	_nw_host.gate_port = int(opts.get("nw_gate_port", HostSessionConfig.DEFAULT_GATE_PORT))
	_nw_host.server_name = String(opts.get("server_name", "OpenNova Host"))
	_nw_host.mission_name = bms_name.get_basename()
	_nw_host.max_players = int(opts.get("max_players", 32))
	# The actually-bound game port the joiner will dial (not the requested bind_port).
	_nw_host.game_port = sim.get_host_listen_port() if sim.has_method("get_host_listen_port") else int(opts.get("bind_port", HostSessionConfig.DEFAULT_LAN_PORT))
	_nw_host.region = String(opts.get("region", "us"))
	_nw_host.player_name = String(opts.get("player_name", "Host"))
	var adv := String(opts.get("advertise", ""))
	if not adv.is_empty():
		_nw_host.advertise_ip = adv
	if _nw_host.has_signal("registered"):
		_nw_host.registered.connect(_on_nw_host_registered)
	if _nw_host.has_signal("error_occurred"):
		_nw_host.error_occurred.connect(_on_nw_host_error)
	_nw_host.start()


func _on_nw_host_registered() -> void:
	print_verbose("NetSessionDrive: listen host registered with the NovaWorld gate (browsable)")


func _on_nw_host_error(message: String) -> void:
	push_warning("NetSessionDrive: NovaWorld host registration error: %s" % message)
