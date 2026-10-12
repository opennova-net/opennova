class_name GameCfgStore
extends RefCounted

# The process's cfg block: retail's game.cfg state, held for the process from
# its start to its exit (the native GameConfigBlock over the engine's
# runtime/inmatch/game_config_block.h, which owns the reads' order, the first
# launch's video test and its trigger, D-GAMECFG-3; witness record
# docs/gamecfg/game-cfg-re.md). game.cfg lives in the directory the game runs
# in (LaunchFlags.working_dir), as player.sav does. Its reads:
#  - the start's, before anything mounts (block()'s first use);
#  - every main-menu entry's (enter_main_menu): the first one runs the
#    mount's read and, on a fresh profile, the video test first; an entry
#    over another game is the reload;
#  - the Mods list's switch (reload_main_menu): the save, then the read.
# A join's expansion switch reads nothing: GameWorld hands the block the new
# mount's weapon table (GameConfigBlock.switch_expansion), so the entry after
# the match is a plain read over it.
# A read whose file sets mpreset ends the process with exit code 0
# (MainGame.exit_process), writing nothing. The shell's save points write the
# block (save()); the options, the HUD's colour and declutter words and the
# score fanfare's gate read and write their words here (GameConfigBlock keys).

const FILE_NAME := "game.cfg"

static var _block: GameConfigBlock = null


## The process's block. Its first use runs the start's read.
static func block() -> GameConfigBlock:
	if _block == null:
		_block = GameConfigBlock.new()
		_block.set_path(_path())
		_block.load()
	return _block


# game.cfg in the working directory as it stands now: retail opens the file by
# its bare name at every read and save.
static func _path() -> String:
	return String(LaunchFlags.working_dir()).path_join(FILE_NAME)


## A main-menu entry over the mounted `root`. False when a read set mpreset:
## the caller ends the process.
static func enter_main_menu(root: ResourceRoot) -> bool:
	var config := block()
	config.set_path(_path())
	return config.enter_main_menu(
		root, Strings.get_table(Strings.TABLE_GAMETEXT), PlayerOptions.desktop_size()
	)


## The expansion or mod reload over the newly mounted `root`. False when its
## read set mpreset.
static func reload_main_menu(root: ResourceRoot) -> bool:
	var config := block()
	config.set_path(_path())
	return config.reload_main_menu(
		root, Strings.get_table(Strings.TABLE_GAMETEXT), PlayerOptions.desktop_size()
	)


## Write the block to game.cfg (a save point).
static func save() -> int:
	var config := block()
	config.set_path(_path())
	return int(config.save())


## Drop the block (tests): the next use reads again in the working directory.
static func reset() -> void:
	_block = null
