class_name MissionCatalog
extends RefCounted

# The one ".bms is the mission set" rule for the game shell: missions are the
# .bms entries the mounted resource root exposes, listed by basename. Every
# shell surface that enumerates missions (menu seeding, host screens, the
# default pick) goes through here. `root` stays duck-typed (NovaResourceRoot
# at runtime; tests inject stand-ins that only implement list_files).


static func mission_names(root) -> PackedStringArray:
	var names := PackedStringArray()
	if root == null or not root.has_method("list_files"):
		return names
	for m in root.list_files(".bms"):
		names.append(String(m).get_file())
	return names


static func first_mission_name(root) -> String:
	var names := mission_names(root)
	return names[0] if names.size() > 0 else ""
