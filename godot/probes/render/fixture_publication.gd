class_name FixturePublication
extends RefCounted

## The fixture publication transaction (docs/render/render-fixtures-v1.json):
## a staging directory under the trusted scratch root, owner/installing/
## installed/rolling-back records, recovery of an abandoned publication and
## the atomic replace of the previous fixture. The render_fixture_capture
## probe drives it; its GUT companion exercises every branch offline.

const PUBLICATION_TRANSACTION_SUFFIX := ".publication-transaction"
const PUBLICATION_OWNER_RECORD := "owner.json"
const PUBLICATION_INSTALL_RECORD := "installing.json"
const PUBLICATION_INSTALLED_RECORD := "installed.json"
const PUBLICATION_ROLLBACK_RECORD := "rolling-back.json"
const PUBLICATION_STAGING_DIR := "staging"
const PUBLICATION_BACKUP_DIR := "previous"
const PUBLICATION_CREATING_SEGMENT := ".creating."
const PUBLICATION_RECOVERING_SEGMENT := ".recovering."
const PUBLICATION_TRANSACTION_SCHEMA := \
		"opennova.fixture-publication-transaction.v1"


static func publication_name_is_canonical(value: String) -> bool:
	if value.is_empty() or value.length() > 180:
		return false
	var bytes := value.to_ascii_buffer()
	for code: int in bytes:
		var alpha_numeric := (code >= 48 and code <= 57) \
				or (code >= 65 and code <= 90) \
				or (code >= 97 and code <= 122)
		if not alpha_numeric and code != 45 and code != 95:
			return false
	var first := int(bytes[0])
	var last := int(bytes[bytes.size() - 1])
	return ((first >= 48 and first <= 57) \
			or (first >= 65 and first <= 90) \
			or (first >= 97 and first <= 122)) \
			and ((last >= 48 and last <= 57) \
			or (last >= 65 and last <= 90) \
			or (last >= 97 and last <= 122))


static func publication_child_path(output_abs: String, filename: String) -> String:
	var output := output_abs.simplify_path()
	if output.is_empty() or not output.is_absolute_path() \
			or filename.is_empty() or filename in [".", ".."] \
			or filename.contains("/") or filename.contains("\\") \
			or filename.contains(":"):
		return ""
	var child := output.path_join(filename).simplify_path()
	if not RenderFixtureContract.same_absolute_path(child.get_base_dir(), output):
		return ""
	return child


static func begin_fixture_publication(
		output_abs: String,
		trusted_root_abs: String,
		owner_is_running: Callable = Callable(),
		after_recovery_claim: Callable = Callable()) -> Dictionary:
	var output := _normalized_publication_path(output_abs)
	var trusted_root := _normalized_publication_path(trusted_root_abs)
	if output.is_empty() or trusted_root.is_empty() \
			or not output.is_absolute_path() \
			or not trusted_root.is_absolute_path() \
			or not path_is_strict_descendant(output, trusted_root):
		return {"error": (
				"fixture publication output must be a strict descendant of its trusted root")}
	if not _publication_path_has_safe_ancestors(output, trusted_root):
		return {"error": "fixture publication output has an unsafe ancestor: %s" \
				% output}
	var parent_error := DirAccess.make_dir_recursive_absolute(
			output.get_base_dir())
	if parent_error != OK:
		return {"error": "cannot create fixture publication parent: %s" \
				% error_string(parent_error)}
	if not _publication_path_has_safe_ancestors(output, trusted_root):
		return {"error": "fixture publication output has an unsafe ancestor: %s" \
				% output}
	if FileAccess.file_exists(output):
		return {"error": "fixture publication output is a file: %s" % output}

	var transaction_root := _publication_transaction_root(output)
	if not _publication_path_has_safe_ancestors(
			transaction_root, trusted_root):
		return {"error": "fixture publication journal has an unsafe ancestor: %s" \
				% transaction_root}
	var recovery_error := _recover_abandoned_publication(
			output, trusted_root, owner_is_running, after_recovery_claim)
	if recovery_error != OK:
		return {"error": "cannot recover prior fixture publication: %s" \
				% error_string(recovery_error)}
	return _create_fixture_publication(output, trusted_root)


