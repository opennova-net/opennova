class_name DebugStatsDisplayRow
extends RefCounted
## Read-only value snapshot of one F3 Stats row. Tests and diagnostic probes
## consume this semantic surface instead of reaching through the pane's Tree.

var id: StringName
var label: String
var average: String
var peak: String
var info: String


func _init(p_id: StringName, p_label: String, p_average: String,
		p_peak: String, p_info: String) -> void:
	id = p_id
	label = p_label
	average = p_average
	peak = p_peak
	info = p_info
