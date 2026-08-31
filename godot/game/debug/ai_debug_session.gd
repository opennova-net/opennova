class_name AiDebugSession
extends RefCounted
## The shell's F3 AI-overlay wiring in one place: the DevTools "ai_view_request"
## signal applied onto the live world's debug-view set, and the two provider
## installs a fresh world needs -- the world's AI view state back into DevTools
## (so the F3 toggle strip shows pushed truth) and the DevTools selection into
## the world (so the overlay can ring the selected brain). Owned by
## DebugPickSession (the shell's F3 debug-session object); a world is handed in
## per mission.

var _dev_tools: DevTools = null
var _world: GameWorld = null


## Wire the toggle-request signal once (the DevTools node lives as long as the
## shell); worlds come and go through begin_world.
func setup(dev_tools: DevTools) -> void:
	_dev_tools = dev_tools
	if not dev_tools.ai_view_request.is_connected(_on_view_request):
		dev_tools.ai_view_request.connect(_on_view_request)


## A fresh mission's world: install both providers. The overlay toggles retain
## on the world side (DebugViewSet), so nothing re-applies here.
func begin_world(world: GameWorld) -> void:
	_world = world
	if world == null:
		if _dev_tools != null:
			_dev_tools.set_ai_view_state_provider(Callable())
		return
	if _dev_tools != null:
		_dev_tools.set_ai_view_state_provider(world.get_ai_view_state)
		world.set_ai_debug_selection_provider(_dev_tools.selected_entity_handle)


func _on_view_request(id: StringName, enabled: bool) -> void:
	if _world == null or not is_instance_valid(_world):
		return
	match id:
		&"overlay":
			_world.set_ai_debug_option(&"show_ai_overlay", enabled)
		&"labels":
			_world.set_ai_debug_option(&"show_ai_labels", enabled)
		&"routes":
			_world.set_ai_debug_option(&"show_ai_routes", enabled)
		&"targets":
			_world.set_ai_debug_option(&"show_ai_targets", enabled)
		&"rings":
			_world.set_ai_debug_option(&"show_ai_rings", enabled)
