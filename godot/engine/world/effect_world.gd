class_name NovaEffectWorld
extends Node3D

## The runtime particle-effect world: loads the mounted .ptl set once per
## mission and spawns named effects as transient NovaParticleEmitter children.
## Host-side and render-only — the deterministic sim never depends on it (the
## original's effect spawns are client-side; a dedicated host is headless,
## [orig: CEffectEmitter_ReleaseSafe @ 0x5F69F0] witness).
##
## Load [orig: CEffectSystem_Init @ 0x5f6070, called from Game_StartMission
## @ 0x524360]: the engine parses EVERY .ptl next to the exe (ptl\*.ptl), the
## .ptu/.ptg alternate set, and every .ptl entry of every mounted PFF volume —
## no fixed list — into one effect world, then resolves references globally
## across files (docs/particles/ptl-format-re.md §1.5).
##
## Spawn [orig: CEffectWorld_SpawnEmitterAtPosition @ 0x5f6df0]: a spawn
## descriptor carries either a 1-based effect HANDLE or an effect NAME
## (CEffectWorld_FindDefByName @ 0x5e34f0). Handles come from an interning
## table that maps names to stable 1-based indices on first use, matched
## case-insensitively [orig: CEffect_FindOrCreateMaterial @ 0x5f7310 — an
## effect-handle intern, not materials]. The WAC fx parameter resolves through
## that intern at script compile [orig: WacScript_ResolveParameter @ 0x4f2920].

## Per the original's FOREVEREMIT flag bit (flag-table idx 18, post-HAZE).
const PARTICLE_FLAG_FOREVER_EMIT := 1 << 18
const PARTICLE_FLAG_BELOW_H2O := 1 << 27
const PARTICLE_FLAG_ABOVE_H2O := 1 << 28

## One loaded .ptl document (a NovaParticleFile) per mounted file.
var _files: Array = []
## Effect registry in load order: [{ "name": String, "effect": NovaParticleEffect, "file": NovaParticleFile }].
var _effect_entries: Array = []
## lower-cased effect name -> _effect_entries element (stricmp lookup).
var _effects_by_name: Dictionary = {}
## The handle intern table: 1-based handle -> _effect_entries element.
## [orig: dword_2C25B18 pool + dword_2C25CE0 count @ CEffect_FindOrCreateMaterial]
var _interned: Array = []
var _interned_by_name: Dictionary = {}
## Merged [tabledef] set across every loaded file — curve *_func references
## resolve globally (table-only files like table.ptl serve the whole set).
var _tables: Array = []
## Host texture source (NovaResourceRoot.load_texture wrapped in a Callable);
## the engine's analogue reads tga\ + the mounted volumes. The root itself is
## held so the Callable's target outlives the caller's local reference.
var _root: NovaResourceRoot
var _texture_provider := Callable()
## Live spawn groups: [{ "id": int, "emitters": Array[NovaParticleEmitter],
##   "window": float, "elapsed": float, "forever": bool }]. Finite groups free
## themselves once their window elapses with nothing alive.
var _live: Array = []
var _spawn_serial := 0
## Caller-owned spawn handles (owner key -> live group id): a re-spawn under the
## same key detaches the previous group [orig: WacScript_SpawnSoundAtEntity
## @ 0x4f23a0 — the per-entity emitter handle; respawn detaches the old one].
var _owned_groups: Dictionary = {}
## Resolves an owned WAC SSN to its current Transform3D (a Vector3 position is
## retained as a compatibility fallback). fx2ssn emitters are attached handles
## in retail, updated every frame and detached when the entity disappears
## [orig: CEffect_UpdateEmitterTransform @ 0x5f7410].
var _owner_position_provider := Callable()
var _water_height := 0.0


func file_count() -> int:
	return _files.size()


func effect_count() -> int:
	return _effect_entries.size()


func live_group_count() -> int:
	return _live.size()


## --- Debug seams (the retail particle debug pages, mimicked; ptl-format-re.md §11) ---

