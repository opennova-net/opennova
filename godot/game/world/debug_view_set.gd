class_name DebugViewSet
extends Node

# The F3 debug-view owner — the world-space debug views (skeleton, user points,
# collision, effect boxes, round trails, hit meshes, portal faces) plus the
# pick highlight/click stack, built on demand and torn down per mission.
# Owned by GameWorld as an internal child node (the NetSessionDrive pattern):
# constructed in the world's _init, wired once through setup().
#
# The views add as CHILDREN OF THE WORLD NODE, not of this set: owners and
# tests pin them as world-relative lookups (main_game_lifecycle_test resolves
# SkeletonDebug/PickDebug/PickClickCatcher under the world) and they draw
# world-space geometry over the world's subtree. Each view receives the WORLD
# in its setup and re-resolves world.get_sim() per frame, so this set is a
# lifecycle owner, never a render parent. This set also keeps ONE typed member
# per view it constructs (construction-time registration, ADR 0034) — the
# status readback calls each view's typed counter directly instead of
# re-scanning the world by node name.

const SkeletonDebugView := preload("res://game/debug/skeleton_debug_view.gd")
const UserPointDebugView := preload("res://game/debug/user_point_debug_view.gd")
const CollisionDebugView := preload("res://game/debug/collision_debug_view.gd")
const OcclusionDebugView := preload("res://game/debug/occlusion_debug_view.gd")
const ParticleDebugView := preload("res://game/debug/particle_debug_view.gd")
const RoundDebugView := preload("res://game/debug/round_debug_view.gd")
const HitboxDebugView := preload("res://game/debug/hitbox_debug_view.gd")
const DebugViewStatus := preload(
		"res://game/debug/debug_view_status.gd")
const SKELETON_DEBUG_NAME := "SkeletonDebug"
const USER_POINT_DEBUG_NAME := "UserPointDebug"
const COLLISION_DEBUG_NAME := "CollisionDebug"
const PARTICLE_DEBUG_NAME := "ParticleDebug"
const OCCLUSION_DEBUG_NAME := "OcclusionDebug"
const ROUND_DEBUG_NAME := "RoundDebug"
const HITBOX_DEBUG_NAME := "HitboxDebug"
const PICK_DEBUG_NAME := "PickDebug"
const PICK_CATCHER_NAME := "PickClickCatcher"

# The GameWorld the views attach under and re-resolve their sim through — its
# PUBLIC surface only; the world's private internals arrive as the setup()
# Callables below.
var _world: GameWorld
var _user_point_sources := Callable()  # () -> Array (the placer's static grouped sources)
var _effect_world_getter := Callable()  # () -> EffectWorld (weakref-guarded by the world)

# The constructed views (typed, one per kind; null while not installed —
# entries revalidate for LIVENESS since unload frees the world-parented nodes).
var _skeleton_view: SkeletonDebugView = null
var _user_point_view: UserPointDebugView = null
var _collision_view: CollisionDebugView = null
var _particle_view: ParticleDebugView = null
var _occlusion_view: OcclusionDebugView = null
var _round_view: RoundDebugView = null
var _hitbox_view: HitboxDebugView = null

# Debug: draw character bones over the world (the dev tools' "Show skeletons"). Off by default.
var _skeleton_debug := false
# Debug: draw named model user points, including static-batched objects. Off by default.
var _user_point_debug := false
# Debug: draw the collision volumes + player capsule (the dev tools' "Show collision"). Off by default.
var _collision_debug := false
var _occlusion_debug := false
# Debug: draw live emitter bounds + effect names (the dev tools' "Show effect boxes").
var _particle_debug := false
var _round_debug := false
var _hitbox_debug := false


## One-time wiring from the owning GameWorld: the world node the views attach
## under, and the two private seams it lends as Callables (its placer's
## get_static_user_point_sources and its get_effect_world, weakref-guarded).
func setup(world: GameWorld, user_point_sources: Callable,
		effect_world_getter: Callable) -> void:
	_world = world
	_user_point_sources = user_point_sources
	_effect_world_getter = effect_world_getter


