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
# The POLICY of all of that — the two ConnectOrHost windows (0xEA60), the
# promote validation, the admission/deploy/loss edge machine with its latches,
# and the expansion reconcile DECISION — is native
# (engine/net/npruntime/join_session_policy.cpp, bound as
# NetSessionPolicy). This node keeps the signals and the lifetime: it reads
# simulation state, forwards it to the policy each frame, and executes exactly
# what the returned edges say — signal emission, the settle call, the preload
# sim pump, and the in-place mount switch.
#
# Engine-side and shell-neutral: no menu/env knowledge lives here (the shell's
# NetSessionController owns the entries; MainGame owns presentation). The
# session signals stay on GameWorld — the shell contract pins them there — so
# this drive emits them THROUGH its world reference
# (world.join_session_identified.emit(...)); there is no connect-and-re-emit
# hop. Both waits are synchronous state machines: this child node's _process
# steps pre-load admission, and GameWorld's world tick steps post-load
# admission. GameWorld constructs and adds the drive in _init, so its process
# step is available before load_as_joiner runs.

const ResourceDirSettings := preload("res://game/resource_index/resource_dir_settings.gd")
const MissionPresentation := preload("res://game/world/mission_presentation.gd")

# The GameWorld this drive loads through — its PUBLIC surface only
# (load_mission / mission_file / get_runtime) plus the session signals emitted
# through it; the world's private internals arrive as the setup() Callables
# below.
var _world: GameWorld
var _load_mission_internal_cb := Callable()  # (mission, bms_name, resource_root) -> int
var _resolve_root_cb := Callable()  # (dir) -> ResourceRoot (or null after load_failed)
var _spawn_loadout_cb := Callable()  # () -> Dictionary (the staged PLAYER_INFO snapshot)

# The native session policy: windows, latches, edge ordering, reason text.
var _policy := NetSessionPolicy.new()

# NovaWorldHost: registers a LAN/co-op listen host with the NovaWorld gate so a
# retail client can browse + join it (F1). Only created when a gate was supplied
# (via _host_config["nw_gate_host"]); absent for pure-LAN play. Fed the live
# player count from observe_tick(), torn down in reset().
var _nw_host: NovaWorldHost = null
var _host_config: Dictionary = {}  # internal staging derived from the typed request; consumed once by stage_runtime_options
# The typed session request at the shell seam (ADR 0017): exactly one is non-null
# during a net load — the host screen's HostSessionConfig or the joiner's dial
# JoinTarget — threaded to MissionPresentation as opts["host_session"]/opts["join_target"].
var _pending_host: HostSessionConfig = null
var _pending_join: JoinTarget = null
# A retail LAN join authenticates before the wire-header world load. This off-tree
# simulation owns that one live socket/session while S2C 0x7B supplies map_file;
# stage_runtime_options surrenders it so the connection is never restarted.
var _join_preload_sim: Simulation
var _join_preload_root: ResourceRoot


## One-time wiring from the owning GameWorld: the world reference the public
## calls + signal emissions go through, and the three private internals it
## lends as Callables (its _load_mission_internal, its _resolve_root, and a
## reader for its staged _local_player_spawn_loadout).
func setup(world: GameWorld, internal_load: Callable, resolve_root: Callable,
		spawn_loadout: Callable) -> void:
	_world = world
	_load_mission_internal_cb = internal_load
	_resolve_root_cb = resolve_root
	_spawn_loadout_cb = spawn_loadout
	set_process(false)


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
# two profile character selections packed from Avatars.def. The bit-pack lives
# at the engine home, engine/net/npwire session_hello.h character_id (bound as
# NetProtocol.pack_character_id), and the companion avatar byte is the selected
# combo's head voice unless the profile has an explicit override.
# [orig: PlayerProfile_InitDefaults @0x54BB40,
#  lookup_entity_slot_and_pack_entry @0x57AD40,
#  sub_57AE60 @0x57AE60, CNapiServerInfo_SerializeToSession @0x4C3650]
static func _join_character_selection(
		db: AvatarDatabase, nat_index: int, div_index: int,
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
	var packed_id := NetProtocol.pack_character_id(
			int(nat.get("id", 0)), int(div.get("id", 0)),
			int(combo.get("id", 0)), expected_alignment)
	var head: Dictionary = combo.get("head", {})
	return {
		"character_id": packed_id,
		"avatar": int(head.get("voice", 1)),
	}


static func _first_join_character_selection(
		db: AvatarDatabase, alignment: int) -> Dictionary:
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
		db: AvatarDatabase, selection: Dictionary = {}) -> Dictionary:
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
		resource_root: ResourceRoot, selection: Dictionary) -> Dictionary:
	if resource_root == null:
		return {}
	var db := AvatarDatabase.new()
	if db.load_from_resource_root(resource_root, "Avatars.def") != OK \
			or not db.is_loaded():
		push_warning("NetSessionDrive: Avatars.def not loaded for LAN join profile (%s)"
				% db.get_last_error())
		return {}
	return character_join_profile_from_database(db, selection)


