extends RefCounted


# THE shell destruction-presentation pass: presents the sim's item destruction on
# the viewing peer — the husk model swap on destroyed items, the death-piece
# debris (trail effects riding the sim's piece pool), resolved section-triangle
# debris and glass userpoint effects,
# the death/fire/other wreck effect families with the random fire crackle, and
# the destruction sounds. Drains Simulation.drain_destruction_events() +
# get_death_pieces() once per present, the fire_present_pass precedent.
#
# [orig map — docs/world/world-wac-ai-re.md §24:
#  husk swap: Flags & 4 switches render + collision to the def husk model
#    (Entity_RaycastCollisionModel @ 0x413086 pick; no husk -> the graphic keeps
#    standing, the witnessed fallback);
#  pieces: Entity_SpawnDeathPieces @ 0x493400 -> the 256-slot pool ticked by
#    DeathPiece_TickAll @ 0x57b900 (each piece renders ONE husk section with a
#    per-type trail effect from g_death_piece_types @ 0x8404f0);
#  section debris: Entity_SpawnSectionDebris @ 0x43f580 — collision-face
#    centroid sampling at stride (scale<<8)/150, Effect_TreeWoodExp per tri
#    (material 17 -> Effect_TreeFoliageExp), directions away from the blast;
#  wreck effects: Entity_InitDeathSounds @ 0x4939b0 (the particledeath family
#    at the Dead bones) + Entity_UpdateDeadWreckEffects @ 0x493140 (the fire
#    family's random crackle Effect_BoatExpSec + EXPLO_SHIP_SM, underwater
#    steam-out Effect_Boat01Steam).]
#
# Remaining stand-ins (tracked in the §24 record + D-ITEM ledger rows): pieces
# draw as their type's trail effect following the sim piece (the single-section
# husk MESH chunk needs per-part render instancing); effect anchors ride the
# entity origin, not the husk Dead/Fire/Other user points. Section debris and
# glass already arrive as resolved transient effect rows from the simulation.


var _sim: Simulation              # live pose source; null in data-driven tests
var _container: Node3D = null     # mission container (node-less husk grafts land here)
var _index: EntityIndex = null
var _placer: MissionObjectPlacer  # husk model builds
var _item_db: ItemDatabase        # husk graphic names
var _anchors: ItemEffectDirector  # owner-anchor registry (GameWorld's), or null
var _dynamic_node_resolver: WirePresentPass  # runtime-only packed handles
var _audio_provider := Callable() # -> MissionAudio (or null)
var _fx_provider := Callable()    # -> EffectWorld (or null)
# (world_pos: Vector3, radius: float) -> the death-flash light route
# (EffectLightDirector.on_death_light) [orig: the Entity_SpawnDeathPieces
# glow @ 0x49351a].
var _death_light := Callable()
var _husked: Dictionary = {}      # canonical mission identity -> husk Node3D/null
var _husk_restore: Dictionary = {} # canonical mission identity -> original state
var _burning: Dictionary = {}     # canonical wreck owner key -> live crackle anchor
var _wreck_anchor_keys: Dictionary = {} # registered wreck owner keys
var _piece_pos: Dictionary = {}   # piece slot -> Vector3 (anchor resolver source)
var _piece_generation: Dictionary = {}  # piece slot -> presented allocation generation


## Typed diagnostic counters (ADR 0017: cross-object contracts are typed
## records) — probes assert the presentation legs actually ran.
class Stats:
	extends RefCounted
	var husk_swaps := 0
	var no_husk := 0
	var pieces_peak := 0
	var debris_triangles := 0
	var effects := 0
	var sounds := 0
	var glass_points := 0
	var crackles := 0


var _stats := Stats.new()


func get_stats() -> Stats:
	return _stats


## Whether an owned fire-family effect is still registered for retail wreck
## crackle updates. The owner key is the same public identity used by the
## effect-anchor registry.
func has_active_wreck_fire(owner_key: String) -> bool:
	return _burning.has(owner_key)