static func _create_fixture_publication(
		output: String, trusted_root: String) -> Dictionary:
	var transaction_root := _publication_transaction_root(output)
	var reservation := "%s%s%d.%d" % [
		transaction_root, PUBLICATION_CREATING_SEGMENT,
		OS.get_process_id(), Time.get_ticks_usec(),
	]
	if not _publication_path_has_safe_ancestors(reservation, trusted_root):
		return {"error": "fixture publication reservation has an unsafe ancestor"}
	var reservation_error := DirAccess.make_dir_absolute(reservation)
	if reservation_error != OK:
		return {"error": "cannot reserve fixture publication journal: %s" \
				% error_string(reservation_error)}
	if not _publication_path_has_safe_ancestors(reservation, trusted_root):
		return {"error": "fixture publication reservation became unsafe"}
	var owner_error := _write_new_atomic_json(
			reservation.path_join(PUBLICATION_OWNER_RECORD), {
				"schema": PUBLICATION_TRANSACTION_SCHEMA,
				"output": output,
				"trusted_root": trusted_root,
				"owner_pid": OS.get_process_id(),
			})
	if owner_error != OK:
		_remove_tree_absolute(reservation, trusted_root)
		return {"error": "cannot write fixture publication journal: %s" \
				% error_string(owner_error)}
	var reserved_staging := reservation.path_join(PUBLICATION_STAGING_DIR)
	if not _publication_path_has_safe_ancestors(
			reserved_staging, trusted_root):
		return {"error": "fixture publication staging reservation became unsafe"}
	var staging_error := DirAccess.make_dir_absolute(reserved_staging)
	if staging_error != OK:
		_remove_tree_absolute(reservation, trusted_root)
		return {"error": "cannot create fixture publication staging path: %s" \
				% error_string(staging_error)}
	if not _publication_path_has_safe_ancestors(
			reserved_staging, trusted_root):
		return {"error": "fixture publication staging reservation became unsafe"}

	# A complete owner/staging journal becomes visible atomically. No competitor
	# can mistake the short construction window for a dead or corrupt journal.
	var claims := _publication_recovery_claims(output, trusted_root)
	if claims.has("error") or not (claims.get("paths", []) as Array).is_empty() \
			or DirAccess.dir_exists_absolute(transaction_root) \
			or FileAccess.file_exists(transaction_root):
		_remove_tree_absolute(reservation, trusted_root)
		return {"error": "fixture publication is busy"}
	var publish_error := DirAccess.rename_absolute(reservation, transaction_root)
	# Windows can transiently reject the rename of a newly created journal.
	# Retry only while the destination is absent and the reservation and its
	# ancestors remain ours. A competing owner or recovery claim still wins.
	if publish_error != OK and OS.get_name() == "Windows":
		for _attempt in range(3):
			OS.delay_msec(1)
			if not DirAccess.dir_exists_absolute(reservation) \
					or DirAccess.dir_exists_absolute(transaction_root) \
					or FileAccess.file_exists(transaction_root) \
					or not _publication_path_has_safe_ancestors(reservation, trusted_root) \
					or not _publication_path_has_safe_ancestors(transaction_root, trusted_root):
				break
			claims = _publication_recovery_claims(output, trusted_root)
			if claims.has("error") or not (claims.get("paths", []) as Array).is_empty():
				break
			publish_error = DirAccess.rename_absolute(reservation, transaction_root)
			if publish_error == OK:
				break
	if publish_error != OK:
		_remove_tree_absolute(reservation, trusted_root)
		return {"error": "cannot publish fixture transaction owner: %s" \
				% error_string(publish_error)}
	claims = _publication_recovery_claims(output, trusted_root)
	if claims.has("error") or not (claims.get("paths", []) as Array).is_empty():
		_recover_fixture_publication(
				output, transaction_root, trusted_root)
		return {"error": "fixture publication recovery is busy"}
	var staging := transaction_root.path_join(PUBLICATION_STAGING_DIR)
	if not _publication_path_has_safe_ancestors(staging, trusted_root):
		_recover_fixture_publication(
				output, transaction_root, trusted_root)
		return {"error": "fixture publication staging has an unsafe ancestor"}
	return {"staging_path": staging}


