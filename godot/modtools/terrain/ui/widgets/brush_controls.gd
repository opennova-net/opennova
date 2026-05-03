class_name BrushControls
extends VBoxContainer

signal radius_changed(value: float)
signal strength_changed(value: float)
signal hardness_changed(value: float)

const TerrainEditorBrushSession = preload("res://modtools/terrain/terrain_editor_brush_session.gd")

@onready var _radius_slider: HSlider = %RadiusSlider
@onready var _radius_spin: SpinBox = %RadiusSpin
@onready var _strength_slider: HSlider = %StrengthSlider
@onready var _strength_spin: SpinBox = %StrengthSpin
@onready var _hardness_slider: HSlider = %HardnessSlider
@onready var _hardness_spin: SpinBox = %HardnessSpin

var _syncing: bool = false


func get_radius_spin() -> SpinBox:
	return _radius_spin


func _ready() -> void:
	_apply_limits()
	_radius_slider.value_changed.connect(_on_radius_slider)
	_radius_spin.value_changed.connect(_on_radius_spin)
	_strength_slider.value_changed.connect(_on_strength_slider)
	_strength_spin.value_changed.connect(_on_strength_spin)
	_hardness_slider.value_changed.connect(_on_hardness_slider)
	_hardness_spin.value_changed.connect(_on_hardness_spin)


func _apply_limits() -> void:
	_radius_slider.min_value = TerrainEditorBrushSession.BRUSH_RADIUS_MIN
	_radius_slider.max_value = TerrainEditorBrushSession.BRUSH_RADIUS_MAX
	_radius_spin.min_value = TerrainEditorBrushSession.BRUSH_RADIUS_MIN
	_radius_spin.max_value = TerrainEditorBrushSession.BRUSH_RADIUS_MAX
	_strength_slider.min_value = TerrainEditorBrushSession.BRUSH_STRENGTH_MIN
	_strength_slider.max_value = TerrainEditorBrushSession.BRUSH_STRENGTH_MAX
	_strength_spin.min_value = TerrainEditorBrushSession.BRUSH_STRENGTH_MIN
	_strength_spin.max_value = TerrainEditorBrushSession.BRUSH_STRENGTH_MAX
	_hardness_slider.min_value = TerrainEditorBrushSession.BRUSH_HARDNESS_MIN
	_hardness_slider.max_value = TerrainEditorBrushSession.BRUSH_HARDNESS_MAX
	_hardness_spin.min_value = TerrainEditorBrushSession.BRUSH_HARDNESS_MIN
	_hardness_spin.max_value = TerrainEditorBrushSession.BRUSH_HARDNESS_MAX


## Update the visible values without emitting change signals.
func set_values(radius: float, strength: float, hardness: float) -> void:
	_syncing = true
	_radius_slider.value = radius
	_radius_spin.value = radius
	_strength_slider.value = strength
	_strength_spin.value = strength
	_hardness_slider.value = hardness
	_hardness_spin.value = hardness
	_syncing = false


func _on_radius_slider(v: float) -> void:
	if _syncing:
		return
	_syncing = true
	_radius_spin.value = v
	_syncing = false
	radius_changed.emit(v)


func _on_radius_spin(v: float) -> void:
	if _syncing:
		return
	_syncing = true
	_radius_slider.value = v
	_syncing = false
	radius_changed.emit(v)


func _on_strength_slider(v: float) -> void:
	if _syncing:
		return
	_syncing = true
	_strength_spin.value = v
	_syncing = false
	strength_changed.emit(v)


func _on_strength_spin(v: float) -> void:
	if _syncing:
		return
	_syncing = true
	_strength_slider.value = v
	_syncing = false
	strength_changed.emit(v)


func _on_hardness_slider(v: float) -> void:
	if _syncing:
		return
	_syncing = true
	_hardness_spin.value = v
	_syncing = false
	hardness_changed.emit(v)


func _on_hardness_spin(v: float) -> void:
	if _syncing:
		return
	_syncing = true
	_hardness_slider.value = v
	_syncing = false
	hardness_changed.emit(v)
