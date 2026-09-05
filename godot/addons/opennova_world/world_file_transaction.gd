@tool
extends RefCounted
## Stages every native writer before replacing any document. Originals stay
## available for rollback until all replacements succeed, on the same volume.


static func write(directory: String, writers: Dictionary, preflight: Callable,
		replace_existing: bool) -> String:
	var reason: String = preflight.call()
	if not reason.is_empty():
		return reason
	var staging := directory.path_join(".opennova-save-%d-%d" % [OS.get_process_id(), Time.get_ticks_usec()])
	if DirAccess.make_dir_absolute(staging) != OK:
		return "Cannot create a save staging folder in %s." % directory
	var staged: PackedStringArray = []
	for filename: String in writers:
		var path := staging.path_join(filename)
		staged.append(path)
		var error: Error = writers[filename].call(path)
		if error != OK:
			_cleanup(staging, staged)
			return "Could not write %s (%s). The original files are unchanged." % [filename, error_string(error)]
	# Detect changes made by another editor while the writers were running.
	reason = preflight.call()
	if not reason.is_empty():
		_cleanup(staging, staged)
		return reason
	var backups: Dictionary[String, String] = {}
	var installed: PackedStringArray = []
	for filename: String in writers:
		var target := directory.path_join(filename)
		if replace_existing:
			var backup := staging.path_join(filename + ".original")
			if DirAccess.rename_absolute(target, backup) != OK:
				return _rollback(staging, staged, installed, backups, "Cannot replace %s." % target)
			backups[target] = backup
		if DirAccess.rename_absolute(staging.path_join(filename), target) != OK:
			return _rollback(staging, staged, installed, backups, "Cannot install %s." % target)
		installed.append(target)
	for backup: String in backups.values():
		staged.append(backup)
	_cleanup(staging, staged)
	return ""


static func _rollback(staging: String, staged: PackedStringArray, installed: PackedStringArray,
		backups: Dictionary[String, String], reason: String) -> String:
	var restored := true
	for path in installed:
		if DirAccess.remove_absolute(path) != OK:
			restored = false
	for target: String in backups:
		if DirAccess.rename_absolute(backups[target], target) != OK:
			restored = false
	if restored:
		_cleanup(staging, staged)
		return reason + " The original files were restored; your edits remain open."
	# Never remove a backup we could not restore. The error names its location.
	return reason + " Recovery files remain in %s; your edits remain open." % staging


static func _cleanup(staging: String, paths: PackedStringArray) -> void:
	for path in paths:
		if FileAccess.file_exists(path):
			DirAccess.remove_absolute(path)
	DirAccess.remove_absolute(staging)
