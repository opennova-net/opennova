class_name GameCfgStore
extends RefCounted

# The process's cfg block: retail's game.cfg state, held for the process from
# its start to its exit (the native GameConfigBlock over the engine's
# runtime/inmatch/game_config_block.h; witness record
# docs/gamecfg/game-cfg-re.md). game.cfg lives in the directory the game runs
# in (LaunchFlags.working_dir), as player.sav does. Three reads take it:
#  - the start's, before anything mounts (block()'s first use);
#  - the first mount's, with the mount's weapon table and gametext texts
#    (read_for_mount); on a fresh profile the first launch's video test then
#    seeds its words (PlayerOptions.seed_fresh_profile) and saves at once
#    (its trigger: D-GAMECFG-3);
#  - a switch of the mounted game (the Mods list, PLAY RETAIL): the save, then
#    the read (read_for_mount on a new mount).
# A read whose file sets mpreset ends the process with exit code 0
# (MainGame.exit_process), writing nothing. The shell's save points write the
# block (save()); the options, the HUD's colour and declutter words and the
# score fanfare's gate read and write their words here (GameConfigBlock keys).

const FILE_NAME := "game.cfg"

static var _block: GameConfigBlock = null
# The mount the block last read over ("" before the first mount's read).
static var _mount_key := ""


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


## The read for a mount the menu starts on. The first mount's is the second
## read, over the mount's weapon table and default texts, then the video test on
## a fresh profile (no file, or one whose version the read discarded); a later
## mount (another game) is the reload: the save with the table the block has,
## the read, then the new mount's table. The same mount reads nothing. False
## when the read set mpreset: the caller ends the process.
static func read_for_mount(root: ResourceRoot) -> bool:
	var key := "<none>"
	if root != null:
		key = "%s|%s" % [String(root.get_root_dir()), String(root.get_expansion()).to_lower()]
	var config := block()
	if key == _mount_key:
		return true
	var first := _mount_key.is_empty()
	_mount_key = key
	config.set_path(_path())
	if first:
		config.set_mount_tables(root, Strings.get_table(Strings.TABLE_GAMETEXT))
		if not config.load():
			return false
		if config.is_version_reset():
			PlayerOptions.seed_fresh_profile(config)
			save()
		return true
	if not config.reload():
		return false
	config.set_mount_tables(root, Strings.get_table(Strings.TABLE_GAMETEXT))
	return true


## Write the block to game.cfg (a save point).
static func save() -> int:
	var config := block()
	config.set_path(_path())
	return int(config.save())


## Drop the block (tests): the next use reads again in the working directory.
static func reset() -> void:
	_block = null
	_mount_key = ""
