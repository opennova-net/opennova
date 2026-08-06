class_name AiProfileSpeeds
extends RefCounted

# Builds the per-profile .aip speed table the runtime feeds
# NovaSimulation.set_ai_profile_speeds — the two witnessed AIProfile fields
# (patrol_speed / combat_speed), read from each mission entity's ai_textfile
# profile via the resource root. Raw authored values; the native brain seed
# applies the witnessed x65536/225 scale. The rest of the .aip parse remains
# the tracked D-AI-11 (h) gap.
## [orig: AIProfile_ParseProperty "patrol_speed" -> profile+0xC0 @0x45E6DF /
##  "combat_speed" -> profile+0xC4; Entity_InitVehicleAIFromDef seeds
##  brain[50]/brain[49] from them @0x4688D3/@0x4688C7]


static func build(mission, resource_root) -> Dictionary:
	if mission == null or resource_root == null:
		return {}
	var out := {}
	for raw in mission.get_all_entities():
		var entity: Dictionary = raw
		var profile := String(entity.get("name2", "")).strip_edges().to_lower()
		if profile.is_empty() or out.has(profile):
			continue
		var speeds := _read_profile_speeds(resource_root, profile)
		if not speeds.is_empty():
			out[profile] = speeds
	return out


static func _read_profile_speeds(resource_root, profile: String) -> Dictionary:
	var file_name := profile + ".aip"
	if not resource_root.has_file(file_name):
		return {}
	var bytes: PackedByteArray = resource_root.read_file(file_name)
	if bytes.is_empty():
		return {}
	var out := {}
	for line in bytes.get_string_from_ascii().split("\n"):
		var tokens := line.replace("\t", " ").strip_edges().split(" ", false)
		if tokens.size() < 2:
			continue
		var key := String(tokens[0]).to_lower()
		if key == "patrol_speed":
			out["patrol"] = int(tokens[1])
		elif key == "combat_speed":
			out["combat"] = int(tokens[1])
	return out
