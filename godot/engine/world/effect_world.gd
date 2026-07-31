class_name NovaEffectWorld
extends Node3D

## Compatibility facade for the portable effect scene and packet renderer.
## Effects, emitters, and particles are values owned by NovaEffectScene; this
## node owns one NovaParticleRenderer for both render domains.

const PARTICLE_FLAG_FOREVER_EMIT := NovaParticleDef.FLAG_FOREVER_EMIT
const ADMISSION_ALWAYS := 0
const ADMISSION_REPLACE_OWNED := 1
const ADMISSION_SUPPRESS_WHILE_OWNED := 2
const BINDING_WORLD := 0
const BINDING_FOLLOW_OWNER := 1
const RENDER_DOMAIN_WORLD := 0
const RENDER_DOMAIN_FIRST_PERSON := 1
const KILL_PLANE_DISABLED := 0

var _files: Array[NovaParticleFile] = []
var _load_report: Dictionary = {}
var _scene: NovaEffectScene
var _renderer: NovaParticleRenderer
var _root: NovaResourceRoot
var _texture_provider := Callable()
var _texture_dir := ""
var _environment_source: Node
var _owner_position_provider := Callable()
var _water_height := 0.0
var _particles_disabled := false

# Keys never become native tokens by hashing. A shared monotonic allocator and
# separate maps give the same Variant distinct slot and owner identities.
var _slot_tokens: Dictionary = {}
var _owner_tokens: Dictionary = {}
var _owner_keys_by_token: Dictionary = {}
var _owner_pose_cache: Dictionary = {}
var _next_token := 1


func _init() -> void:
	_scene = NovaEffectScene.new()
	var empty_files: Array[NovaParticleFile] = []
	_load_report = _scene.open(empty_files)


func _ready() -> void:
	_ensure_renderer()
	set_process(true)


func _ensure_renderer() -> void:
	if is_instance_valid(_renderer):
		return
	_renderer = NovaParticleRenderer.new()
	_renderer.name = "ParticleRenderer"
	add_child(_renderer)
	_renderer.set_scene(_scene)
	_renderer.set_texture_provider(_texture_provider)
	_renderer.set_texture_dir(_texture_dir)
	_renderer.set_environment_source(_environment_source)
	_renderer.set_hidden(_particles_disabled)


func set_environment_source(source: Node) -> void:
	_environment_source = source
	_ensure_renderer()
	_renderer.set_environment_source(source)


func file_count() -> int:
	return _files.size()


func effect_count() -> int:
	return int(_load_report.get("effect_count", 0))


func live_group_count() -> int:
	return int(_scene.get_live_counts().get("group_count", 0))


func active_entry_count() -> int:
	return 0 if _particles_disabled else live_group_count()


func interned_count() -> int:
	return int(_scene.inspect().get("interned_effect_count", 0))


func set_particles_hidden(hidden: bool) -> void:
	_particles_disabled = hidden
	visible = not hidden
	_ensure_renderer()
	_renderer.set_hidden(hidden)


func are_particles_hidden() -> bool:
	return _particles_disabled


func get_texture_provider() -> Callable:
	return _texture_provider


func set_owner_position_provider(provider: Callable) -> void:
	_owner_position_provider = provider


func set_water_height(value: float) -> void:
	_water_height = value


## Loads every mounted .ptl in VFS order, then opens the catalog once.
func load_from_resource_root(root: NovaResourceRoot) -> int:
	clear_world()
	if root == null:
		return 0
	_root = root
	_texture_provider = Callable(root, "load_texture")
	_ensure_renderer()
	_renderer.set_texture_provider(_texture_provider)
	for entry_v in root.list_file_entries(".ptl"):
		var entry := entry_v as Dictionary
		var logical_name := String(entry.get("logical_name", ""))
		if logical_name.is_empty():
			continue
		var bytes := root.read_file(logical_name)
		if bytes.is_empty():
			continue
		var file := NovaParticleFile.new()
		if file.load_from_buffer(bytes, logical_name) != OK:
			push_warning("effect world: failed to parse %s" % logical_name)
			continue
		_files.append(file)
	_open_files()
	return effect_count()


