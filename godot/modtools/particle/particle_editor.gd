class_name ParticleEditor
extends RefCounted
## Document controller for the particle workspace. Owns the parsed
## NovaParticleFile (loaded from .ptl), tracks dirty state, and exposes the
## current selection to the workspace shell.
## RefCounted (not Node): no scene-tree role; freed when the workspace
## adapter drops the only reference.

signal document_changed
signal selection_changed
signal dirty_changed(is_dirty: bool)
## Lightweight presentation updates (ids, references, card summaries, curves).
## Unlike document_changed, this never asks inspectors to rebuild their forms.
signal presentation_changed
## Emitted when an edit should be reflected in the live preview (debounced by
## the preview itself). Kept separate from document_changed so field tweaks
## don't force a full inspector rebuild.
signal preview_refresh_requested

var particle_file: NovaParticleFile
var current_path: String = ""
var current_effect: NovaParticleEffect
var current_particle: NovaParticleDef
var current_table: NovaParticleTable
var is_dirty: bool = false


func _init() -> void:
	new_document()


func new_document() -> void:
	particle_file = NovaParticleFile.new()
	current_path = ""
	current_effect = null
	current_particle = null
	current_table = null
	_set_dirty(false)
	document_changed.emit()
	selection_changed.emit()


func open_file(path: String) -> Error:
	var file := NovaParticleFile.new()
	var err: int = file.load_from_file(path)
	if err != OK:
		return err
	_adopt_opened(file, path)
	return OK


## VFS open: parse .ptl bytes read from a mounted resource root (PFF archives).
## display_path is what save/status surfaces show; saving writes loose to it.
func open_bytes(bytes: PackedByteArray, display_path: String) -> Error:
	var file := NovaParticleFile.new()
	var err: int = file.load_from_buffer(bytes, display_path)
	if err != OK:
		return err
	_adopt_opened(file, display_path)
	return OK


func _adopt_opened(file: NovaParticleFile, path: String) -> void:
	particle_file = file
	current_path = path
	current_effect = null
	current_particle = null
	current_table = null
	if particle_file.particles.size() > 0:
		current_particle = particle_file.particles[0]
	if particle_file.effects.size() > 0:
		current_effect = particle_file.effects[0]
	if particle_file.tables.size() > 0:
		current_table = particle_file.tables[0]
	_set_dirty(false)
	document_changed.emit()
	selection_changed.emit()


func save_to_path(path: String) -> Error:
	if particle_file == null:
		return ERR_DOES_NOT_EXIST
	var err: int = particle_file.save_to_file(path)
	if err == OK:
		current_path = path
		_set_dirty(false)
	return err


func save_current() -> Error:
	if current_path.is_empty():
		return ERR_FILE_BAD_PATH
	return save_to_path(current_path)


func can_save() -> bool:
	return particle_file != null and not current_path.is_empty()


func mark_dirty() -> void:
	_set_dirty(true)


func _set_dirty(value: bool) -> void:
	if is_dirty == value:
		return
	is_dirty = value
	dirty_changed.emit(is_dirty)


func select_effect(effect: NovaParticleEffect) -> void:
	current_effect = effect
	selection_changed.emit()


func select_particle(particle: NovaParticleDef) -> void:
	current_particle = particle
	selection_changed.emit()


func select_table(table: NovaParticleTable) -> void:
	current_table = table
	selection_changed.emit()


func effect_count() -> int:
	return particle_file.effects.size() if particle_file != null else 0


func particle_count() -> int:
	return particle_file.particles.size() if particle_file != null else 0


func table_count() -> int:
	return particle_file.tables.size() if particle_file != null else 0


## Commit an in-place particle/graphic edit without rebuilding its inspector.
func notify_particle_changed() -> void:
	_set_dirty(true)
	presentation_changed.emit()
	preview_refresh_requested.emit()


func notify_table_changed() -> void:
	_set_dirty(true)
	presentation_changed.emit()
	preview_refresh_requested.emit()


# --- Identifier helpers -------------------------------------------------------

func _particle_ids() -> PackedStringArray:
	var ids := PackedStringArray()
	if particle_file != null:
		for entry in particle_file.particles:
			if entry != null:
				ids.append(entry.id)
	return ids


func _effect_ids() -> PackedStringArray:
	var ids := PackedStringArray()
	if particle_file != null:
		for entry in particle_file.effects:
			if entry != null:
				ids.append(entry.id)
	return ids


func _table_ids() -> PackedStringArray:
	var ids := PackedStringArray()
	if particle_file != null:
		for entry in particle_file.tables:
			if entry != null:
				ids.append(entry.id)
	return ids


