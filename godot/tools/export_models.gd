extends SceneTree
## Source-project entry point for the headless model export command.


func _initialize() -> void:
	quit(ExportModelsCli.run(OS.get_cmdline_user_args()))
