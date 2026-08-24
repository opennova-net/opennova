class_name PackGameCli
extends RefCounted

## Headless `--pack-game <src_dir> <game_dir>` driver.
##
## Packs a loose asset directory into a runnable game dir through
## [EditorGamePacker.export_game] — the same seam the Export Game action and Play in
## Retail's pack step use — so a scripted export cannot produce a game dir the editor
## could not. CI runs the just-exported editor with this flag to build the shipped
## zip's game with the shipped editor's own packer:
##
##   opennova-modtools.exe --headless -- --pack-game <src_dir> <game_dir>
##
## `game_dir` may be a populated directory (exes, a retail runtime); only the game's
## own artifacts — the archive and the loose-by-contract files — are written into it.
## Prints a summary and exits 0, or the failure and exits 1 (a CLI driver, so the
## console writes here are the allowlisted exception to the no-print rule).

const FLAG := "--pack-game"


class DirRoot:
	extends RefCounted
	var dir: String = ""
	func get_root_dir() -> String:
		return dir


static func wants_run(user_args: PackedStringArray) -> bool:
	return user_args.has(FLAG)


static func run(user_args: PackedStringArray) -> int:
	var idx := user_args.find(FLAG)
	if idx < 0 or idx + 2 >= user_args.size():
		printerr("usage: --pack-game <src_dir> <game_dir>")
		return 1
	var src := String(user_args[idx + 1]).strip_edges()
	var game_dir := String(user_args[idx + 2]).strip_edges()
	if src.is_empty() or game_dir.is_empty():
		printerr("usage: --pack-game <src_dir> <game_dir>")
		return 1
	if not DirAccess.dir_exists_absolute(src):
		printerr("pack-game: source directory not found: %s" % src)
		return 1

	var root := DirRoot.new()
	root.dir = src
	var out: Dictionary = EditorGamePacker.export_game(root, game_dir)
	if not bool(out.get("ok", false)):
		printerr("pack-game: %s" % String(out.get("error", "packing failed")))
		return 1
	print("pack-game: %d files archived into %s; exported %s -> %s" % [
			(out["archived"] as PackedStringArray).size(),
			String(out["archive"]).get_file(),
			", ".join(out["exported"] as PackedStringArray),
			game_dir])
	return 0