static func commit_fixture_publication(
		staging_abs: String,
		output_abs: String,
		install_rename: Callable = Callable(),
		restore_rename: Callable = Callable(),
		installed_marker_write: Callable = Callable(),
		cleanup_transaction: Callable = Callable()) -> Error:
	var staging := _normalized_publication_path(staging_abs)
	var output := _normalized_publication_path(output_abs)
	var transaction_root := _publication_transaction_root(output)
	if staging.is_empty() or output.is_empty() \
			or not output.is_absolute_path() \
			or staging != transaction_root.path_join(PUBLICATION_STAGING_DIR):
		return ERR_INVALID_PARAMETER
	var owner := _read_publication_owner(transaction_root, output)
	if owner.is_empty() or int(owner.get("owner_pid", -1)) != OS.get_process_id():
		return ERR_UNAUTHORIZED
	var trusted_root := String(owner.get("trusted_root", ""))
	for path in [output, transaction_root, staging]:
		if not _publication_path_has_safe_ancestors(path, trusted_root):
			return ERR_INVALID_PARAMETER
	if not DirAccess.dir_exists_absolute(staging):
		return ERR_DOES_NOT_EXIST
	if FileAccess.file_exists(transaction_root.path_join(
			PUBLICATION_INSTALL_RECORD)) \
			or FileAccess.file_exists(transaction_root.path_join(
					PUBLICATION_INSTALLED_RECORD)):
		return ERR_ALREADY_IN_USE
	if FileAccess.file_exists(output):
		return ERR_ALREADY_EXISTS

	var backup := transaction_root.path_join(PUBLICATION_BACKUP_DIR)
	if not _publication_path_has_safe_ancestors(backup, trusted_root) \
			or DirAccess.dir_exists_absolute(backup) \
			or FileAccess.file_exists(backup):
		return ERR_ALREADY_EXISTS
	var had_output := DirAccess.dir_exists_absolute(output)
	var install_record_error := _write_new_atomic_json(
			transaction_root.path_join(PUBLICATION_INSTALL_RECORD), {
				"had_output": had_output,
			})
	if install_record_error != OK:
		return install_record_error
	if had_output:
		# Re-check the root immediately before the first rename. A directory link or
		# junction is never renamed into the owned journal and never traversed.
		if not _publication_path_has_safe_ancestors(output, trusted_root) \
				or not _publication_path_has_safe_ancestors(
						backup, trusted_root):
			return ERR_INVALID_PARAMETER
		var backup_error := DirAccess.rename_absolute(output, backup)
		if backup_error != OK:
			return backup_error

	if not _publication_path_has_safe_ancestors(staging, trusted_root) \
			or not _publication_path_has_safe_ancestors(output, trusted_root):
		return ERR_INVALID_PARAMETER
	var install_error := _rename_absolute_with(
			staging, output, install_rename)
	if install_error == OK and (not DirAccess.dir_exists_absolute(output) \
			or DirAccess.dir_exists_absolute(staging)):
		install_error = ERR_CANT_CREATE
	if install_error != OK:
		var rollback_error := _write_new_atomic_json(
				transaction_root.path_join(PUBLICATION_ROLLBACK_RECORD), {
					"had_output": had_output,
				})
		if rollback_error != OK:
			return rollback_error
		var restore_error := _restore_prior_fixture_publication(
				output, backup, had_output, restore_rename, trusted_root)
		if restore_error != OK:
			return restore_error
		var cleanup_error := _cleanup_publication_transaction_with(
				transaction_root, output, trusted_root, cleanup_transaction)
		return cleanup_error if cleanup_error != OK else install_error

	var installed_error := _write_new_atomic_json_with(
			transaction_root.path_join(PUBLICATION_INSTALLED_RECORD), {
				"complete": true,
			}, installed_marker_write)
	if installed_error != OK:
		var rollback_error := _write_new_atomic_json(
				transaction_root.path_join(PUBLICATION_ROLLBACK_RECORD), {
					"had_output": had_output,
				})
		if rollback_error != OK:
			return rollback_error
		var restore_error := _restore_prior_fixture_publication(
				output, backup, had_output, restore_rename, trusted_root)
		if restore_error != OK:
			return restore_error
		var cleanup_error := _cleanup_publication_transaction_with(
				transaction_root, output, trusted_root, cleanup_transaction)
		return cleanup_error if cleanup_error != OK else installed_error
	return _cleanup_publication_transaction_with(
			transaction_root, output, trusted_root, cleanup_transaction)


static func abort_fixture_publication(staging_abs: String) -> Error:
	var staging := _normalized_publication_path(staging_abs)
	if staging.is_empty() or staging.get_file() != PUBLICATION_STAGING_DIR:
		return ERR_INVALID_PARAMETER
	var transaction_root := staging.get_base_dir()
	if not transaction_root.ends_with(PUBLICATION_TRANSACTION_SUFFIX):
		return ERR_INVALID_PARAMETER
	var output := transaction_root.trim_suffix(PUBLICATION_TRANSACTION_SUFFIX)
	if not output.is_absolute_path():
		return ERR_INVALID_PARAMETER
	if not DirAccess.dir_exists_absolute(transaction_root):
		return OK
	var owner := _read_publication_owner(transaction_root, output)
	if owner.is_empty() or int(owner.get("owner_pid", -1)) != OS.get_process_id():
		return ERR_UNAUTHORIZED
	return _recover_fixture_publication(
			output, transaction_root, String(owner.trusted_root))


static func _publication_transaction_root(output_abs: String) -> String:
	return output_abs.simplify_path() + PUBLICATION_TRANSACTION_SUFFIX


