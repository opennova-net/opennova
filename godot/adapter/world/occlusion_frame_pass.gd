class_name OcclusionFramePass
extends RefCounted

# The render-occlusion frame (docs/render/render-occlusion-re.md §3/§4/§5),
# extracted from GameWorld: the blink letter gates, the per-render-frame
# section-mask/portal apply, the probe A/B seam edges, and the unload reset.
#
# HOT PATH: GameWorld calls apply_blink_gates()/apply_frame() directly every
# frame. Every entry here is a plain method call on a stored direct reference
# — ZERO Callables/lambdas anywhere in the frame path (the dispatcher-perf
# rule) — and steady frames allocate nothing at this seam (GUT-pinned by
# test_occlusion_steady_frames_touch_no_nodes).
#
# Diff-applied: the sim emits verdict CHANGES (get_building_visibility_changes /
# get_render_culled_changes) and only transitions touch nodes, so a steady frame
# does no per-node work. Two ownership bits decide final visibility:
# the present pass owns the sim's intent (PF_HIDDEN), this system owns the
# occlusion hide — the present pass consults _occlusion_hidden_ids (shared by
# reference) so it never fights an occlusion hide, and an occlusion release
# lands on sim.entity_present_visible() so a sim-hidden entity never flashes.
#
# The two shared dictionaries below are created ONCE here and NEVER
# reassigned: GameWorld._start_runtime hands these SAME instances by reference
# into present_options (NovaPresentApplier stores them; the shared-reference
# contract is pinned by mission_present_pass_test). Unload clears IN PLACE.

# The GameWorld whose render nodes this pass drives — a direct reference,
# stored once in setup(); per-frame reads go through its public getters as
# direct calls. Untyped: the world script owns this object (the DebugViewSet
# convention), and harness runtimes ride the world's _runtime seam.
var _world

# The local player's applied blink letter gates (render-occlusion-re.md §4):
# accum bit 0x2 hides the terrain render (near detail + far foliage ride the
# terrain node) and the sky dome + celestials; bit 0x8 hides the water passes.
# blink_indoors is deliberately a public plain var: the world's frame
# clear-color pass reads it directly every frame (indoors clears BLACK).
var blink_indoors := false
var _blink_water_suppressed := false
# bms_id -> true for every node occlusion currently hides (buildings whose
# batch verdict culled them, entities the render gates culled).
var _occlusion_hidden_ids: Dictionary = {}
# Exact MissionPresentPass bms_id -> visibility intent, shared by reference.
# Occlusion only layers hides on top of this value and never reconstructs the
# present predicate independently.
var _present_visibility: Dictionary = {}
# bms_id -> resolved node, so steady frames skip registry lookups. Entries
# revalidate with is_instance_valid on use; reset on unload/A-B seams.
var _occlusion_node_cache: Dictionary = {}

# The shared F3 frame-stats board (null outside the game shell), re-handed by
# GameWorld wherever its own board changes: this pass lands the OCCL_* split
# spans itself from apply_frame.
var _frame_stats: FrameStatsBoard = null
# Mirrors GameWorld._perf_probe_enabled, written only at the probe toggle edge
# (set_perf_probe_enabled): the manual A/B probe shares apply_frame's clock
# reads with the F3 Stats capture, exactly as before the extraction.
var probe_timing := false
# The two apply_frame halves, valid while probe/stats timing runs:
# the native run_occlusion_frame call and the GDScript node application.
var _perf_occl_native_us := 0
var _perf_occl_apply_us := 0


## One-time wiring from the owning GameWorld (constructed in the world's
## _init, before any load).
func setup(world) -> void:
	_world = world


func set_frame_stats_board(board: FrameStatsBoard) -> void:
	_frame_stats = board


## The occlusion-claim set the present pass consults — shared BY REFERENCE
## (never copied, never reassigned; see the header contract).
func occlusion_hidden_ids() -> Dictionary:
	return _occlusion_hidden_ids


## The present pass's exact visibility intent — shared BY REFERENCE.
func present_visibility() -> Dictionary:
	return _present_visibility


# --- Blink frame gates (docs/render/render-occlusion-re.md §4) -----------------
# The local player's accumulated blink letters gate whole render passes. The
# letters are authored PER BOX (init 0x3E; letters clear bits), so windowed
# buildings simply don't carry the indoors letter and keep the outside world
# rendering — no portal special-casing at the gate level. The per-section
# interior visibility masks (the portal traversal) are the next occlusion slice.

