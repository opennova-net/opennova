class_name SignalRebind
extends RefCounted

## Moves a single callable subscription from one emitter to another: disconnect
## the callable from old_source's signal (when connected) and connect it to
## new_source's (when not already), tolerating null sources and sources that
## lack the signal. Collapses the disconnect / assign / reconnect dance repeated
## across the editor's set_editor() / set_*_editor() methods.
##
## Typical use, replacing the inline three-step block:
##   func set_editor(value: TerrainEditor) -> void:
##       SignalRebind.rebind(terrain_editor, value, &"ui_state_changed",
##           Callable(self, "_on_editor_ui_state_changed"))
##       terrain_editor = value
##       refresh()


static func rebind(old_source: Object, new_source: Object, signal_name: StringName, callable: Callable, flags: int = 0) -> void:
	if old_source != null and old_source.has_signal(signal_name) and old_source.is_connected(signal_name, callable):
		old_source.disconnect(signal_name, callable)
	if new_source != null and new_source.has_signal(signal_name) and not new_source.is_connected(signal_name, callable):
		new_source.connect(signal_name, callable, flags)
