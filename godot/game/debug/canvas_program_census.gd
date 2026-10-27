class_name CanvasProgramCensus
extends RefCounted

## The canvas programs the scene's items draw through (an instrument, --verbose only): each call
## prints every program a CanvasItem's material draws through that no earlier call saw, named by
## its shader's resource path, by the first characters of the code of a shader built from code, or
## as the program a CanvasItemMaterial generates, with the item that draws it. The reveal takes
## the census first (ShellPresentationSession) and the first post-load frames take it again
## (SlowFrameReport), so a program first seen on a frame past the reveal is a candidate for a
## "Shader cache miss" past the load's end (D-NET-1367).

const CODE_HEAD_CHARS := 96

static var _seen := {}


## Print the programs first seen now under `label`; returns how many there were.
static func note(tree: SceneTree, label: String) -> int:
	if tree == null or not OS.is_stdout_verbose():
		return 0
	var fresh := PackedStringArray()
	for node in tree.root.find_children("*", "CanvasItem", true, false):
		var material := (node as CanvasItem).material
		if material == null:
			continue
		var program: Object = material
		var program_name := ""
		if material is ShaderMaterial:
			program = (material as ShaderMaterial).shader
			if program == null:
				continue
			program_name = _shader_name(program as Shader)
		elif material is CanvasItemMaterial:
			var item_material := material as CanvasItemMaterial
			program_name = "CanvasItemMaterial blend %d light %d" % [
					item_material.blend_mode, item_material.light_mode]
		else:
			continue
		var id := program.get_instance_id()
		if _seen.has(id):
			continue
		_seen[id] = true
		fresh.append("%s on %s" % [program_name, node.name])
	if not fresh.is_empty():
		print_verbose("CanvasPrograms %s: %d first seen [%s]" % [label, fresh.size(), "; ".join(fresh)])
	return fresh.size()


static func _shader_name(shader: Shader) -> String:
	if not shader.resource_path.is_empty():
		return shader.resource_path
	var code := shader.code.strip_edges().replace("\n", " ").replace("\t", " ")
	return "code \"%s\"" % code.substr(0, CODE_HEAD_CHARS)