## Re-apply the letter gates after a sim tick. `forces_indoors` is
## GameWorld._mission_forces_indoors, passed per call — the mission attribute
## is mission state and stays (test-pinned) on the world.
func apply_blink_gates(forces_indoors: bool) -> void:
	# Duck-typed like the world's silhouette-anchor pull: harness runtimes
	# supply value-only sims without weakening get_sim()'s NovaSimulation
	# contract.
	var runtime = _world.get_runtime()
	if runtime == null or not runtime.has_method("get_sim"):
		return
	var sim = runtime.get_sim()
	if sim == null or not sim.has_method("local_player_blink_flags"):
		return
	# The mission force-indoors attribute ORs the indoors letter into the frame
	# view for BOTH consumers, matching run_occlusion_frame's camera input
	# [orig: Bms_AttribFlags & 0x10 @ 0x5ca1c8 -> accum |= 2].
	var flags := int(sim.local_player_blink_flags()) | (
			NovaSimulation.BLINK_INDOORS if forces_indoors else 0)
	# Accum bit 0x2 (indoors): the terrain render is skipped entirely — the
	# near-detail and far-foliage tiers are terrain children here, matching
	# retail where the detail cells ride the skipped terrain traversal and the
	# far patches carry their own bit-2 gate — and the skybox pass (dome +
	# celestials) is skipped [orig: render_main_scene @ 0x5c1353 (PolyTrn
	# skip), terrain_scene_render @ 0x5d0570, Terrain_RenderSkyboxPass skip
	# @ 0x5ca84f, Foliage_RenderFarPatchesPass skips @ 0x5c95bf/0x5c9665].
	var indoors := (flags & NovaSimulation.BLINK_INDOORS) != 0
	if indoors != blink_indoors:
		blink_indoors = indoors
		var terrain: NovaTerrain = _world.get_terrain_node()
		if terrain != null:
			terrain.visible = not indoors
		var sky: Node = _world.get_node_or_null("NovaSky")
		if sky != null:
			sky.visible = not indoors
		var celestial: Node = _world.get_node_or_null("NovaCelestial")
		if celestial != null:
			celestial.visible = not indoors
	# Accum bit 0x8 (the authored water letter): both water passes skipped.
	# Letter bits only accumulate while inside a box, so the outdoors leg of
	# retail's override is implicit; the remaining g_BlinkWaterVisible legs
	# (a camera building straddling the water plane, the window latch) ride
	# the section-mask slice [orig: Terrain_RenderSceneWithReflection
	# @ 0x5c93cb / @ 0x5c95d2-0x5c95ea].
	var water_off := (flags & NovaSimulation.BLINK_WATER_OFF) != 0
	if water_off != _blink_water_suppressed:
		_blink_water_suppressed = water_off
		var water: Node = _world.get_water_node()
		if water != null:
			water.visible = not water_off


