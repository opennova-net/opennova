extends GutTest

# The watercraft W3/W4 present pass on its public typed-data seam. The model,
# particle world, and anchor registry are all production classes; the synthetic
# tank fixture supplies a rear FX00 userpoint without requiring retail assets.

const MODEL_3DI := "res://../fixtures/threedi/synth/tank.3di"
const MODEL_GRAPHIC := "wake_test_tank"
const W3_EFFECT := "fx_sml_wk"
const W4_EFFECT := "fx_sml_wk_f"
const HANDLE := 0x0123
const POSITION_EPS := Vector3(0.001, 0.001, 0.001)


func _catalog_file() -> ParticleFile:
	var def := ParticleDef.new()
	def.id = "wake dots"
	def.emit_dur = 0.1
	def.emit_rate = 30.0
	def.emit_rate_adj = 30.0
	def.emit_burst = 1
	def.y_offset = 2.0
	def.z_offset = 4.0
	def.age = 1.0
	def.flags = ParticleDef.FLAG_FOREVER_EMIT

	var file := ParticleFile.new()
	var particles: Array = file.particles
	particles.append(def)
	file.particles = particles
	var effects: Array = file.effects
	for effect_name in [W3_EFFECT, W4_EFFECT]:
		var effect := ParticleEffect.new()
		effect.id = effect_name
		effect.pdefs = PackedStringArray([def.id])
		effects.append(effect)
	file.effects = effects
	return file


func _make_fx(anchors: ItemEffectDirector) -> EffectWorld:
	var fx := EffectWorld.new()
	add_child_autofree(fx)
	fx.load_particle_file(_catalog_file())
	fx.set_owner_position_provider(anchors.resolve_owner_transform)
	return fx


# Returns [placer, ObjectData, ObjectModel].
func _tank_fixture(container: Node3D) -> Array:
	var placer := MissionObjectPlacer.new()
	var data := ObjectData.new()
	assert_eq(data.open_file(ProjectSettings.globalize_path(MODEL_3DI)), OK,
			"the synthetic tank fixture loads")
	assert_true(placer.register_object_data(MODEL_GRAPHIC, data))
	var model: ObjectModel = placer.build_model_from_graphic(
			MODEL_GRAPHIC, "", container, "")
	assert_not_null(model, "the fixture model builds through the real placer")
	return [placer, data, model]


func _make_presenter(container: Node3D, placer: MissionObjectPlacer,
		fx: EffectWorld, anchors: ItemEffectDirector) -> EntityPresenter:
	var presenter := EntityPresenter.new()
	add_child_autofree(presenter)
	presenter.setup(null, null, placer)
	presenter.setup_passes(container, null, null, null, fx, null, null, anchors)
	return presenter


func _wake_row(generation: int, position: Vector3, rotation_deg: Vector3,
		water_height: float, w3_magnitude: float, w4_magnitude: float,
		w3_userpoint := "FX00", w4_userpoint := "fx00",
		afloat := true) -> VehicleWakeVisualRow:
	return VehicleWakeVisualRow.make(
			HANDLE, generation, position, rotation_deg, water_height, afloat,
			W3_EFFECT, w3_userpoint, w3_magnitude,
			W4_EFFECT, w4_userpoint, w4_magnitude,
			0, 255, 0xFFFFFF, 1293, 42)


func _userpoint_index(data: ObjectData, name: String) -> int:
	for index in range(mini(data.get_user_point_count(), 16)):
		var point: ModelUserPoint = data.get_user_point_info(index)
		if point != null and point.name.nocasecmp_to(name) == 0:
			return index
	return -1


func _owner_key(generation: int, slot: int, point_index: int) -> String:
	return "vehicle-wake:%d:%d:w%d:p%d" % [
			generation, HANDLE, slot, point_index]


func _live_owned_row(fx: EffectWorld, owner_key: String) -> EffectGroupReport:
	for value in fx.get_debug_group_report():
		var row := value as EffectGroupReport
		if row.owner_key == owner_key and not row.detached:
			return row
	return null


func _row_by_id(fx: EffectWorld, group_id: int) -> EffectGroupReport:
	for value in fx.get_debug_group_report():
		var row := value as EffectGroupReport
		if int(row.id) == group_id:
			return row
	return null


func _emitter(row: EffectGroupReport) -> EffectEmitterReport:
	if row == null or row.emitters.is_empty():
		return null
	return row.emitters[0] as EffectEmitterReport


