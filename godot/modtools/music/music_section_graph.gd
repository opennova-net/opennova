class_name MusicSectionGraph
extends RefCounted

# Returns Dictionary{section_name: Array[section_name]} of edges from setstate
# targets parsed out of the decompiled text. Per Phase A there is no AST API,
# so we walk the decompile output looking for "section <NAME> {" boundaries
# and "setstate <TARGET>" calls within each.

static func analyze(script_resource: NovaMusicScript, script_name: StringName) -> Dictionary:
	var graph: Dictionary = {}
	var section_names: PackedStringArray = script_resource.get_section_names(script_name)
	for s in section_names:
		graph[s] = []
	var text := script_resource.get_decompiled_text(script_name)
	var lines := text.split("\n")
	var current_section := ""
	for line in lines:
		var stripped: String = line.strip_edges()
		if stripped.begins_with("section "):
			var parts := stripped.split(" ", false)
			if parts.size() >= 2:
				current_section = parts[1].rstrip(" {")
		elif stripped.begins_with("setstate "):
			var target := stripped.substr(9).rstrip(" ;")
			if current_section != "" and target in section_names:
				if not graph[current_section].has(target):
					graph[current_section].append(target)
	return graph
