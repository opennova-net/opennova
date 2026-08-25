extends RefCounted


# THE shell impact-scar presentation pass: pulls the sim's scar draw list once
# per present frame (Simulation.get_scar_draw_list — World::scars compiled by
# renderer::compile_scar_draws) and hands it to the ScarPresenter device, which
# uploads the shared ring as one world mesh and each entity ring under its
# owner's struck section node. The sim stays render-free; this pass supplies
# the three device inputs the renderer's compile reads — the camera ground
# position and fog distance for the fog-box cull, and the combined terrain
# light colour folded onto every vertex — and resolves each entity-ring owner
# to its live model node.
#
# [orig map — docs/world/world-wac-ai-re.md §24.9:
#  Scar_RenderAllCaches @0x5CDF70 (the shared ring first, then every live
#    entity ring) -> Scar_RenderCache @0x5CD830 (the fog box
#    `|p - cam| <= fog + r` on the ground axes, the owner-visibility gate, the
#    six vertices per slot coloured Env_TerrainLightCombined | FF000000);
#  the drawer Scar_DrawBatches (ex Terrain_RenderFoliageBatches) @0x5ccd10 (the IDB's kong misnomer;
#  the Scar_DrawBatches rename is proposed) after the lit sector entities.]


var _sim: Simulation                 # live draw-list source; null in data-driven tests
var _container: Node = null          # hosts the presenter node when this pass creates it
var _index: EntityIndex = null       # authored-entity owners
var _wire: WirePresentPass = null    # runtime-only (wire) owners
var _presenter: ScarPresenter = null
var _owns_presenter := false
var _camera_provider := Callable()   # -> Vector3 camera position (Godot space)
var _environment_provider := Callable()  # -> MissionEnvironment (or null)


## Typed diagnostic counters (ADR 0017: cross-object contracts are typed
## records) — probes assert the presentation leg actually ran.
class Stats:
	extends RefCounted
	var slots_live := 0
	var slots_culled := 0
	var rings_leased := 0
	var batches := 0
	var world_surfaces := 0
	var entity_meshes := 0
	var textures_missing := 0
	var strips_unsupported := 0   # strips whose mode word is neither shipped drawer state
	var owners_unresolved := 0


var _stats := Stats.new()


func get_stats() -> Stats:
	return _stats


func get_presenter() -> ScarPresenter:
	return _presenter


## `presenter` may be supplied (tests, tooling); otherwise the pass creates one
## under `container`. The camera provider is the same callable the fire pass
## ticks with (the camera IS the listener); the environment provider returns
## the live MissionEnvironment for the fog distance + terrain light.
func setup(sim: Simulation, container: Node, index: EntityIndex,
		wire: WirePresentPass, resource_root: ResourceRoot,
		camera_provider: Callable = Callable(),
		environment_provider: Callable = Callable(),
		presenter: ScarPresenter = null) -> void:
	_sim = sim
	_container = container
	_index = index
	_wire = wire
	_camera_provider = camera_provider
	_environment_provider = environment_provider
	_presenter = presenter
	_owns_presenter = false
	if _presenter == null and container != null and is_instance_valid(container):
		_presenter = ScarPresenter.new()
		_presenter.name = "ScarPresenter"
		container.add_child(_presenter)
		_owns_presenter = true
	if _presenter != null:
		_presenter.set_resource_root(resource_root)


func teardown() -> void:
	reset_runtime_state()
	if _owns_presenter and _presenter != null and is_instance_valid(_presenter):
		_presenter.queue_free()
	_presenter = null
	_owns_presenter = false


## Discard mission-run presentation state (the Stop -> Play boundary): every
## scar mesh goes; the presenter and its texture cache stay.
func reset_runtime_state() -> void:
	if _presenter != null and is_instance_valid(_presenter):
		_presenter.clear()
	_stats = Stats.new()


