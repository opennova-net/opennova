extends SceneTree
## Source-project entry point shared by CI and local game-data packaging.


func _initialize() -> void:
	quit(PackGameCli.run(OS.get_cmdline_user_args()))