static func _normalized_publication_path(path: String) -> String:
	return ProjectSettings.globalize_path(path).simplify_path() \
			.replace("\\", "/").trim_suffix("/")


static func path_is_strict_descendant(path: String, root: String) -> bool:
	var normalized_path := _normalized_publication_path(path)
	var normalized_root := _normalized_publication_path(root)
	if normalized_path.is_empty() or normalized_root.is_empty():
		return false
	if OS.get_name() == "Windows":
		normalized_path = normalized_path.to_lower()
		normalized_root = normalized_root.to_lower()
	return normalized_path.begins_with(normalized_root + "/")


static func _publication_path_has_safe_ancestors(
		path: String, trusted_root: String, allow_root: bool = false) -> bool:
	var normalized_path := _normalized_publication_path(path)
	var normalized_root := _normalized_publication_path(trusted_root)
	if normalized_path.is_empty() or normalized_root.is_empty() \
			or not normalized_path.is_absolute_path() \
			or not normalized_root.is_absolute_path():
		return false
	var same_as_root := RenderFixtureContract.same_absolute_path(normalized_path, normalized_root)
	if (not allow_root and same_as_root) \
			or (not same_as_root \
					and not path_is_strict_descendant(
							normalized_path, normalized_root)):
		return false
	if not DirAccess.dir_exists_absolute(normalized_root) \
			or FileAccess.file_exists(normalized_root) \
			or _path_entry_is_link(normalized_root):
		return false
	if same_as_root:
		return true

	var relative := normalized_path.substr(normalized_root.length()).trim_prefix("/")
	var components := relative.split("/", false)
	var current := normalized_root
	for index in range(components.size()):
		current = current.path_join(components[index])
		# A dangling symlink/junction often reports neither file_exists nor
		# dir_exists. Ask the parent directory about the entry unconditionally.
		if _path_entry_is_link(current):
			return false
		var is_file := FileAccess.file_exists(current)
		if is_file and index + 1 < components.size():
			return false
	return true


static func _path_entry_is_link(path: String) -> bool:
	if path.is_empty() or path.get_file().is_empty():
		return false
	var parent := DirAccess.open(path.get_base_dir())
	return parent != null and parent.is_link(path.get_file())


static func _write_new_atomic_json(path: String, value: Dictionary) -> Error:
	if FileAccess.file_exists(path) or DirAccess.dir_exists_absolute(path):
		return ERR_ALREADY_EXISTS
	var temp := "%s.tmp.%d.%d" % [
			path, OS.get_process_id(), Time.get_ticks_usec()]
	var write_error := RenderFixtureContract.write_bytes(
			temp, JSON.stringify(McpJson.sanitize(value), "\t").to_utf8_buffer())
	if write_error != OK:
		DirAccess.remove_absolute(temp)
		return write_error
	if FileAccess.file_exists(path) or DirAccess.dir_exists_absolute(path):
		DirAccess.remove_absolute(temp)
		return ERR_ALREADY_EXISTS
	var rename_error := DirAccess.rename_absolute(temp, path)
	if rename_error != OK:
		DirAccess.remove_absolute(temp)
	return rename_error


static func _write_new_atomic_json_with(
		path: String, value: Dictionary, operation: Callable) -> Error:
	if not operation.is_valid():
		return _write_new_atomic_json(path, value)
	var result: Variant = operation.call(path, value)
	return int(result) if result is int else ERR_INVALID_DATA


static func _read_publication_owner(
		transaction_root: String,
		output: String,
		expected_trusted_root: String = "") -> Dictionary:
	var path := transaction_root.path_join(PUBLICATION_OWNER_RECORD)
	if not FileAccess.file_exists(path):
		return {}
	var parsed: Variant = JSON.parse_string(FileAccess.get_file_as_string(path))
	if not (parsed is Dictionary):
		return {}
	var owner := parsed as Dictionary
	var trusted_root := String(owner.get("trusted_root", ""))
	if String(owner.get("schema", "")) != PUBLICATION_TRANSACTION_SCHEMA \
			or not RenderFixtureContract.same_absolute_path(String(owner.get("output", "")), output) \
			or trusted_root.is_empty() \
			or not trusted_root.is_absolute_path() \
			or not path_is_strict_descendant(output, trusted_root) \
			or (not expected_trusted_root.is_empty() \
					and not RenderFixtureContract.same_absolute_path(
							trusted_root, expected_trusted_root)) \
			or int(owner.get("owner_pid", -1)) <= 0:
		return {}
	return owner


