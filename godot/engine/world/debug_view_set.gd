class_name DebugViewSet
extends Node

# The F3 debug-view host — the world-space debug views (skeleton, user points,
# collision, effect boxes, round trails, hit meshes, portal faces) plus the
# pick highlight/click stack, built on demand and torn down per mission.
# Owned by GameWorld as an internal child node (the NetSessionDrive pattern):
# constructed in the world's _init, wired once through setup().
#
# The views add as CHILDREN OF THE WORLD NODE, not of this set: hosts and
# tests pin them as world-relative lookups (main_game_lifecycle_test resolves
# SkeletonDebug/PickDebug/PickClickCatcher under the world) and they draw
# world-space geometry over the world's subtree. Each view receives the WORLD
# in its setup — the sim-driven views duck-type world.get_sim() per frame, and
# harness stubs ride the world's _runtime seam — so this set is a lifecycle
# owner, never a render parent.

const SkeletonDebugView := preload("res://engine/debug/skeleton_debug_view.gd")
const UserPointDebugView := preload("res://engine/debug/user_point_debug_view.gd")
const CollisionDebugView := preload("res://engine/debug/collision_debug_view.gd")
const OcclusionDebugView := preload("res://engine/debug/occlusion_debug_view.gd")
const ParticleDebugView := preload("res://engine/debug/particle_debug_view.gd")
const RoundDebugView := preload("res://engine/debug/round_debug_view.gd")
const HitboxDebugView := preload("res://engine/debug/hitbox_debug_view.gd")
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
# PUBLIC surface only (add_child/get_node_or_null/get_sim duck-typing); the
# world's private internals arrive as the setup() Callables below. Untyped:
# the world script owns this node.
var _world
var _user_point_sources := Callable()  # () -> Array (the placer's static grouped sources)
var _effect_world_getter := Callable()  # () -> NovaEffectWorld (weakref-guarded by the world)

# Debug: draw character bones over the world (F3 overlay's "Show skeletons"). Off by default.
var _skeleton_debug := false
# Debug: draw named model user points, including static-batched objects. Off by default.
var _user_point_debug := false
# Debug: draw the collision volumes + player capsule (F3 overlay's "Show collision"). Off by default.
var _collision_debug := false
var _occlusion_debug := false
# Debug: draw live emitter bounds + effect names (F3 overlay's "Show effect boxes").
var _particle_debug := false


## One-time wiring from the owning GameWorld: the world node the views attach
## under, and the two private seams it lends as Callables (its placer's
## get_static_user_point_sources and its get_effect_world, weakref-guarded).
func setup(world, user_point_sources: Callable, effect_world_getter: Callable) -> void:
	_world = world
	_user_point_sources = user_point_sources
	_effect_world_getter = effect_world_getter


## A load completed: re-arm the retained user-point toggle against the freshly
## loaded world (its sources come from the new placer).
func on_loaded() -> void:
	if _user_point_debug:
		_refresh_user_point_debug()


## Mission teardown — preserves the exact retain/free split the world's unload
## always had. ParticleDebug, PickDebug, and PickClickCatcher deliberately
## survive: they re-resolve the effect world / sim through the world every
## frame, so mission reloads never leave them stale (see their build comments).
func on_unload() -> void:
	# The user-point view retains its toggle and re-arms on the next successful load.
	_remove_user_point_debug_view()
	# Skeleton/collision/occlusion overlays are also freed for a clean teardown.
	var skel_debug: Node = _world.get_node_or_null(NodePath(SKELETON_DEBUG_NAME))
	if skel_debug != null:
		skel_debug.queue_free()
	var col_debug: Node = _world.get_node_or_null(NodePath(COLLISION_DEBUG_NAME))
	if col_debug != null:
		col_debug.queue_free()
	var occ_debug: Node = _world.get_node_or_null(NodePath(OCCLUSION_DEBUG_NAME))
	if occ_debug != null:
		occ_debug.queue_free()
	var rnd_debug: Node = _world.get_node_or_null(NodePath(ROUND_DEBUG_NAME))
	if rnd_debug != null:
		rnd_debug.queue_free()
	var hb_debug: Node = _world.get_node_or_null(NodePath(HITBOX_DEBUG_NAME))
	if hb_debug != null:
		hb_debug.queue_free()


# --- Skeleton debug view (F3 overlay's "Show skeletons") ---------------------
# Build / free a child SkeletonDebugView that draws every character's bones over the world.
# Mirrors the editor's set_pick_debug -> _refresh_pick_debug build/free toggle flow.

func set_skeleton_debug(enabled: bool) -> void:
	_skeleton_debug = enabled
	_refresh_skeleton_debug()

func is_skeleton_debug() -> bool:
	return _skeleton_debug

func _refresh_skeleton_debug() -> void:
	var existing: Node = _world.get_node_or_null(NodePath(SKELETON_DEBUG_NAME))
	if existing != null:
		existing.queue_free()
	if not _skeleton_debug:
		return
	var view := SkeletonDebugView.new()
	view.name = SKELETON_DEBUG_NAME
	_world.add_child(view)
	view.setup(_world)  # walks the world's subtree for Skeleton3D nodes each frame


# --- User-point debug view (F3 overlay's "Show user points") ------------------
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
	view.name = USER_POINT_DEBUG_NAME
	_world.add_child(view)
	var static_sources: Array = _user_point_sources.call()
	view.setup(_world, static_sources)


