class_name ProbeClock
extends RefCounted

## Frame/wall-clock waits the manual probes share. Every helper is a
## coroutine over the caller's SceneTree (`get_tree()` from a Node probe,
## `self` from a SceneTree probe): `await ProbeClock.settle(get_tree(), 3)`.


## Let `frames` process frames pass.
static func settle(tree: SceneTree, frames: int) -> void:
	for _i in frames:
		await tree.process_frame


## Let process frames pass until `ms` wall milliseconds have elapsed.
static func settle_ms(tree: SceneTree, ms: int) -> void:
	var deadline := Time.get_ticks_msec() + ms
	while Time.get_ticks_msec() < deadline:
		await tree.process_frame


## Wait `seconds` of MISSION time: wall time scaled by the probe's
## Engine.time_scale so a sped-up probe waits the same simulated span.
static func mission_wait(tree: SceneTree, seconds: float, time_scale: float) -> void:
	var t0 := Time.get_ticks_msec()
	while float(Time.get_ticks_msec() - t0) * time_scale < seconds * 1000.0:
		await tree.process_frame
