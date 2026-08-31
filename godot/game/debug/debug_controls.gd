class_name DebugControls
extends RefCounted

const WeatherRows := preload("res://game/debug/debug_controls_weather_rows.gd")
## The typed debug-control table (ADR 0042 d5): every F3/MCP debug knob as one
## Row with typed read/write/invoke closures over the GameShellSeams suppliers
## and the Simulation typed API. Reflective StringName dispatch (the retired
## DebugSession/DebugCatalog/DebugTarget family) is gone: a row's closures
## resolve their live owner INSIDE the closure on every call, so a world
## reload is picked up without any retained object — and without any
## cross-mission replay: a fresh mission gets fresh debug state.
##
## Rows carry `owner`: "engine" rows end in a Simulation / Terrain / Weather /
## MissionEnvironment engine call; "device" rows mutate what exists only
## because a Godot viewport, node, or audio bus exists.
##
## This table shares the MCP boundary's zero-witness-cite floor
## (scripts/lint/ratchet_counts.py): a debug row that needs an [orig] cite is
## re-deriving engine behavior instead of calling it.

enum Kind {
	CHECK,
	SLIDER,
	ENUM,
	ACTION,
}

enum Authority {
	ANY,
	HOST_ONLY,
}

const OWNER_ENGINE := "engine"
const OWNER_DEVICE := "device"

# Wire-stable owner-surface names (the legacy "target" row key).
const TARGET_WORLD := "world"
const TARGET_PLAYER := "player"
const TARGET_SIM := "simulation"
const TARGET_TERRAIN := "terrain"
const TARGET_VIEWPORT := "viewport"
const TARGET_GAME_SHELL := "game_shell"
const TARGET_ENVIRONMENT := "environment"
const TARGET_WEATHER := "weather"

# The engine edit domains: mission coordinates ride signed 16.16 carriers
# (world/geom.h kMissionCoordMin/MaxUnits) and entity health the retail
# signed-16 storage (world/entity.h kRetailI16Min/Max).
static var MISSION_COORD_MIN: float = Simulation.mission_coord_min()
static var MISSION_COORD_MAX: float = Simulation.mission_coord_max()
const ENTITY_HEALTH_MIN := Simulation.ENTITY_HEALTH_MIN
const ENTITY_HEALTH_MAX := Simulation.ENTITY_HEALTH_MAX
const MISSION_VAR_COUNT := Simulation.MISSION_VAR_COUNT
const AUDIO_BUS_VOLUME_MIN_DB := -60.0
const AUDIO_BUS_VOLUME_MAX_DB := 6.0

const REASON_HOST_ONLY := "Only the session host can change authoritative game state."
const REASON_CONFIRM := "This control changes authoritative state; pass confirm_authority=true on an authority-owning session."

const VIEWPORT_DRAW_CHOICES: Array[String] = [
	"Normal",
	"Unshaded",
	"Lighting only",
	"Overdraw",
	"Wireframe",
	"Normal buffer",
	"Voxel GI albedo",
	"Voxel GI lighting",
	"Voxel GI emission",
	"Shadow atlas",
	"Directional shadow atlas",
	"Scene luminance",
	"SSAO",
	"SSIL",
	"PSSM splits",
	"Decal atlas",
	"SDFGI",
	"SDFGI probes",
	"GI buffer",
	"Disable LOD",
	"Cluster omni lights",
	"Cluster spot lights",
	"Cluster decals",
	"Cluster reflection probes",
	"Occluders",
	"Motion vectors",
	"Internal buffer",
]


## One typed control row. The closures are the whole contract: `read` returns
## the live value (null while the owner is away), `write` applies one
## normalized value, `invoke` runs one action over a validated argument
## Array. A row naming an owner method that does not exist fails at parse
## time — there is no soft "does not expose" state any more.
class Row:
	extends RefCounted

	var id: StringName
	var page: StringName
	var label: String
	var tooltip: String
	var kind := DebugControls.Kind.CHECK
	## Wire-stable owner-surface name (the legacy "target" row key).
	var target: String
	## "engine" (ends in a Simulation/Terrain/Weather/MissionEnvironment
	## engine call) or "device" (Godot viewport/node/audio-bus state).
	var owner: String = DebugControls.OWNER_DEVICE
	var default_value: Variant
	var minimum := 0.0
	var maximum := 1.0
	var step := 0.1
	var choices: Array[String] = []
	## Mutation needs the caller's per-call confirm_authority (the wire key
	## stays "requires_unlock"; the F3 Live-edits latch died with the
	## DebugSession family).
	var requires_confirm := false
	var authority := DebugControls.Authority.ANY
	## func() -> String: "" while the owner resolves, else the reason row.
	var availability := Callable()
	## func() -> Variant: the live value; null while the owner is away.
	var read := Callable()
	## func(value: Variant) -> Error: apply one normalized value.
	var write := Callable()
	## func(args: Array) -> Dictionary {"error": int, "result": Variant}.
	var invoke := Callable()

	## JSON-facing representation used by MCP catalog responses. The key set
	## is the legacy wire contract ("description" carries the tooltip,
	## "requires_unlock" the confirm gate).
	func to_json_value() -> Variant:
		return {
			"id": String(id),
			"page": String(page),
			"label": label,
			"description": tooltip,
			"kind": kind_name(),
			"target": target,
			"minimum": minimum,
			"maximum": maximum,
			"step": step,
			"choices": choices.duplicate(),
			"requires_unlock": requires_confirm,
			"authority": "host" \
					if authority == DebugControls.Authority.HOST_ONLY else "any",
		}

	func kind_name() -> String:
		match kind:
			DebugControls.Kind.CHECK:
				return "check"
			DebugControls.Kind.SLIDER:
				return "slider"
			DebugControls.Kind.ENUM:
				return "enum"
			DebugControls.Kind.ACTION:
				return "action"
		return "unknown"


