class_name BootRootMount
extends RefCounted
## Mount the CLI-supplied or picked game directory (--loose-root permits a
## loose-only root), or OpenNova's own bundled assets/ (ADR 0048).

## Where godot/web/opennova_stage.js copies the site's assets/ (keep in step).
const WEB_BUNDLED_ASSETS_DIR := "/opennova/assets"

static func mount(dir: String, allow_loose_root: bool) -> ResourceRoot:
	var root := ResourceRoot.new()
	var expansion := LaunchFlags.expansion()
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


## The game code the bundled game mounts under: its project's target game
## (assets/project.opennova's `target_game`).
const BUNDLED_GAME := "jo"
## Where the OpenNova Editor's Export of a project puts the game as it ships,
## relative to the project (assets/project.opennova's `export.output`): the
## editor's Export, `opennova-project export assets`.
const PROJECT_EXPORT_DIR := "build/export"
## The archive every export of a standalone game holds (its boot table's).
const EXPORT_ARCHIVE := "resource.pff"


## The bundled game's folder (ADR 0048 d8). An exported exe's `assets/` beside
## it, which the game zip fills with the editor's export of the base game
## (scripts/package_godot_windows.ps1). A source run's, the same export of the
## base game project (project_game_dir). The web build has no exe beside
## anything: its page stages the site's assets/ into the in-memory filesystem
## before the engine starts (ADR 0049).
static func bundled_assets_dir() -> String:
	if OS.has_feature("web"):
		return WEB_BUNDLED_ASSETS_DIR
	if OS.has_feature("template"):
		return OS.get_executable_path().get_base_dir().path_join("assets")
	return project_game_dir(bundled_project_dir())


## The base game's editor project, a source run's repo `assets/` beside the
## Godot project (ADR 0048 d7). Its files, not the game the Build makes of them.
static func bundled_project_dir() -> String:
	return ProjectSettings.globalize_path("res://").path_join("../assets").simplify_path()


## The game an editor project ships: its export folder where an export of it
## is there, else, never exported, the project folder itself, which boots its
## menu but holds none of the files its imports make (a mission's terrain among
## them).
static func project_game_dir(project_dir: String) -> String:
	var exported := project_dir.path_join(PROJECT_EXPORT_DIR)
	return exported if FileAccess.file_exists(exported.path_join(EXPORT_ARCHIVE)) else project_dir


## Mount the bundled game: the editor's Build of the base game (its boot-table
## archives and loose files) mounted as an install is, else, with no archive,
## the folder as a plain loose root (a source run's unexported project, the web
## build's staged files). It is OpenNova's own data, which the Build's gate
## vouched for, so the retail boot manifest is not reported against it. The
## expansion it mounts over the base is chosen as for any game folder (mount):
## `/exp` (or `/mod`) on the command line, so an expansion shipped beside the
## base game in `expansion/<name>/` plays as it does in the original game run in
## that folder; one the folder lacks falls back to the base game, as the
## engine's mount does (ADR 0048 d8). The Mods list's pick lasts its run
## (D-MNU-31).
static func mount_bundled(dir: String) -> ResourceRoot:
	var root := ResourceRoot.new()
	var expansion := LaunchFlags.expansion()
	if root.mount_runtime(dir, expansion, false, BUNDLED_GAME) == OK:
		return root
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
		var text := "BootRootMount: %s%s — retail: %s" \
				% [ResourceRoot.boot_resource_missing_marker(), name, root.boot_resource_failure_text(name)]
		if root.is_runtime_mount():
			push_error(text)
		else:
			push_warning(text)