static func _publication_recovery_claims(
		output: String, trusted_root: String) -> Dictionary:
	var transaction_root := _publication_transaction_root(output)
	var parent := transaction_root.get_base_dir()
	if not _publication_path_has_safe_ancestors(
			parent, trusted_root, true):
		return {"error": ERR_INVALID_PARAMETER}
	var dir := DirAccess.open(parent)
	if dir == null:
		return {"error": ERR_CANT_OPEN}
	var prefix := transaction_root.get_file() + PUBLICATION_RECOVERING_SEGMENT
	var comparison_prefix := prefix.to_lower() if OS.get_name() == "Windows" \
			else prefix
	for file_name in dir.get_files():
		var comparison_name := file_name.to_lower() \
				if OS.get_name() == "Windows" else file_name
		if comparison_name.begins_with(comparison_prefix):
			return {"error": ERR_FILE_CORRUPT}
	var paths: Array = []
	for directory_name in dir.get_directories():
		var comparison_name := directory_name.to_lower() \
				if OS.get_name() == "Windows" else directory_name
		if not comparison_name.begins_with(comparison_prefix):
			continue
		var claim := parent.path_join(directory_name)
		if not _publication_path_has_safe_ancestors(claim, trusted_root):
			return {"error": ERR_INVALID_PARAMETER}
		paths.append(claim)
	paths.sort()
	return {"paths": paths}


static func _recovery_claim_pid(claim: String, output: String) -> int:
	var prefix := _publication_transaction_root(output) \
			+ PUBLICATION_RECOVERING_SEGMENT
	var claim_key := claim.to_lower() if OS.get_name() == "Windows" else claim
	var prefix_key := prefix.to_lower() if OS.get_name() == "Windows" else prefix
	if not claim_key.begins_with(prefix_key):
		return -1
	var suffix := claim.substr(prefix.length())
	var pid_token := suffix.get_slice(".", 0)
	return pid_token.to_int() if pid_token.is_valid_int() \
			and pid_token.to_int() > 0 else -1


static func _publication_owner_is_running(
		pid: int, owner_is_running: Callable) -> bool:
	return bool(owner_is_running.call(pid)) if owner_is_running.is_valid() \
			else OS.is_process_running(pid)


static func _publication_directory_is_empty(
		path: String, trusted_root: String) -> bool:
	if not _publication_path_has_safe_ancestors(path, trusted_root):
		return false
	var dir := DirAccess.open(path)
	return dir != null and dir.get_directories().is_empty() \
			and dir.get_files().is_empty()


static func _recover_abandoned_publication(
		output: String,
		trusted_root: String,
		owner_is_running: Callable,
		after_recovery_claim: Callable) -> Error:
	var claims := _publication_recovery_claims(output, trusted_root)
	if claims.has("error"):
		return int(claims.error)
	var claim_paths := claims.paths as Array
	if claim_paths.size() > 1:
		return ERR_FILE_CORRUPT
	if claim_paths.size() == 1:
		var existing_claim := String(claim_paths[0])
		var claimant_pid := _recovery_claim_pid(existing_claim, output)
		if claimant_pid <= 0:
			return ERR_FILE_CORRUPT
		if _publication_directory_is_empty(existing_claim, trusted_root):
			var remove_error := DirAccess.remove_absolute(existing_claim)
			return OK if remove_error in [OK, ERR_DOES_NOT_EXIST] \
					else remove_error
		if _publication_owner_is_running(claimant_pid, owner_is_running):
			return ERR_BUSY
		return _claim_and_recover_publication(
				existing_claim, output, trusted_root, claimant_pid,
				owner_is_running, after_recovery_claim)

	var transaction_root := _publication_transaction_root(output)
	if FileAccess.file_exists(transaction_root):
		return ERR_INVALID_DATA
	if not DirAccess.dir_exists_absolute(transaction_root):
		return OK
	if not _publication_path_has_safe_ancestors(
			transaction_root, trusted_root):
		return ERR_INVALID_PARAMETER
	if _publication_directory_is_empty(transaction_root, trusted_root):
		var remove_error := DirAccess.remove_absolute(transaction_root)
		return OK if remove_error in [OK, ERR_DOES_NOT_EXIST] else remove_error
	var owner := _read_publication_owner(
			transaction_root, output, trusted_root)
	if owner.is_empty():
		return ERR_FILE_CORRUPT
	if _publication_owner_is_running(int(owner.owner_pid), owner_is_running):
		return ERR_BUSY
	return _claim_and_recover_publication(
			transaction_root, output, trusted_root, -1,
			owner_is_running, after_recovery_claim)