## The retail "particles off" master switch [orig: byte_24D261D — every effect
## facade no-ops when set; retail arms it from the command line]. The mimic
## hides the render output (the sim keeps running so re-enabling is seamless);
## it exists so the F3 overlay can bisect "is the artifact particles at all?".
func set_particles_hidden(hidden: bool) -> void:
	visible = not hidden


func are_particles_hidden() -> bool:
	return not visible


## Counts for the overlay's header line [orig: Debug_DrawParticleStats
## @ 0x44c840 — "Current Particle Count: current / peak"; the PEAK latch +
## its reset-at-zero quirk live overlay-side, as in retail].
func get_debug_stats() -> Dictionary:
	var alive := 0
	var rendered := 0
	for group in _live:
		for emitter_v in group.get("emitters", []):
			var emitter := emitter_v as NovaParticleEmitter
			if emitter == null:
				continue
			alive += emitter.get_alive_count()
			rendered += emitter.get_rendered_instance_count()
	return {
		"alive": alive,
		"rendered": rendered,
		"groups": _live.size(),
		"effects": _effect_entries.size(),
		"interned": _interned.size(),
	}


## Per-live-group report for the overlay's list [orig: Debug_DrawParticleStats
## lists entry names via the world; Debug_DrawEffectBrowser @ 0x44c950 shows
## each effect's NAME + source FILE]. `unresolved` carries the authored
## graphic names that resolved to no texture (the D-PTL-14 misses, live).
func get_debug_group_report() -> Array:
	var out: Array = []
	for group in _live:
		var handle := int(group.get("handle", 0))
		var effect_name := ""
		var source_file := ""
		if handle >= 1 and handle <= _interned.size():
			var entry: Dictionary = _interned[handle - 1]
			effect_name = String(entry.get("name", ""))
			source_file = String(entry.get("source", ""))
		var emitters: Array = []
		var unresolved := PackedStringArray()
		for emitter_v in group.get("emitters", []):
			var emitter := emitter_v as NovaParticleEmitter
			if emitter == null:
				continue
			emitters.append({
				"name": String(emitter.name),
				"alive": emitter.get_alive_count(),
				"rendered": emitter.get_rendered_instance_count(),
				"node": emitter,
			})
			for miss in emitter.get_unresolved_texture_names():
				if not unresolved.has(miss):
					unresolved.append(miss)
		out.append({
			"id": int(group.get("id", 0)),
			"name": effect_name,
			"source": source_file,
			"forever": bool(group.get("forever", false)),
			"emitters": emitters,
			"unresolved": unresolved,
		})
	return out


func get_texture_provider() -> Callable:
	return _texture_provider


func set_owner_position_provider(provider: Callable) -> void:
	_owner_position_provider = provider


func get_owner_position_provider() -> Callable:
	return _owner_position_provider


func set_water_height(value: float) -> void:
	_water_height = value


## Load every .ptl reachable in the mounted root (loose overrides + all PFF
## volumes — list_files walks both). Returns the number of effects registered.
func load_from_resource_root(root: NovaResourceRoot) -> int:
	clear_world()
	if root == null:
		return 0
	_root = root
	_texture_provider = Callable(root, "load_texture")
	# list_file_entries: logical names resolve through read_file for BOTH loose
	# and archive entries (list_files returns loose entries as absolute paths).
	for entry_v in root.list_file_entries(".ptl"):
		var name := String((entry_v as Dictionary).get("logical_name", ""))
		if name.is_empty():
			continue
		var bytes: PackedByteArray = root.read_file(name)
		if bytes.is_empty():
			continue
		var file := NovaParticleFile.new()
		if file.load_from_buffer(bytes, name) != OK:
			push_warning("effect world: failed to parse %s" % name)
			continue
		_register_file(file)
	return _effect_entries.size()


## Load a single parsed document (tests, tools).
func load_particle_file(file: NovaParticleFile) -> void:
	if file != null:
		_register_file(file)