var _seams: GameShellSeams = null
var _shell: GameDebugAdapter = null
var _rows: Dictionary = {}
var _order: Array[StringName] = []


## The shipping table is built over the shell's seams record and its debug
## adapter (audio buses, transport, viewport, authority). A test stub subclass
## may construct with neither and override the public surface.
func _init(seams: GameShellSeams = null, shell: GameDebugAdapter = null) -> void:
	_seams = seams
	_shell = shell
	if seams == null or shell == null:
		return
	_register_option_rows()
	_register_terrain_rows()
	_register_rendering_rows()
	_register_edit_actions()
	_register_audio_actions()
	_register_runtime_rows()
	_register_automation_actions()
	_register_spectator_row()


func control(id: StringName) -> Row:
	return _rows.get(id) as Row


func row_ids() -> Array[StringName]:
	return _order.duplicate()


## Break the Row -> closure -> DebugControls ownership cycle before the game
## shell is destroyed. RefCounted does not collect cycles, so merely dropping
## the adapter's _controls reference leaves every row and bound owner alive.
func clear() -> void:
	for value in _rows.values():
		var row := value as Row
		if row == null:
			continue
		row.availability = Callable()
		row.read = Callable()
		row.write = Callable()
		row.invoke = Callable()
	_rows.clear()
	_order.clear()
	_shell = null
	_seams = null


## JSON-safe definitions paired with a live state, suitable for MCP.
## `allow_authority` mirrors the write path's per-call confirmation: rows
## report writability for THAT caller, so an MCP client holding
## confirm_authority is not told its own successful writes are locked.
func list_controls(
		page_id: StringName = &"",
		filter_text: String = "",
		allow_authority: bool = false) -> Array[Dictionary]:
	var output: Array[Dictionary] = []
	var needle := filter_text.strip_edges().to_lower()
	for id in _order:
		var row := control(id)
		if page_id != &"" and row.page != page_id:
			continue
		if not needle.is_empty():
			var haystack := ("%s %s %s %s" % [
				row.id, row.page, row.label, row.tooltip]).to_lower()
			if not haystack.contains(needle):
				continue
		var json: Dictionary = row.to_json_value()
		json["state"] = get_control_state(id, allow_authority).to_json_value()
		output.append(json)
	return output


func get_control_state(
		id: StringName,
		allow_authority: bool = false) -> DebugControlState:
	var state := DebugControlState.new()
	state.id = id
	var row := control(id)
	if row == null:
		state.reason = "Unknown debug control."
		return state
	state.kind = row.kind
	state.desired_value = row.default_value
	var reason := String(row.availability.call())
	if not reason.is_empty():
		state.value = row.default_value
		state.reason = reason
		return state
	state.available = true
	state.writable = _write_allowed(row, allow_authority)
	if not state.writable:
		state.reason = _policy_reason(row, allow_authority)
	if row.kind == Kind.ACTION:
		return state
	state.value = row.read.call()
	state.desired_value = state.value
	state.authoritative = true
	return state


func set_control_value(
		id: StringName,
		value: Variant,
		allow_authority: bool = false) -> Error:
	var row := control(id)
	if row == null:
		return ERR_DOES_NOT_EXIST
	if row.kind == Kind.ACTION:
		return ERR_INVALID_PARAMETER
	var normalized := normalize_value(row, value)
	if not bool(normalized["ok"]):
		return ERR_INVALID_PARAMETER
	if not _write_allowed(row, allow_authority):
		return ERR_UNAUTHORIZED
	var applied: Error = row.write.call(normalized["value"])
	return applied


## Actions accept null (no arguments), an Array, or one scalar argument.
## Returns a JSON-safe result record for automation.
func invoke_control(
		id: StringName,
		args: Variant = null,
		allow_authority: bool = false) -> Dictionary:
	var row := control(id)
	if row == null:
		return _invoke_result(ERR_DOES_NOT_EXIST, null, id, allow_authority)
	if row.kind != Kind.ACTION:
		var error := set_control_value(id, args, allow_authority)
		return _invoke_result(error, null, id, allow_authority)
	if not _write_allowed(row, allow_authority):
		return _invoke_result(ERR_UNAUTHORIZED, null, id, allow_authority)
	var call_args: Array = []
	if args is Array:
		call_args = args
	elif args != null:
		call_args = [args]
	var outcome: Dictionary = row.invoke.call(call_args)
	var action_error: Error = int(outcome["error"])
	return _invoke_result(
			action_error,
			outcome.get("result"),
			id,
			allow_authority)


## JSON-facing snapshot; typed controls are serialized only at this boundary.
## "edit_unlocked" is wire-stable: the F3 Live-edits latch died with the
## DebugSession family, so it reports false forever.
func capture_snapshot(
		filter_text: String = "",
		allow_authority: bool = false) -> Variant:
	var runtime: Variant = {}
	if _shell != null:
		runtime = DebugControlState._json_value(_shell.runtime_status())
	return {
		"runtime": runtime,
		"edit_unlocked": false,
		"controls": list_controls(&"", filter_text, allow_authority),
	}


