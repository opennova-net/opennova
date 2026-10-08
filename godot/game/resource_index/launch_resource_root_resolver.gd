class_name LaunchResourceRootResolver
extends ResourceRootResolver

## Explicit launch data (the directory, `/exp`) and the game selection, also
## used when a multiplayer join remounts the resource root. The Mods list's pick
## lives on the shell's live root, which the world is handed (D-MNU-31).


func _resource_dir() -> String:
	return LaunchFlags.resource_dir()


func _expansion() -> String:
	return LaunchFlags.expansion()


func _game() -> String:
	return LaunchFlags.game(ResourceDirSettings.get_game())