func clear_world() -> void:
	for group in _live:
		for emitter in group.emitters:
			if is_instance_valid(emitter):
				emitter.queue_free()
	_live.clear()
	_owned_groups.clear()
	_files.clear()
	_effect_entries.clear()
	_effects_by_name.clear()
	_interned.clear()
	_interned_by_name.clear()
	_tables.clear()
	_root = null
	_texture_provider = Callable()
	_owner_position_provider = Callable()
	_water_height = 0.0
	_spawn_serial = 0


func _register_file(file: NovaParticleFile) -> void:
	_files.append(file)
	for effect in file.get_effects():
		if effect == null:
			continue
		var entry := {"name": String(effect.id), "effect": effect, "file": file,
				"source": String(file.get_source_path()).get_file()}
		_effect_entries.append(entry)
		var key := String(effect.id).to_lower()
		# First registration wins on duplicate ids (linear-scan-first semantics).
		if not _effects_by_name.has(key):
			_effects_by_name[key] = entry
	for table in file.get_tables():
		if table != null:
			_tables.append(table)


## Intern an effect name to its stable 1-based handle, registering on first
## use. An unknown name clones the "stockeffect" def under the requested name
## (still 0 when no stockeffect is mounted — e.g. an empty resource root)
## [orig: CEffectWorld_InternEffectHandle @ 0x5f7310 — linear stricmp scan,
## miss -> FindDefByName, still missing -> the vtable+28 stockeffect clone
## registered under the requested name; docs/particles/ptl-format-re.md D-PTL-8].
func intern_effect(name: String) -> int:
	var key := name.to_lower()
	var existing: int = _interned_by_name.get(key, 0)
	if existing > 0:
		return existing
	var entry: Dictionary = _effects_by_name.get(key, {})
	if entry.is_empty():
		var stock: Dictionary = _effects_by_name.get("stockeffect", {})
		if stock.is_empty():
			return 0
		entry = stock.duplicate()
		entry["name"] = name
		_effects_by_name[key] = entry
		push_warning("NovaEffectWorld: unknown effect '%s' — cloned stockeffect [orig: @ 0x5f7310]" % name)
	_interned.append(entry)
	_interned_by_name[key] = _interned.size()
	return _interned.size()


func effect_name_for_handle(handle: int) -> String:
	if handle < 1 or handle > _interned.size():
		return ""
	return String((_interned[handle - 1] as Dictionary).get("name", ""))


## Spawn a named effect at a world-space position. `orientation` is the
## emitter forward (the original passes a direction vector in the spawn
## descriptor; terrain-scripted spawns pass the surface normal). Returns the
## 1-based handle of the interned effect, 0 when the name is unknown.
func spawn_effect(name: String, position: Vector3, orientation: Vector3 = Vector3.ZERO) -> int:
	var handle := intern_effect(name)
	if handle == 0:
		push_warning("effect world: unknown effect '%s'" % name)
		return 0
	_spawn_interned(handle, position, orientation)
	return handle


## Spawn a named effect owned by a caller key (the WAC per-entity handle model):
## a re-spawn under the same key detaches the previous group — its emission
## stops and it frees once the last particle drains — so scripted re-triggers
## and FOREVEREMIT effects never stack [orig: WacScript_SpawnSoundAtEntity
## @ 0x4f23a0 owns one emitter handle per entity; respawn detaches the old one].
## Returns the interned effect handle, 0 when the name is unknown.
func spawn_effect_owned(owner_key: Variant, name: String, position: Vector3,
		orientation: Vector3 = Vector3.ZERO) -> int:
	var handle := intern_effect(name)
	if handle == 0:
		push_warning("effect world: unknown effect '%s'" % name)
		return 0
	var previous: int = int(_owned_groups.get(owner_key, 0))
	if previous > 0:
		stop_group(previous)
	var group_id := _spawn_interned(handle, position, orientation)
	if group_id > 0:
		_owned_groups[owner_key] = group_id
		for group in _live:
			if int(group.get("id", 0)) == group_id:
				group.owner_key = owner_key
				group.follow_owner = true
				break
	return handle


