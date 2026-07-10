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
## Live spawn groups: [{ "emitters": Array[NovaParticleEmitter], "window": float,
##   "elapsed": float, "forever": bool }]. Finite groups free themselves once
## their window elapses with nothing alive.
var _live: Array = []
var _spawn_serial := 0


func file_count() -> int:
	return _files.size()


func effect_count() -> int:
	return _effect_entries.size()


func live_group_count() -> int:
	return _live.size()


func get_texture_provider() -> Callable:
	return _texture_provider


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
	_files.clear()
	_effect_entries.clear()
	_effects_by_name.clear()
	_interned.clear()
	_interned_by_name.clear()
	_tables.clear()
	_root = null
	_texture_provider = Callable()
	_spawn_serial = 0


func _register_file(file: NovaParticleFile) -> void:
	_files.append(file)
	for effect in file.get_effects():
		if effect == null:
			continue
		var entry := {"name": String(effect.id), "effect": effect, "file": file}
		_effect_entries.append(entry)
		var key := String(effect.id).to_lower()
		# First registration wins on duplicate ids (linear-scan-first semantics).
		if not _effects_by_name.has(key):
			_effects_by_name[key] = entry
	for table in file.get_tables():
		if table != null:
			_tables.append(table)


## Intern an effect name to its stable 1-based handle, registering on first
## use. 0 = unknown effect. [orig: CEffect_FindOrCreateMaterial @ 0x5f7310;
## the original clones "stockeffect" under an unknown name — not ported yet,
## recorded as follow-up in docs/particles/ptl-format-re.md §8.]
func intern_effect(name: String) -> int:
	var key := name.to_lower()
	var existing: int = _interned_by_name.get(key, 0)
	if existing > 0:
		return existing
	var entry: Dictionary = _effects_by_name.get(key, {})
	if entry.is_empty():
		return 0
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


## Spawn by an already-interned 1-based handle (the WAC fx parameter shape).
func spawn_effect_by_handle(handle: int, position: Vector3, orientation: Vector3 = Vector3.ZERO) -> bool:
	if handle < 1 or handle > _interned.size():
		return false
	_spawn_interned(handle, position, orientation)
	return true


func _spawn_interned(handle: int, position: Vector3, orientation: Vector3) -> void:
	var entry: Dictionary = _interned[handle - 1]
	var effect: NovaParticleEffect = entry.get("effect")
	var file: NovaParticleFile = entry.get("file")
	if effect == null:
		return
	var group := {"emitters": [], "window": 1.0, "forever": false, "elapsed": 0.0}
	var window := 0.0
	var pdefs: PackedStringArray = effect.pdefs
	for i in range(pdefs.size()):
		var def := _find_particle(file, pdefs[i])
		if def == null:
			continue
		var emitter := NovaParticleEmitter.new()
		emitter.name = "Fx%d_%d" % [_spawn_serial, i]
		emitter.seed = 1 + ((_spawn_serial * 17 + i) % 1023)
		emitter.set_tables(_tables)
		if _texture_provider.is_valid():
			emitter.texture_provider = _texture_provider
		emitter.def = def
		add_child(emitter)
		emitter.global_position = position
		if orientation.length_squared() > 0.0001:
			# Aim the emitter frame down the descriptor direction (cone/EMITVECTOR
			# shapes emit around the forward axis).
			var up := Vector3.UP if absf(orientation.normalized().dot(Vector3.UP)) < 0.99 else Vector3.RIGHT
			emitter.look_at(position + orientation.normalized(), up)
		emitter.play()
		group.emitters.append(emitter)
		window = maxf(window, _def_window_seconds(def))
		group.forever = group.forever or (int(def.flags) & PARTICLE_FLAG_FOREVER_EMIT) != 0
	_spawn_serial += 1
	if group.emitters.is_empty():
		return
	group.window = maxf(window, 1.0)
	_live.append(group)


## pdef lookup: the owning file first, then globally across every loaded file
## (cross-file id resolution, ptl-format-re §1.5).
func _find_particle(file: NovaParticleFile, id: String) -> NovaParticleDef:
	if file != null:
		var local := file.find_particle(id)
		if local != null:
			return local
	for other in _files:
		if other == file:
			continue
		var def: NovaParticleDef = other.find_particle(id)
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
		group.elapsed += delta
		var finished := false
		if not group.forever and group.elapsed >= group.window:
			finished = true
			for emitter in group.emitters:
				if is_instance_valid(emitter) and emitter.get_alive_count() > 0:
					finished = false
					break
		if finished:
			for emitter in group.emitters:
				if is_instance_valid(emitter):
					emitter.queue_free()
			_live.remove_at(i)
		else:
			i += 1
