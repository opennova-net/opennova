class_name BootRootMount
extends RefCounted
## Mount the CLI-supplied or picked game directory (--loose-root permits a
## loose-only root), or OpenNova's own bundled assets/ (ADR 0048).

static func mount(dir: String, allow_loose_root: bool) -> ResourceRoot:
	var root := ResourceRoot.new()
	var expansion := LaunchFlags.expansion(ResourceDirSettings.get_expansion())
	var game := LaunchFlags.game(ResourceDirSettings.get_game())
	var err: int = root.mount_runtime(
			dir, expansion, LaunchFlags.loose_override_enabled(), game)
	if err != OK:
		# ERR_FILE_NOT_FOUND is specifically the zero-archives fatal; other
		# errors (missing dir, unreadable root) fail the loose mount too.
		if err == ERR_FILE_NOT_FOUND and allow_loose_root \
				and root.set_root_dir(dir) == OK:
			report_missing_boot_resources(root)
			return root
		push_warning("BootRootMount: %s" % root.get_last_error())
		return null
	report_missing_boot_resources(root)
	return root


## The assets/ directory shipped beside the exported exe; from the editor or a
## source run, the repo's own assets/ beside the Godot project.
static func bundled_assets_dir() -> String:
	if OS.has_feature("template"):
		return OS.get_executable_path().get_base_dir().path_join("assets")
	return ProjectSettings.globalize_path("res://").path_join("../assets").simplify_path()


## Mount the bundled assets/ as a plain loose root. It is OpenNova's own data,
## not a retail install, so the retail boot manifest is not reported against it.
static func mount_bundled(dir: String) -> ResourceRoot:
	var root := ResourceRoot.new()
	if root.set_root_dir(dir) != OK:
		push_warning("BootRootMount: bundled assets not found at %s" % dir)
		return null
	return root


# Honest missing-resource errors over the witnessed boot manifest (ENG-6,
# docs/required-resources.md): name each missing fatal-set file with retail's
# witnessed failure behavior instead of dead-ending silently later. Reported,
# not enforced — the shell keeps running so a partial dir stays inspectable
# where retail shows a MessageBox and exits. On the
# sanctioned loose-root play-test mount an authoring extract is expectedly
# partial, so the same report warns instead of erroring.
static func report_missing_boot_resources(root: ResourceRoot) -> void:
	for name in root.list_missing_boot_resources():
		var text := "BootRootMount: boot-required resource missing: %s — retail: %s" \
				% [name, root.boot_resource_failure_text(name)]
		if root.is_runtime_mount():
			push_error(text)
		else:
			push_warning(text)