## Once per present, after the entity rows (their section nodes host the
## entity-ring meshes).
func present() -> void:
	if _sim == null:
		return
	var camera := Vector3.ZERO
	if _camera_provider.is_valid():
		var cam_v: Variant = _camera_provider.call()
		if cam_v is Vector3:
			camera = cam_v
	var fog_distance := 0.0
	var terrain_light := Color.WHITE
	var env: MissionEnvironment = null
	if _environment_provider.is_valid():
		var env_v: Variant = _environment_provider.call()
		if env_v is MissionEnvironment and is_instance_valid(env_v):
			env = env_v
	if env != null:
		fog_distance = env.get_fog_distance()
		# Env_TerrainLightCombined = light * 0xB5/256 + sky (env-tod-re.md
		# "Derived render colors"; the same chain the water surface lights by).
		var sun: Vector3 = env.get_sun_light()
		var sky: Vector3 = env.get_sky_ambient()
		terrain_light = EnvFile.combine_terrain_light(
				Color(sun.x, sun.y, sun.z), Color(sky.x, sky.y, sky.z))
	present_draw_list(_sim.get_scar_draw_list(camera, fog_distance, terrain_light))


## The pure-data presentation leg (the present_snapshot precedent): production
## present() feeds the typed sim's draw list; tests feed the same dictionary.
func present_draw_list(draw: Dictionary) -> void:
	_stats.slots_live = int(draw.get("slots_live", 0))
	_stats.slots_culled = int(draw.get("slots_culled", 0))
	_stats.rings_leased = int(draw.get("rings_leased", 0))
	if _presenter == null or not is_instance_valid(_presenter):
		return
	var owner_nodes := _resolve_owner_nodes(draw)
	_presenter.present(draw, owner_nodes)
	var device: Dictionary = _presenter.get_stats()
	_stats.batches = int(device.get("batches", 0))
	_stats.world_surfaces = int(device.get("world_surfaces", 0))
	_stats.entity_meshes = int(device.get("entity_meshes", 0))
	_stats.textures_missing = int(device.get("textures_missing", 0))
	_stats.strips_unsupported = int(device.get("strips_unsupported", 0))


# Every entity-ring owner in the list -> its live node (packed handle -> Node3D).
func _resolve_owner_nodes(draw: Dictionary) -> Dictionary:
	var owners: PackedInt32Array = draw.get("batch_owner", PackedInt32Array())
	var flags: PackedInt32Array = draw.get("batch_flags", PackedInt32Array())
	var bms_ids: PackedInt32Array = draw.get("batch_bms_id", PackedInt32Array())
	var origins: PackedInt64Array = draw.get("batch_spawn_origin", PackedInt64Array())
	var out := {}
	_stats.owners_unresolved = 0
	for i in range(owners.size()):
		if i >= flags.size() or (flags[i] & 1) == 0:
			continue
		var owner := owners[i]
		if out.has(owner):
			continue
		var bms_id := bms_ids[i] if i < bms_ids.size() else 0
		var origin := int(origins[i]) if i < origins.size() else SpawnOrigin.NONE
		var node := _resolve_owner(bms_id, origin, owner)
		if node != null:
			out[owner] = node
		else:
			_stats.owners_unresolved += 1
	return out


# The destruction pass's identity rule: a runtime-only row (no BMS id, no
# authored origin) resolves through its wire node; an authored entity through
# the shared index by (bms_id, kind, index). The wire node is the fallback for
# an authored row the index does not carry (a joiner's wire-header world).
func _resolve_owner(bms_id: int, spawn_origin: int, wire_handle: int) -> Node3D:
	var dynamic_identity := bms_id == 0 and spawn_origin == SpawnOrigin.NONE
	if not dynamic_identity and _index != null:
		var node: Node3D = _index.resolve(bms_id, SpawnOrigin.kind(spawn_origin),
				SpawnOrigin.index(spawn_origin))
		if node != null and is_instance_valid(node):
			return node
	if _wire != null and is_instance_valid(_wire):
		var wire_node: Node3D = _wire.resolve_wire_handle(wire_handle)
		if wire_node != null and is_instance_valid(wire_node):
			return wire_node
	return null