# LIVENESS check on a typed view member: unload frees the world-parented view
# nodes while the members retain their last reference.
func _view_live(view: Node) -> bool:
	return view != null and is_instance_valid(view) 			and not view.is_queued_for_deletion()


## Readback for the dev tools and the MCP: toggle intent, installed view, and whether that
## installed view currently has anything it can draw are deliberately separate
## facts. A retained toggle can be enabled while its mission-owned view is
## detached during unload/reload.
func get_debug_view_statuses() -> Array[DebugViewStatus]:
	var statuses: Array[DebugViewStatus] = []
	statuses.append(_view_status(
			&"show_skeletons", _skeleton_debug, _view_live(_skeleton_view),
			_skeleton_view.get_debug_drawable_count() \
					if _view_live(_skeleton_view) else 0,
			"No skeletons to draw", "skeleton", "skeletons"))
	statuses.append(_view_status(
			&"show_user_points", _user_point_debug, _view_live(_user_point_view),
			_user_point_view.get_debug_drawable_count() \
					if _view_live(_user_point_view) else 0,
			"No user points to draw", "user point", "user points"))
	statuses.append(_view_status(
			&"show_collision", _collision_debug, _view_live(_collision_view),
			_collision_view.get_debug_drawable_count() \
					if _view_live(_collision_view) else 0,
			"No collision shapes to draw", "collision shape", "collision shapes"))
	statuses.append(_view_status(
			&"show_effect_boxes", _particle_debug, _view_live(_particle_view),
			_particle_view.get_debug_drawable_count() \
					if _view_live(_particle_view) else 0,
			"No live effect bounds to draw", "effect box", "effect boxes"))
	statuses.append(_view_status(
			&"show_portal_faces", _occlusion_debug, _view_live(_occlusion_view),
			_occlusion_view.get_debug_drawable_count() \
					if _view_live(_occlusion_view) else 0,
			"No portal faces in range", "portal face", "portal faces"))
	statuses.append(_view_status(
			&"show_round_trails", _round_debug, _view_live(_round_view),
			_round_view.get_debug_drawable_count() \
					if _view_live(_round_view) else 0,
			"No recent rounds to draw", "round trail", "round trails"))
	statuses.append(_view_status(
			&"show_hit_meshes", _hitbox_debug, _view_live(_hitbox_view),
			_hitbox_view.get_debug_drawable_count() \
					if _view_live(_hitbox_view) else 0,
			"No hit meshes in range", "hit mesh", "hit meshes"))
	return statuses


func _view_status(id: StringName, enabled: bool, installed: bool,
		drawable_count: int, empty_reason: String, singular: String,
		plural: String) -> DebugViewStatus:
	var reason := "Disabled"
	if enabled and not installed:
		reason = "Waiting for a loaded world"
	elif enabled and drawable_count == 0:
		reason = empty_reason
	elif enabled:
		reason = "Drawing %d %s" % [
			drawable_count,
			singular if drawable_count == 1 else plural,
		]
	return DebugViewStatus.new(
			id, enabled, installed, drawable_count, reason)


## A load completed: rebuild every retained view whose data belongs to the
## freshly loaded world.
func on_loaded() -> void:
	if _skeleton_debug:
		_refresh_skeleton_debug()
	if _user_point_debug:
		_refresh_user_point_debug()
	if _collision_debug:
		_refresh_collision_debug()
	if _occlusion_debug:
		set_occlusion_debug(true)
	if _round_debug:
		set_round_debug(true)
	if _hitbox_debug:
		set_hitbox_debug(true)