func load_particle_file(file: NovaParticleFile) -> void:
	if file == null:
		return
	_files.append(file)
	if _root == null and _texture_dir.is_empty():
		_texture_dir = String(file.get_source_path()).get_base_dir()
	_open_files()


func _open_files() -> void:
	_scene = NovaEffectScene.new()
	_load_report = _scene.open(_files)
	_ensure_renderer()
	_renderer.set_scene(_scene)
	_renderer.set_texture_provider(_texture_provider)
	_renderer.set_texture_dir(_texture_dir)


func clear_world() -> void:
	_files.clear()
	_slot_tokens.clear()
	_owner_tokens.clear()
	_owner_keys_by_token.clear()
	_owner_pose_cache.clear()
	_next_token = 1
	_root = null
	_texture_provider = Callable()
	_texture_dir = ""
	_owner_position_provider = Callable()
	_water_height = 0.0
	_scene = NovaEffectScene.new()
	var empty_files: Array[NovaParticleFile] = []
	_load_report = _scene.open(empty_files)
	_ensure_renderer()
	_renderer.set_scene(_scene)
	_renderer.set_texture_provider(_texture_provider)
	_renderer.set_texture_dir("")


## Clear live simulation values while preserving the compiled catalog, native
## definition identity, interned handles, texture atlas, and renderer pipelines.
## Mission Stop uses this instead of clear_world() so the next Play reuses every
## load-time warm result.
func reset_runtime_state() -> void:
	_slot_tokens.clear()
	_owner_tokens.clear()
	_owner_keys_by_token.clear()
	_owner_pose_cache.clear()
	_next_token = 1
	if is_instance_valid(_renderer):
		_renderer.clear_warm_pipelines()
	_scene.reset_runtime_state()


func intern_effect(name: String) -> int:
	return int(_scene.intern(name))


func effect_name_for_handle(handle: int) -> String:
	return _scene.effect_name(handle)


func _allocate_token() -> int:
	var token := _next_token
	_next_token += 1
	return token


func _slot_token_for(key: Variant) -> int:
	if _slot_tokens.has(key):
		return int(_slot_tokens[key])
	var token := _allocate_token()
	_slot_tokens[key] = token
	return token


func _owner_token_for(key: Variant) -> int:
	if _owner_tokens.has(key):
		return int(_owner_tokens[key])
	var token := _allocate_token()
	_owner_tokens[key] = token
	_owner_keys_by_token[token] = key
	return token


func _pose(position: Vector3, forward_value: Vector3) -> Transform3D:
	if forward_value.length_squared() <= 0.000001:
		return Transform3D(Basis.IDENTITY, position)
	var forward := forward_value.normalized()
	var up_hint := Vector3.UP
	if absf(forward.dot(up_hint)) > 0.999:
		up_hint = Vector3.RIGHT
	var right := up_hint.cross(forward).normalized()
	var up := forward.cross(right).normalized()
	return Transform3D(Basis(right, up, forward), position)


func _seed_owner_pose(owner_token: int, transform: Transform3D) -> void:
	_owner_pose_cache[owner_token] = transform
	_scene.apply_owner_poses([{
		"owner_token": owner_token,
		"transform": transform,
		"present": true,
	}])


func _disabled_receipt() -> Dictionary:
	return {
		"status": -1,
		"status_name": "particles_disabled",
		"effect_handle": 0,
		"group_id": 0,
		"replaced_group_id": 0,
		"spawned": false,
		"accepted": false,
	}