func setup(sim: Simulation, container: Node3D, index: EntityIndex,
		placer: MissionObjectPlacer, item_db: ItemDatabase,
		anchors: ItemEffectDirector,
		audio_provider: Callable, fx_provider: Callable,
		dynamic_node_resolver: WirePresentPass = null,
		death_light := Callable()) -> void:
	_sim = sim
	_container = container
	_index = index
	_placer = placer
	_item_db = item_db
	_anchors = anchors
	_dynamic_node_resolver = dynamic_node_resolver
	_audio_provider = audio_provider
	_fx_provider = fx_provider
	_death_light = death_light


func teardown() -> void:
	reset_runtime_state()


## Discard mission-run presentation state without discarding setup dependencies.
## This is the Stop -> Play boundary as well as the teardown primitive: restore
## intact visuals, remove transient husk grafts, and retire every anchor whose
## resolver points into the prior simulation incarnation.
func reset_runtime_state() -> void:
	for slot in _piece_generation.keys():
		_unregister_piece_anchor(int(slot))
	_piece_generation.clear()
	_piece_pos.clear()

	for key in _wreck_anchor_keys.keys():
		_unregister_effect_anchor(key)
	_wreck_anchor_keys.clear()
	_burning.clear()

	for husk_key in _husk_restore.keys():
		var restore_v: Variant = _husk_restore[husk_key]
		if not (restore_v is Dictionary):
			continue
		var restore: Dictionary = restore_v
		var restored_bms_id := int(restore.get('bms_id', 0))
		if _placer != null and restored_bms_id != 0:
			_placer.clear_static_terrain_shadow_replacement(restored_bms_id)
		if String(restore.get('kind', '')) == 'static':
			if _placer != null:
				_placer.show_static_instance(restored_bms_id)
			continue
		var children_v: Variant = restore.get('children', [])
		if not (children_v is Array):
			continue
		var children: Array = children_v
		for saved_v in children:
			if not (saved_v is Dictionary):
				continue
			var saved: Dictionary = saved_v
			var child: Variant = saved.get('node')
			if child is Node3D and is_instance_valid(child):
				(child as Node3D).visible = bool(saved.get('visible', true))
	_husk_restore.clear()

	for husk_key in _husked.keys():
		var husk: Variant = _husked[husk_key]
		if husk is Node3D and is_instance_valid(husk):
			(husk as Node3D).visible = false
			(husk as Node3D).queue_free()
	_husked.clear()


## Once per present, after the sim advanced (beside the fire pass).
func present() -> void:
	if _sim == null:
		return
	present_drained(_sim.drain_destruction_events(), _sim.get_death_pieces())


## The pure-data presentation leg (the present_snapshot precedent): production
## present() drains the typed sim; tests feed the same event/piece rows.
func present_drained(events: DestructionDrain, pieces: Array) -> void:
	if events != null:
		for husk: HuskSwapEvent in events.husk_swaps:
			_apply_husk_swap(husk)
		for eff: DestructionEffectEvent in events.effects:
			_apply_effect(eff)
		for snd: DestructionSoundEvent in events.sounds:
			_apply_sound(snd)
		if _death_light.is_valid():
			for light: DeathLightEvent in events.death_lights:
				_death_light.call(light.pos, light.radius)
		_stats.debris_triangles += events.debris_triangles
		_stats.glass_points += events.glass_points
		# Sim-side rolls (S12b): the crackle EFFECT rides the ordinary effects
		# drain above; its sound rides the fire pass's drain_fire_sounds.
		_stats.crackles += events.crackles
	_sync_static_husks()
	_present_pieces(pieces)
	_tick_wreck_fires()


# Authored destruction events retain the mission-present value identity: file
# BMS id plus packed (kind,index), with the origin as the zero-id leg. Synthetic
# runtime entities instead use their packed wire handle because siblings share
# both zero BMS id and the non-BMS origin sentinel.
func _spawn_origin_parts(spawn_origin: int) -> Vector2i:
	return Vector2i(SpawnOrigin.kind(spawn_origin), SpawnOrigin.index(spawn_origin))