## Kind-typed value normalization (the write path's argument check): CHECK
## takes exactly a bool, SLIDER a finite number clamped and snapped to the
## row's domain, ENUM a valid choice index.
static func normalize_value(row: Row, value: Variant) -> Dictionary:
	match row.kind:
		Kind.CHECK:
			if typeof(value) != TYPE_BOOL:
				return {"ok": false}
			return {"ok": true, "value": bool(value)}
		Kind.SLIDER:
			if typeof(value) != TYPE_INT and typeof(value) != TYPE_FLOAT:
				return {"ok": false}
			var number := float(value)
			if not is_finite(number):
				return {"ok": false}
			number = clampf(number, row.minimum, row.maximum)
			if row.step > 0.0:
				number = snappedf(number, row.step)
			return {"ok": true, "value": number}
		Kind.ENUM:
			if typeof(value) != TYPE_INT:
				return {"ok": false}
			var index := int(value)
			if index < 0 or index >= row.choices.size():
				return {"ok": false}
			return {"ok": true, "value": index}
	return {"ok": false}


func _write_allowed(row: Row, allow_authority: bool) -> bool:
	if row.requires_confirm and not allow_authority:
		return false
	if row.authority == Authority.HOST_ONLY and not _has_host_authority():
		return false
	return true


func _policy_reason(row: Row, allow_authority: bool) -> String:
	if row.authority == Authority.HOST_ONLY and not _has_host_authority():
		return REASON_HOST_ONLY
	if row.requires_confirm and not allow_authority:
		return REASON_CONFIRM
	return ""


## Authority is the session-role fact the adapter reads from
## Simulation.session_role(): ROLE_JOINER is the one non-authoritative role.
func _has_host_authority() -> bool:
	return _shell != null and _shell.has_debug_authority()


func _invoke_result(
		error: Error,
		result: Variant,
		id: StringName,
		allow_authority: bool = false) -> Dictionary:
	return {
		"error": int(error),
		"result": DebugControlState._json_value(result),
		"state": get_control_state(id, allow_authority).to_json_value(),
	}


static func _action_error(error: Error) -> Dictionary:
	return {"error": int(error), "result": null}


static func _action_result(result: Variant) -> Dictionary:
	return {"error": int(OK), "result": result}


# --- row registration --------------------------------------------------------


func _register(row: Row) -> void:
	assert(row.id != &"" and not _rows.has(row.id))
	_rows[row.id] = row
	_order.append(row.id)


func _check(
		id: StringName,
		page: StringName,
		label: String,
		tooltip: String,
		target: String,
		owner_kind: String,
		default_on := false) -> Row:
	var row := Row.new()
	row.id = id
	row.page = page
	row.label = label
	row.tooltip = tooltip
	row.kind = Kind.CHECK
	row.target = target
	row.owner = owner_kind
	row.default_value = default_on
	row.availability = _availability_for(target)
	_register(row)
	return row


func _slider(
		id: StringName,
		page: StringName,
		label: String,
		tooltip: String,
		target: String,
		owner_kind: String,
		default: float,
		minimum: float,
		maximum: float,
		step: float) -> Row:
	var row := Row.new()
	row.id = id
	row.page = page
	row.label = label
	row.tooltip = tooltip
	row.kind = Kind.SLIDER
	row.target = target
	row.owner = owner_kind
	row.default_value = default
	row.minimum = minimum
	row.maximum = maximum
	row.step = step
	row.availability = _availability_for(target)
	_register(row)
	return row


func _enum(
		id: StringName,
		page: StringName,
		label: String,
		tooltip: String,
		target: String,
		owner_kind: String,
		default: int,
		choices: Array[String]) -> Row:
	var row := Row.new()
	row.id = id
	row.page = page
	row.label = label
	row.tooltip = tooltip
	row.kind = Kind.ENUM
	row.target = target
	row.owner = owner_kind
	row.default_value = default
	row.choices = choices.duplicate()
	row.availability = _availability_for(target)
	_register(row)
	return row


func _action(
		id: StringName,
		page: StringName,
		label: String,
		tooltip: String,
		target: String,
		owner_kind: String) -> Row:
	var row := Row.new()
	row.id = id
	row.page = page
	row.label = label
	row.tooltip = tooltip
	row.kind = Kind.ACTION
	row.target = target
	row.owner = owner_kind
	row.availability = _availability_for(target)
	_register(row)
	return row


func _authoritative(row: Row) -> void:
	row.requires_confirm = true
	row.authority = Authority.HOST_ONLY


## A CHECK row over one live-resolved GameWorld toggle: the pickers are
## typed closures over the resolved world, so a missing method fails at
## parse time and the owner is re-resolved per call.
func _world_check(
		id: StringName,
		page: StringName,
		label: String,
		tooltip: String,
		get_value: Callable,
		set_value: Callable) -> Row:
	var row := _check(id, page, label, tooltip, TARGET_WORLD, OWNER_DEVICE)
	row.read = func() -> Variant:
		var world := _world()
		return get_value.call(world) if world != null else null
	row.write = func(value: Variant) -> Error:
		var world := _world()
		if world == null:
			return ERR_UNAVAILABLE
		set_value.call(world, bool(value))
		return OK
	return row


func _player_check(
		id: StringName,
		page: StringName,
		label: String,
		tooltip: String,
		get_value: Callable,
		set_value: Callable) -> Row:
	var row := _check(id, page, label, tooltip, TARGET_PLAYER, OWNER_DEVICE)
	row.read = func() -> Variant:
		var player := _player()
		return get_value.call(player) if player != null else null
	row.write = func(value: Variant) -> Error:
		var player := _player()
		if player == null:
			return ERR_UNAVAILABLE
		set_value.call(player, bool(value))
		return OK
	return row


