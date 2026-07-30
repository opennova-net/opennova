class_name SimDebugView
extends Node3D

# Shared scaffolding for the world-space debug views that read one
# NovaSimulation debug accessor through GameWorld: host/sim resolution, the
# per-frame refresh template, and the common wireframe/label drawing recipes.
# Each view names
# its accessor (_sim_debug_method), builds its meshes (_build_view), refreshes
# from a live sim (_refresh_from_sim), and drops its artifacts (_clear_all).
#
# The sim is re-resolved through the host every refresh so mission reloads
# never leave a view pointing at a freed NovaSimulation. Views are built /
# freed by the host (GameWorld) in response to the F3 overlay's toggle
# signals — the overlay itself never reaches into the 3D scene.

const MissionOverlayUtil := preload("res://engine/mission/mission_overlay_util.gd")

var _world: Node  # duck-typed host (get_sim()); re-resolved every refresh


## `world` is the node owning the running sim (the GameWorld).
func setup(world: Node) -> void:
	_world = world
	_build_view()


func _process(_delta: float) -> void:
	refresh_now()


## Immediately refresh the debug geometry from the current simulation.
## The process hook delegates here; tests and tooling can request a
## deterministic refresh without reaching into Godot's private frame callback.
func refresh_now() -> void:
	var sim := _resolve_sim()
	if sim == null:
		_clear_all()
		return
	_refresh_from_sim(sim)


func _resolve_sim() -> Object:
	if _world == null or not is_instance_valid(_world) or not _world.has_method("get_sim"):
		return null
	var sim: Variant = _world.get_sim()
	if sim == null or not is_instance_valid(sim) \
			or not (sim as Object).has_method(_sim_debug_method()):
		return null
	return sim


# --- Per-view hooks -------------------------------------------------------------

## The NovaSimulation debug accessor this view depends on (e.g. "get_round_debug").
func _sim_debug_method() -> String:
	return ""


## Build the view's meshes/labels once, from setup().
func _build_view() -> void:
	pass


## Refresh from a resolved, live sim (guaranteed to expose _sim_debug_method()).
func _refresh_from_sim(_sim: Object) -> void:
	pass


## Drop every drawn artifact (no host, or the host has no sim).
func _clear_all() -> void:
	pass


# --- Shared drawing recipes -----------------------------------------------------

## An unshaded, vertex-colored wireframe mesh node. `depth_test_off` lets the
## lines read through the meshes they describe (most views); hitbox statics
## keep the depth test so the wireframe hugs the visual model.
static func _make_lines_node(node_name: String, mesh: ImmediateMesh,
		depth_test_off := true) -> MeshInstance3D:
	var mi := MeshInstance3D.new()
	mi.name = node_name
	mi.mesh = mesh
	var mat := MissionOverlayUtil.line_material()
	mat.no_depth_test = depth_test_off
	mi.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	mi.material_override = mat
	return mi


## The overlay label recipe: billboarded, world-sized, depth-test off, dark
## outline, hidden until a refresh positions it.
static func _make_overlay_label(node_name: String, pixel_size: float,
		font_size: int, outline_size: int) -> Label3D:
	var lb := Label3D.new()
	lb.name = node_name
	lb.billboard = BaseMaterial3D.BILLBOARD_ENABLED
	lb.fixed_size = false
	lb.pixel_size = pixel_size
	lb.no_depth_test = true
	lb.font_size = font_size
	lb.outline_size = outline_size
	lb.outline_modulate = Color(0.0, 0.0, 0.0, 0.85)
	lb.visible = false
	return lb


## An axis cross (3 segments) at `at` — a point marker.
static func _cross(segments: Array, at: Vector3, arm: float, color: Color) -> void:
	segments.append({ "a": at - Vector3(arm, 0, 0), "b": at + Vector3(arm, 0, 0), "color": color })
	segments.append({ "a": at - Vector3(0, arm, 0), "b": at + Vector3(0, arm, 0), "color": color })
	segments.append({ "a": at - Vector3(0, 0, arm), "b": at + Vector3(0, 0, arm), "color": color })


## A horizontal diamond (4 segments) of radius `r` around `at` — reads as the
## point's radius without the vertex cost of a circle.
static func _diamond(segments: Array, at: Vector3, r: float, color: Color) -> void:
	var px := at + Vector3(r, 0, 0)
	var nx := at + Vector3(-r, 0, 0)
	var pz := at + Vector3(0, 0, r)
	var nz := at + Vector3(0, 0, -r)
	segments.append({ "a": px, "b": pz, "color": color })
	segments.append({ "a": pz, "b": nx, "color": color })
	segments.append({ "a": nx, "b": nz, "color": color })
	segments.append({ "a": nz, "b": px, "color": color })
