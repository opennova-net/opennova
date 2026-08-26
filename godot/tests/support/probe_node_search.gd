class_name ProbeNodeSearch
extends RefCounted

## Node-tree searches the manual probes share. Test-only: find_by_method is
## a has_method duck probe by design (the probes locate shell nodes by the
## seam they drive, not by class), which production code never does.


## Depth-first: the first node in the subtree exposing `method`, else null.
static func find_by_method(node: Node, method: String) -> Node:
	if node.has_method(method):
		return node
	for child in node.get_children():
		var found := find_by_method(child, method)
		if found != null:
			return found
	return null


## The merged mesh AABB of a subtree in `to_local` space; null when no
## MeshInstance3D with a mesh lives under `node`.
static func combined_aabb(node: Node, to_local: Transform3D) -> Variant:
	var out: Variant = null
	if node is MeshInstance3D:
		var mi := node as MeshInstance3D
		if mi.mesh != null:
			out = ((to_local * mi.global_transform) * mi.mesh.get_aabb()) as AABB
	for child in node.get_children():
		var sub: Variant = combined_aabb(child, to_local)
		if sub == null:
			continue
		out = sub if out == null else (out as AABB).merge(sub as AABB)
	return out