func _terrain_check(
		id: StringName,
		label: String,
		tooltip: String,
		get_value: Callable,
		set_value: Callable) -> Row:
	var row := _check(id, &"Terrain", label, tooltip, TARGET_TERRAIN, OWNER_ENGINE)
	row.read = func() -> Variant:
		var terrain := _terrain()
		return get_value.call(terrain) if terrain != null else null
	row.write = func(value: Variant) -> Error:
		var terrain := _terrain()
		if terrain == null:
			return ERR_UNAVAILABLE
		set_value.call(terrain, bool(value))
		return OK
	return row


## The world-overlay and player-camera debug toggles (the retired
## DebugOptions registry, now typed device rows). Without a live world they
## report unavailable — there is no recorded intent and no replay.
func _register_option_rows() -> void:
	_world_check(&"show_skeletons", &"Animation", "Show skeletons",
			"Draw character bones (joint-to-parent lines + axis crosses) over the world.",
			func(world: GameWorld) -> bool: return world.is_skeleton_debug(),
			func(world: GameWorld, on: bool) -> void: world.set_skeleton_debug(on))
	_world_check(&"show_user_points", &"Animation", "Show user points",
			"Draw every named model user point as a cyan marker + label, following live animated bones and including static-batched mission objects.",
			func(world: GameWorld) -> bool: return world.is_user_point_debug(),
			func(world: GameWorld, on: bool) -> void: world.set_user_point_debug(on))
	_world_check(&"show_collision", &"Rounds", "Show collision",
			"Draw object collision volumes (type-colored boxes) and the player's capsule test points over the world.",
			func(world: GameWorld) -> bool: return world.is_collision_debug(),
			func(world: GameWorld, on: bool) -> void: world.set_collision_debug(on))
	_world_check(&"hide_foliage", &"Terrain", "Hide foliage",
			"Hide the scattered vegetation (grass / bushes / trees) to see the terrain under it.",
			func(world: GameWorld) -> bool: return world.is_foliage_hidden(),
			func(world: GameWorld, on: bool) -> void: world.set_foliage_hidden(on))
	_world_check(&"hide_particles", &"Particles", "Hide particles",
			"Hide every particle effect (the retail master particle switch) — flip it to check whether an artifact is particles at all.",
			func(world: GameWorld) -> bool: return world.is_particles_hidden(),
			func(world: GameWorld, on: bool) -> void: world.set_particles_hidden(on))
	_world_check(&"show_effect_boxes", &"Particles", "Show effect boxes",
			"Draw a red wireframe box (retail's debug box color) and effect name over every live emitter. Missing textures stay in the Particles catalog issues report.",
			func(world: GameWorld) -> bool: return world.is_particle_debug(),
			func(world: GameWorld, on: bool) -> void: world.set_particle_debug(on))
	_world_check(&"show_portal_faces", &"Occlusion", "Show portal faces",
			"Draw every nearby building's occlusion faces over the world — windows, portals and welded links as colored outlines with section labels, plain occluder faces in gray.",
			func(world: GameWorld) -> bool: return world.is_occlusion_debug(),
			func(world: GameWorld, on: bool) -> void: world.set_occlusion_debug(on))
	_world_check(&"show_round_trails", &"Rounds", "Show round trails",
			"Draw the recent round outcomes over the world — flight segments and hit markers colored by result (green = face hit, amber = sphere stand-in, red ring = a graze whose face test missed and flew on).",
			func(world: GameWorld) -> bool: return world.is_round_debug(),
			func(world: GameWorld, on: bool) -> void: world.set_round_debug(on))
	_world_check(&"show_hit_meshes", &"Rounds", "Show hit meshes",
			"Hit geometry is sampled at 6 Hz. Draw nearby hit geometry within 80 mission units of the local player: object bullet meshes and broad-phase spheres, plus posed person bone spheres (local player omitted; up to 96 targets). Person colors show normal-infantry damage zones: orange = x1.25 (0-4), cyan = x1.0 (5-8), lime = x0.5 (9-12/15-18), magenta = x3.0 head (13-14), dark red = masked, amber = unresolved fallback.",
			func(world: GameWorld) -> bool: return world.is_hitbox_debug(),
			func(world: GameWorld, on: bool) -> void: world.set_hitbox_debug(on))
	_player_check(&"force_fp_arms", &"Player", "Always draw FP arms",
			"Keep the first-person arms + weapon drawn in every camera mode (debug experiment).",
			func(player: LocalPlayerPresenter) -> bool: return player.is_debug_force_viewmodel(),
			func(player: LocalPlayerPresenter, on: bool) -> void: player.set_debug_force_viewmodel(on))
	_player_check(&"body_in_first_person", &"Player", "Show body in first person",
			"Draw your own body in first person — look down to see your legs and feet (debug experiment; expect the head/shoulders to clip the camera).",
			func(player: LocalPlayerPresenter) -> bool: return player.is_debug_body_in_first_person(),
			func(player: LocalPlayerPresenter, on: bool) -> void: player.set_debug_body_in_first_person(on))
	_player_check(&"third_person_on_foot", &"Player", "Third person on foot",
			"Force the chase camera while on foot. Stock JO only resolves third person in a vehicle control seat with Chase View (F4) selected — the per-frame arbiter, net-re §5.39 — so this is the onhook debug patch's affordance, not a gameplay key.",
			func(player: LocalPlayerPresenter) -> bool: return player.is_debug_third_person(),
			func(player: LocalPlayerPresenter, on: bool) -> void: player.set_debug_third_person(on))


func _register_spectator_row() -> void:
	var spectator := _check(&"local_spectator", &"Player", "Spectator free camera",
			"Detach the authority-owned local player from gameplay and unlock the free camera while the match continues ticking.",
			TARGET_SIM, OWNER_ENGINE)
	spectator.read = func() -> Variant:
		var sim := _sim()
		return sim.is_local_spectator() if sim != null else null
	spectator.write = func(value: Variant) -> Error:
		var sim := _sim()
		if sim == null:
			return ERR_UNAVAILABLE
		return OK if sim.set_local_spectator(bool(value)) else ERR_UNAUTHORIZED
	_authoritative(spectator)