## Return `base` if free, else `base2`, `base3`, ... — first name not in `taken`.
func _unique_id(base: String, taken: PackedStringArray) -> String:
	if base.is_empty():
		base = "item"
	if not taken.has(base):
		return base
	var n := 2
	while taken.has("%s%d" % [base, n]):
		n += 1
	return "%s%d" % [base, n]


# --- Particles: add / duplicate / remove -------------------------------------

## Create a visible default particle (white additive puff) and select it.
func add_particle() -> NovaParticleDef:
	if particle_file == null:
		return null
	var p := NovaParticleDef.new()
	p.id = _unique_id("particle", _particle_ids())
	p.emit_dur = 1.0
	p.emit_rate = 24.0
	p.emit_burst = 1
	p.emit_shape = 0
	p.age = 1.5
	p.alpha = 1.0
	p.scale_value = 2.0
	p.spread = 18.0
	p.speed = 2.0
	p.color1 = Color(1.0, 1.0, 1.0, 1.0)
	p.color2 = Color(1.0, 1.0, 1.0, 1.0)
	p.color3 = Color(1.0, 1.0, 1.0, 1.0)
	p.color4 = Color(1.0, 1.0, 1.0, 1.0)
	# Give it one present graphic layer so it renders immediately.
	var graphics := p.get_graphics()
	if graphics.size() > 0 and graphics[0] != null:
		var layer: NovaParticleGraphicLayer = graphics[0]
		layer.present = true
		layer.index = 1
		layer.blend_mode = 1  # Additive — reads well on the dark preview
		p.set_graphics(graphics)
	_append_to(particle_file, "particles", p)
	current_particle = p
	_set_dirty(true)
	document_changed.emit()
	selection_changed.emit()
	return p


func duplicate_particle(src: NovaParticleDef) -> NovaParticleDef:
	if particle_file == null or src == null:
		return null
	var p: NovaParticleDef = src.clone()
	p.id = _unique_id(src.id + "_copy", _particle_ids())
	_append_to(particle_file, "particles", p)
	current_particle = p
	_set_dirty(true)
	document_changed.emit()
	selection_changed.emit()
	return p


func remove_particle(p: NovaParticleDef) -> void:
	if particle_file == null or p == null:
		return
	var arr: Array = particle_file.particles
	var idx := arr.find(p)
	if idx < 0:
		return
	arr.remove_at(idx)
	particle_file.particles = arr
	if current_particle == p:
		current_particle = arr[mini(idx, arr.size() - 1)] if arr.size() > 0 else null
	_set_dirty(true)
	document_changed.emit()
	selection_changed.emit()


# --- Effects: add / duplicate / remove ---------------------------------------

func add_effect() -> NovaParticleEffect:
	if particle_file == null:
		return null
	var e := NovaParticleEffect.new()
	e.id = _unique_id("effect", _effect_ids())
	_append_to(particle_file, "effects", e)
	current_effect = e
	_set_dirty(true)
	document_changed.emit()
	selection_changed.emit()
	return e


func duplicate_effect(src: NovaParticleEffect) -> NovaParticleEffect:
	if particle_file == null or src == null:
		return null
	var e := NovaParticleEffect.new()
	e.id = _unique_id(src.id + "_copy", _effect_ids())
	e.pdefs = src.pdefs.duplicate()
	_append_to(particle_file, "effects", e)
	current_effect = e
	_set_dirty(true)
	document_changed.emit()
	selection_changed.emit()
	return e


func remove_effect(e: NovaParticleEffect) -> void:
	if particle_file == null or e == null:
		return
	var arr: Array = particle_file.effects
	var idx := arr.find(e)
	if idx < 0:
		return
	arr.remove_at(idx)
	particle_file.effects = arr
	if current_effect == e:
		current_effect = arr[mini(idx, arr.size() - 1)] if arr.size() > 0 else null
	_set_dirty(true)
	document_changed.emit()
	selection_changed.emit()


# --- Tables: add / duplicate / remove ----------------------------------------

func add_table() -> NovaParticleTable:
	if particle_file == null:
		return null
	var t := NovaParticleTable.new()
	t.id = _unique_id("table", _table_ids())
	# Default to a linear 0..255 ramp so the curve is immediately usable.
	var data := PackedByteArray()
	data.resize(256)
	for i in range(256):
		data[i] = i
	t.set_data(data)
	_append_to(particle_file, "tables", t)
	current_table = t
	_set_dirty(true)
	document_changed.emit()
	selection_changed.emit()
	return t