## Load as a LAN co-op JOINER (a non-authority client). Every endpoint authenticates
## first, learns map_file from S2C 0x7B, and builds from the host's exact S2C 0x0B
## header + world stream on the SAME socket. `target.mission` is browse/debug display
## metadata only; it can never bypass the retail wire-driven load (D-NET-194).
## `target.player_name` rides ClientAuth and is echoed in our organic-spawn record.
func load_as_joiner(target: JoinTarget) -> int:
	if target == null:
		_world.load_failed.emit("join: no join target")
		return ERR_INVALID_PARAMETER
	_cancel_join_preload()
	_policy.reset_for_join()
	_pending_join = target
	# Internal staging for the option spread; the 0x7B promote below refreshes it
	# with the authoritative session record before the runtime consumes it.
	_host_config = {
		"net_transport": "lan-join",
		"host_ip": target.host_ip,
		"port": target.port,
		"player_name": target.player_name,
	}
	var resource_root: ResourceRoot = _resolve_root_cb.call(target.dir)
	if resource_root == null:
		_clear_pending_session()
		return ERR_CANT_OPEN
	_join_preload_sim = Simulation.new()
	# Retail builds g_CharAttr from the boot-soft charattr.def before any
	# network receive can deliver the 0x41 property clears or 0x39 challenge.
	# A missing file deliberately leaves the inactive all-zero table.
	_join_preload_sim.load_charattr_challenge(resource_root)
	_join_preload_sim.set_join_character_profile(
			_build_join_character_profile(
					resource_root, _spawn_loadout_cb.call()))
	if not target.integrity_profile.is_empty() and not \
			_join_preload_sim.set_join_integrity_profile(target.integrity_profile):
		_join_preload_sim.free()
		_join_preload_sim = null
		_clear_pending_session()
		_world.load_failed.emit("join: unknown integrity profile '%s'" % \
				target.integrity_profile)
		return ERR_INVALID_PARAMETER
	if not _join_preload_sim.enable_join(
			target.host_ip, target.port, target.player_name):
		_join_preload_sim.free()
		_join_preload_sim = null
		_clear_pending_session()
		_world.load_failed.emit("join: could not open the LAN session socket")
		return ERR_CANT_CONNECT
	_join_preload_sim.set_join_world_ready(false)
	# The JOIN VERSIONCRCSTRING checksum reads the loose
	# expansion/<name>/version.txt under this install root (D-NET-166).
	_join_preload_sim.set_join_expansion_version_root(resource_root.get_root_dir())
	_join_preload_root = resource_root
	_policy.arm_preload(Time.get_ticks_msec())
	set_process(true)
	return OK


# Drive one frame of the witnessed pre-world connect/session exchange while the
# loading screen is visible. The deadline is the retail ConnectOrHost window
# (0xEA60; the policy owns it). This node-owned process step must stay
# synchronous: awaiting here lets a freed GameWorld strand and later resume a
# method on this freed child.
func _process(_delta: float) -> void:
	_drive_join_preload_step()