static func _claim_and_recover_publication(
		source_root: String,
		output: String,
		trusted_root: String,
		source_claimant_pid: int,
		owner_is_running: Callable,
		after_recovery_claim: Callable) -> Error:
	var original_owner := _read_publication_owner(
			source_root, output, trusted_root)
	if original_owner.is_empty():
		return ERR_FILE_CORRUPT
	if source_claimant_pid > 0 and _publication_owner_is_running(
			source_claimant_pid, owner_is_running):
		return ERR_BUSY
	if _publication_owner_is_running(
			int(original_owner.owner_pid), owner_is_running):
		return ERR_BUSY

	var claimed_root := "%s%s%d.%d" % [
		_publication_transaction_root(output), PUBLICATION_RECOVERING_SEGMENT,
		OS.get_process_id(), Time.get_ticks_usec(),
	]
	if not _publication_path_has_safe_ancestors(claimed_root, trusted_root) \
			or DirAccess.dir_exists_absolute(claimed_root) \
			or FileAccess.file_exists(claimed_root):
		return ERR_ALREADY_EXISTS
	var claim_error := DirAccess.rename_absolute(source_root, claimed_root)
	if claim_error != OK:
		return ERR_BUSY
	if not _publication_path_has_safe_ancestors(claimed_root, trusted_root):
		return ERR_INVALID_PARAMETER
	var claimed_owner := _read_publication_owner(
			claimed_root, output, trusted_root)
	if claimed_owner.is_empty() \
			or int(claimed_owner.owner_pid) != int(original_owner.owner_pid):
		return ERR_FILE_CORRUPT

	# Liveness is deliberately checked again after the atomic rename. A process
	# that became live between preflight and claim retains ownership untouched.
	var prior_owner_became_live := _publication_owner_is_running(
			int(claimed_owner.owner_pid), owner_is_running)
	var prior_claimant_became_live := source_claimant_pid > 0 \
			and _publication_owner_is_running(
					source_claimant_pid, owner_is_running)
	if prior_owner_became_live or prior_claimant_became_live:
		var release_error := DirAccess.rename_absolute(claimed_root, source_root)
		return ERR_BUSY if release_error == OK else release_error
	if after_recovery_claim.is_valid():
		after_recovery_claim.call(claimed_root)
	return _recover_fixture_publication(
			output, claimed_root, trusted_root)


static func _recover_fixture_publication(
		output: String,
		transaction_root: String,
		trusted_root: String) -> Error:
	if not DirAccess.dir_exists_absolute(transaction_root):
		return OK
	for path in [output, transaction_root]:
		if not _publication_path_has_safe_ancestors(path, trusted_root):
			return ERR_INVALID_PARAMETER
	var owner := _read_publication_owner(
			transaction_root, output, trusted_root)
	if owner.is_empty():
		return ERR_FILE_CORRUPT

	var staging := transaction_root.path_join(PUBLICATION_STAGING_DIR)
	var backup := transaction_root.path_join(PUBLICATION_BACKUP_DIR)
	for path in [staging, backup]:
		if not _publication_path_has_safe_ancestors(path, trusted_root):
			return ERR_INVALID_PARAMETER
	var installing_path := transaction_root.path_join(PUBLICATION_INSTALL_RECORD)
	var installed := FileAccess.file_exists(transaction_root.path_join(
			PUBLICATION_INSTALLED_RECORD))
	var rollback_path := transaction_root.path_join(PUBLICATION_ROLLBACK_RECORD)
	var rolling_back := FileAccess.file_exists(rollback_path)
	if installed and rolling_back:
		return ERR_FILE_CORRUPT
	if not FileAccess.file_exists(installing_path):
		return _cleanup_publication_transaction(
				transaction_root, output, trusted_root)
	var install_value: Variant = JSON.parse_string(
			FileAccess.get_file_as_string(installing_path))
	if not (install_value is Dictionary) \
			or not (install_value as Dictionary).has("had_output"):
		return ERR_FILE_CORRUPT
	var had_output := bool((install_value as Dictionary).had_output)
	if rolling_back:
		var rollback_value: Variant = JSON.parse_string(
				FileAccess.get_file_as_string(rollback_path))
		if not (rollback_value is Dictionary) \
				or not (rollback_value as Dictionary).has("had_output") \
				or bool((rollback_value as Dictionary).had_output) != had_output:
			return ERR_FILE_CORRUPT
	var output_exists := DirAccess.dir_exists_absolute(output)
	var backup_exists := DirAccess.dir_exists_absolute(backup)
	var staging_exists := DirAccess.dir_exists_absolute(staging)
	if FileAccess.file_exists(output) or FileAccess.file_exists(backup):
		return ERR_INVALID_DATA

	if installed:
		if not output_exists:
			if not backup_exists:
				return ERR_DOES_NOT_EXIST
			if not _publication_path_has_safe_ancestors(
					backup, trusted_root):
				return ERR_INVALID_PARAMETER
			var restore_error := DirAccess.rename_absolute(backup, output)
			if restore_error != OK:
				return restore_error
		return _cleanup_publication_transaction(
				transaction_root, output, trusted_root)

	# No installed marker means the transaction never crossed its durable commit
	# point. Restore the prior complete fixture before this output can be reused.
	if backup_exists:
		if output_exists:
			var remove_error := _remove_tree_absolute(output, trusted_root)
			if remove_error != OK:
				return remove_error
		if not _publication_path_has_safe_ancestors(
				backup, trusted_root):
			return ERR_INVALID_PARAMETER
		var restore_error := DirAccess.rename_absolute(backup, output)
		if restore_error != OK:
			return restore_error
	elif had_output:
		# With staging still present, the backup rename never happened and output
		# remains the old complete fixture. Without either staging or backup, the
		# prior fixture cannot be proven recoverable, so fail closed.
		if not output_exists or (not staging_exists and not rolling_back):
			return ERR_FILE_CORRUPT
	elif output_exists:
		if staging_exists and not rolling_back:
			return ERR_BUSY
		var remove_error := _remove_tree_absolute(output, trusted_root)
		if remove_error != OK:
			return remove_error
	return _cleanup_publication_transaction(
			transaction_root, output, trusted_root)