func duplicate_table(src: NovaParticleTable) -> NovaParticleTable:
	if particle_file == null or src == null:
		return null
	var t := NovaParticleTable.new()
	t.id = _unique_id(src.id + "_copy", _table_ids())
	t.set_data(src.get_data().duplicate())
	_append_to(particle_file, "tables", t)
	current_table = t
	_set_dirty(true)
	document_changed.emit()
	selection_changed.emit()
	return t


func remove_table(t: NovaParticleTable) -> void:
	if particle_file == null or t == null:
		return
	var arr: Array = particle_file.tables
	var idx := arr.find(t)
	if idx < 0:
		return
	arr.remove_at(idx)
	particle_file.tables = arr
	if current_table == t:
		current_table = arr[mini(idx, arr.size() - 1)] if arr.size() > 0 else null
	_set_dirty(true)
	document_changed.emit()
	selection_changed.emit()


# --- Graphic layers -----------------------------------------------------------

## Mark the first non-present layer present. Editor-created layers use the first
## free slot; loaded retail files may still retain sparse authored slots.
## Returns the new layer index, or -1 if full.
func add_graphic_layer(p: NovaParticleDef) -> int:
	if p == null:
		return -1
	var graphics := p.get_graphics()
	for i in range(graphics.size()):
		var layer: NovaParticleGraphicLayer = graphics[i]
		if layer != null and not layer.present:
			layer.present = true
			layer.index = i + 1
			p.set_graphics(graphics)
			_set_dirty(true)
			presentation_changed.emit()
			return i
	return -1


## Remove a layer and COMPACT the remaining present layers into slots 0..k-1 so
## the writer (which emits graphic1..N from graphics[0..N-1]) stays valid.
func remove_graphic_layer(p: NovaParticleDef, slot: int) -> void:
	if p == null:
		return
	var graphics := p.get_graphics()
	if slot < 0 or slot >= graphics.size():
		return
	var kept: Array[NovaParticleGraphicLayer] = []
	for i in range(graphics.size()):
		if i == slot:
			continue
		var layer: NovaParticleGraphicLayer = graphics[i]
		if layer != null and layer.present:
			kept.append(layer)
	var out: Array[NovaParticleGraphicLayer] = []
	for i in range(kept.size()):
		kept[i].index = i + 1
		out.append(kept[i])
	while out.size() < 4:
		var blank := NovaParticleGraphicLayer.new()
		blank.present = false
		blank.index = out.size() + 1
		out.append(blank)
	p.set_graphics(out)
	_set_dirty(true)
	presentation_changed.emit()


func present_graphic_count(p: NovaParticleDef) -> int:
	if p == null:
		return 0
	var count := 0
	for layer in p.get_graphics():
		if layer != null and layer.present:
			count += 1
	return count


# --- Reference edits (used by the blueprint graph + inspectors) --------------

func set_particle_id(particle: NovaParticleDef, value: String) -> void:
	if particle == null or particle.id == value:
		return
	var old_id := String(particle.id)
	particle.id = value
	if particle_file != null:
		for effect_entry in particle_file.effects:
			var effect := effect_entry as NovaParticleEffect
			if effect == null:
				continue
			var pdefs := effect.pdefs
			var changed := false
			for i in range(pdefs.size()):
				if pdefs[i] == old_id:
					pdefs[i] = value
					changed = true
			if changed:
				effect.pdefs = pdefs
		for entry in particle_file.particles:
			var candidate := entry as NovaParticleDef
			if candidate != null and candidate.child_id == old_id:
				candidate.child_id = value
	_set_dirty(true)
	presentation_changed.emit()
	preview_refresh_requested.emit()


func set_table_id(table: NovaParticleTable, value: String) -> void:
	if table == null or table.id == value:
		return
	var old_id := String(table.id)
	table.id = value
	if particle_file != null:
		for entry in particle_file.particles:
			var particle := entry as NovaParticleDef
			if particle != null:
				_rename_table_refs(particle, old_id, value)
		for entry in particle_file.table_handles:
			var handles := entry as NovaParticleTableHandles
			if handles != null and handles.table_id == old_id:
				handles.table_id = value
	_set_dirty(true)
	presentation_changed.emit()
	preview_refresh_requested.emit()


func _rename_table_refs(particle: NovaParticleDef, old_id: String, new_id: String) -> void:
	const PARTICLE_CURVES := [
		"scale_func", "alpha_func", "red_func", "green_func", "blue_func",
		"emit_rate_func",
	]
	const GRAPHIC_CURVES := [
		"scale_func", "alpha_func", "red_func", "green_func", "blue_func",
	]
	for field in PARTICLE_CURVES:
		var curve := particle.get(field) as NovaParticleCurveRef
		if curve != null and curve.name == old_id:
			curve.name = new_id
	for graphic_entry in particle.get_graphics():
		var graphic := graphic_entry as NovaParticleGraphicLayer
		if graphic == null:
			continue
		for field in GRAPHIC_CURVES:
			var curve := graphic.get(field) as NovaParticleCurveRef
			if curve != null and curve.name == old_id:
				curve.name = new_id


