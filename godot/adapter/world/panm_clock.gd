class_name PanmClock
extends RefCounted

# Retail samples GetTickCount once into one global DWORD for a rendered frame.
# Models, material animation, and Generic collision all consume that same value.
var time_ms: int = 0
var _sampled_frame: int = -1


func sample_frame() -> bool:
	return sample(int(Time.get_ticks_msec()), int(Engine.get_process_frames()))


func sample(value_ms: int, frame: int) -> bool:
	if frame == _sampled_frame:
		return false
	_sampled_frame = frame
	time_ms = value_ms & 0xffffffff
	return true


func set_time_ms_for_test(value_ms: int) -> void:
	time_ms = value_ms & 0xffffffff
	_sampled_frame = -1
