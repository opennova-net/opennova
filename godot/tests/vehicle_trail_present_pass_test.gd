extends GutTest

# The native bank supplies already posed points; the real particle device owns
# their persistent groups and retires them on point/registry lifetime changes.
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


func _make_presenter(fx: EffectWorld, anchors: ItemEffectDirector) -> EntityPresenter:
	var presenter := EntityPresenter.new()
	add_child_autofree(presenter)
	presenter.setup(null, null, null)
	presenter.setup_passes(null, null, null, null, fx, null, null, anchors)
	return presenter


func _row(generation: int, point: int, position: Vector3, magnitude: float,
		effect := W3_EFFECT) -> VehicleTrailVisualRow:
	return VehicleTrailVisualRow.make(HANDLE, generation, point, effect,
			position, Vector3(0, 0, 1), magnitude, 42)


func _owner_key(generation: int, point: int) -> String:
	return "vehicle-trail:%d:%d:p%d" % [generation, HANDLE, point]


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


func test_points_follow_sampled_pose_and_tune_without_respawn() -> void:
	var anchors := ItemEffectDirector.new()
	var fx := PresentPassFixture.make_fx(self, anchors, _catalog_file())
	var presenter := _make_presenter(fx, anchors)
	var a_key := _owner_key(41, 0)
	var b_key := _owner_key(41, 1)
	var position := Vector3(12.5, 3.0, -7.25)
	var sample := _row(41, 0, position, 0.75)
	assert_eq(sample.pos, position, "typed seam preserves the sampled position")
	assert_eq(sample.point, 0)
	assert_eq(sample.source_tick, 42)
	presenter.present_vehicle_trail_visuals([
		sample, _row(41, 1, position + Vector3.RIGHT, 0.25, W4_EFFECT)])
	var a := _live_owned_row(fx, a_key)
	var b := _live_owned_row(fx, b_key)
	assert_not_null(a)
	assert_not_null(b)
	if a == null or b == null:
		return
	var a_id := int(a.id)
	var b_id := int(b.id)
	assert_almost_eq(_emitter(a).emit_rate, 45.0, 0.0001)
	assert_almost_eq(_emitter(b).emit_rate, 15.0, 0.0001)
	# The offset control turns the authored y_offset 2 + z_offset 4 * (2 * magnitude - 1)
	# into the live spawn height and clears the camera pull: 0.75 -> 4.0, 0.25 -> 0.0
	# (CEffectWorld_UpdateBlendValues, EffectScene.set_group_parameters).
	assert_almost_eq(_emitter(a).spawn_y_offset, 4.0, 0.0001)
	assert_almost_eq(_emitter(a).camera_pull, 0.0, 0.0001)
	assert_almost_eq(_emitter(b).spawn_y_offset, 0.0, 0.0001)
	position += Vector3(5, 2, -1)
	presenter.present_vehicle_trail_visuals([
		_row(41, 0, position, 1.25),
		_row(41, 1, position + Vector3.RIGHT, 0.5, W4_EFFECT)])
	a = _live_owned_row(fx, a_key)
	assert_eq(int(a.id), a_id, "fixed samples update the same group")
	assert_eq(int(_live_owned_row(fx, b_key).id), b_id)
	var moved: Variant = anchors.resolve_owner_transform(a_key)
	assert_true(moved is Transform3D)
	if moved is Transform3D:
		assert_almost_eq((moved as Transform3D).origin, position, POSITION_EPS)
	fx.advance_fixed_tick(0.0)
	a = _live_owned_row(fx, a_key)
	assert_almost_eq(_emitter(a).position, position, POSITION_EPS)
	assert_almost_eq(_emitter(a).emit_rate, 75.0, 0.0001,
			"magnitudes above one remain unclamped")
	assert_almost_eq(_emitter(a).spawn_y_offset, 8.0, 0.0001,
			"1.25 extrapolates the spawn height past the authored z_offset")
	presenter.present_vehicle_trail_visuals([_row(41, 1, position, 0.0, W4_EFFECT)])
	assert_null(_live_owned_row(fx, a_key))
	assert_true(_row_by_id(fx, a_id).detached)
	assert_false(anchors.has_effect_anchor(a_key))
	assert_eq(int(_live_owned_row(fx, b_key).id), b_id,
			"an active zero parameter is distinct from an absent point")
	presenter.present_vehicle_trail_visuals([])
	assert_null(_live_owned_row(fx, b_key))
	assert_true(_row_by_id(fx, b_id).detached)
	assert_false(anchors.has_effect_anchor(b_key))


