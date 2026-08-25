class_name LaunchFlags
extends RefCounted

## Parses the original-engine launch flags the runtime honors. These mirror
## Jointops.exe's command line, so a user launches the runtime the same way:
##
##   /d            dev mode: loose files next to the PFFs override the packed
##                 entries. Without it the runtime reads from PFFs exclusively.
##   /exp <name>   mount the expansion <name> (e.g. "jox01") over the base game.
##   /game <code>  which game the data is from (e.g. "jo", "jodemo"); selects the
##                 SCR decode key. Defaults to "jo" when absent.
##   --resource-dir <absolute path>
##                 use this resource directory for this process without changing
##                 the runtime's persisted preference.
##   --loose-mission <name.bms>
##                 boot the exact top-level loose BMS from --resource-dir.
##   --loose-root  ONED/dev runs: when the directory holds none of the
##                 packed game archives, mount it as loose files instead of
##                 failing the boot. Ordinary standalone
##                 launches omit it, keeping retail's no-archives fatal error
##                 (ADR 0025).
##   --oned-run-id / --oned-run-descriptor
##                 opaque identity consumed by the optional runtime control
##                 service. Ordinary launches omit both;
##                 GameMcpService owns their parsing and validation.
##
## Flags are scanned from both the engine args and the user args (anything after
## a `--` separator), case-insensitively, so either launch style works.


## All command-line tokens the game was launched with (engine + user args).
static func _all_args() -> PackedStringArray:
	var args := OS.get_cmdline_args()
	args.append_array(OS.get_cmdline_user_args())
	return args


## Return the token following `flag`, or "" when absent/empty. Godot commands put
## custom options behind Godot's `--` separator, but _all_args deliberately
## scans both arrays so packaged and source launches share one parser.
static func _value_after(flag: String) -> String:
	var wanted := flag.to_lower()
	var args := _all_args()
	for i in args.size():
		if args[i].to_lower() == wanted and i + 1 < args.size():
			return args[i + 1].strip_edges()
	return ""


## True when `/d` (dev / loose-override) was passed.
static func loose_override_enabled() -> bool:
	for arg in _all_args():
		if arg.to_lower() == "/d":
			return true
	return false


## The expansion requested via `/exp <name>`, or "" when none was given. Falls
## back to the persisted runtime setting only when no flag is present, so a launch
## flag always wins over stale config.
static func expansion(fallback: String = "") -> String:
	var value := _value_after("/exp")
	if not value.is_empty():
		return value
	return fallback


## The game code requested via `/game <code>` (e.g. "jodemo"), lowercased. Falls back
## to the persisted setting, then to "jo", so a launch flag always wins over stale
## config and the absence of any choice is the JO default. An unknown code resolves
## to the JO default downstream (gameprofile_scr_policy_for_code).
static func game(fallback: String = "jo") -> String:
	var value := _value_after("/game")
	if not value.is_empty():
		return value.to_lower()
	var fb := fallback.strip_edges().to_lower()
	return fb if not fb.is_empty() else "jo"


## The exact process-local resource directory supplied on the command line. This is a
## process-local override: callers must not persist it.
static func resource_dir(fallback: String = "") -> String:
	var value := _value_after("--resource-dir")
	return value if not value.is_empty() else fallback.strip_edges()


# The boot-table archive names our runtime mount probes (the fixed set retail's boot
# table carries); any one present makes a directory a mountable game dir.
const BUNDLED_BOOT_ARCHIVES := ["localres.pff", "resource.pff", "language.pff"]

## Tests substitute the directory probed for a bundled game; the real runtime probes
## its own exe's directory.
static var bundled_probe_override: String = ""


## The boot resource dir, in priority order: the --resource-dir flag,
## the persisted pick, then the game bundled around a shipped exe — the exe's own
## directory when it carries a boot archive (the tagged release zip, retail-style), else
## the loose assets/ beside it (the dev zip, where the game plays the same tree the
## ONED exposes). The bundled defaults are per-boot and never persisted (an explicit
## pick still writes the settings key through the picker's own path), and dev runs from
## the Godot editor are unchanged: its binary's dir carries neither. "" means ask.
static func boot_resource_dir(persisted: String) -> String:
	var dir := resource_dir(persisted)
	if dir.is_empty():
		dir = bundled_game_dir(_bundled_probe_dir())
	if dir.is_empty():
		dir = bundled_assets_dir(_bundled_probe_dir())
	return dir


## Whether `dir` may fall back to a loose mount when it holds no packed
## archives: the explicit --loose-root flag (ADR 0025), or the bundled loose
## assets/ default itself — the dev zip ships sources only, and blessing exactly that
## directory keeps a picked or persisted loose dir on retail's no-archives fatal.
static func boot_loose_allowed(dir: String) -> bool:
	if loose_root_allowed():
		return true
	return not dir.is_empty() and dir == bundled_assets_dir(_bundled_probe_dir())


## `exe_dir` when it holds any boot-table archive — a shipped game dir — else "".
static func bundled_game_dir(exe_dir: String) -> String:
	if exe_dir.is_empty():
		return ""
	for archive_name in BUNDLED_BOOT_ARCHIVES:
		if FileAccess.file_exists(exe_dir.path_join(archive_name)):
			return exe_dir
	return ""


## The loose game sources bundled beside a shipped exe: `<exe_dir>/assets` when it
## exists, else "".
static func bundled_assets_dir(exe_dir: String) -> String:
	if exe_dir.is_empty():
		return ""
	var dir := exe_dir.path_join("assets")
	return dir if DirAccess.dir_exists_absolute(dir) else ""


static func _bundled_probe_dir() -> String:
	if not bundled_probe_override.is_empty():
		return bundled_probe_override
	return OS.get_executable_path().get_base_dir()


## A top-level loose BMS to boot directly, or "" for the normal menu flow.
static func loose_mission() -> String:
	return _value_after("--loose-mission")


## True when `--loose-root` was passed: this run may play a directory with no
## packed archives through the loose mount instead of retail's fatal no-archives error.
static func loose_root_allowed() -> bool:
	for arg in _all_args():
		if arg.to_lower() == "--loose-root":
			return true
	return false