# --- The render-occlusion frame (docs/render/render-occlusion-re.md §3/§5) -----
# Per render frame: run the sim's occlusion pipeline (camera blink query ->
# portal traversal -> section masks + TOC occluder culling + the entity render
# gates), then drive the de-batched building nodes' per-section masks and the
# gated entities' visibility. Runs after the present pass (inside
# tick_realtime) so present's base visibility is re-asserted first each frame.
# (The marched iris-exposure weather feed that renders alongside stays on
# GameWorld — _stamp_iris_samples.)
func apply_frame(camera_xform: Transform3D, forces_indoors: bool) -> void:
	var runtime = _world.get_runtime()
	if runtime == null or not runtime.has_method("get_sim"):
		return
	var sim = runtime.get_sim()
	if sim == null or not sim.has_method("run_occlusion_frame"):
		return
	var registry = runtime.get_registry() if runtime.has_method("get_registry") else null
	if registry == null:
		return
	var fov_y := 70.0
	var near := 0.05
	var aspect := 16.0 / 9.0
	if _world.is_inside_tree():
		var cam: Camera3D = _world.get_viewport().get_camera_3d()
		if cam != null:
			fov_y = cam.fov
			near = cam.near
		var vs: Vector2 = _world.get_viewport().get_visible_rect().size
		if vs.y > 0.0:
			aspect = vs.x / vs.y
	var fog := 1000.0
	var env: Node = _world.get_environment_node()
	if env != null and env.has_method("get_fog_distance"):
		fog = float(env.get_fog_distance())
	var water: Node = _world.get_water_node()
	var water_z := -100000.0
	if _world.is_water_render_active():
		var wh = water.get("water_height")
		if wh != null:
			water_z = float(wh)
	var stats_on := _frame_stats != null and _frame_stats.is_capture_active()
	var timing := probe_timing or stats_on
	var native_start := Time.get_ticks_usec() if timing else 0
	sim.run_occlusion_frame(camera_xform, fov_y, aspect, near, fog, water_z,
			forces_indoors)
	var native_end := Time.get_ticks_usec() if timing else 0

	# Building batch visibility + per-section masks (bit N = render part N,
	# forced-visible def bits already merged by the sim), applied as CHANGES:
	# the sim diffs against what this shell last applied, so a steady frame
	# walks nothing. Batch culls claim the occlusion-hidden bit; the same
	# verdicts as the full-walk form land on the nodes.
	# [orig: Terrain_RenderSectorModels @ 0x5c5d30]
	var changes: PackedInt64Array = sim.get_building_visibility_changes()
	for i in range(0, changes.size(), 2):
		var bms_id := int(changes[i])
		var node := _occlusion_node(registry, bms_id)
		if node == null:
			continue
		var packed := int(changes[i + 1])
		if node.has_method("set_section_visibility_mask"):
			node.set_section_visibility_mask(packed & 0xFFFFFFFF)
		_set_occlusion_hidden(sim, node, bms_id, ((packed >> 32) & 1) == 0)

	# Entity render gates (the blink-hits gate + the outdoors three-ray latch),
	# also applied as changes. [orig: the collector gates @ 0x5c7022-0x5c708a / §3.4]
	var culled_changes: PackedInt32Array = sim.get_render_culled_changes()
	if culled_changes.size() >= 2:
		var added := int(culled_changes[0])
		for i in range(1, 1 + added):
			var node := _occlusion_node(registry, int(culled_changes[i]))
			if node != null:
				_set_occlusion_hidden(sim, node, int(culled_changes[i]), true)
		for i in range(2 + added, culled_changes.size()):
			var node := _occlusion_node(registry, int(culled_changes[i]))
			if node != null:
				_set_occlusion_hidden(sim, node, int(culled_changes[i]), false)

	# The g_BlinkWaterVisible override legs the slice-1 gate deferred: with the
	# authored water letter suppressing (accum bit 0x8), the water still renders
	# when the frame latched the exterior or a camera building straddles the
	# water plane. [orig: @ 0x5c93cb / @ 0x5c95d2 + g_BlinkWaterVisible
	# @ 0x29ACE40]
	if water != null and sim.has_method("occlusion_water_visible"):
		water.visible = not _blink_water_suppressed or bool(sim.occlusion_water_visible())

	if timing:
		_perf_occl_native_us = native_end - native_start
		_perf_occl_apply_us = Time.get_ticks_usec() - native_end
	if stats_on:
		_frame_stats.add(FrameStatsBoard.OCCL_APPLY, _perf_occl_apply_us)
		# The native call's internal split; the remainder of the bound call
		# (marshalling + the handle collection) lands in the glue slot so the
		# pane's Occlusion group still sums to the whole frame cost.
		if sim.has_method("get_last_occlusion_build_us"):
			var build_us := int(sim.get_last_occlusion_build_us())
			var probe_us := int(sim.get_last_occlusion_probe_us())
			_frame_stats.add(FrameStatsBoard.OCCL_BUILD, build_us)
			_frame_stats.add(FrameStatsBoard.OCCL_PROBE, probe_us)
			_frame_stats.add(FrameStatsBoard.OCCL_GLUE,
					maxi(_perf_occl_native_us - build_us - probe_us, 0))
		else:
			_frame_stats.add(FrameStatsBoard.OCCL_GLUE, _perf_occl_native_us)


## The probe A/B seam, entering the occlusion skip: restore the water to the
## authored blink state, then release every occlusion override. Mission
## blink/indoors semantics remain authoritative (release keeps them; iris
## keeps sampling on the world).
func enter_probe_skip() -> void:
	var water: Node = _world.get_water_node()
	if water != null:
		water.visible = not _blink_water_suppressed
	release_overrides(false)


## Leaving the skip: re-arm a full re-emit from the sim's delta baseline.
func leave_probe_skip() -> void:
	_reset_apply_baseline()


# Resolve (and cache) the node a bms_id drives. Cache entries revalidate with
# is_instance_valid; a freed node re-resolves through the registry (reloads
# recreate nodes under the same ids).
func _occlusion_node(registry, bms_id: int) -> Node3D:
	var cached: Variant = _occlusion_node_cache.get(bms_id)
	if cached != null and is_instance_valid(cached):
		return cached
	var node: Node = registry.resolve_single(bms_id)
	if node == null or not (node is Node3D):
		_occlusion_node_cache.erase(bms_id)
		return null
	_occlusion_node_cache[bms_id] = node
	return node


