class_name MusInputNames
extends RefCounted

# Per-section friendly names for a state's caller inputs (the l_<base+4k>
# locals a frame-setup `enter` banks). Display-only, like MusVarNames: the
# script always serializes the canonical l_N tokens; these labels only change
# what the editor SHOWS ("Mission event" instead of "Input 1").
#
# Stored in the same .music_profile.json sidecar MusVarNames owns, under a
# sibling top-level key (its read-merge-write preserves keys it doesn't know):
#   { "script_name": "gamescript",
#     "vars": { ... },
#     "section_inputs": { "Begin": { "0": { "label": "Mission event" } } } }
# Inputs are keyed by SECTION because the same l_32 slot means different
# things in different states (each frame-setup banks its own values there).

const _MAX_INPUT := 15


# {input_index (0-based) -> label} for one section, {} when nothing is named.
static func labels_for(script_name: String, section_name: String, profile_path: String) -> Dictionary:
	var sections := _profile_sections(script_name, profile_path)
	var raw: Dictionary = sections.get(section_name, {})
	var out := {}
	for raw_key in raw.keys():
		var key_str := String(raw_key)
		if not key_str.is_valid_int():
			continue
		var idx := int(key_str)
		if idx < 0 or idx > _MAX_INPUT:
			continue
		if raw[raw_key] is Dictionary:
			var label := String((raw[raw_key] as Dictionary).get("label", ""))
			if label != "":
				out[idx] = label
	return out


# Write (or clear, with an empty label) one input's friendly name. Read-merge-
# write: other sections' inputs, the var labels, and any future profile keys
# survive. A profile carrying a DIFFERENT script's name is replaced outright
# (mirrors MusVarNames.set_label).
static func set_input_label(profile_path: String, script_name: String, section_name: String, input_index: int, label: String) -> int:
	if profile_path == "" or section_name == "" or input_index < 0 or input_index > _MAX_INPUT:
		return ERR_INVALID_PARAMETER
	var profile := _read_profile(profile_path)
	var profile_script := String(profile.get("script_name", ""))
	if profile_script != "" and profile_script != script_name:
		profile = {}
	profile["script_name"] = script_name
	var sections: Dictionary = profile.get("section_inputs", {}) if profile.get("section_inputs", null) is Dictionary else {}
	var sec: Dictionary = sections.get(section_name, {}) if sections.get(section_name, null) is Dictionary else {}
	var key := str(input_index)
	var entry: Dictionary = sec.get(key, {}) if sec.get(key, null) is Dictionary else {}
	var clean := label.strip_edges()
	if clean == "":
		entry.erase("label")
	else:
		entry["label"] = clean
	if entry.is_empty():
		sec.erase(key)
	else:
		sec[key] = entry
	if sec.is_empty():
		sections.erase(section_name)
	else:
		sections[section_name] = sec
	if sections.is_empty():
		profile.erase("section_inputs")
	else:
		profile["section_inputs"] = sections
	return _write_profile(profile_path, profile)


# Keep input names attached through a state rename (call right after the
# document's rename_section succeeds). A missing old entry is a quiet OK.
static func rename_section(profile_path: String, script_name: String, old_name: String, new_name: String) -> int:
	if profile_path == "" or old_name == "" or new_name == "" or old_name == new_name:
		return ERR_INVALID_PARAMETER
	var profile := _read_profile(profile_path)
	var profile_script := String(profile.get("script_name", ""))
	if profile_script != "" and profile_script != script_name:
		return OK
	if not (profile.get("section_inputs", null) is Dictionary):
		return OK
	var sections: Dictionary = profile["section_inputs"]
	if not sections.has(old_name):
		return OK
	sections[new_name] = sections[old_name]
	sections.erase(old_name)
	profile["section_inputs"] = sections
	return _write_profile(profile_path, profile)


static func _read_profile(profile_path: String) -> Dictionary:
	if profile_path == "" or not FileAccess.file_exists(profile_path):
		return {}
	var f := FileAccess.open(profile_path, FileAccess.READ)
	if f == null:
		return {}
	var parsed = JSON.parse_string(f.get_as_text())
	return parsed if parsed is Dictionary else {}


static func _write_profile(profile_path: String, profile: Dictionary) -> int:
	var out := FileAccess.open(profile_path, FileAccess.WRITE)
	if out == null:
		return FileAccess.get_open_error()
	out.store_string(JSON.stringify(profile, "  "))
	out.close()
	return OK


static func _profile_sections(script_name: String, profile_path: String) -> Dictionary:
	var profile := _read_profile(profile_path)
	if profile.is_empty():
		return {}
	var profile_script := String(profile.get("script_name", ""))
	if profile_script != "" and profile_script != script_name:
		return {}
	if not (profile.get("section_inputs", null) is Dictionary):
		return {}
	return profile["section_inputs"]
