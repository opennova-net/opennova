extends SceneTree
## Source-project entry point for the headless clip export command.


func _initialize() -> void:
	quit(ExportClipsCli.run(OS.get_cmdline_user_args()))