## Spawn suppressed-while-alive (the weapon muzzle model): a re-spawn under the same
## key is IGNORED while the key's previous group still lives. The original records the
## live emitter on the weapon slot and skips the spawn until the emitter's death
## callback clears the record [orig: ActionSlot_ExecuteActionWithEffect @ 0x541860
## spawns only when slot+24 is clear; ActionSlot_SpawnEffect @ 0x401f20 records the
## handle + the on-death ActionSlot_ClearEffectHandle @ 0x53f760 rides the descriptor].
## Returns the interned effect handle (0 = unknown), even when suppressed.
func spawn_effect_unless_alive(owner_key: Variant, name: String, position: Vector3,
		orientation: Vector3 = Vector3.ZERO) -> int:
	var handle := intern_effect(name)
	if handle == 0:
		push_warning("effect world: unknown effect '%s'" % name)
		return 0
	var previous: int = int(_owned_groups.get(owner_key, 0))
	if previous > 0 and _group_alive(previous):
		return handle
	var group_id := _spawn_interned(handle, position, orientation)
	if group_id > 0:
		_owned_groups[owner_key] = group_id
		for group in _live:
			if int(group.get("id", 0)) == group_id:
				group.owner_key = owner_key
				break
	return handle


func _group_alive(group_id: int) -> bool:
	for group in _live:
		if int(group.get("id", 0)) == group_id:
			return true
	return false


## Spawn by an already-interned 1-based handle (the WAC fx parameter shape).
func spawn_effect_by_handle(handle: int, position: Vector3, orientation: Vector3 = Vector3.ZERO) -> bool:
	if handle < 1 or handle > _interned.size():
		return false
	_spawn_interned(handle, position, orientation)
	return true


## Detach a live group: stop its emitters spawning and let alive particles
## drain; the sweep frees it once empty (forever groups become finite).
func stop_group(group_id: int) -> void:
	for group in _live:
		if int(group.get("id", 0)) != group_id:
			continue
		group.forever = false
		group.window = 0.0
		group.follow_owner = false
		for emitter in group.emitters:
			if is_instance_valid(emitter):
				emitter.stop_emitting()
		return


## Spawns one live group; returns its id (0 = nothing spawned).
func _spawn_interned(handle: int, position: Vector3, orientation: Vector3) -> int:
	var entry: Dictionary = _interned[handle - 1]
	var effect: NovaParticleEffect = entry.get("effect")
	if effect == null:
		return 0
	var group := {
		"id": _spawn_serial + 1,
		"handle": handle,
		"emitters": [],
		"window": 1.0,
		"forever": false,
		"elapsed": 0.0,
		"owner_key": null,
		"follow_owner": false,
	}
	var window := 0.0
	var pdefs: PackedStringArray = effect.pdefs
	for i in range(pdefs.size()):
		var def := _find_particle(pdefs[i])
		if def == null:
			continue
		var emitter := NovaParticleEmitter.new()
		emitter.name = "Fx%d_%d" % [_spawn_serial, i]
		emitter.seed = 1 + ((_spawn_serial * 17 + i) % 1023)
		if (int(def.flags) & PARTICLE_FLAG_BELOW_H2O) != 0:
			emitter.kill_plane_mode = 1
			emitter.kill_plane_y = _water_height
		elif (int(def.flags) & PARTICLE_FLAG_ABOVE_H2O) != 0:
			emitter.kill_plane_mode = 2
			emitter.kill_plane_y = _water_height
		emitter.set_tables(_tables)
		if _texture_provider.is_valid():
			# Call the setter: texture_provider is bound as set/get methods without a
			# registered property, so property-style assignment raises "Invalid
			# assignment ... of type 'Callable'" and the emitter silently loses its
			# textures (the muzzle-flash-invisible bug).
			emitter.set_texture_provider(_texture_provider)
		emitter.def = def
		add_child(emitter)
		emitter.global_position = position
		if orientation.length_squared() > 0.0001:
			# The descriptor direction feeds the SIMULATOR frame (cone/EMITVECTOR
			# shapes emit around Emitter::forward). The node transform is not the
			# seam — quads render world-space top-level, so rotating the node did
			# nothing.
			emitter.emission_forward = orientation
		emitter.play()
		group.emitters.append(emitter)
		window = maxf(window, _def_window_seconds(def))
		group.forever = group.forever or (int(def.flags) & PARTICLE_FLAG_FOREVER_EMIT) != 0
	_spawn_serial += 1
	if group.emitters.is_empty():
		return 0
	group.window = maxf(window, 1.0)
	_live.append(group)
	return int(group.id)


