class_name NovaLaunchFlags
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
##                 the persisted editor/game preference.
##   --loose-mission <name.bms>
##                 boot the exact top-level loose BMS from --resource-dir.
##   --loose-root  editor-managed runs: when the directory holds none of the
##                 packed game archives, mount it as loose files (the editor's
##                 own mount) instead of failing the boot. Ordinary standalone
##                 launches omit it, keeping retail's no-archives fatal error
##                 (ADR 0025).
##   --oned-run-id / --oned-run-descriptor
##                 opaque editor-run identity consumed by the optional runtime
##                 control service. Ordinary standalone launches omit both;
##                 GameMcpService owns their parsing and validation.
##
## Flags are scanned from both the engine args and the user args (anything after
## a `--` separator), case-insensitively, so either launch style works.


## All command-line tokens the game was launched with (engine + user args).
static func _all_args() -> PackedStringArray:
	var args := OS.get_cmdline_args()
	args.append_array(OS.get_cmdline_user_args())
	return args


## Return the token following `flag`, or "" when absent/empty. The editor puts
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
## back to the persisted editor setting only when no flag is present, so a launch
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


## The exact resource directory supplied by an editor-managed run. This is a
## process-local override: callers must not persist it.
static func resource_dir(fallback: String = "") -> String:
	var value := _value_after("--resource-dir")
	return value if not value.is_empty() else fallback.strip_edges()


## A top-level loose BMS to boot directly, or "" for the normal menu flow.
static func loose_mission() -> String:
	return _value_after("--loose-mission")


## True when `--loose-root` was passed: this editor-managed run may play a
## directory with no packed archives (a loose authoring root) through the
## editor's loose mount instead of retail's fatal no-archives error.
static func loose_root_allowed() -> bool:
	for arg in _all_args():
		if arg.to_lower() == "--loose-root":
			return true
	return false
