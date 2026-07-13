class_name NovaLaunchFlags
extends RefCounted

## Parses the two public launch options supported by the game:
##
##   /d            retail dev mode: loose files beside the PFFs override packed
##                 entries. PFFs remain required.
##   /exp <name>   select an expansion inside the active resource root.
##
## ONED Play also passes a private --oned-resource-root <path> handoff after
## Godot's argument separator. It is accepted only together with /d and selects
## the editor's loose-only session mode. It is not a public game option.


class Result:
	extends RefCounted

	var loose_override := false
	var expansion := ""
	var has_expansion_override := false
	var oned_resource_root := ""


## All custom tokens presented to the running game. Godot separates arguments
## before and after its separator; parsing the combined list keeps source and
## exported launches equivalent.
static func _all_args() -> PackedStringArray:
	var args := OS.get_cmdline_args()
	args.append_array(OS.get_cmdline_user_args())
	return args


## Parse an explicit token list. Keeping this pure is what makes malformed and
## mixed option sequences testable without spawning a process.
static func parse(args: PackedStringArray, fallback_expansion: String = "") -> Result:
	var result := Result.new()
	result.expansion = fallback_expansion.strip_edges()
	var oned_root := ""
	var i := 0
	while i < args.size():
		var option := args[i].to_lower()
		match option:
			"/d":
				result.loose_override = true
			"/exp":
				if i + 1 < args.size():
					var value := args[i + 1].strip_edges()
					if _is_expansion_value(value):
						result.expansion = value
						result.has_expansion_override = true
						i += 1
			"--oned-resource-root":
				if i + 1 < args.size():
					var value := args[i + 1].strip_edges()
					if not value.is_empty() and not _is_known_option(value):
						oned_root = value
						i += 1
		i += 1
	if result.loose_override:
		result.oned_resource_root = oned_root
	return result


## Resolve the process command line once, with the saved game expansion as the
## fallback only when /exp was not supplied.
static func from_process(fallback_expansion: String = "") -> Result:
	return parse(_all_args(), fallback_expansion)


static func loose_override_enabled() -> bool:
	return from_process().loose_override


static func expansion(fallback: String = "") -> String:
	return from_process(fallback).expansion


static func _is_expansion_value(value: String) -> bool:
	return not value.is_empty() and not value.begins_with("/") and not value.begins_with("--")


static func _is_known_option(value: String) -> bool:
	var option := value.to_lower()
	return option == "/d" or option == "/exp" or option == "--oned-resource-root"
