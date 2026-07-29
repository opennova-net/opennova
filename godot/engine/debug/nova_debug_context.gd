class_name NovaDebugContext
extends RefCounted
## Shared data access for the F3 overlay's pages: the host-supplied source
## Callables plus validated resolver helpers. One instance is created by the
## overlay and handed to every page at setup().
##
## Every source is re-resolved on EVERY call — mission reloads free and
## recreate the runtime/effect world, so a held reference would go stale.
## All helpers return null when the source is unset, invalid, or resolves to
## a freed instance; pages render their own empty states off that.

var runtime_source := Callable()
var view_context_source := Callable()
var effect_world_source := Callable()
var world_source := Callable()

## The overlay-wide repaint (status line + active page), for page handlers
## whose action changes what other rows show (sim transport, var edits).
var request_refresh := Callable()


## The current MissionRuntime, or null. Duck-typed: anything with get_sim().
func runtime() -> Object:
	if not runtime_source.is_valid():
		return null
	var value: Variant = runtime_source.call()
	if value == null or not is_instance_valid(value):
		return null
	if not (value as Object).has_method("get_sim"):
		return null
	return value


## The runtime's live NovaSimulation, or null.
func sim() -> Object:
	var live_runtime := runtime()
	if live_runtime == null:
		return null
	var value: Variant = live_runtime.get_sim()
	if value == null or not is_instance_valid(value):
		return null
	return value


## The world host (GameWorld or a duck-typed stand-in), or null.
func world() -> Object:
	return _resolve_object(world_source)


## The live NovaEffectWorld, or null.
func effect_world() -> Object:
	return _resolve_object(effect_world_source)


## The host's typed camera record, or null when the source is unset or
## returns anything else.
func view_context() -> NovaDebugViewContext:
	if not view_context_source.is_valid():
		return null
	var value: Variant = view_context_source.call()
	if value is NovaDebugViewContext:
		return value
	return null


func _resolve_object(source: Callable) -> Object:
	if not source.is_valid():
		return null
	var value: Variant = source.call()
	if value is Object and is_instance_valid(value):
		return value
	return null