func _remove_user_point_debug_view() -> void:
	var existing: Node = _world.get_node_or_null(NodePath(USER_POINT_DEBUG_NAME))
	if existing == null:
		return
	# Detach before deferred destruction so an unload+reload in one frame can
	# create the same stable child name without stale-source/name collisions.
	_world.remove_child(existing)
	existing.queue_free()


# --- Collision debug view (F3 overlay's "Show collision") --------------------
# Build / free a child CollisionDebugView drawing the sim's collision volumes +
# the local player's capsule test points over the world. Same build/free toggle
# flow as the skeleton view; the view re-resolves the sim through the world
# every frame, so mission reloads never leave it stale.

func set_collision_debug(enabled: bool) -> void:
	_collision_debug = enabled
	_refresh_collision_debug()


# Build / free a child ParticleDebugView drawing every live emitter's bounds +
# effect name, on the overlay's "Show effect boxes" toggle (the collision-view
# contract; survives mission reloads by re-resolving the effect world through
# the world-lent getter).
func set_particle_debug(enabled: bool) -> void:
	_particle_debug = enabled
	var existing: Node = _world.get_node_or_null(NodePath(PARTICLE_DEBUG_NAME))
	if existing != null:
		existing.queue_free()
	if not enabled:
		return
	var view := ParticleDebugView.new()
	view.name = PARTICLE_DEBUG_NAME
	_world.add_child(view)
	view.setup(_effect_world_getter)

func _refresh_collision_debug() -> void:
	var existing: Node = _world.get_node_or_null(NodePath(COLLISION_DEBUG_NAME))
	if existing != null:
		existing.queue_free()
	if not _collision_debug:
		return
	var view := CollisionDebugView.new()
	view.name = COLLISION_DEBUG_NAME
	_world.add_child(view)
	view.setup(_world)  # duck-typed get_sim(), re-resolved per frame


# --- Round debug view (F3 overlay's "Show round trails") ---------------------
# Build / free a child RoundDebugView drawing the RoundSim debug ring (flight
# segments + hit markers + labels) over the world — the collision-view
# contract: the view re-resolves the sim through the world every frame,
# so mission reloads never leave it stale.

func set_round_debug(enabled: bool) -> void:
	var existing: Node = _world.get_node_or_null(NodePath(ROUND_DEBUG_NAME))
	if existing != null:
		existing.queue_free()
	if not enabled:
		return
	var view := RoundDebugView.new()
	view.name = ROUND_DEBUG_NAME
	_world.add_child(view)
	view.setup(_world)  # duck-typed get_sim(), re-resolved per frame


# Build / free a child HitboxDebugView drawing the round hit-detection reality
# (bullet-mesh wireframes + bound spheres + posed organic bone spheres) — the
# collision-view contract, on the overlay's "Show hit meshes" toggle.
func set_hitbox_debug(enabled: bool) -> void:
	var existing: Node = _world.get_node_or_null(NodePath(HITBOX_DEBUG_NAME))
	if existing != null:
		existing.queue_free()
	if not enabled:
		return
	var view := HitboxDebugView.new()
	view.name = HITBOX_DEBUG_NAME
	_world.add_child(view)
	view.setup(_world)  # duck-typed get_sim(), re-resolved per frame


# --- Pick debug (the F3 pick list: world highlight + overlay-open clicking) --
# The pick list itself is HOST-owned (crosshair picks work before F3 ever
# opens); the world only renders it and, while the overlay is up, feeds it
# from clicks. Same build/free contract as every debug view.

var _pick_list: NovaDebugPickList = null


## Install (or clear, with null) the host's pick list: builds the world
## highlight view that follows it. The click catcher (see below) picks into
## the same list.
func set_pick_debug(pick_list: NovaDebugPickList) -> void:
	_pick_list = pick_list
	var existing: Node = _world.get_node_or_null(NodePath(PICK_DEBUG_NAME))
	if existing != null:
		existing.queue_free()
	if pick_list == null:
		set_pick_click_enabled(false)
		return
	var view := PickDebugView.new()
	view.name = PICK_DEBUG_NAME
	view.set_pick_list(pick_list)
	_world.add_child(view)
	view.setup(_world)  # duck-typed get_sim(), re-resolved per frame


## While the F3 overlay is open (mouse released), a world click ray-picks the
## entity under the cursor into the installed pick list. The catcher lives in
## the world's subtree so ONED PIE's SubViewportContainer forwarding works
## unchanged.
func set_pick_click_enabled(enabled: bool) -> void:
	var existing: Node = _world.get_node_or_null(NodePath(PICK_CATCHER_NAME))
	if existing != null:
		existing.queue_free()
	if not enabled or _pick_list == null:
		return
	var catcher := PickClickCatcher.new()
	catcher.name = PICK_CATCHER_NAME
	_world.add_child(catcher)
	catcher.setup(_world, _pick_list)


# --- Occlusion debug view (F3 overlay's "Show portal faces") -----------------
# Build / free a child OcclusionDebugView drawing the render-occlusion portal
# faces (type-colored outlines + section labels) over the world — the
# collision-view contract: the view re-resolves the sim through the world
# every frame, so mission reloads never leave it stale.

func set_occlusion_debug(enabled: bool) -> void:
	_occlusion_debug = enabled
	var existing: Node = _world.get_node_or_null(NodePath(OCCLUSION_DEBUG_NAME))
	if existing != null:
		existing.queue_free()
	if not enabled:
		return
	var view := OcclusionDebugView.new()
	view.name = OCCLUSION_DEBUG_NAME
	_world.add_child(view)
	view.setup(_world)  # duck-typed get_sim(), re-resolved per frame

func is_occlusion_debug() -> bool:
	return _occlusion_debug