## Deep spawn seam. Admission, binding, and render domain are explicit values;
## arbitrary slot_key/owner_key Variants are interned to stable native tokens.
func spawn_effect_request(name: String, transform: Transform3D,
		options: Dictionary = {}) -> Dictionary:
	if _particles_disabled:
		return _disabled_receipt()
	var handle := intern_effect(name)
	var admission := int(options.get("admission", ADMISSION_ALWAYS))
	var binding := int(options.get("binding", BINDING_WORLD))
	var slot_token := int(options.get("slot_token", 0))
	var owner_token := int(options.get("owner_token", 0))
	var has_slot_key := options.has("slot_key")
	if has_slot_key:
		slot_token = _slot_token_for(options["slot_key"])
	if options.has("owner_key"):
		owner_token = _owner_token_for(options["owner_key"])
	var owner_transform := Transform3D.IDENTITY
	var seed_owner_after_spawn := false
	if owner_token > 0 and options.has("owner_transform"):
		owner_transform = options["owner_transform"]
		# ReplaceOwned must detach its predecessor at that predecessor's last
		# pose. Seeding the new pose first would move both groups before the
		# portable scene gets a chance to detach the old one.
		seed_owner_after_spawn = admission == ADMISSION_REPLACE_OWNED
		if not seed_owner_after_spawn:
			_seed_owner_pose(owner_token, owner_transform)
	var request := {
		"effect_handle": handle,
		"transform": transform,
		"admission": admission,
		"binding": binding,
		"render_domain": int(options.get("render_domain", RENDER_DOMAIN_WORLD)),
		"slot_token": slot_token,
		"owner_token": owner_token,
		"owner_relative_transform": options.get(
				"owner_relative_transform", Transform3D.IDENTITY),
		"initial_age_ticks": int(options.get("initial_age_ticks", 0)),
		"source_tick": int(options.get("source_tick", 0)),
		"source_order": int(options.get("source_order", 0)),
		"color_tint": options.get("color_tint", Vector3.ONE),
		"spring_const": float(options.get("spring_const", 0.0)),
		"lod_divisor": int(options.get("lod_divisor", 1)),
		"kill_plane": int(options.get("kill_plane", KILL_PLANE_DISABLED)),
		"kill_plane_y": float(options.get("kill_plane_y", _water_height)),
	}
	var receipt: Dictionary = _scene.spawn(request)
	if seed_owner_after_spawn and bool(receipt.get("spawned", false)):
		_seed_owner_pose(owner_token, owner_transform)
	return receipt


## Load-time warm pass: spawn every catalog effect once at `position` so the
## lazily-deferred one-times (texture resolves, the renderer's first-draw
## pipeline compiles) are paid behind the loading screen instead of as a
## ~90 ms hitch on the player's first live shot. Retail pays this at load —
## CEffectSystem_Init loads every .ptl AND its textures up front
## [orig: @ 0x5f6070 <- Game_StartMission @ 0x524980]; the deferred-resolve
## shell path is what made first fire hitch. The caller renders a frame or two
## (advancing the fixed tick so fresh emitters actually emit and draw), then
## clears the warm spawns via reset_runtime_state(). Returns spawn count.
func warm_all_effects(position: Vector3) -> int:
	# Deterministic half first: one quad per FirstPerson blend shader plus a
	# compositor request for all eight World RD pipelines, independent of
	# emitter timing (delayed emitters emit nothing during the warm frames).
	_ensure_renderer()
	_renderer.warm_pipelines(position)
	var seen := {}
	var spawned := 0
	for file in _files:
		if file == null:
			continue
		for effect_v in file.get_effects():
			var effect_id := String(effect_v.get_id())
			if effect_id.is_empty() or seen.has(effect_id):
				continue
			seen[effect_id] = true
			# Catalog values warm through the uncapped World packet. The
			# deterministic helpers above already compile all eight
			# FirstPerson shaders; cloning a large catalog into that ArrayMesh
			# domain can exceed Godot's per-mesh surface limit.
			if spawn_effect_transient(effect_id, position) != 0:
				spawned += 1
	# GameWorld skips its draw/reset leg when there was nothing to warm. Do not
	# strand the deterministic shader helper quads in that empty-catalog path.
	if spawned == 0:
		_renderer.clear_warm_pipelines()
	return spawned