static func _rename_absolute_with(
		source: String, target: String, operation: Callable) -> Error:
	if not operation.is_valid():
		return DirAccess.rename_absolute(source, target)
	var result: Variant = operation.call(source, target)
	return int(result) if result is int else ERR_INVALID_DATA


static func _restore_prior_fixture_publication(
		output: String,
		backup: String,
		had_output: bool,
		restore_rename: Callable,
		trusted_root: String) -> Error:
	for path in [output, backup]:
		if not _publication_path_has_safe_ancestors(path, trusted_root):
			return ERR_INVALID_PARAMETER
	if FileAccess.file_exists(output) or FileAccess.file_exists(backup):
		return ERR_INVALID_DATA
	if DirAccess.dir_exists_absolute(output):
		var remove_error := _remove_tree_absolute(output, trusted_root)
		if remove_error != OK:
			return remove_error
	if not had_output:
		return OK
	if not DirAccess.dir_exists_absolute(backup):
		return ERR_DOES_NOT_EXIST
	if not _publication_path_has_safe_ancestors(backup, trusted_root):
		return ERR_INVALID_PARAMETER
	var restore_error := _rename_absolute_with(
			backup, output, restore_rename)
	if restore_error != OK:
		return restore_error
	if not DirAccess.dir_exists_absolute(output) \
			or DirAccess.dir_exists_absolute(backup):
		return ERR_CANT_CREATE
	return OK


static func _cleanup_publication_transaction_with(
		transaction_root: String,
		output: String,
		trusted_root: String,
		operation: Callable) -> Error:
	if not operation.is_valid():
		return _cleanup_publication_transaction(
				transaction_root, output, trusted_root)
	var result: Variant = operation.call(
			transaction_root, output, trusted_root)
	return int(result) if result is int else ERR_INVALID_DATA