func _register_terrain_rows() -> void:
	var draw_mode := _enum(&"terrain_draw_mode", &"Terrain", "Draw mode",
			"Color terrain by renderer decisions instead of textures.",
			TARGET_TERRAIN, OWNER_ENGINE, 0,
			["Normal", "Detail levels", "Sector colors", "Surface angle", "Height map"])
	draw_mode.read = func() -> Variant:
		var terrain := _terrain()
		return terrain.get_debug_mode() if terrain != null else null
	draw_mode.write = func(value: Variant) -> Error:
		var terrain := _terrain()
		if terrain == null:
			return ERR_UNAVAILABLE
		terrain.set_debug_mode(int(value))
		return OK

	var lod := _slider(&"terrain_lod_quality", &"Terrain", "Terrain detail",
			"Scale how aggressively terrain refines toward the camera.",
			TARGET_TERRAIN, OWNER_ENGINE, 1.0, 0.1, 4.0, 0.1)
	lod.read = func() -> Variant:
		var terrain := _terrain()
		return terrain.get_lod_quality() if terrain != null else null
	lod.write = func(value: Variant) -> Error:
		var terrain := _terrain()
		if terrain == null:
			return ERR_UNAVAILABLE
		terrain.set_lod_quality(float(value))
		return OK

	_terrain_check(&"terrain_no_frustum", "Disable all culling",
			"Show terrain patches that the camera would normally reject.",
			func(terrain: Terrain) -> bool: return terrain.get_debug_no_frustum(),
			func(terrain: Terrain, on: bool) -> void: terrain.set_debug_no_frustum(on))
	_terrain_check(&"terrain_no_nearfar", "Disable distance culling",
			"Ignore the camera's near and far terrain planes.",
			func(terrain: Terrain) -> bool: return terrain.get_debug_no_nearfar(),
			func(terrain: Terrain, on: bool) -> void: terrain.set_debug_no_nearfar(on))
	_terrain_check(&"terrain_no_sideplanes", "Disable side-plane culling",
			"Ignore the left, right, top and bottom terrain planes.",
			func(terrain: Terrain) -> bool: return terrain.get_debug_no_sideplanes(),
			func(terrain: Terrain, on: bool) -> void: terrain.set_debug_no_sideplanes(on))
	_terrain_check(&"terrain_no_partial_subdiv", "Disable partial subdivision",
			"Require complete terrain subdivision decisions.",
			func(terrain: Terrain) -> bool: return terrain.get_debug_no_partial_subdiv(),
			func(terrain: Terrain, on: bool) -> void: terrain.set_debug_no_partial_subdiv(on))
	_terrain_check(&"terrain_force_leaves", "Force leaf patches",
			"Render only terrain quadtree leaves.",
			func(terrain: Terrain) -> bool: return terrain.get_debug_force_leaves(),
			func(terrain: Terrain, on: bool) -> void: terrain.set_debug_force_leaves(on))
	_terrain_check(&"terrain_force_lod0", "Force highest detail",
			"Force terrain patches to the highest available detail level.",
			func(terrain: Terrain) -> bool: return terrain.get_debug_force_lod0(),
			func(terrain: Terrain, on: bool) -> void: terrain.set_debug_force_lod0(on))


func _register_rendering_rows() -> void:
	var draw_mode := _enum(&"viewport_debug_draw", &"Rendering", "Viewport view",
			"Show the renderer's built-in diagnostic buffers.",
			TARGET_VIEWPORT, OWNER_DEVICE, 0, VIEWPORT_DRAW_CHOICES)
	draw_mode.read = func() -> Variant:
		var viewport := _viewport()
		return int(viewport.debug_draw) if viewport != null else null
	draw_mode.write = func(value: Variant) -> Error:
		var viewport := _viewport()
		if viewport == null:
			return ERR_UNAVAILABLE
		viewport.debug_draw = int(value) as Viewport.DebugDraw
		return OK
	_world_check(&"occlusion_culling", &"Rendering", "Godot occlusion culling",
			"Run Godot's occluder pass over the world viewport: the conservative second layer under the retail portal verdict, off by default (it cost ~0.6 ms of render CPU per frame and culled nothing at the measured poses). It only has something to cull with while the loaded mission placed authored OOBJ occluders (game_render_diagnostics mission_placement.authored_occluder_models counts them). Flip it to weigh the pass against what it culls (render-occlusion-re.md, Conservative device occluders).",
			func(world: GameWorld) -> bool: return world.is_occlusion_culling_enabled(),
			func(world: GameWorld, on: bool) -> void: world.set_occlusion_culling_enabled(on))