## Compile and submit the latest scene snapshot synchronously. The normal
## process path calls the renderer every frame; mission loading uses this seam
## between fixed warm ticks and forced draws so freshly emitted values are in
## the submitted packet immediately. Returns the number of material runs.
func render_now() -> int:
	_ensure_renderer()
	_renderer.render_now()
	return int(_renderer.get_draw_command_count())


func spawn_effect_transient(name: String, position: Vector3,
		orientation: Vector3 = Vector3.ZERO, initial_age_ticks: int = 0,
		render_domain: int = RENDER_DOMAIN_WORLD, source_tick: int = 0,
		source_order: int = 0) -> int:
	var receipt := spawn_effect_request(name, _pose(position, orientation), {
		"initial_age_ticks": initial_age_ticks,
		"render_domain": render_domain,
		"source_tick": source_tick,
		"source_order": source_order,
	})
	return int(receipt.get("effect_handle", 0))


func spawn_effect(name: String, position: Vector3,
		orientation: Vector3 = Vector3.ZERO) -> int:
	return spawn_effect_transient(name, position, orientation)


func spawn_effect_owned_request(owner_key: Variant, name: String, position: Vector3,
		orientation: Vector3 = Vector3.ZERO) -> Dictionary:
	if _particles_disabled:
		return _disabled_receipt()
	var initial_transform := _pose(position, orientation)
	return spawn_effect_request(name, initial_transform, {
		"admission": ADMISSION_REPLACE_OWNED,
		"binding": BINDING_FOLLOW_OWNER,
		"slot_key": owner_key,
		"owner_key": owner_key,
		"owner_transform": initial_transform,
		"owner_relative_transform": Transform3D.IDENTITY,
	})


func spawn_effect_owned(owner_key: Variant, name: String, position: Vector3,
		orientation: Vector3 = Vector3.ZERO) -> int:
	var receipt := spawn_effect_owned_request(owner_key, name, position, orientation)
	return int(receipt.get("effect_handle", 0))


func spawn_effect_attached_request(owner_key: Variant, name: String,
		initial_transform: Transform3D, local_pos: Vector3,
		local_dir: Vector3) -> Dictionary:
	if _particles_disabled:
		return _disabled_receipt()
	var local_transform := _pose(local_pos, local_dir)
	return spawn_effect_request(name,
			initial_transform * local_transform, {
		"binding": BINDING_FOLLOW_OWNER,
		"owner_key": owner_key,
		"owner_transform": initial_transform,
		"owner_relative_transform": local_transform,
	})


func spawn_effect_attached(owner_key: Variant, name: String,
		initial_transform: Transform3D, local_pos: Vector3,
		local_dir: Vector3) -> int:
	var receipt := spawn_effect_attached_request(owner_key, name,
			initial_transform, local_pos, local_dir)
	if not bool(receipt.get("spawned", false)):
		return 0
	return int(receipt.get("effect_handle", 0))


func spawn_effect_unless_alive(owner_key: Variant, name: String,
		position: Vector3, orientation: Vector3 = Vector3.ZERO) -> int:
	if _particles_disabled:
		return 0
	var receipt := spawn_effect_request(name, _pose(position, orientation), {
		"admission": ADMISSION_SUPPRESS_WHILE_OWNED,
		"slot_key": owner_key,
	})
	return int(receipt.get("effect_handle", 0))


func spawn_effect_by_handle(handle: int, position: Vector3,
		orientation: Vector3 = Vector3.ZERO) -> bool:
	if _particles_disabled or effect_name_for_handle(handle).is_empty():
		return false
	var receipt: Dictionary = _scene.spawn({
		"effect_handle": handle,
		"transform": _pose(position, orientation),
		"kill_plane_y": _water_height,
	})
	return bool(receipt.get("spawned", false))