func _uses_dynamic_husk_identity(bms_id: int, spawn_origin: int,
		wire_handle: int) -> bool:
	if wire_handle < 0 or wire_handle == WireHandle.INVALID or bms_id != 0:
		return false
	# A real authored origin remains canonical even when its BMS id is zero.
	# Runtime-only entities carry either no origin or the promotion sentinel.
	return spawn_origin == SpawnOrigin.NONE


func _husk_identity_key(bms_id: int, spawn_origin: int,
		wire_handle: int = -1) -> String:
	if _uses_dynamic_husk_identity(bms_id, spawn_origin, wire_handle):
		return 'wire:%d' % wire_handle
	var origin := _spawn_origin_parts(spawn_origin)
	return '%d:%d:%d' % [bms_id, origin.x, origin.y]


func _resolve_entity_node(bms_id: int, spawn_origin: int = SpawnOrigin.NONE,
		wire_handle: int = -1) -> Node3D:
	var dynamic_identity := _uses_dynamic_husk_identity(
			bms_id, spawn_origin, wire_handle)
	if dynamic_identity:
		# A runtime-only owner has no authored mission identity. Never fall
		# through to the BMS/static lookup when its wire node is unavailable.
		if _dynamic_node_resolver == null:
			return null
		return _dynamic_node_resolver.resolve_wire_handle(wire_handle)
	if _index == null:
		return null
	var origin := _spawn_origin_parts(spawn_origin)
	var node := _index.resolve(bms_id, origin.x, origin.y)
	return node if node != null and is_instance_valid(node) else null