func _expected_anchor(data: ObjectData, point_index: int, position: Vector3,
		rotation_deg: Vector3, water_height: float) -> Transform3D:
	var point: ModelUserPoint = data.get_user_point_info(point_index)
	var vehicle := MissionObjectPlacer.entity_transform(
			MissionObjectPlacer.godot_to_bms_position(position), rotation_deg)
	var expected := vehicle * EffectWorld.forward_pose(
			point.position, point.rotation)
	expected.origin.y = water_height
	return expected


func test_groups_follow_sampled_pose_and_live_tune_without_respawn() -> void:
	var container := Node3D.new()
	add_child_autofree(container)
	var fixture := _tank_fixture(container)
	var placer: MissionObjectPlacer = fixture[0]
	var data: ObjectData = fixture[1]
	var model: ObjectModel = fixture[2]
	var point_index := _userpoint_index(data, "FX00")
	assert_gte(point_index, 0, "the fixture has a first-16 FX00 anchor")
	if point_index < 0 or model == null:
		return

	var anchors := ItemEffectDirector.new()
	var fx := _make_fx(anchors)
	var presenter := _make_presenter(container, placer, fx, anchors)
	presenter.register_wire_node(HANDLE, model)
	var generation := 41
	var w3_key := _owner_key(generation, 3, point_index)
	var w4_key := _owner_key(generation, 4, point_index)
	var position := Vector3(12.5, 3.0, -7.25)
	var rotation := Vector3(10.0, 35.0, -5.0)
	var water_height := 1.25

	presenter.present_vehicle_wake_visuals([
		_wake_row(generation, position, rotation, water_height, 0.75, 0.25)])
	var w3 := _live_owned_row(fx, w3_key)
	var w4 := _live_owned_row(fx, w4_key)
	assert_not_null(w3, "W3 command-speed wake spawns")
	assert_not_null(w4, "W4 signed-motion wake spawns")
	if w3 == null or w4 == null:
		return
	var w3_id := int(w3.id)
	var w4_id := int(w4.id)
	assert_eq(w3.name, W3_EFFECT)
	assert_eq(w4.name, W4_EFFECT)
	assert_true(anchors.has_effect_anchor(w3_key))
	assert_true(anchors.has_effect_anchor(w4_key))

	var expected := _expected_anchor(
			data, point_index, position, rotation, water_height)
	for key in [w3_key, w4_key]:
		var resolved: Variant = anchors.resolve_owner_transform(key)
		assert_true(resolved is Transform3D)
		if resolved is Transform3D:
			assert_almost_eq((resolved as Transform3D).origin, expected.origin,
					POSITION_EPS, "the authored point is clamped to sampled water")
			assert_true((resolved as Transform3D).basis.is_equal_approx(expected.basis),
					"the authored userpoint direction follows the sampled boat pose")

	var w3_emitter := _emitter(w3)
	var w4_emitter := _emitter(w4)
	assert_not_null(w3_emitter)
	assert_not_null(w4_emitter)
	if w3_emitter != null and w4_emitter != null:
		assert_almost_eq(w3_emitter.emit_rate, 45.0, 0.0001)
		assert_almost_eq(w3_emitter.spawn_y_offset, 4.0, 0.0001)
		assert_almost_eq(w3_emitter.camera_pull, 0.0, 0.0001)
		assert_almost_eq(w4_emitter.emit_rate, 15.0, 0.0001)
		assert_almost_eq(w4_emitter.spawn_y_offset, 0.0, 0.0001)
		assert_almost_eq(w4_emitter.camera_pull, 0.0, 0.0001)

	position = Vector3(-4.0, 8.0, 16.0)
	rotation = Vector3(-12.0, 110.0, 7.0)
	water_height = -2.5
	presenter.present_vehicle_wake_visuals([
		_wake_row(generation, position, rotation, water_height, 1.25, 0.5)])
	w3 = _live_owned_row(fx, w3_key)
	w4 = _live_owned_row(fx, w4_key)
	assert_eq(int(w3.id), w3_id, "W3 parameter and pose updates reuse the group")
	assert_eq(int(w4.id), w4_id, "W4 parameter and pose updates reuse the group")
	expected = _expected_anchor(data, point_index, position, rotation, water_height)
	var moved: Variant = anchors.resolve_owner_transform(w3_key)
	assert_true(moved is Transform3D)
	if moved is Transform3D:
		assert_almost_eq((moved as Transform3D).origin, expected.origin, POSITION_EPS)
		assert_almost_eq((moved as Transform3D).origin.y, water_height, 0.0001)
	fx.advance_fixed_tick(0.0)
	w3 = _live_owned_row(fx, w3_key)
	assert_almost_eq(_emitter(w3).position, expected.origin, POSITION_EPS,
			"fixed-tick owner sync moves the existing emitter")
	assert_almost_eq(_emitter(w3).emit_rate, 75.0, 0.0001,
			"magnitudes above one remain unclamped")
	assert_almost_eq(_emitter(w3).spawn_y_offset, 8.0, 0.0001)

	presenter.present_vehicle_wake_visuals([
		_wake_row(generation, position, rotation, water_height, 0.0, 0.5)])
	assert_null(_live_owned_row(fx, w3_key), "zero W3 magnitude releases its wake")
	assert_true(_row_by_id(fx, w3_id).detached)
	assert_false(anchors.has_effect_anchor(w3_key))
	assert_eq(int(_live_owned_row(fx, w4_key).id), w4_id,
			"the independently nonzero W4 wake stays live")

	presenter.present_vehicle_wake_visuals([])
	assert_null(_live_owned_row(fx, w4_key), "an absent vehicle retires its wake")
	assert_true(_row_by_id(fx, w4_id).detached)
	assert_false(anchors.has_effect_anchor(w4_key))


