class_name OcclusionRecorder
extends RefCounted

## Occlusion recorder driven through the typed Callable override seam
## (set_occlusion_override) of MissionAudio and the sound bank; the live path is
## the Simulation provider. The value-only stand-in pins what the consumer does
## with the returned retail occlusion distance without fabricating collision
## internals: a negative `distance_q16` passes the raw distance through.

var distance_q16 := -1
var calls := 0
var source_bms_ids: Array[int] = []


func _init(p_distance_q16: int = -1) -> void:
	distance_q16 = p_distance_q16


func occlude(_listener_pos: Vector3, _source_pos: Vector3,
		raw_distance_q16: int, source_bms_id: int) -> int:
	calls += 1
	source_bms_ids.append(source_bms_id)
	return raw_distance_q16 if distance_q16 < 0 else distance_q16