func stop_group(group_id: int) -> void:
	_scene.detach(group_id)


## Releases the script/native binding identity for an owner whose lifecycle is
## complete. Call after stopping its group: generation-scoped owners (flying
## rounds, debris pieces) otherwise accumulate one slot key, owner key, and
## cached pose for every lifetime until the whole mission is reset.
func release_effect_binding(owner_key: Variant) -> void:
	if _slot_tokens.has(owner_key):
		_scene.detach_slot(int(_slot_tokens[owner_key]))
		_slot_tokens.erase(owner_key)
	if not _owner_tokens.has(owner_key):
		return
	var owner_token := int(_owner_tokens[owner_key])
	# A group may already be detached by stop_group(), but explicitly retiring
	# the native pose keeps this safe for rejected spawns and callers that only
	# know the owner identity.
	_scene.apply_owner_poses([{
		"owner_token": owner_token,
		"present": false,
	}])
	_owner_pose_cache.erase(owner_token)
	_owner_keys_by_token.erase(owner_token)
	_owner_tokens.erase(owner_key)


## True while this owner key holds a live binding identity (slot token, owner
## token, and reverse lookup). Leak-check seam for generation-scoped owners
## (flying rounds, debris pieces) whose keys must not accumulate across
## lifetimes (ADR 0017: typed scalars, not a Dictionary report).
func has_owner_binding(owner_key: Variant) -> bool:
	return _slot_tokens.has(owner_key) and _owner_tokens.has(owner_key) \
			and _owner_keys_by_token.has(_owner_tokens.get(owner_key, -1))


## True while this owner key's binding also carries a cached native pose. A
## rejected ReplaceOwned spawn allocates its identities but never seeds one.
func has_cached_owner_pose(owner_key: Variant) -> bool:
	if not _owner_tokens.has(owner_key):
		return false
	return _owner_pose_cache.has(int(_owner_tokens[owner_key]))


## True when every owner-binding table is empty, i.e. no retired owner key is
## still holding a slot token, owner token, reverse lookup, or cached pose.
func has_no_owner_bindings() -> bool:
	return _slot_tokens.is_empty() and _owner_tokens.is_empty() \
			and _owner_keys_by_token.is_empty() and _owner_pose_cache.is_empty()


func _sync_owner_poses(refresh_frame := true) -> void:
	if not _owner_position_provider.is_valid():
		return
	var updates: Array = []
	for owner_token_v in _scene.get_active_owner_tokens():
		var owner_token := int(owner_token_v)
		if not _owner_keys_by_token.has(owner_token):
			continue
		var owner_key: Variant = _owner_keys_by_token[owner_token]
		var state: Variant = _owner_position_provider.call(owner_key)
		if state is Transform3D:
			var owner_transform: Transform3D = state
			var cached: Variant = _owner_pose_cache.get(owner_token)
			if cached is Transform3D:
				var cached_transform: Transform3D = cached
				if cached_transform.is_equal_approx(owner_transform):
					continue
			_owner_pose_cache[owner_token] = owner_transform
			updates.append({
				"owner_token": owner_token,
				"transform": owner_transform,
				"present": true,
			})
		elif state is Vector3:
			var had_cached := _owner_pose_cache.has(owner_token)
			var cached: Transform3D = _owner_pose_cache.get(
					owner_token, Transform3D.IDENTITY)
			var translated := Transform3D(cached.basis, state)
			if had_cached and cached.is_equal_approx(translated):
				continue
			_owner_pose_cache[owner_token] = translated
			updates.append({
				"owner_token": owner_token,
				"transform": translated,
				"present": true,
			})
		else:
			_owner_pose_cache.erase(owner_token)
			updates.append({"owner_token": owner_token, "present": false})
	if not updates.is_empty():
		if refresh_frame:
			_scene.apply_owner_poses(updates)
		else:
			_scene.apply_owner_poses_in_place(updates)


