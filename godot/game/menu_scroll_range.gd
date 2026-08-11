extends RefCounted

# Typed CScrollWnd render state shared by MenuDriver's saved-state store and
# the frame replay module. Construction owns the original range normalization:
# invalid min/max resets to zero and the current value is clamped.

var minimum: int
var maximum: int
var page: int
var value: int


func _init(p_minimum: int, p_maximum: int, p_page: int, p_value: int) -> void:
	minimum = p_minimum
	maximum = p_maximum
	page = p_page
	value = p_value