func _register_edit_actions() -> void:
	var teleport := _action(&"teleport_local_player", &"Player", "Teleport player",
			"Move the local player to a mission-space position.",
			TARGET_SIM, OWNER_ENGINE)
	_authoritative(teleport)
	teleport.invoke = func(args: Array) -> Dictionary:
		var sim := _sim()
		if sim == null:
			return _action_error(ERR_UNAVAILABLE)
		if not DebugControlArgs.teleport(args):
			return _action_error(ERR_INVALID_PARAMETER)
		return _action_error(sim.debug_teleport_local_player(
				args[0], float(args[1]), float(args[2])))

	var map_cycle := _action(&"cycle_map_mode", &"Player", "Cycle map mode",
			"Step the M-key map cycle: off -> window -> fullscreen -> off.",
			TARGET_SIM, OWNER_ENGINE)
	map_cycle.requires_confirm = true
	map_cycle.invoke = func(_args: Array) -> Dictionary:
		var sim := _sim()
		if sim == null:
			return _action_error(ERR_UNAVAILABLE)
		return _action_result(sim.request_hud_map_cycle())

	var health := _action(&"set_entity_health", &"Entities", "Set health",
			"Set the selected simulation entity's health.",
			TARGET_SIM, OWNER_ENGINE)
	_authoritative(health)
	health.invoke = func(args: Array) -> Dictionary:
		var sim := _sim()
		if sim == null:
			return _action_error(ERR_UNAVAILABLE)
		if not DebugControlArgs.health(args):
			return _action_error(ERR_INVALID_PARAMETER)
		return _action_error(sim.debug_set_entity_health(
				int(args[0]), int(args[1])))

	var position := _action(&"set_entity_position", &"Entities", "Move entity",
			"Move the selected simulation entity to a mission-space position.",
			TARGET_SIM, OWNER_ENGINE)
	_authoritative(position)
	position.invoke = func(args: Array) -> Dictionary:
		var sim := _sim()
		if sim == null:
			return _action_error(ERR_UNAVAILABLE)
		if not DebugControlArgs.entity_position(args):
			return _action_error(ERR_INVALID_PARAMETER)
		return _action_error(sim.debug_set_entity_position(
				int(args[0]), args[1]))

	var item_attrib := _action(&"set_entity_item_attrib", &"Entities", "Set item attribs",
			"Write both items.def attrib words on one entity by its wire_handle (brainless "
			+ "rows included); a per-entity override the next item-traits sweep re-stamps.",
			TARGET_SIM, OWNER_ENGINE)
	_authoritative(item_attrib)
	item_attrib.invoke = func(args: Array) -> Dictionary:
		var sim := _sim()
		if sim == null:
			return _action_error(ERR_UNAVAILABLE)
		if not DebugControlArgs.item_attrib(args):
			return _action_error(ERR_INVALID_PARAMETER)
		return _action_error(sim.debug_set_entity_item_attrib(
				int(args[0]), int(args[1]), int(args[2])))


func _register_audio_actions() -> void:
	var volume := _action(&"set_audio_bus_volume", &"Audio", "Set bus volume",
			"Set one named audio bus volume in decibels.",
			TARGET_GAME_SHELL, OWNER_DEVICE)
	volume.invoke = func(args: Array) -> Dictionary:
		if _shell == null:
			return _action_error(ERR_UNAVAILABLE)
		if not DebugControlArgs.audio_bus_volume(args):
			return _action_error(ERR_INVALID_PARAMETER)
		return _action_error(_shell.debug_set_audio_bus_volume(
				String(args[0]), float(args[1])))

	var mute := _action(&"set_audio_bus_mute", &"Audio", "Set bus mute",
			"Set one named audio bus mute state.",
			TARGET_GAME_SHELL, OWNER_DEVICE)
	mute.invoke = func(args: Array) -> Dictionary:
		if _shell == null:
			return _action_error(ERR_UNAVAILABLE)
		if not DebugControlArgs.audio_bus_switch(args):
			return _action_error(ERR_INVALID_PARAMETER)
		return _action_error(_shell.debug_set_audio_bus_mute(
				String(args[0]), bool(args[1])))

	var solo := _action(&"set_audio_bus_solo", &"Audio", "Set bus solo",
			"Set one named audio bus solo state.",
			TARGET_GAME_SHELL, OWNER_DEVICE)
	solo.invoke = func(args: Array) -> Dictionary:
		if _shell == null:
			return _action_error(ERR_UNAVAILABLE)
		if not DebugControlArgs.audio_bus_switch(args):
			return _action_error(ERR_INVALID_PARAMETER)
		return _action_error(_shell.debug_set_audio_bus_solo(
				String(args[0]), bool(args[1])))

	var bypass := _action(&"set_audio_bus_bypass", &"Audio", "Set bus effect bypass",
			"Set one named audio bus effect bypass state.",
			TARGET_GAME_SHELL, OWNER_DEVICE)
	bypass.invoke = func(args: Array) -> Dictionary:
		if _shell == null:
			return _action_error(ERR_UNAVAILABLE)
		if not DebugControlArgs.audio_bus_switch(args):
			return _action_error(ERR_INVALID_PARAMETER)
		return _action_error(_shell.debug_set_audio_bus_bypass(
				String(args[0]), bool(args[1])))