func _drive_join_preload_step() -> void:
	if _join_preload_sim == null:
		set_process(false)
		return
	if not _join_preload_sim.is_join_preload_ready():
		_join_preload_sim.poll_join_preload()
		var step: int = _policy.preload_step(
				String(_join_preload_sim.get_join_error()), Time.get_ticks_msec())
		if step == NetSessionPolicy.STEP_FAIL:
			_fail_join_preload(_policy.fail_reason())
		return
	# Stop the frame pump before the synchronous load transfers or releases the
	# preload simulation. Every failure below owns its own cleanup/reporting leg.
	set_process(false)
	_policy.disarm_preload()
	# Reconcile the mount with the host's data set BEFORE any referenced assets are
	# resolved through it. The mission body itself comes from the host's world stream;
	# the mount supplies shared terrain/environment/model definitions (D-NET-178/194).
	if not _reconcile_join_expansion():
		return

	if not _policy.validate_promote_mission_file(
			String(_join_preload_sim.get_join_mission_file())):
		_fail_join_preload(_policy.fail_reason())
		return
	var bms := _policy.promoted_mission_file()
	var resource_root := _join_preload_root
	if resource_root == null:
		_fail_join_preload("join: resource root is unavailable after session identification")
		return
	# Retail does not open the advertised .bms on a joining client. The server
	# already copied its exact 0x268-byte header into S2C 0x0B, then streamed the
	# live world and optional mission .til overlay. Requiring the map locally made
	# retail-host -> OpenNova-client fail for custom maps even though the reverse
	# direction worked (D-NET-194).
	var wire_header := _join_preload_sim.get_join_mission_header()
	if not _policy.validate_promote_header(wire_header.size()):
		_fail_join_preload(_policy.fail_reason())
		return
	var mission := MissionData.new()
	if mission.open_wire_header(wire_header) != OK:
		_fail_join_preload("join: failed to parse host S2C 0x0B mission header for %s: %s" % [
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
	# Post-load joiner watchdog: the admission tail (C2S 0x0A -> world stream ->
	# loadout grants) is server-driven with no protocol-level timeout, so a
	# stalled or incompatible host would leave the player loaded but hidden
	# forever. The policy reuses the retail ConnectOrHost window from
	# world-ready and composes a stage-named failure — the reachable analog of
	# retail's post-load network-wait failure returns [orig:
	# NapiClient_WaitForGameStart @ 0x42cc10 failure legs -> "Mission loading
	# aborted"]. It covers only SERVER-owed transitions: at the player-paced
	# deployment pick the watchdog ends (retail's DEATH screen simply waits;
	# net-re 5.61).
	_policy.arm_admission_watch(Time.get_ticks_msec())


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
# The keep/remount/fail DECISION is native (np::decide_join_expansion); this
# executes it against the live mount. Returns false when the join has been
# failed and the driver must stop.
func _reconcile_join_expansion() -> bool:
	var resource_root := _join_preload_root
	if resource_root == null:
		return true
	var action: int = _policy.decide_expansion(
		String(_join_preload_sim.get_join_expansion()),
		String(resource_root.get_expansion()),
		resource_root.list_expansions(resource_root.get_root_dir()))
	if action == NetSessionPolicy.ACTION_KEEP:
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
	if action == NetSessionPolicy.ACTION_FAIL:
		_fail_join_preload(_policy.decision_error())
		return false
	var target_expansion := _policy.decided_expansion()
	var dir := resource_root.get_root_dir()
	var previous := String(resource_root.get_expansion())
	# Switch THIS root rather than swapping in a second one, the same in-place remount
	# MenuShell._apply_expansion does for the Mods screen: every holder (the menu shell, the
	# loading screen) is meant to move with it, and mount_runtime rebuilds the index and bumps
	# the cache epoch, so their caches self-clear. The persisted expansion setting is NOT
	# written — the host owns this session's data set, not the local menu choice. Same layering
	# as GameWorld._mount_runtime_root (see it for the flag rules); only the expansion differs.
	if resource_root.mount_runtime(dir, target_expansion, LaunchFlags.loose_override_enabled(),
			LaunchFlags.game(ResourceDirSettings.get_game())) != OK:
		# A hard mount failure clears the root, and the shell shares this object, so put the
		# previous expansion back before aborting to the menu (MenuShell._apply_expansion rolls
		# back the same way). The failure surfaces through the preload's abort leg rather than a
		# bare load_failed, so the live session is torn down too.
		var mount_error := String(resource_root.get_last_error())
		resource_root.mount_runtime(dir, previous, LaunchFlags.loose_override_enabled(),
			LaunchFlags.game(ResourceDirSettings.get_game()))
		_fail_join_preload("join: could not mount host expansion '%s' from %s: %s" % [
			target_expansion, dir, mount_error])
		return false
	# mount_runtime succeeds even when the expansion never layered (opennova::Vfs::mount_game
	# falls back to base game silently), so read back what ACTUALLY mounted. Without this the
	# abort leg above would be bypassed by a root that is quietly base game again. No rollback
	# here: unlike the hard failure above, the root holds a valid mount of whatever DID layer,
	# so the shell survives the abort on it.
	if String(resource_root.get_expansion()).to_lower() != target_expansion.to_lower():
		# The installed set is rendered by the same native helper the decision
		# leg uses ("none — base game only" when empty), so both abort reasons
		# read identically (one impl — engine/net/npruntime join_session_policy).
		_fail_join_preload("join: host runs expansion '%s' but %s mounted '%s' (installed: %s)" % [
			target_expansion, dir, String(resource_root.get_expansion()),
			NetSessionPolicy.describe_installed(resource_root.list_expansions(dir))])
		return false
	return true


# The per-frame admission/deploy/loss observer: read the joiner state, forward
# it to the native edge machine, execute what the returned flags say. The
# ordering, the latches, the windows, and every reason text live in the policy
# (loss wins over every admission edge; the deploy latch re-arms when pending
# clears; admission-ready is once per join and holds for the cold wire drain
# behind the loading hold [orig: the reap @ 0x4ca4a0 -> @ 0x4c63d0]).
func _update_joiner_admission_signals() -> void:
	var runtime: MissionPresentation = _world.get_runtime()
	if runtime == null:
		_policy.disarm_admission_watch()
		return
	var sim: Simulation = runtime.get_sim()
	if sim == null or not bool(sim.is_joiner()):
		_policy.disarm_admission_watch()
		return
	var loss_reason := String(sim.get_session_loss_reason())
	var deploy_pending := bool(sim.is_join_deploy_pick_pending())
	# The first valid S2C 0x5A opens retail's independent gameplay gate and can
	# make is_joined_in_match true BEFORE 0x0F supplies the deployment policy or
	# the second initial grant makes a required DEATH pick ready. Only the native
	# initial-admission boundary distinguishes that split ordering from a complete
	# no-pick join; later redeploys leave the predicate monotonically true.
	var initial_admission_complete := bool(sim.is_join_initial_admission_complete())
	# The join error + admission stage only matter to the armed watchdog — read
	# them exactly when the old inline machine did.
	var join_error := ""
	var admission_stage := ""
	if _policy.is_admission_watch_active():
		join_error = String(sim.get_join_error())
		admission_stage = String(sim.get_join_admission_stage())
	var pre: int = _policy.begin_admission_frame(loss_reason, deploy_pending,
			initial_admission_complete, join_error, admission_stage,
			Time.get_ticks_msec())
	if pre & NetSessionPolicy.EMIT_SESSION_LOST:
		# MainGame handles this signal synchronously and unloads the world. All
		# drive/policy mutation is already finished, so no owner method runs
		# after teardown.
		_world.session_lost.emit(_policy.session_loss_reason())
	if pre & NetSessionPolicy.FRAME_DONE:
		if pre & NetSessionPolicy.LOAD_FAILED:
			_world.load_failed.emit(_policy.fail_reason())
		return
	var settle_ok := true
	if pre & NetSessionPolicy.SETTLE_REQUIRED:
		settle_ok = _world.settle_join_wire_assets()
	# The revealed world must not race the budgeted cold wire materialization
	# (WirePresentPass.DEFAULT_COLD_SPAWN_BUDGET): the policy holds the no-pick
	# reveal — with its watchdog deadline still armed — until the presenter's
	# deferred-spawn queue drains behind the loading hold.
	var post: int = _policy.finish_admission_frame(
			settle_ok, _world.is_join_wire_present_drained())
	if post & NetSessionPolicy.SETTLE_FAILED:
		_world.report_join_wire_asset_failure(_policy.fail_reason())
		return
	if post & NetSessionPolicy.LOAD_FAILED:
		_world.load_failed.emit(_policy.fail_reason())
		return
	if post & NetSessionPolicy.EMIT_DEPLOY_PICK:
		_world.join_deploy_pick_required.emit()
	if post & NetSessionPolicy.EMIT_ADMISSION_READY:
		_world.join_admission_ready.emit()


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
## operation.call()" reasoning covers the host/SP map load, NOT this one -- the
## admission state machine is polled once per world tick, so the ESC window is
## as reachable here as it is in the pre-load connect wait. Without this a
## player who joins a host that stalls after the wire-header world load has no way out for the
## full ConnectOrHost window. Returns true when a live admission wait was told
## to abort; the watchdog reports it through the ordinary load-failure leg.
func cancel_admission_wait() -> bool:
	return _policy.request_admission_abort()


func _fail_join_preload(reason: String) -> void:
	_cancel_join_preload()
	_clear_pending_session()
	_world.load_failed.emit(reason)


func _cancel_join_preload() -> void:
	set_process(false)
	_policy.disarm_preload()
	if _join_preload_sim != null:
		_join_preload_sim.free()
	_join_preload_sim = null
	_join_preload_root = null


# True between load_as_joiner and stage_runtime_options' config consume: this load is a
# co-op joiner, so remote pool-0 rows render wire-direct while pools 1-3 use exact
# streamed native handles rather than local mission placement.
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
## into opts["simulation"] and is cleared here so MissionPresentation remains the one
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
	# Consume the already-authenticated joiner. MissionPresentation adopts and frees
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
func observe_tick(runtime: MissionPresentation) -> void:
	_update_joiner_admission_signals()
	# Keep the gate's advertised occupancy current (host + admitted joiners).
	# set_player_count self-dedupes, so this is a no-op until the count changes.
	if _nw_host != null and runtime != null:
		var sim: Simulation = runtime.get_sim()
		if sim != null:
			_nw_host.set_player_count(1 + sim.get_host_peer_count())


## One teardown for everything this drive staged or stood up, called from the
## world's unload(): the preload sim/root, the policy's windows + notification
## latches, the typed request staging, and the gate registration.
func reset() -> void:
	_cancel_join_preload()
	_policy.reset()
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
	var runtime: MissionPresentation = _world.get_runtime()
	var sim: Simulation = runtime.get_sim() if runtime != null else null
	if sim == null or not sim.is_host_listening():
		return  # the listen socket never came up; nothing reachable to advertise
	_nw_host = NovaWorldHost.new()
	add_child(_nw_host)
	_nw_host.host = gate_host
	_nw_host.gate_port = int(opts.get("nw_gate_port", HostSessionConfig.DEFAULT_GATE_PORT))
	_nw_host.server_name = String(opts.get("server_name", "OpenNova Host"))
	_nw_host.mission_name = bms_name.get_basename()
	_nw_host.max_players = int(opts.get("max_players", 32))
	# The actually-bound game port the joiner will dial (not the requested bind_port).
	_nw_host.game_port = sim.get_host_listen_port()
	_nw_host.region = String(opts.get("region", "us"))
	_nw_host.player_name = String(opts.get("player_name", "Host"))
	var adv := String(opts.get("advertise", ""))
	if not adv.is_empty():
		_nw_host.advertise_ip = adv
	_nw_host.registered.connect(_on_nw_host_registered)
	_nw_host.error_occurred.connect(_on_nw_host_error)
	_nw_host.start()


func _on_nw_host_registered() -> void:
	print_verbose("NetSessionDrive: listen host registered with the NovaWorld gate (browsable)")


func _on_nw_host_error(message: String) -> void:
	push_warning("NetSessionDrive: NovaWorld host registration error: %s" % message)
