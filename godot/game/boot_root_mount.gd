class_name BootRootMount
extends RefCounted
## The boot-root mount policy, split out of the shell: mount a runtime install
## (or, under the --loose-root play-test contract, a loose authoring dir) and
## report the witnessed boot manifest honestly. Stateless — both entry points
## are static and read only the launch/settings singletons.


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


# Honest missing-resource errors over the witnessed boot manifest (ENG-6,
# docs/required-resources.md): name each missing fatal-set file with retail's
# witnessed failure behavior instead of dead-ending silently later. Reported,
# not enforced — the shell keeps running so a partial dir stays inspectable
# (the picker flow), where retail shows a MessageBox and exits. On the
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
