class_name NovaLaunchFlags
extends RefCounted

## Parses the original-engine launch flags the runtime honors. These mirror
## Jointops.exe's command line, so a user launches the runtime the same way:
##
##   /d            dev mode: loose files next to the PFFs override the packed
##                 entries. Without it the runtime reads from PFFs exclusively.
##   /exp <name>   mount the expansion <name> (e.g. "jox01") over the base game.
##
## Flags are scanned from both the engine args and the user args (anything after
## a `--` separator), case-insensitively, so either launch style works.


## All command-line tokens the game was launched with (engine + user args).
static func _all_args() -> PackedStringArray:
	var args := OS.get_cmdline_args()
	args.append_array(OS.get_cmdline_user_args())
	return args


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
	var args := _all_args()
	for i in args.size():
		if args[i].to_lower() == "/exp" and i + 1 < args.size():
			return args[i + 1].strip_edges()
	return fallback