# The occlusion-hidden ownership bit. A hide claims the id (the present pass
# consults the shared set and never fights it); a release clears the claim and
# lands the node on the sim's CURRENT present intent, so a WAC/sim-hidden
# entity never flashes for a frame.
func _set_occlusion_hidden(sim, node: Node3D, bms_id: int, hidden: bool) -> void:
	if hidden:
		if not _occlusion_hidden_ids.has(bms_id):
			_occlusion_hidden_ids[bms_id] = true
			if node.visible:
				node.visible = false
	elif _occlusion_hidden_ids.erase(bms_id):
		var present_visible := _entity_present_visible(sim, bms_id)
		if present_visible and not node.visible:
			node.visible = true


# Duck-typed sim resolution for the occlusion apply paths: harness runtimes
# serve stub sims that the typed get_sim() accessor cannot return.
func _occlusion_sim() -> Object:
	var runtime = _world.get_runtime()
	if runtime == null or not runtime.has_method("get_sim"):
		return null
	var sim: Variant = runtime.get_sim()
	return sim if sim is Object else null


func _entity_present_visible(sim: Object, bms_id: int) -> bool:
	if _present_visibility.has(bms_id):
		return bool(_present_visibility[bms_id])
	# Compatibility/test sources without MissionPresentPass retain the native
	# base predicate. Production placed nodes always publish the exact combined
	# hidden + local-view-suppressed intent above.
	if sim != null and sim.has_method("entity_present_visible"):
		return bool(sim.entity_present_visible(bms_id))
	return true


# Release every occlusion override: restore claimed nodes to the sim's present
# intent, clear section masks to fully-visible, drop the caches, and forget the
# sim's delta baseline so a later re-enable re-emits full state. The A/B seam
# keeps mission blink/indoors semantics (reset_semantics=false); reset_semantics
# = true marks the unload-time release — the mission force-indoors attribute
# itself stays on GameWorld (test-pinned by name) and unload clears it THERE,
# so the flag carries no extra work here.
func release_overrides(_reset_semantics: bool) -> void:
	var sim := _occlusion_sim()
	for bms_id in _occlusion_hidden_ids:
		var node: Variant = _occlusion_node_cache.get(bms_id)
		if node != null and is_instance_valid(node):
			var present_visible := _entity_present_visible(sim, int(bms_id))
			if present_visible:
				(node as Node3D).visible = true
	_occlusion_hidden_ids.clear()
	for bms_id in _occlusion_node_cache:
		var node: Variant = _occlusion_node_cache[bms_id]
		if node != null and is_instance_valid(node) \
				and (node as Node).has_method("set_section_visibility_mask"):
			(node as Node).set_section_visibility_mask(-1)
	_occlusion_node_cache.clear()
	_reset_apply_baseline()


# Forget the sim's applied-state baseline so the next occlusion frame re-emits
# everything (the shell caches were dropped or the A/B skip ended).
func _reset_apply_baseline() -> void:
	var sim := _occlusion_sim()
	if sim != null and sim.has_method("reset_occlusion_apply_baseline"):
		sim.reset_occlusion_apply_baseline()


## Unload teardown. The ordering replicates the pre-extraction unload EXACTLY:
## 1. the shared present-visibility intent clears first (GameWorld.unload
##    cleared it well before its blink/occlusion resets), so step 3's release
##    consult falls back to sim.entity_present_visible for every claimed node;
## 2. the blink letter gates restore [orig: the letter-bit clear @ 0x525c45 at
##    mission start] — an unload while indoors must not leave the next
##    mission's terrain/sky/water hidden;
## 3. every occlusion override releases and the sim delta baseline resets.
## Both shared dictionaries clear IN PLACE — their identity is pinned.
func reset() -> void:
	_present_visibility.clear()
	_reset_blink_frame_gates()
	release_overrides(true)


func _reset_blink_frame_gates() -> void:
	if blink_indoors:
		var terrain: NovaTerrain = _world.get_terrain_node()
		if terrain != null:
			terrain.visible = true
		var sky: Node = _world.get_node_or_null("NovaSky")
		if sky != null:
			sky.visible = true
		var celestial: Node = _world.get_node_or_null("NovaCelestial")
		if celestial != null:
			celestial.visible = true
	if _blink_water_suppressed:
		var water: Node = _world.get_water_node()
		if water != null:
			water.visible = true
	blink_indoors = false
	_blink_water_suppressed = false
