@tool
class_name PackGameCli
extends RefCounted

## Headless project command used by local and CI release packaging:
##   godot --headless --path godot --script res://tools/pack_game.gd -- --pack-game <src_dir> <game_dir>

const FLAG := "--pack-game"


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

	var out: GamePackResult = GamePacker.export_game(src, game_dir)
	if not out.ok:
		printerr("pack-game: %s" % out.error)
		return 1
	print("pack-game: %d files archived into %s; exported %s -> %s" % [
			(out.archived as PackedStringArray).size(),
			String(out.archive).get_file(),
			", ".join(out.exported as PackedStringArray),
			game_dir])
	return 0
