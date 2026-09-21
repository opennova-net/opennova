class_name AuthoredItemFixture
extends RefCounted

# Fixture selection for installed presentation tests, read from the authored
# items.def asset independently of the native extraction under test. Only the
# item ids, attachment inputs and weapon names needed to set up those scenes
# are read here. Parser/extractor semantics are covered by def_parse_items and
# mission_seat_spec_extract; this is not a runtime definition parser.
static func read_rows(root: ResourceRoot) -> Dictionary:
	var rows := {}
	var current := {}
	for line: String in root.read_file("items.def").get_string_from_ascii().split("\n"):
		var words := line.strip_edges().replace("\t", " ").split(" ", false)
		if words.is_empty():
			continue
		var key := words[0].to_lower()
		if key == "begin":
			current = {"id": 0, "weapon": "", "attachments": []}
		elif key == "end" and not current.is_empty():
			rows[current["id"]] = current
			current = {}
		elif not current.is_empty():
			if key == "id" and words.size() >= 2:
				current["id"] = int(words[1])
			elif key == "primary_weapon" and words.size() >= 2:
				current["weapon"] = words[1]
			elif key in ["addeweap", "addeweapg", "addeweapc"] and words.size() >= 3:
				current["attachments"].append({
					"kind": key, "userpoint": words[1], "item_id": int(words[2]),
				})
	return rows