## Mission teardown — preserves the exact retain/free split the world's unload
## always had. ParticleDebug, PickDebug, and PickClickCatcher deliberately
## survive: they re-resolve the effect world / sim through the world every
## frame, so mission reloads never leave them stale (see their build comments).
func on_unload() -> void:
	# Retain the toggles but detach every world-fed visualizer immediately. A
	# successful next load rebuilds the enabled set under the same stable names,
	# even when unload+load happens within one frame.
	for debug_name in [
		SKELETON_DEBUG_NAME,
		USER_POINT_DEBUG_NAME,
		COLLISION_DEBUG_NAME,
		OCCLUSION_DEBUG_NAME,
		ROUND_DEBUG_NAME,
		HITBOX_DEBUG_NAME,
	]:
		_remove_debug_view(debug_name)


func _remove_debug_view(debug_name: String) -> void:
	var existing: Node = _world.get_node_or_null(NodePath(debug_name))
	if existing == null:
		return
	_world.remove_child(existing)
	existing.queue_free()


# --- Skeleton debug view (the dev tools' "Show skeletons") -------------------
# Build / free a child SkeletonDebugView that draws every character's bones over the world.
# Mirrors the editor's set_pick_debug -> _refresh_pick_debug build/free toggle flow.

func set_skeleton_debug(enabled: bool) -> void:
	_skeleton_debug = enabled
	_refresh_skeleton_debug()

func is_skeleton_debug() -> bool:
	return _skeleton_debug

func _refresh_skeleton_debug() -> void:
	_remove_debug_view(SKELETON_DEBUG_NAME)
	if not _skeleton_debug:
		return
	var view := SkeletonDebugView.new()
	_skeleton_view = view
	view.name = SKELETON_DEBUG_NAME
	_world.add_child(view)
	view.setup(_world)  # walks the world's subtree for Skeleton3D nodes each frame


# --- User-point debug view (the dev tools' "Show user points") ----------------
# Live models are discovered under the world. Static mission objects have no
# per-entity nodes after batching, so the placer supplies the exact grouped
# placement-time sources that successfully rendered (lent as the
# _user_point_sources Callable).

func set_user_point_debug(enabled: bool) -> void:
	_user_point_debug = enabled
	_refresh_user_point_debug()


func is_user_point_debug() -> bool:
	return _user_point_debug


func _refresh_user_point_debug() -> void:
	_remove_user_point_debug_view()
	if not _user_point_debug:
		return
	var view := UserPointDebugView.new()
	_user_point_view = view
	view.name = USER_POINT_DEBUG_NAME
	_world.add_child(view)
	var static_sources: Array = _user_point_sources.call()
	view.setup(_world, static_sources)


func _remove_user_point_debug_view() -> void:
	_remove_debug_view(USER_POINT_DEBUG_NAME)


# --- Collision debug view (the dev tools' "Show collision") ------------------
# Build / free a child CollisionDebugView drawing the sim's collision volumes +
# the local player's capsule test points over the world. Same build/free toggle
# flow as the skeleton view; the view re-resolves the sim through the world
# every frame, so mission reloads never leave it stale.

func set_collision_debug(enabled: bool) -> void:
	_collision_debug = enabled
	_refresh_collision_debug()


func is_collision_debug() -> bool:
	return _collision_debug


# Build / free a child ParticleDebugView drawing every live emitter's bounds +
# effect name, on the overlay's "Show effect boxes" toggle (the collision-view
# contract; survives mission reloads by re-resolving the effect world through
# the world-lent getter).
func set_particle_debug(enabled: bool) -> void:
	_particle_debug = enabled
	_remove_debug_view(PARTICLE_DEBUG_NAME)
	if not enabled:
		return
	var view := ParticleDebugView.new()
	_particle_view = view
	view.name = PARTICLE_DEBUG_NAME
	_world.add_child(view)
	view.setup(_effect_world_getter)


func is_particle_debug() -> bool:
	return _particle_debug


