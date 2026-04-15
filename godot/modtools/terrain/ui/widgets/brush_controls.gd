class_name BrushControls
extends VBoxContainer

signal radius_changed(value: float)
signal strength_changed(value: float)
signal hardness_changed(value: float)

const RADIUS_MIN := 1.0
const RADIUS_MAX := 128.0
const STRENGTH_MIN := 0.01
const STRENGTH_MAX := 5.0
const HARDNESS_MIN := 0.0
const HARDNESS_MAX := 1.0

@onready var _radius_slider: HSlider = %RadiusSlider
@onready var _radius_spin: SpinBox = %RadiusSpin
@onready var _strength_slider: HSlider = %StrengthSlider
@onready var _strength_spin: SpinBox = %StrengthSpin
@onready var _hardness_slider: HSlider = %HardnessSlider
@onready var _hardness_spin: SpinBox = %HardnessSpin

var _syncing: bool = false


func _ready() -> void:
	_radius_slider.value_changed.connect(_on_radius_slider)
	_radius_spin.value_changed.connect(_on_radius_spin)
	_strength_slider.value_changed.connect(_on_strength_slider)
	_strength_spin.value_changed.connect(_on_strength_spin)
	_hardness_slider.value_changed.connect(_on_hardness_slider)
	_hardness_spin.value_changed.connect(_on_hardness_spin)


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