static func _cleanup_publication_transaction(
		transaction_root: String,
		output: String,
		trusted_root: String) -> Error:
	var canonical_root := _publication_transaction_root(output)
	var is_canonical := RenderFixtureContract.same_absolute_path(transaction_root, canonical_root)
	var is_recovery_claim := transaction_root.begins_with(
			canonical_root + PUBLICATION_RECOVERING_SEGMENT)
	if transaction_root.is_empty() \
			or not transaction_root.is_absolute_path() \
			or (not is_canonical and not is_recovery_claim) \
			or not _publication_path_has_safe_ancestors(
					transaction_root, trusted_root):
		return ERR_INVALID_PARAMETER
	var dir := DirAccess.open(transaction_root)
	if dir == null:
		return OK if not DirAccess.dir_exists_absolute(transaction_root) \
				else ERR_CANT_OPEN
	var directories := dir.get_directories()
	for directory_name in directories:
		if directory_name not in [
			PUBLICATION_STAGING_DIR,
			PUBLICATION_BACKUP_DIR,
		]:
			return ERR_FILE_CORRUPT
	var files := dir.get_files()
	for file_name in files:
		var recognized := file_name == PUBLICATION_OWNER_RECORD \
				or file_name == PUBLICATION_INSTALL_RECORD \
				or file_name == PUBLICATION_INSTALLED_RECORD \
				or file_name == PUBLICATION_ROLLBACK_RECORD \
				or file_name.begins_with(PUBLICATION_OWNER_RECORD + ".tmp.") \
				or file_name.begins_with(PUBLICATION_INSTALL_RECORD + ".tmp.") \
				or file_name.begins_with(PUBLICATION_INSTALLED_RECORD + ".tmp.") \
				or file_name.begins_with(PUBLICATION_ROLLBACK_RECORD + ".tmp.")
		if not recognized \
				or _path_entry_is_link(transaction_root.path_join(file_name)):
			return ERR_FILE_CORRUPT
	var committed := FileAccess.file_exists(transaction_root.path_join(
			PUBLICATION_INSTALLED_RECORD))

	# A rollback caller has already restored the prior fixture. Release its
	# installing marker before deleting staging so a crash cannot leave a state
	# that resembles a lost pre-commit output. A committed install keeps both
	# markers until its old/staged payloads are gone.
	if not committed:
		for file_name in files:
			if file_name == PUBLICATION_INSTALL_RECORD \
					or file_name.begins_with(
							PUBLICATION_INSTALL_RECORD + ".tmp."):
				var record_error := DirAccess.remove_absolute(
						transaction_root.path_join(file_name))
				if record_error != OK:
					return record_error

	# For a committed install, remove payload directories before either durable
	# marker. If cleanup is interrupted, owner.json and installed.json still
	# describe a state that the next begin_fixture_publication() can finish.
	for directory_name in [PUBLICATION_STAGING_DIR, PUBLICATION_BACKUP_DIR]:
		var child := transaction_root.path_join(directory_name)
		if _path_entry_is_link(child) or FileAccess.file_exists(child):
			return ERR_INVALID_DATA
		if DirAccess.dir_exists_absolute(child):
			var child_error := _remove_tree_absolute(child, trusted_root)
			if child_error != OK:
				return child_error

	dir = DirAccess.open(transaction_root)
	if dir == null:
		return ERR_CANT_OPEN
	if not dir.get_directories().is_empty():
		return ERR_FILE_CORRUPT

	# The owner record is the lock and is deliberately the final file removed.
	# The installed marker outlives the installing marker, so every intermediate
	# cleanup state either proves the new output committed or has no install
	# record and can simply finish cleanup while retaining that output.
	var remaining_records := [
		PUBLICATION_ROLLBACK_RECORD,
		PUBLICATION_INSTALLED_RECORD,
	]
	if committed:
		remaining_records.push_front(PUBLICATION_INSTALL_RECORD)
	for record_name in remaining_records:
		for file_name in files:
			if file_name == record_name \
					or file_name.begins_with(record_name + ".tmp."):
				var record_path := transaction_root.path_join(file_name)
				if not FileAccess.file_exists(record_path):
					continue
				var record_error := DirAccess.remove_absolute(
						record_path)
				if record_error != OK:
					return record_error
	for file_name in files:
		if file_name.begins_with(PUBLICATION_OWNER_RECORD + ".tmp."):
			var temp_error := DirAccess.remove_absolute(
					transaction_root.path_join(file_name))
			if temp_error != OK:
				return temp_error
	var owner_path := transaction_root.path_join(PUBLICATION_OWNER_RECORD)
	if FileAccess.file_exists(owner_path):
		var owner_error := DirAccess.remove_absolute(owner_path)
		if owner_error != OK:
			return owner_error

	dir = DirAccess.open(transaction_root)
	if dir == null:
		return ERR_CANT_OPEN
	if not dir.get_directories().is_empty() or not dir.get_files().is_empty():
		return ERR_FILE_CORRUPT
	return DirAccess.remove_absolute(transaction_root)


static func _remove_tree_absolute(
		path: String, trusted_root: String = "") -> Error:
	if path.is_empty() or not path.is_absolute_path():
		return ERR_INVALID_PARAMETER
	if (not trusted_root.is_empty() \
			and not _publication_path_has_safe_ancestors(path, trusted_root)) \
			or _path_entry_is_link(path):
		return ERR_INVALID_PARAMETER
	if FileAccess.file_exists(path):
		return DirAccess.remove_absolute(path)
	var dir := DirAccess.open(path)
	if dir == null:
		return OK if not DirAccess.dir_exists_absolute(path) else ERR_CANT_OPEN
	for name in dir.get_files():
		var file_error := DirAccess.remove_absolute(path.path_join(name))
		if file_error != OK:
			return file_error
	for name in dir.get_directories():
		var child := path.path_join(name)
		var child_error := DirAccess.remove_absolute(child) \
				if dir.is_link(name) else _remove_tree_absolute(child, trusted_root)
		if child_error != OK:
			return child_error
	return DirAccess.remove_absolute(path)