# The husk model swap: on a per-entity node, hide the intact node's visual
# children and graft the husk model as a child (it inherits the node transform,
# so settling wrecks keep moving with the present pass). Batched statics have no
# node: the instance is carved out of its graphic's MultiMesh batches and the
# husk grafts into the mission container at the placed transform. No husk
# authored -> the intact graphic keeps standing, dead — the witnessed
# render-pick fallback (batched statics stay in their batches).
func _apply_husk_swap(husk: HuskSwapEvent) -> void:
	var bms_id := husk.bms_id
	var spawn_origin := husk.spawn_origin
	var wire_handle := husk.wire_handle
	var husk_key := _husk_identity_key(bms_id, spawn_origin, wire_handle)
	if _husked.has(husk_key):
		return
	_stats.husk_swaps += 1
	var item_id := husk.item_id
	var def_id := item_id + MissionData.ITEM_ID_OFFSET  # wire type id -> items.def id
	var husk_graphic := ""
	if _item_db != null:
		husk_graphic = String(_item_db.get_husk(def_id))
		if husk_graphic.is_empty():
			husk_graphic = String(_item_db.get_huskfinal(def_id))
	if husk_graphic.is_empty() or _placer == null:
		_husked[husk_key] = null
		_stats.no_husk += 1
		return
	var node := _resolve_entity_node(bms_id, spawn_origin, wire_handle)
	if node != null and is_instance_valid(node):
		# A qualifying intact model transfers its static-caster role to the husk.
		var individual_casts_static_shadow := \
				_node_has_static_shadow_caster(node)
		var individual_mirror_reflected := node is ObjectModel \
				and bool((node as ObjectModel).mirror_reflected)
		var model: ObjectModel = _placer.build_model_from_graphic(
				husk_graphic, "", node, "", "", true)
		if model == null:
			_husked[husk_key] = null
			_stats.no_husk += 1
			return
		model.name = "HuskModel"
		_set_husk_static_shadow(model, individual_casts_static_shadow)
		# The reflect flag belongs to the entity, not its current graphic. The
		# individual branch must preserve it just like the batched carve branch
		# below; build_model_from_graphic has already built the replacement, so
		# apply the layer choice through one rebuild.
		if individual_mirror_reflected:
			model.set_mirror_reflected(true)
			model.rebuild()
		var child_visibility: Array = []
		for child in node.get_children():
			if child is Node3D and child != model:
				child_visibility.append({
					'node': child,
					'visible': (child as Node3D).visible,
				})
				(child as Node3D).visible = false
		_husk_restore[husk_key] = {
			'kind': 'individual',
			'bms_id': bms_id,
			'spawn_origin': spawn_origin,
			'wire_handle': wire_handle,
			'children': child_visibility,
		}
		_husked[husk_key] = model
		if bms_id != 0:
			_placer.set_static_terrain_shadow_replacement(bms_id,
					husk_graphic, node.transform,
					individual_casts_static_shadow)
		return
	if _uses_dynamic_husk_identity(bms_id, spawn_origin, wire_handle):
		# The dynamic row may already have retired or failed model resolution.
		# There is no safe static fallback: bms_id zero is a valid authored key.
		_husked[husk_key] = null
		_stats.no_husk += 1
		return
	# Batched static (world-wac-ai-re §24.6): carve the instance, graft at its
	# placed transform. An unknown bms_id (individual entity whose node is gone)
	# grafts nothing.
	if _container == null or not is_instance_valid(_container):
		_husked[husk_key] = null
		return
	# Batched replacements inherit the carved instance's authored eligibility.
	var batched_casts_static_shadow: bool = \
			bool(_placer.static_instance_casts_terrain_shadow(bms_id))
	var batched_mirror_reflected: bool = \
			bool(_placer.static_instance_is_mirror_reflected(bms_id))
	var graft: ObjectModel = _placer.build_model_from_graphic(
			husk_graphic, "", _container, "", "", true)
	if graft == null:
		_husked[husk_key] = null
		_stats.no_husk += 1
		return
	# The placer owns its static lookup by raw BMS id; canonical ownership above
	# must not change the key used to carve and later restore this batch slot.
	var xform_v: Variant = _placer.hide_static_instance(bms_id)
	if not (xform_v is Transform3D):
		graft.visible = false
		graft.queue_free()
		_husked[husk_key] = null
		return
	_husk_restore[husk_key] = {
		'kind': 'static',
		'bms_id': bms_id,
		'spawn_origin': spawn_origin,
		'placed_transform': xform_v,
		'husk_graphic': husk_graphic,
		'casts_static_shadow': batched_casts_static_shadow,
	}
	graft.name = "HuskModel_%d" % bms_id
	graft.transform = xform_v as Transform3D
	_set_husk_static_shadow(graft, batched_casts_static_shadow)
	# Retail's husk swap keeps the entity's reflect flag: the mirror
	# collectors keep filtering on entity+36 & 0x400, which destruction never
	# clears [orig: Entity_SpawnFromBMSRecord @ 0x40ed1d..0x40ed2b writer;
	# husk swap flips only Flags & 4].
	if batched_mirror_reflected:
		graft.set_mirror_reflected(true)
		graft.rebuild()
	_placer.set_static_terrain_shadow_replacement(bms_id, husk_graphic,
			graft.transform, batched_casts_static_shadow)
	_husked[husk_key] = graft


func _node_has_static_shadow_caster(root: Node) -> bool:
	if root is VisualInstance3D \
			and (((root as VisualInstance3D).layers \
				& Water.VISUAL_LAYER_STATIC_SHADOW_CASTER) != 0):
		return true
	for child in root.get_children():
		if _node_has_static_shadow_caster(child):
			return true
	return false


func _set_husk_static_shadow(model: ObjectModel, enabled: bool) -> void:
	if enabled and model != null:
		model.set_static_shadow_caster_enabled(true)


