class_name WirePresentStats
extends RefCounted
## Typed diagnostic snapshot for the wire-present adapter.

var live: int
var spawned: int
var unresolved: int


func _init(p_live := 0, p_spawned := 0, p_unresolved := 0) -> void:
	live = p_live
	spawned = p_spawned
	unresolved = p_unresolved
