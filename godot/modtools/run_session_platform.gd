class_name RunSessionPlatform
extends RefCounted

## The OS, process and staging platform GameRunSession drives. This class IS the
## one production implementation (Godot/Process plus GamePacker); every verb is
## public so the deterministic session tests hand the session a fake that
## overrides them (ADR 0043 rule 11: fakes behind a narrow interface, never a
## private override).


func valid_resource_dir(path: String) -> bool:
	return ResourceDirSettings.is_valid_root(path)


func file_exists(path: String) -> bool:
	return FileAccess.file_exists(path)


func supports_working_directory() -> bool:
	return Process.supports_working_directory()


func now_msec() -> int:
	return Time.get_ticks_msec()


func retail_install_error(retail_dir: String) -> String:
	return GamePacker.retail_install_error(retail_dir)


func stage_retail(resource_dir: String, retail_dir: String) -> Dictionary:
	return GamePacker.stage_retail(resource_dir, retail_dir)


func spawn_process(path: String, args: PackedStringArray, cwd: String) -> int:
	if Process.supports_working_directory():
		return Process.spawn_in_dir(path, args, cwd)
	return OS.create_process(path, args)


func process_is_alive(pid: int) -> bool:
	return Process.is_running(pid) if Process.supports_working_directory() \
			else OS.is_process_running(pid)


func kill_process(pid: int) -> bool:
	return Process.kill_pid(pid) if Process.supports_working_directory() \
			else OS.kill(pid) == OK


func wait_for_exit(pid: int, timeout_msec: int) -> bool:
	if Process.supports_working_directory():
		return Process.wait_for_exit(pid, timeout_msec)
	# The portable fallback polls to the same deadline.
	var deadline := Time.get_ticks_msec() + timeout_msec
	while OS.is_process_running(pid):
		if Time.get_ticks_msec() >= deadline:
			return false
		OS.delay_msec(GameRunSession.EXIT_POLL_MSEC)
	return true


func release_process(pid: int) -> void:
	if Process.supports_working_directory():
		Process.release(pid)


func oned_executable_path() -> String:
	return OS.get_executable_path()


func project_dir() -> String:
	return ProjectSettings.globalize_path("res://")


func is_dev_mode() -> bool:
	return OS.has_feature("editor")
