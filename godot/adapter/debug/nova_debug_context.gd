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

## UI-free catalog/execution module shared by the overlay and runtime MCP.
## Pages use it for generic controls; data-only pages may keep reading the
## typed resolver helpers below.
var session: NovaDebugSession = null

## The shared NovaDebugOptions value store. Pages build controls against it
## (NovaDebugPage.add_option_check); the overlay routes its `changed` through
## the shared session, whose `control_invoked` is the observable channel.
var options: NovaDebugOptionState = null

## The HOST-owned debug pick list (null until the host injects one). Pages
## render and curate it; the host feeds it from the crosshair hotkey and the
## overlay-open click catcher.
var pick_list: NovaDebugPickList = null

## The overlay's snapshot orchestration: call with a path override ("" for
## the timestamped default) and receive the writer's {path, error} — pages
## put either on their own status labels.
var dump_snapshot := Callable()


## The current MissionRuntime, or null. Duck-typed: anything with get_sim().
func runtime() -> Object:
	if not runtime_source.is_valid():
		return null
	var value: Variant = runtime_source.call()
	if not (value is Object) or not is_instance_valid(value):
		return null
	var live_runtime := value as Object
	if not live_runtime.has_method("get_sim"):
		return null
	return live_runtime


## The runtime's live NovaSimulation, or null.
func sim() -> Object:
	var live_runtime := runtime()
	if live_runtime == null:
		return null
	var value: Variant = live_runtime.get_sim()
	if value == null or not is_instance_valid(value):
		return null
	return value


## The concrete engine simulation, for pages that consume its mandatory
## public API rather than the generic harness seam exposed by sim().
func nova_simulation() -> NovaSimulation:
	var value := sim()
	return value as NovaSimulation if value is NovaSimulation else null


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
