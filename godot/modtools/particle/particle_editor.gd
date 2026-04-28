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
	return OK


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
