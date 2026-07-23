extends RefCounted


static func disable_processing(nodes: Array) -> Array:
	var states: Array = []
	for node_v in nodes:
		var node := node_v as Node
		if not is_instance_valid(node):
			continue
		states.append({
			node = node,
			proc = node.is_processing(),
			phys = node.is_physics_processing(),
		})
		node.set_process(false)
		node.set_physics_process(false)
	return states


static func restore_processing(states: Array) -> void:
	for state_v in states:
		var state: Dictionary = state_v
		var node := state.node as Node
		if is_instance_valid(node):
			node.set_process(bool(state.proc))
			node.set_physics_process(bool(state.phys))