func _refresh_collision_debug() -> void:
	_remove_debug_view(COLLISION_DEBUG_NAME)
	if not _collision_debug:
		return
	var view := CollisionDebugView.new()
	_collision_view = view
	view.name = COLLISION_DEBUG_NAME
	_world.add_child(view)
	view.setup(_world)  # duck-typed get_sim(), re-resolved per frame


# --- Round debug view (the dev tools' "Show round trails") -------------------
# Build / free a child RoundDebugView drawing the RoundSim debug ring (flight
# segments + hit markers + labels) over the world — the collision-view
# contract: the view re-resolves the sim through the world every frame,
# so mission reloads never leave it stale.

func set_round_debug(enabled: bool) -> void:
	_round_debug = enabled
	_remove_debug_view(ROUND_DEBUG_NAME)
	if not enabled:
		return
	var view := RoundDebugView.new()
	_round_view = view
	view.name = ROUND_DEBUG_NAME
	_world.add_child(view)
	view.setup(_world)  # duck-typed get_sim(), re-resolved per frame


func is_round_debug() -> bool:
	return _round_debug


# Build / free a child HitboxDebugView drawing the round hit-detection reality
# (bullet-mesh wireframes + bound spheres + posed organic bone spheres) — the
# collision-view contract, on the overlay's "Show hit meshes" toggle.
func set_hitbox_debug(enabled: bool) -> void:
	_hitbox_debug = enabled
	_remove_debug_view(HITBOX_DEBUG_NAME)
	if not enabled:
		return
	var view := HitboxDebugView.new()
	_hitbox_view = view
	view.name = HITBOX_DEBUG_NAME
	_world.add_child(view)
	view.setup(_world)  # duck-typed get_sim(), re-resolved per frame


func is_hitbox_debug() -> bool:
	return _hitbox_debug


# --- Pick debug (the F3 pick list: world highlight + overlay-open clicking) --
# The pick list itself is SHELL-owned (crosshair picks work before F3 ever
# opens); the world only renders it and, while the overlay is up, feeds it
# from clicks. Same build/free contract as every debug view.

var _pick_list: DebugPickList = null


## Install (or clear, with null) the shell's pick list: builds the world
## highlight view that follows it. The click catcher (see below) picks into
## the same list.
func set_pick_debug(pick_list: DebugPickList) -> void:
	_pick_list = pick_list
	_remove_debug_view(PICK_DEBUG_NAME)
	if pick_list == null:
		set_pick_click_enabled(false)
		return
	var view := PickDebugView.new()
	view.name = PICK_DEBUG_NAME
	view.set_pick_list(pick_list)
	_world.add_child(view)
	view.setup(_world)  # duck-typed get_sim(), re-resolved per frame


## While the dev tools are open (mouse released), a world click ray-picks the
## entity under the cursor into the installed pick list.
func set_pick_click_enabled(enabled: bool) -> void:
	_remove_debug_view(PICK_CATCHER_NAME)
	if not enabled or _pick_list == null:
		return
	var catcher := PickClickCatcher.new()
	catcher.name = PICK_CATCHER_NAME
	_world.add_child(catcher)
	catcher.setup(_world, _pick_list)


# --- Occlusion debug view (the dev tools' "Show portal faces") ---------------
# Build / free a child OcclusionDebugView drawing the render-occlusion portal
# faces (type-colored outlines + section labels) over the world — the
# collision-view contract: the view re-resolves the sim through the world
# every frame, so mission reloads never leave it stale.

func set_occlusion_debug(enabled: bool) -> void:
	_occlusion_debug = enabled
	_remove_debug_view(OCCLUSION_DEBUG_NAME)
	if not enabled:
		return
	var view := OcclusionDebugView.new()
	_occlusion_view = view
	view.name = OCCLUSION_DEBUG_NAME
	_world.add_child(view)
	view.setup(_world)  # duck-typed get_sim(), re-resolved per frame

func is_occlusion_debug() -> bool:
	return _occlusion_debug