## The only simulation clock. Callers feed fixed mission ticks (1 / 62.5 s).
func advance_fixed_tick(delta: float) -> void:
	_sync_owner_poses(false)
	_scene.advance_in_place(maxf(delta, 0.0))


## Compatibility alias used by deterministic tests/tools.
func sweep(delta: float) -> void:
	advance_fixed_tick(delta)


## Render-frame work may update attachment poses, but never advances particles.
func _process(_delta: float) -> void:
	_sync_owner_poses()


## Value-only F3 read model. Emitter ids join portable simulation values to
## the renderer's packet bounds; no particle/render Nodes escape this facade.
func get_debug_group_report() -> Array:
	var out: Array = []
	if _particles_disabled:
		return out
	_ensure_renderer()
	var rendered_by_id: Dictionary = {}
	for row_v in _renderer.get_debug_emitter_bounds():
		var row := row_v as Dictionary
		rendered_by_id[int(row.get("emitter_id", 0))] = row
	# F3 and the optional effect-box view are recurring UI reads. Ask for the
	# compact topology/pose view and use renderer-owned bounds; serializing the
	# immutable catalog and every live particle here creates a low-FPS feedback
	# loop precisely while the counters are being observed.
	var debug := _scene.inspect(false)
	for group_v in debug.get("groups", []):
		var group := group_v as Dictionary
		var group_id := int(group.get("group_id", 0))
		var emitter_rows: Array = []
		var forever := false
		for emitter_v in group.get("emitters", []):
			var emitter := emitter_v as Dictionary
			var emitter_id := int(emitter.get("emitter_id", 0))
			var rendered: Dictionary = rendered_by_id.get(emitter_id, {})
			forever = forever or (
					int(emitter.get("definition_flags", 0))
							& PARTICLE_FLAG_FOREVER_EMIT) != 0
			var bounds_valid := bool(rendered.get("bounds_valid", false))
			var bounds := AABB()
			if bounds_valid:
				bounds = rendered.get("bounds", AABB())
			emitter_rows.append({
				"name": String(emitter.get("definition_name", "")),
				"alive": int(emitter.get("alive_particle_count", 0)),
				"emitting": bool(emitter.get("emitting", false)),
				"rendered": int(rendered.get("quad_count", 0)),
				"bounds": bounds,
				"bounds_valid": bounds_valid,
				"emitter_id": emitter_id,
				"position": emitter.get("position", Vector3.ZERO),
				"forward": emitter.get("forward", Vector3.FORWARD),
				"age": float(emitter.get("age", 0.0)),
				"kill_plane": int(emitter.get("kill_plane", KILL_PLANE_DISABLED)),
				"kill_plane_y": float(emitter.get("kill_plane_y", _water_height)),
			})
		out.append({
			"id": group_id,
			"name": String(group.get("effect_name", "")),
			"source": String(group.get("source", "")).get_file(),
			"forever": forever and not bool(group.get("detached", false)),
			"admission": int(group.get("admission", ADMISSION_ALWAYS)),
			"binding": int(group.get("binding", BINDING_WORLD)),
			"render_domain": int(group.get("render_domain", RENDER_DOMAIN_WORLD)),
			"detached": bool(group.get("detached", false)),
			"transform": group.get("transform", Transform3D.IDENTITY),
			"source_tick": int(group.get("source_tick", 0)),
			"source_order": int(group.get("source_order", 0)),
			"emitters": emitter_rows,
		})
	return out


## Unresolved names describe the loaded atlas catalog as a whole, not any one
## live group; keeping them here prevents one unrelated missing frame from
## falsely implicating every effect.
func get_unresolved_texture_names() -> PackedStringArray:
	_ensure_renderer()
	return _renderer.get_unresolved_texture_names()
