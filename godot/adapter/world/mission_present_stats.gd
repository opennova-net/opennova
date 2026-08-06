class_name MissionPresentStats
extends RefCounted
## Typed diagnostic snapshot for the native mission-present row applier.

var moved: int
var posed: int
var hidden: int
var muzzles: int
var plan_rebuilds: int
var transform_builds: int
var aim_dispatches: int
var rhc_dispatches: int
var part_dispatches: int
var control_dispatches: int
var body_dispatches: int
var muzzle_queries: int


func _init(native_stats: Dictionary = {}) -> void:
	# NovaPresentApplier's binding Dictionary is the native transport edge;
	# callers receive this typed ADR-0017 record.
	moved = int(native_stats.get("moved", 0))
	posed = int(native_stats.get("posed", 0))
	hidden = int(native_stats.get("hidden", 0))
	muzzles = int(native_stats.get("muzzles", 0))
	plan_rebuilds = int(native_stats.get("plan_rebuilds", 0))
	transform_builds = int(native_stats.get("transform_builds", 0))
	aim_dispatches = int(native_stats.get("aim_dispatches", 0))
	rhc_dispatches = int(native_stats.get("rhc_dispatches", 0))
	part_dispatches = int(native_stats.get("part_dispatches", 0))
	control_dispatches = int(native_stats.get("control_dispatches", 0))
	body_dispatches = int(native_stats.get("body_dispatches", 0))
	muzzle_queries = int(native_stats.get("muzzle_queries", 0))