func _register_runtime_rows() -> void:
	var transport := _action(&"runtime_transport", &"Sim", "Runtime transport",
			"Resume, pause, or single-step the real game runtime.",
			TARGET_GAME_SHELL, OWNER_ENGINE)
	_authoritative(transport)
	transport.invoke = func(args: Array) -> Dictionary:
		if _shell == null:
			return _action_error(ERR_UNAVAILABLE)
		if not DebugControlArgs.transport(args):
			return _action_error(ERR_INVALID_PARAMETER)
		return _action_error(_shell.mcp_game_control(String(args[0])))

	var return_to_menu := _action(&"runtime_return_to_menu", &"Sim", "Return to menu",
			"Leave the current world locally and return to the game menu.",
			TARGET_GAME_SHELL, OWNER_DEVICE)
	return_to_menu.invoke = func(_args: Array) -> Dictionary:
		if _shell == null:
			return _action_error(ERR_UNAVAILABLE)
		return _action_error(_shell.debug_return_to_menu())

	var scripts_paused := _check(&"runtime_wac_paused", &"Sim", "Pause mission scripts",
			"Pause WAC scripts while the rest of the world continues.",
			TARGET_SIM, OWNER_ENGINE)
	_authoritative(scripts_paused)
	scripts_paused.read = func() -> Variant:
		var sim := _sim()
		return sim.is_wac_paused() if sim != null else null
	scripts_paused.write = func(value: Variant) -> Error:
		var sim := _sim()
		if sim == null:
			return ERR_UNAVAILABLE
		sim.set_wac_paused(bool(value))
		return OK

	var mission_variable := _action(&"set_mission_variable", &"Vars", "Set mission variable",
			"Set one live V0..V511 mission-script variable.",
			TARGET_SIM, OWNER_ENGINE)
	_authoritative(mission_variable)
	mission_variable.invoke = func(args: Array) -> Dictionary:
		var sim := _sim()
		if sim == null:
			return _action_error(ERR_UNAVAILABLE)
		if not DebugControlArgs.mission_variable(args):
			return _action_error(ERR_INVALID_PARAMETER)
		sim.set_mission_variable(int(args[0]), int(args[1]))
		return _action_result(null)

	var time_of_day := _slider(&"environment_time_of_day", &"Environment", "Time of day",
			"Scrub the mission clock by minute of day (0 = 00:00, 1439 = 23:59).",
			TARGET_ENVIRONMENT, OWNER_ENGINE, 720.0, 0.0, 1439.0, 1.0)
	_authoritative(time_of_day)
	time_of_day.read = func() -> Variant:
		var world := _world()
		return world.get_debug_mission_minute_of_day() if world != null else null
	time_of_day.write = func(value: Variant) -> Error:
		var world := _world()
		if world == null:
			return ERR_UNAVAILABLE
		return world.debug_set_mission_minute_of_day(float(value))

	var wind := _slider(&"environment_wind_strength", &"Environment", "Wind strength",
			"Scale the wind driving foliage sway and weather gusts.",
			TARGET_WEATHER, OWNER_ENGINE, 100.0, 0.0, 100.0, 1.0)
	_authoritative(wind)
	wind.read = func() -> Variant:
		var weather := _weather()
		return weather.get_wind_strength() if weather != null else null
	wind.write = func(value: Variant) -> Error:
		var weather := _weather()
		if weather == null:
			return ERR_UNAVAILABLE
		weather.set_wind_strength(float(value))
		return OK

	var lightning_short := _action(&"environment_lightning_short", &"Environment", "Lightning (short)",
			"Trigger the weather controller's short lightning strike.",
			TARGET_WEATHER, OWNER_ENGINE)
	_authoritative(lightning_short)
	lightning_short.invoke = func(_args: Array) -> Dictionary:
		var weather := _weather()
		if weather == null:
			return _action_error(ERR_UNAVAILABLE)
		weather.trigger_lightning_short()
		return _action_result(null)

	var lightning_long := _action(&"environment_lightning_long", &"Environment", "Lightning (long)",
			"Trigger the weather controller's long lightning strike.",
			TARGET_WEATHER, OWNER_ENGINE)
	_authoritative(lightning_long)
	lightning_long.invoke = func(_args: Array) -> Dictionary:
		var weather := _weather()
		if weather == null:
			return _action_error(ERR_UNAVAILABLE)
		weather.trigger_lightning_long()
		return _action_result(null)

	# The Environment rows (the WAC weather commands + the weather-home read)
	# live in debug_controls_weather_rows.gd; they register here so the table
	# order (the wire ids the MCP catalog test pins) is unchanged.
	WeatherRows.register(self)



