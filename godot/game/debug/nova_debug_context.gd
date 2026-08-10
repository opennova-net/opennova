class_name DebugContext
extends RefCounted
## Shared data access for the F3 overlay's pages: the host-supplied source
## Callables plus typed resolver helpers. One instance is created by the
## overlay and handed to every page at setup().
##
## Every source is re-resolved on EVERY call — mission reloads free and
## recreate the runtime/effect world, so a held reference would go stale.
## The source Callables are the untyped host seam; each helper converts the
## resolved value ONCE to its concrete class (ADR 0034) and returns null when
## the source is unset, invalid, freed, or the wrong type; pages render their
## own empty states off that.

const MissionRuntime := preload("res://game/world/mission_runtime.gd")

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
var session: DebugSession = null

## The shared DebugOptions value store. Pages build controls against it
## (DebugPage.add_option_check); the overlay routes its `changed` through
## the shared session, whose `control_invoked` is the observable channel.
var options: DebugOptionState = null

## The HOST-owned debug pick list (null until the host injects one). Pages
## render and curate it; the host feeds it from the crosshair hotkey and the
## overlay-open click catcher.
var pick_list: DebugPickList = null

## The overlay's snapshot orchestration: call with a path override ("" for
## the timestamped default) and receive the writer's {path, error} — pages
## put either on their own status labels.
var dump_snapshot := Callable()


## The current MissionRuntime, or null.
func runtime() -> MissionRuntime:
	if not runtime_source.is_valid():
		return null
	var value: Variant = runtime_source.call()
	if value is MissionRuntime and is_instance_valid(value):
		return value
	return null


## The runtime's live Simulation, or null.
func sim() -> Simulation:
	var live_runtime := runtime()
	if live_runtime == null:
		return null
	var value := live_runtime.get_sim()
	if value == null or not is_instance_valid(value):
		return null
	return value


## The world host (GameWorld), or null.
func world() -> GameWorld:
	if not world_source.is_valid():
		return null
	var value: Variant = world_source.call()
	if value is GameWorld and is_instance_valid(value):
		return value
	return null


## The live EffectWorld, or null.
func effect_world() -> EffectWorld:
	if not effect_world_source.is_valid():
		return null
	var value: Variant = effect_world_source.call()
	if value is EffectWorld and is_instance_valid(value):
		return value
	return null


## The host's typed camera record, or null when the source is unset or
## returns anything else.
func view_context() -> DebugViewContext:
	if not view_context_source.is_valid():
		return null
	var value: Variant = view_context_source.call()
	if value is DebugViewContext:
		return value
	return null