# Node-less wrecks still move while death physics settles them. Resolve the
# same compact present pose consumed by the other shell presentation paths.
func _present_transform_for_identity(bms_id: int,
		spawn_origin: int = SpawnOrigin.NONE) -> Variant:
	if _sim == null:
		return null
	var state := PackedVector3Array()
	if bms_id > 0:
		state = _sim.get_present_effect_state_for_bms_id(bms_id)
	if state.size() != Simulation.EFFECT_STATE_COUNT and spawn_origin != SpawnOrigin.NONE:
		state = _sim.get_present_effect_state_for_origin(
				SpawnOrigin.kind(spawn_origin), SpawnOrigin.index(spawn_origin))
	if state.size() != Simulation.EFFECT_STATE_COUNT:
		return null
	var rotation_deg := state[Simulation.EFFECT_STATE_ROTATION_DEG]
	var basis := MissionObjectPlacer.bms_to_godot_basis(rotation_deg)
	# Compact peer poses carry yaw only. Static death motion changes position but
	# not orientation, so retain the exact authored basis carved from the batch
	# when pitch/roll are unavailable. Host/listen poses carry the full Euler
	# angles and take the live-basis path above.
	if is_zero_approx(rotation_deg.x) and is_zero_approx(rotation_deg.z):
		var husk_key := _husk_identity_key(bms_id, spawn_origin)
		var restore_v: Variant = _husk_restore.get(husk_key)
		if restore_v is Dictionary:
			var placed_v: Variant = (restore_v as Dictionary).get('placed_transform')
			if placed_v is Transform3D:
				basis = (placed_v as Transform3D).basis
	return Transform3D(basis, state[Simulation.EFFECT_STATE_POSITION])


func _sync_static_husks() -> void:
	for husk_key in _husk_restore.keys():
		var restore_v: Variant = _husk_restore[husk_key]
		if not (restore_v is Dictionary):
			continue
		var restore: Dictionary = restore_v
		if String(restore.get('kind', '')) != 'static':
			continue
		var graft_v: Variant = _husked.get(husk_key)
		if not (graft_v is Node3D) or not is_instance_valid(graft_v):
			continue
		var live_v: Variant = _present_transform_for_identity(
				int(restore.get('bms_id', 0)),
				int(restore.get('spawn_origin', SpawnOrigin.NONE)))
		if live_v is Transform3D:
			var graft := graft_v as Node3D
			var live := live_v as Transform3D
			# The husk registration set the replacement once; this per-frame
			# sync only re-pushes on an actual transform change.
			if graft.transform.is_equal_approx(live):
				continue
			graft.transform = live
			if _placer != null:
				_placer.set_static_terrain_shadow_replacement(
						int(restore.get('bms_id', 0)),
						String(restore.get('husk_graphic', '')),
						live,
						bool(restore.get('casts_static_shadow', false)))


func _apply_effect(eff: DestructionEffectEvent) -> void:
	var fx: EffectWorld = _fx_provider.call() if _fx_provider.is_valid() else null
	if fx == null:
		return
	var effect := eff.effect
	if effect.is_empty():
		return
	var pos := eff.pos
	var family := eff.family
	var net_id := eff.attach_net_id
	var bms_id := eff.attach_bms_id
	var spawn_origin := eff.attach_spawn_origin
	var wire_handle := eff.attach_wire_handle
	var dynamic_identity := _uses_dynamic_husk_identity(
			bms_id, spawn_origin, wire_handle)
	if family == 0 or (net_id == 0 and not dynamic_identity):
		fx.spawn_effect(effect, pos, eff.dir)
		_stats.effects += 1
		return
	# Attached families (death smoke / fire / other): one owned group per
	# (entity, family), anchored at the wreck (the husk Dead/Fire/Other
	# user-point anchors are the tracked refinement).
	var key := ('wreck:wire:%d:%d' % [wire_handle, family]
			if dynamic_identity else "wreck:%d:%d" % [net_id, family])
	fx.spawn_effect_owned(key, effect, pos, Vector3.UP)
	_stats.effects += 1
	if _anchors != null:
		var node: Node3D = null
		if dynamic_identity:
			node = _resolve_entity_node(bms_id, spawn_origin, wire_handle)
		elif _index != null and bms_id != 0:
			node = _index.resolve_single(bms_id)
		if node != null and is_instance_valid(node):
			_anchors.register_effect_anchor(key, func() -> Variant:
				return node.global_transform if is_instance_valid(node) else null)
			if family == 2:
				_burning[key] = {"node": node}
		else:
			# Batched-static wreck: no node. Resolve the authoritative present
			# pose while it settles, with the event pose as an identity fallback.
			var fixed := Transform3D(Basis.IDENTITY, pos)
			_anchors.register_effect_anchor(key, func() -> Variant:
				var live_v: Variant = null if dynamic_identity \
						else _present_transform_for_identity(bms_id, spawn_origin)
				return live_v if live_v is Transform3D else fixed)
			if family == 2:
				if dynamic_identity:
					# A missing runtime node has no sibling-safe positional lookup;
					# retain its event pose instead of querying the shared sentinel.
					_burning[key] = {"pos": pos}
				else:
					_burning[key] = {
						"bms_id": bms_id,
						"spawn_origin": spawn_origin,
						"pos": pos,
					}
		_wreck_anchor_keys[key] = true