## pdef lookup: ONE global pool across every loaded file, scanned in load order
## (first registration wins — the same linear-scan-first semantics as the effect
## registry). The witnessed resolve is global, with no owning-file preference
## [orig: CEffectDef_ResolveAllReferences @ 0x5e9d70; ptl-format-re §1.5], so a
## duplicated pdef id binds every effect to the same winner.
func _find_particle(id: String) -> NovaParticleDef:
	for file in _files:
		var def: NovaParticleDef = file.find_particle(id)
		if def != null:
			return def
	return null


func _def_window_seconds(def: NovaParticleDef) -> float:
	var life: float = def.emit_delay + def.age + 0.5 + def.emit_dur
	return clampf(life, 1.0, 30.0)


func _process(delta: float) -> void:
	sweep(delta)


## Advance group lifetimes and free finished finite groups: past their window
## with no particle alive. Public so tests drive expiry deterministically.
func sweep(delta: float) -> void:
	if _live.is_empty():
		return
	var i := 0
	while i < _live.size():
		var group: Dictionary = _live[i]
		if bool(group.get("follow_owner", false)) and _owner_position_provider.is_valid():
			var owner_key: Variant = group.get("owner_key")
			var current_state: Variant = _owner_position_provider.call(owner_key)
			var owner_alive := current_state is Transform3D or current_state is Vector3
			if not owner_alive:
				# The attached entity was destroyed: detach the emitter handle just
				# like retail, allowing already-live world-space particles to drain.
				group.forever = false
				group.window = 0.0
				group.follow_owner = false
				if int(_owned_groups.get(owner_key, 0)) == int(group.id):
					_owned_groups.erase(owner_key)
				for emitter in group.emitters:
					if is_instance_valid(emitter):
						emitter.stop_emitting()
			else:
				var current_position: Vector3
				var current_forward := Vector3.ZERO
				var has_forward := false
				if current_state is Transform3D:
					var current_transform: Transform3D = current_state
					current_position = current_transform.origin
					current_forward = current_transform.basis.z
					has_forward = current_forward.length_squared() > 0.000001
				else:
					current_position = current_state
				for emitter in group.emitters:
					if is_instance_valid(emitter):
						emitter.global_position = current_position
						if has_forward:
							emitter.emission_forward = current_forward.normalized()
		group.elapsed += delta
		# The native emitter owns randomized delay/duration and terminal-frame
		# state. A heuristic wall-clock window can expire before a delayed emitter
		# ever starts, so completion must come from the emitters themselves.
		var finished := not bool(group.forever)
		if finished:
			for emitter in group.emitters:
				if is_instance_valid(emitter) and not emitter.is_finished():
					finished = false
					break
		if finished:
			var owner_key: Variant = group.get("owner_key")
			if owner_key != null and int(_owned_groups.get(owner_key, 0)) == int(group.id):
				_owned_groups.erase(owner_key)
			for emitter in group.emitters:
				if is_instance_valid(emitter):
					emitter.queue_free()
			_live.remove_at(i)
		else:
			i += 1
