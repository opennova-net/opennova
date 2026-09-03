class_name SettingsResourceRootResolver
extends ResourceRootResolver

## The shell's persisted resource settings (ResourceDirSettings) behind the
## world's resource-root resolver hook: the game path (no injected root)
## mounts from the persisted directory with the persisted expansion and game
## code; a joiner's expansion remount reads its game code the same way.


func _resource_dir() -> String:
	return ResourceDirSettings.get_resource_dir()


func _expansion() -> String:
	return ResourceDirSettings.get_expansion()


func _game() -> String:
	return ResourceDirSettings.get_game()