## The controls scripted runs drive over MCP in place of the retired NW_*
## environment hooks (ADR 0041): the deploy pick, the viewmodel A/B rig, the
## joiner diagnostics trace, and the one-shot entity mutations the AI probe
## used to read from its env console.
func _register_automation_actions() -> void:
	var deploy_pick := _action(&"deploy_pick", &"Sim", "Deploy pick",
			"Send one deployment pick (0 = the Default Spawn) while the deploy screen is owed; the host silently drops invalid or contested picks.",
			TARGET_SIM, OWNER_ENGINE)
	deploy_pick.requires_confirm = true
	deploy_pick.invoke = func(args: Array) -> Dictionary:
		var sim := _sim()
		if sim == null:
			return _action_error(ERR_UNAVAILABLE)
		if not DebugControlArgs.deploy_pick(args):
			return _action_error(ERR_INVALID_PARAMETER)
		return _action_result(sim.send_deployment_pick(int(args[0])))

	var viewmodel := _action(&"set_viewmodel_weapon", &"Player", "Set viewmodel weapon",
			"Rig the first-person viewmodel and action FSM to a weapon.def name (A/B against another SKU's def).",
			TARGET_WORLD, OWNER_DEVICE)
	viewmodel.requires_confirm = true
	viewmodel.invoke = func(args: Array) -> Dictionary:
		var world := _world()
		if world == null:
			return _action_error(ERR_UNAVAILABLE)
		if not DebugControlArgs.weapon_name(args):
			return _action_error(ERR_INVALID_PARAMETER)
		return _action_result(world.set_local_player_weapon_by_name(String(args[0])))

	var clear_viewmodel := _action(&"clear_viewmodel_weapon", &"Player", "Clear viewmodel weapon",
			"Drop the equipped viewmodel (the armory NONE row).",
			TARGET_WORLD, OWNER_DEVICE)
	clear_viewmodel.requires_confirm = true
	clear_viewmodel.invoke = func(_args: Array) -> Dictionary:
		var world := _world()
		if world == null:
			return _action_error(ERR_UNAVAILABLE)
		world.clear_local_player_weapon()
		return _action_result(null)

	var diagnostics := _check(&"net_joiner_diagnostics", &"Net", "Joiner diagnostics",
			"Emit the per-second joiner freeze-tripwire trace (renders via print_verbose; run with --verbose).",
			TARGET_SIM, OWNER_ENGINE)
	diagnostics.read = func() -> Variant:
		var sim := _sim()
		return sim.is_joiner_network_diagnostics_enabled() if sim != null else null
	diagnostics.write = func(value: Variant) -> Error:
		var sim := _sim()
		if sim == null:
			return ERR_UNAVAILABLE
		sim.set_joiner_network_diagnostics_enabled(bool(value))
		return OK

	var kill_group := _action(&"kill_group", &"Entities", "Kill group",
			"Kill every live entity of a mission group; returns the count killed.",
			TARGET_SIM, OWNER_ENGINE)
	_authoritative(kill_group)
	kill_group.invoke = func(args: Array) -> Dictionary:
		var sim := _sim()
		if sim == null:
			return _action_error(ERR_UNAVAILABLE)
		if not DebugControlArgs.kill_group(args):
			return _action_error(ERR_INVALID_PARAMETER)
		return _action_result(sim.debug_kill_group(int(args[0])))

	var crew_vehicle := _action(&"crew_vehicle", &"Entities", "Crew vehicle",
			"Seat an AI occupant (by SSN) into a vehicle (by SSN) as its pilot.",
			TARGET_SIM, OWNER_ENGINE)
	_authoritative(crew_vehicle)
	crew_vehicle.invoke = func(args: Array) -> Dictionary:
		var sim := _sim()
		if sim == null:
			return _action_error(ERR_UNAVAILABLE)
		if not DebugControlArgs.crew_vehicle(args):
			return _action_error(ERR_INVALID_PARAMETER)
		return _action_error(sim.debug_crew_vehicle(int(args[0]), int(args[1])))

	var crew_local := _action(&"crew_local_player", &"Entities", "Crew local player",
			"Seat the local player into a vehicle (by SSN).",
			TARGET_SIM, OWNER_ENGINE)
	_authoritative(crew_local)
	crew_local.invoke = func(args: Array) -> Dictionary:
		var sim := _sim()
		if sim == null:
			return _action_error(ERR_UNAVAILABLE)
		if not DebugControlArgs.crew_local_player(args):
			return _action_error(ERR_INVALID_PARAMETER)
		return _action_error(sim.debug_crew_local_player(int(args[0])))

	var look := _action(&"local_player_look", &"Player", "Local player look",
			"Feed one mouse-look delta (dx_px, dy_px screen pixels) through the local player's look path.",
			TARGET_SIM, OWNER_ENGINE)
	look.requires_confirm = true
	look.invoke = func(args: Array) -> Dictionary:
		var sim := _sim()
		if sim == null:
			return _action_error(ERR_UNAVAILABLE)
		if not DebugControlArgs.look(args):
			return _action_error(ERR_INVALID_PARAMETER)
		sim.add_local_player_look(float(args[0]), float(args[1]))
		return _action_result(null)


# --- live owner resolution (per call, never retained) ------------------------


func _runtime() -> MissionPresentation:
	if _seams == null or not _seams.runtime_source.is_valid():
		return null
	var value: Variant = _seams.runtime_source.call()
	if value is MissionPresentation and is_instance_valid(value):
		return value
	return null


func _sim() -> Simulation:
	var runtime := _runtime()
	return runtime.get_sim() if runtime != null else null


func _world() -> GameWorld:
	if _seams == null or not _seams.world_source.is_valid():
		return null
	var value: Variant = _seams.world_source.call()
	# The world rows draw over a LOADED mission: the shell's GameWorld node
	# outlives the mission, so an unloaded one reads as no world.
	if value is GameWorld and is_instance_valid(value) and value.is_loaded():
		return value
	return null


func _player() -> LocalPlayerPresenter:
	if _seams == null or not _seams.presenter_source.is_valid():
		return null
	var value: Variant = _seams.presenter_source.call()
	if value is LocalPlayerPresenter and is_instance_valid(value):
		return value
	return null


func _terrain() -> Terrain:
	var world := _world()
	return world.get_terrain_node() if world != null else null


func _weather() -> Weather:
	var world := _world()
	return world.get_weather_node() if world != null else null


func _viewport() -> Viewport:
	if _shell == null or not _shell.is_inside_tree():
		return null
	return _shell.get_viewport()


func _availability_for(target: String) -> Callable:
	match target:
		TARGET_WORLD:
			return _world_availability
		TARGET_PLAYER:
			return _player_availability
		TARGET_SIM:
			return _sim_availability
		TARGET_TERRAIN:
			return _terrain_availability
		TARGET_VIEWPORT:
			return _viewport_availability
		TARGET_GAME_SHELL:
			return _shell_availability
		TARGET_ENVIRONMENT:
			return _environment_availability
		TARGET_WEATHER:
			return _weather_availability
	return func() -> String: return "Unknown debug owner surface."


func _world_availability() -> String:
	return "" if _world() != null else "No game world is loaded."


func _player_availability() -> String:
	return "" if _player() != null else "No local player presenter is active."


func _sim_availability() -> String:
	return "" if _sim() != null else "No simulation is active."


func _terrain_availability() -> String:
	return "" if _terrain() != null else "The current world has no terrain."


func _viewport_availability() -> String:
	return "" if _viewport() != null else "No render viewport is available."


func _shell_availability() -> String:
	return "" if _shell != null else "The game shell is not available."


func _environment_availability() -> String:
	return "" if _world() != null else "The current world has no environment."


func _weather_availability() -> String:
	return "" if _weather() != null else "The current world has no weather controller."
