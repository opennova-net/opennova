class_name LaunchResourceRootResolver
extends ResourceRootResolver

## Explicit launch data and the current game/expansion selection, also used
## when a multiplayer join remounts the resource root.


func _resource_dir() -> String:
	return LaunchFlags.resource_dir()


func _expansion() -> String:
	return LaunchFlags.expansion(ResourceDirSettings.get_expansion())


func _game() -> String:
	return LaunchFlags.game(ResourceDirSettings.get_game())