func test_unresolved_model_retries_and_missing_userpoint_has_no_origin_fallback() -> void:
	var container := Node3D.new()
	add_child_autofree(container)
	var fixture := _tank_fixture(container)
	var placer: MissionObjectPlacer = fixture[0]
	var data: ObjectData = fixture[1]
	var model: ObjectModel = fixture[2]
	var point_index := _userpoint_index(data, "FX00")
	if point_index < 0 or model == null:
		return
	var anchors := ItemEffectDirector.new()
	var fx := _make_fx(anchors)
	var presenter := _make_presenter(container, placer, fx, anchors)
	var generation := 52
	var key := _owner_key(generation, 3, point_index)
	var position := Vector3(1, 2, 3)

	presenter.present_vehicle_wake_visuals([
		_wake_row(generation, position, Vector3.ZERO, 0.0, 0.5, 0.0)])
	assert_null(_live_owned_row(fx, key),
			"an unresolved wire model does not create an origin wake")

	presenter.register_wire_node(HANDLE, model)
	presenter.present_vehicle_wake_visuals([
		_wake_row(generation, position, Vector3.ZERO, 0.0, 0.5, 0.0,
				"not_a_point", "")])
	assert_null(_live_owned_row(fx, key),
			"W3/W4 require an authored point and do not use the generic fallback")

	presenter.present_vehicle_wake_visuals([
		_wake_row(generation, position, Vector3.ZERO, 0.0, 0.5, 0.0)])
	assert_not_null(_live_owned_row(fx, key),
			"the same row succeeds once its model becomes resolvable")


func test_registry_generation_replaces_same_handle_ownership() -> void:
	var container := Node3D.new()
	add_child_autofree(container)
	var fixture := _tank_fixture(container)
	var placer: MissionObjectPlacer = fixture[0]
	var data: ObjectData = fixture[1]
	var model: ObjectModel = fixture[2]
	var point_index := _userpoint_index(data, "FX00")
	if point_index < 0 or model == null:
		return
	var anchors := ItemEffectDirector.new()
	var fx := _make_fx(anchors)
	var presenter := _make_presenter(container, placer, fx, anchors)
	presenter.register_wire_node(HANDLE, model)
	var old_key := _owner_key(61, 3, point_index)
	var next_key := _owner_key(62, 3, point_index)

	presenter.present_vehicle_wake_visuals([
		_wake_row(61, Vector3.ZERO, Vector3.ZERO, 0.0, 0.5, 0.0)])
	var old := _live_owned_row(fx, old_key)
	assert_not_null(old)
	if old == null:
		return
	var old_id := int(old.id)

	presenter.present_vehicle_wake_visuals([
		_wake_row(62, Vector3.ZERO, Vector3.ZERO, 0.0, 0.5, 0.0)])
	assert_null(_live_owned_row(fx, old_key))
	assert_true(_row_by_id(fx, old_id).detached,
			"the previous incarnation is retired even when its handle is reused")
	assert_false(anchors.has_effect_anchor(old_key))
	assert_not_null(_live_owned_row(fx, next_key))
	assert_true(anchors.has_effect_anchor(next_key))