func test_registry_generation_replaces_same_handle_ownership() -> void:
	var anchors := ItemEffectDirector.new()
	var fx := PresentPassFixture.make_fx(self, anchors, _catalog_file())
	var presenter := _make_presenter(fx, anchors)
	var old_key := _owner_key(61, 0)
	var next_key := _owner_key(62, 0)
	presenter.present_vehicle_trail_visuals([_row(61, 0, Vector3.ZERO, 0.5)])
	var old := _live_owned_row(fx, old_key)
	assert_not_null(old)
	if old == null:
		return
	var old_id := int(old.id)
	presenter.present_vehicle_trail_visuals([_row(62, 0, Vector3.ZERO, 0.5)])
	assert_null(_live_owned_row(fx, old_key))
	assert_true(_row_by_id(fx, old_id).detached)
	assert_false(anchors.has_effect_anchor(old_key))
	assert_not_null(_live_owned_row(fx, next_key))
	assert_true(anchors.has_effect_anchor(next_key))


func test_focal_wind_moves_the_authored_second_part_and_clears() -> void:
	var container := Node3D.new()
	add_child_autofree(container)
	var data := ObjectData.new()
	assert_eq(data.open_file(ProjectSettings.globalize_path(
			"res://../fixtures/threedi/synth/tank.3di")), OK)
	var placer := MissionObjectPlacer.new()
	assert_true(placer.register_object_data("wind_tank", data))
	var model := placer.build_model_from_graphic("wind_tank", "", container, "")
	assert_not_null(model)
	if model == null:
		return
	var parts := model.get_render_part_nodes()
	assert_true(parts.has(1), "fixture exposes the second render part")
	if not parts.has(1):
		return
	var part := parts[1] as Node3D
	model.set_on_screen(true)
	ObjectModel.advance_awake_frame(0.016)
	var rest: Transform3D = data.evaluate_panm(0, 0, {})[1]
	model.rotation.y = 0.6
	var world_offset := Vector3(2, 0.5, -1)
	var bend := Basis.from_euler(Vector3(0.1, -0.08, 0.03))
	# Hand-derived from the retail Sway pose (BoneCallback_Sway_World: the second
	# part rotates about its authored pivot, then takes the wind offset), written
	# in Godot's frame: R = flip * Ry(-0.08) * Rx(0.1) * Rz(0.03) * flip with
	# flip = diag(-1, 1, 1); origin = pivot - R * pivot + Ry(0.6)^-1 * (2, 0.5, -1)
	# for the tank.3di second-part pivot (0, 1.3, 0). Literal columns and origin,
	# not the presenter's own composition.
	var sway_basis := Basis(
			Vector3(0.996114, -0.029846, -0.082864),
			Vector3(0.037874, 0.994556, 0.097072),
			Vector3(0.079515, -0.099833, 0.991822))
	var sway_origin := Vector3(2.166077, 0.507077, 0.177755)
	var expected := Transform3D(sway_basis, sway_origin) * rest
	model.set_focal_sway(true, bend, world_offset)
	assert_almost_eq(part.position, expected.origin, POSITION_EPS,
			"world wind rotates around the authored pivot in model space")
	assert_almost_eq(part.basis.x, expected.basis.x, POSITION_EPS)
	assert_almost_eq(part.basis.y, expected.basis.y, POSITION_EPS)
	assert_almost_eq(part.basis.z, expected.basis.z, POSITION_EPS)
	ObjectModel.advance_awake_frame(0.016)
	assert_almost_eq(part.position, expected.origin, POSITION_EPS,
			"ordinary frame updates preserve the focal-wind pose")
	model.set_focal_sway(false, Basis(), Vector3.ZERO)
	assert_almost_eq(part.position, rest.origin, POSITION_EPS)
	assert_almost_eq(part.basis.get_euler(), rest.basis.get_euler(), POSITION_EPS)