func set_effect_id(effect: NovaParticleEffect, value: String) -> void:
	if effect == null or effect.id == value:
		return
	effect.id = value
	_set_dirty(true)
	document_changed.emit()


func set_effect_pdefs(effect: NovaParticleEffect, values: PackedStringArray) -> void:
	if effect == null or effect.pdefs == values:
		return
	effect.pdefs = values
	_set_dirty(true)
	document_changed.emit()
	preview_refresh_requested.emit()

func effect_add_pdef(effect: NovaParticleEffect, pdef_id: String) -> void:
	if effect == null or pdef_id.is_empty():
		return
	var arr := effect.pdefs
	if arr.has(pdef_id):
		return
	arr.append(pdef_id)
	effect.pdefs = arr
	_set_dirty(true)
	document_changed.emit()
	preview_refresh_requested.emit()


func effect_remove_pdef(effect: NovaParticleEffect, pdef_id: String) -> void:
	if effect == null:
		return
	var arr := effect.pdefs
	var idx := arr.find(pdef_id)
	if idx < 0:
		return
	arr.remove_at(idx)
	effect.pdefs = arr
	_set_dirty(true)
	document_changed.emit()
	preview_refresh_requested.emit()


func set_child_id(p: NovaParticleDef, child: String) -> void:
	if p == null or p.child_id == child:
		return
	p.child_id = child
	_set_dirty(true)
	presentation_changed.emit()
	preview_refresh_requested.emit()


## Assign (or clear, when table_id == "") a [tabledef] to one of the particle's
## *_func curve slots. Keeps CurveRef.present in sync with the writer's gate.
func assign_curve(p: NovaParticleDef, field: String, table_id: String) -> void:
	if p == null:
		return
	var curve: NovaParticleCurveRef = _curve_for(p, field)
	if curve == null:
		return
	curve.name = table_id
	curve.present = not table_id.is_empty()
	_apply_curve(p, field, curve)
	_set_dirty(true)


func _curve_for(p: NovaParticleDef, field: String) -> NovaParticleCurveRef:
	match field:
		"scale_func": return p.get_scale_func()
		"alpha_func": return p.get_alpha_func()
		"red_func": return p.get_red_func()
		"green_func": return p.get_green_func()
		"blue_func": return p.get_blue_func()
		"emit_rate_func": return p.get_emit_rate_func()
	return null


func _apply_curve(p: NovaParticleDef, field: String, curve: NovaParticleCurveRef) -> void:
	match field:
		"scale_func": p.set_scale_func(curve)
		"alpha_func": p.set_alpha_func(curve)
		"red_func": p.set_red_func(curve)
		"green_func": p.set_green_func(curve)
		"blue_func": p.set_blue_func(curve)
		"emit_rate_func": p.set_emit_rate_func(curve)


# --- Pre-save validation ------------------------------------------------------

## Return an array of {severity: "error"|"warning", message: String}. Errors
## block a save; warnings are surfaced but allow it. The C++ writer guarantees
## structural validity, so this only guards model-level mistakes the UI could
## introduce.
func validate() -> Array:
	var issues: Array = []
	if particle_file == null:
		return issues
	_validate_unique_nonempty(particle_file.particles, "particle", issues)
	_validate_unique_nonempty(particle_file.effects, "effect", issues)
	_validate_unique_nonempty(particle_file.tables, "table", issues)
	for entry in particle_file.particles:
		var p: NovaParticleDef = entry
		if p == null:
			continue
		if p.emit_burst < 1:
			issues.append({"severity": "error",
					"message": "Particle '%s': burst count must be at least 1." % p.id})
	return issues


func _validate_unique_nonempty(items: Array, label: String, issues: Array) -> void:
	var seen := {}
	for entry in items:
		if entry == null:
			continue
		var id: String = entry.id
		if id.strip_edges().is_empty():
			issues.append({"severity": "error", "message": "A %s has an empty name." % label})
		elif seen.has(id):
			issues.append({"severity": "warning",
					"message": "Duplicate %s name '%s'." % [label, id]})
		else:
			seen[id] = true


# --- Internal array mutation --------------------------------------------------

## get -> append -> set-back, the safe TypedArray-property mutation pattern.
func _append_to(file: NovaParticleFile, prop: String, value) -> void:
	var arr: Array = file.get(prop)
	arr.append(value)
	file.set(prop, arr)