func _apply_sound(snd: DestructionSoundEvent) -> void:
	var audio: MissionAudio = _audio_provider.call() \
			if _audio_provider.is_valid() else null
	if audio == null:
		return
	var name := snd.sound
	if name.is_empty():
		return
	audio.fire_soundset(name, snd.pos, 0)
	_stats.sounds += 1


# Death pieces: the sim owns positions/physics; each live piece carries its
# type's trail effect as an owned follow group. The single-section husk mesh
# chunk is the tracked residual (§24).
func _present_pieces(pieces: Array) -> void:
	var fx: EffectWorld = _fx_provider.call() if _fx_provider.is_valid() else null
	_stats.pieces_peak = maxi(_stats.pieces_peak, pieces.size())
	var seen: Dictionary = {}
	for piece_v in pieces:
		var piece: Dictionary = piece_v
		var slot := int(piece.get("slot", -1))
		if slot < 0:
			continue
		seen[slot] = true
		var pos: Vector3 = piece.get("pos", Vector3.ZERO)
		_piece_pos[slot] = pos
		var generation := int(piece.get("generation", 0))
		var is_new_generation := int(_piece_generation.get(slot, -1)) != generation
		if is_new_generation:
			if _piece_generation.has(slot):
				_unregister_piece_anchor(slot)
			_piece_generation[slot] = generation
		if bool(piece.get("settled", false)):
			continue
		if is_new_generation:
			# The type's trail effect rides the drain row from the ONE native
			# table (world/destruction death_piece_trail_effect, S12b)
			# [orig: g_death_piece_types @ 0x8404f0 +0x2C].
			var trail := String(piece.get("trail", ""))
			if fx != null and not trail.is_empty():
				var key := "piece:%d" % slot
				fx.spawn_effect_owned(key, trail, pos, Vector3.UP)
				if _anchors != null:
					_anchors.register_effect_anchor(key, func() -> Variant:
						return _piece_pos.get(slot) if _piece_generation.has(slot) else null)
	for slot in _piece_generation.keys():
		if not seen.has(int(slot)):
			_unregister_piece_anchor(int(slot))
			_piece_generation.erase(slot)
			_piece_pos.erase(slot)


func _unregister_piece_anchor(slot: int) -> void:
	_unregister_effect_anchor('piece:%d' % slot)


func _unregister_effect_anchor(key: Variant) -> void:
	if _anchors != null:
		_anchors.unregister_effect_anchor(key)


# The wreck-fire registry prune: drop entries whose node died. The random
# crackle itself rolls in the SIM on the engine PRNG stream, per logic tick,
# and arrives as an ordinary transient effect + distance-delay-gated sound
# (S12b; world/destruction destruction_tick_dead_items
# [orig: Entity_UpdateDeadWreckEffects @ 0x493140]).
func _tick_wreck_fires() -> void:
	if _burning.is_empty():
		return
	for owner_key in _burning.keys():
		var entry: Dictionary = _burning[owner_key]
		var node: Variant = entry.get("node")
		if node is Node3D and not is_instance_valid(node):
			_burning.erase(owner_key)
			continue
		if node == null and not entry.has("bms_id") and not entry.has("pos"):
			_burning.erase(owner_key)
