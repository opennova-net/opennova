extends GutTest

# The fire present pass (EntityPresenter's FirePresenter member, ADR 0043 d9)
# on the typed surfaces (ADR 0034): the drained rows are pure data fed through
# the public present_* data legs (the present_snapshot precedent — production
# present_passes() drains the typed Simulation and forwards the same rows),
# and the audio sink is a REAL MissionAudio over a staged root whose mission
# bank authors every set the rows name (ADR 0043 rule 11: no production
# subclass). Its recent-fires ring, mixer channel census, and the spawned
# AudioStreamPlayer3D voices under the container are the read seams.

# Every set the drained rows below name, authored into the staged mission bank
# (one layer each, playing the fixture tone).
const SOUND_SETS: PackedStringArray = [
	"AI_FIRE", "GS_END", "FSP_DIRT_L", "FREEFALL", "V_TRUCK_ILP"]

var _root_dir := ""
var _audio: MissionAudio = null


func after_each() -> void:
	if _audio != null:
		_audio.teardown()
		_audio = null
	if not _root_dir.is_empty():
		TestFs.remove_dir_recursive(_root_dir)
		_root_dir = ""


# A REAL MissionAudio set up over a staged root carrying a global bank
# (game.LWF authors SOUND_SETS), its audio root parented under
# `container`. No listener is stamped: a one-shot fired before the first tick
# skips the set-range cull, the way retail's play-before-first-frame does.
func _staged_audio(container: Node3D) -> MissionAudio:
	_root_dir = OS.get_cache_dir().path_join(
			"opennova_fire_present_pass_%d" % Time.get_ticks_usec())
	assert_eq(DirAccess.make_dir_recursive_absolute(_root_dir), OK)
	WorldFixture.stage_sound_bank(_root_dir, SOUND_SETS, "game.LWF")
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(_root_dir), OK, "the staged root mounts")
	var mission := MissionData.new()
	assert_eq(mission.create_default(), OK)
	_audio = MissionAudio.create(root, null)
	var stats := _audio.setup(mission, "fire.bms", container)
	assert_eq(int(stats.banks_loaded), 1, "the staged mission bank loads")
	return _audio


# The positional voices MissionAudio spawned under `container`.
func _voices(container: Node) -> Array[AudioStreamPlayer3D]:
	var out: Array[AudioStreamPlayer3D] = []
	for value in container.find_children("*", "AudioStreamPlayer3D", true, false):
		out.append(value as AudioStreamPlayer3D)
	return out


func _event(pos: Vector3, source_bms_id: int, is_local_player: bool = false) -> FirePresentationEvent:
	return FirePresentationEvent.make(pos, source_bms_id, -1, is_local_player)


func _fire_sound(soundset: String, pos: Vector3, source_bms_id: int) -> FireSoundRow:
	return FireSoundRow.make(soundset, pos, source_bms_id)


func _slot_sound(soundset: String, pos: Vector3, handle: int, slot: int) -> SlotSoundRow:
	return SlotSoundRow.make(soundset, pos, handle, slot)


# A REAL EntityPresenter with its fire pass wired over `audio` (the sound
# legs) and, when given, `container` (the tracer geometry's host); no effect
# world, no sim — the data legs are the drive.
func _make_presenter(audio: MissionAudio, container: Node3D = null) -> EntityPresenter:
	var presenter := EntityPresenter.new()
	add_child_autofree(presenter)
	presenter.setup_passes(container, null, null, audio, null, null, null, null)
	return presenter


func test_drained_fire_sounds_play_with_source_identity() -> void:
	# The distance gate and delay countdown live in the sim (world/fire_sound.h,
	# pinned by the fire_sound ctest); the pass plays each drained row verbatim.
	var container := Node3D.new()
	add_child_autofree(container)
	var audio := _staged_audio(container)
	var presenter := _make_presenter(audio)

	presenter.present_fire_sounds([
		_fire_sound("AI_FIRE", Vector3(10, 0, 0), 77),
		_fire_sound("GS_END", Vector3(4, 1, 2), 0),
	])

	var fired := audio.recent_fired_soundsets()
	assert_eq(fired.size(), 2)
	if fired.size() == 2:
		assert_eq(fired[0].set_name, "AI_FIRE")
		assert_eq(fired[0].source_bms_id, 77,
				"the shooter's BMS identity reaches the bank's occlusion leg")
		assert_false(fired[0].slot, "drained fire rows ride the fire_soundset leg")
		assert_true(fired[0].played, "the staged bank resolves the set and spawns its voice")
		assert_eq(fired[1].set_name, "GS_END")
		assert_eq(fired[1].position, Vector3(4, 1, 2))
		assert_eq(fired[1].source_bms_id, 0)
		assert_true(fired[1].played)
	assert_eq(_voices(container).size(), 2, "each drained row spawned one positional voice")
	assert_eq(presenter.get_fire_present_stats().sounds, 2)
	presenter.teardown()


func test_no_audio_skips_the_sound_legs_but_not_the_effect_legs() -> void:
	# The dedicated-host tri-state: a presenter with no MissionAudio plays no
	# fire or slot sound (nothing to play them on) while the fire drain's
	# effect legs still run and count.
	var presenter := _make_presenter(null)

	presenter.present_fire_sounds([_fire_sound("AI_FIRE", Vector3(10, 0, 0), 77)])
	presenter.present_slot_sounds([_slot_sound("FSP_DIRT_L", Vector3(1, 0, 0), 3, 17)])
	presenter.present_fires([_event(Vector3(2, 0, 0), 22)])

	var stats := presenter.get_fire_present_stats()
	assert_eq(stats.sounds, 0, "no audio: the sound legs are skipped")
	assert_eq(stats.fires, 1, "no audio: the effect legs still present the fire")
	presenter.teardown()


func test_joiner_style_drain_presents_remote_and_discards_local_prediction() -> void:
	# A joiner feeds both its local predicted round and decoded remote tag-2
	# rounds into one visual RoundSim queue. The pass must present only the
	# remote record; the local action-slot leg already presented the prediction.
	# (The sim's own seed applies the same local filter to the sound queue —
	# the fire_sound ctest pins that half; the drain-consumption itself is the
	# native queue's contract, exercised by present() over the typed sim.)
	var audio := MissionAudio.create(null, null)
	autofree(audio)
	var presenter := _make_presenter(audio)
	var local := _event(Vector3(1, 0, 0), 11, true)
	var remote := _event(Vector3(2, 0, 0), 22)

	for _frame in range(128):
		presenter.present_fires([local, remote])

	assert_eq(presenter.get_fire_present_stats().fires, 128,
			"only the decoded remote shot reaches the presentation legs")
	assert_true(audio.recent_fired_soundsets().is_empty(),
			"the effect drain plays no sound of its own")
	presenter.teardown()


# A current render camera above the points: the ribbons face it.
func _ribbon_camera(container: Node3D) -> Camera3D:
	var camera := Camera3D.new()
	container.add_child(camera)
	camera.position = Vector3(0, 5, 10)
	camera.current = true
	return camera


func _surface_material(mesh: ArrayMesh, surface: int) -> ShaderMaterial:
	return mesh.surface_get_material(surface) as ShaderMaterial


func test_tracer_trails_build_the_stock_ribbon() -> void:
	# A live stdred channel (style 1, 4 points along +X) draws ONE surface on the
	# stock material: pairs at points 0..count-2 (the newest point steers
	# direction only), the strip as a triangle list, fogged to black, on the
	# camera-side tracer rung while the eye is above the water
	# [orig: CEffectChannel_RenderRibbon @ 0x5DB8A0; CEffectEmitterPool_RenderMainPass
	# @ 0x5DCAF0 called @ 0x5C9687].
	var audio := MissionAudio.create(null, null)
	autofree(audio)
	var container := Node3D.new()
	add_child_autofree(container)
	_ribbon_camera(container)
	var presenter := _make_presenter(audio, container)

	presenter.draw_tracer_rows(PackedFloat32Array([
		1.0, 1.0, 4.0,  # style stdred, age 1, count 4
		0.0, 1.0, 0.0, 1.0,
		2.0, 1.0, 0.0, 1.0,
		4.0, 1.0, 0.0, 1.0,
		6.0, 1.0, 0.0, 1.0,
	]))

	var mesh: ArrayMesh = presenter.fire_ribbon_mesh()
	assert_not_null(mesh, "a container hosts the ribbon geometry")
	if mesh == null:
		return
	assert_eq(mesh.get_surface_count(), 1, "one stock surface")
	if mesh.get_surface_count() == 1:
		var arrays := mesh.surface_get_arrays(0)
		var verts: PackedVector3Array = arrays[Mesh.ARRAY_VERTEX]
		assert_eq(verts.size(), 6, "3 drawn pairs (points 0..2), 2 verts each")
		var indices: PackedInt32Array = arrays[Mesh.ARRAY_INDEX]
		assert_eq(indices.size(), 12, "the 6-vertex strip as four triangles")
		var cols: PackedColorArray = arrays[Mesh.ARRAY_COLOR]
		assert_almost_eq(cols[0].r, 0.0, 0.01, "oldest pair rides the base color (black)")
		assert_almost_eq(cols[4].r, 0xC0 / 255.0, 0.01, "ramp index (4 - 2) + 1 - 1 = 2")
		var material := _surface_material(mesh, 0)
		assert_not_null(material)
		if material != null:
			assert_eq(material.shader.resource_path, "res://shaders/tracer_ribbon_stock.gdshader")
			assert_true(material.get_shader_parameter("fog_black"),
					"the stock styles fog to black [orig: SetFogAndBlendMode mode 2]")
			assert_eq(material.render_priority, ObjectShaderCache.RENDER_RUNG_TRACER_CAMERA_SIDE)
	assert_eq(presenter.get_fire_present_stats().tracer_peak, 1)
	presenter.teardown()


func test_tracer_smoke_style_builds_the_textured_cross_section() -> void:
	# A rocket channel (style 3) draws the four-vertex cross-section on the smoke
	# material: two texture coordinate sets, scene fog, the inner pair on the
	# ramp and the outer pair on the base colour
	# [orig: CEffectChannel_RenderRibbon @ 0x5DC4F6..0x5DC796].
	var audio := MissionAudio.create(null, null)
	autofree(audio)
	var container := Node3D.new()
	add_child_autofree(container)
	_ribbon_camera(container)
	var presenter := _make_presenter(audio, container)

	presenter.draw_tracer_rows(PackedFloat32Array([
		3.0, 1.0, 3.0,
		0.0, 1.0, 0.0, 1.0,
		2.0, 1.0, 0.0, 1.02,
		4.0, 1.0, 0.0, 0.98,
	]))

	var mesh: ArrayMesh = presenter.fire_ribbon_mesh()
	assert_not_null(mesh)
	if mesh == null:
		return
	assert_eq(mesh.get_surface_count(), 1, "one smoke surface")
	if mesh.get_surface_count() == 1:
		var arrays := mesh.surface_get_arrays(0)
		var verts: PackedVector3Array = arrays[Mesh.ARRAY_VERTEX]
		assert_eq(verts.size(), 8, "two four-vertex cross-sections")
		var indices: PackedInt32Array = arrays[Mesh.ARRAY_INDEX]
		assert_eq(indices.size(), 18, "one six-triangle bridge")
		var uv2: PackedVector2Array = arrays[Mesh.ARRAY_TEX_UV2]
		assert_eq(uv2.size(), 8, "the second texture layer's coordinates")
		var cols: PackedColorArray = arrays[Mesh.ARRAY_COLOR]
		assert_almost_eq(cols[5].r, 0.75, 0.01, "0xC0 gray ramp on the inner pair")
		assert_almost_eq(cols[5].a, 246 / 255.0, 0.01, "ramp index (3 - 1) + 1 - 1 = 2")
		assert_almost_eq(cols[4].a, 0.0, 0.01, "the outer pair rides the base colour")
		var material := _surface_material(mesh, 0)
		assert_not_null(material)
		if material != null:
			assert_eq(material.shader.resource_path, "res://shaders/tracer_ribbon_smoke.gdshader")
			assert_false(material.get_shader_parameter("fog_black"),
					"smoke fogs to the scene colour [orig: SetFogAndBlendMode mode 0]")
	presenter.teardown()


func test_tracer_rung_follows_the_eye_water_side() -> void:
	# Below the water the pool draws in its far-side call, before the water
	# surface [orig: CEffectEmitterPool_RenderMainPass(0, eyeBelow) @ 0x5C95AC].
	var audio := MissionAudio.create(null, null)
	autofree(audio)
	var container := Node3D.new()
	add_child_autofree(container)
	_ribbon_camera(container)
	var environment := MissionEnvironment.new()
	add_child_autofree(environment)
	environment.set_underwater_view(true)
	var presenter := EntityPresenter.new()
	add_child_autofree(presenter)
	presenter.setup_passes(container, null, null, audio, null, null, environment, null)

	presenter.draw_tracer_rows(PackedFloat32Array([
		1.0, 0.0, 3.0,
		0.0, 1.0, 0.0, 1.0,
		2.0, 1.0, 0.0, 1.0,
		4.0, 1.0, 0.0, 1.0,
	]))

	var mesh: ArrayMesh = presenter.fire_ribbon_mesh()
	assert_eq(mesh.get_surface_count(), 1)
	if mesh.get_surface_count() == 1:
		assert_eq(_surface_material(mesh, 0).render_priority,
				ObjectShaderCache.RENDER_RUNG_TRACER_FAR_SIDE)
	presenter.teardown()


const MAIN_VIEW_NO_REFLECTION := 1 << 27
const INSET_VIEW := 1 << 21
const MIRROR_VIEW := 1 << 24
const INSET_MIRROR_VIEW := 1 << 25


# A water mirror's rig: a camera in its own target, posed at `xform`; the
# mirror renders while its target's update mode is not DISABLED. Its frame is
# the scene view's (the main mirror's node-side frame is its source's,
# env/water.h), so its projection is the scene camera's.
func _mirror_camera(xform: Transform3D, update_mode: int) -> Camera3D:
	var target := SubViewport.new()
	var frame := get_viewport().get_visible_rect().size
	target.size = Vector2i(maxi(int(frame.x), 1), maxi(int(frame.y), 1))
	target.render_target_update_mode = update_mode
	add_child_autofree(target)
	var camera := Camera3D.new()
	target.add_child(camera)
	camera.global_transform = xform
	return camera


# The up-preserved proper mirror of `xform` about the water plane at `height`
# (engine environment/water_mirror.h build_water_mirror_view, at or above it):
# each basis column reflected, the reflected up negated, the eye reflected.
func _proper_mirror(xform: Transform3D, height: float) -> Transform3D:
	var b := xform.basis
	var mirrored := Basis(Vector3(b.x.x, -b.x.y, b.x.z), Vector3(-b.y.x, b.y.y, -b.y.z),
			Vector3(b.z.x, -b.z.y, b.z.z))
	var o := xform.origin
	return Transform3D(mirrored, Vector3(o.x, 2.0 * height - o.y, o.z))


func _vertices(mesh: ArrayMesh) -> PackedVector3Array:
	return mesh.surface_get_arrays(0)[Mesh.ARRAY_VERTEX]


# The first drawn pair's side direction: the strip writes P + rs, then P - rs.
func _first_side(mesh: ArrayMesh) -> Vector3:
	var verts := _vertices(mesh)
	return (verts[0] - verts[1]).normalized()


# Each water mirror's reflected subscene runs the tracer pool's main pass under
# the mirror's own view: the main scene rebuilds the viewport block from the
# mirrored camera right ahead of the subscene, and the ribbon build reads that
# block's eye and view direction. So a mirror's ribbons face the mirror's eye
# (the mirrored one above the water, the live one below it), while the scene
# views keep theirs, byte for byte. A segment along a view's x axis takes its
# side along that view's up: the pitched scene camera's and its mirror's differ.
# [orig: CEffectChannel_RenderRibbon @0x5DB8A0; Render_MainScene @0x5C1240 -
# the Viewport_BuildProjectionMatrix call @0x5C170B; Water_RenderReflectedWorldScene
# @0x5C8510 - the CEffectEmitterPool_RenderMainPass call @0x5C85F6]
func test_a_mirror_builds_its_ribbons_against_its_own_eye() -> void:
	var audio := MissionAudio.create(null, null)
	autofree(audio)
	var container := Node3D.new()
	add_child_autofree(container)
	var camera := _ribbon_camera(container)
	camera.look_at(Vector3.ZERO, Vector3.UP)
	var presenter := _make_presenter(audio, container)
	var rows := PackedFloat32Array([
		1.0, 1.0, 4.0,  # stdred, age 1, count 4, along +X
		0.0, 1.0, 0.0, 1.0,
		2.0, 1.0, 0.0, 1.0,
		4.0, 1.0, 0.0, 1.0,
		6.0, 1.0, 0.0, 1.0,
	])

	# The main view's ribbons and each mirror's ride their own bits.
	assert_eq((container.get_node("FireTracers") as MeshInstance3D).layers,
			MAIN_VIEW_NO_REFLECTION, "the main view's ribbons: no other view admits the bit")
	assert_eq((container.get_node("FireTracersMirror") as MeshInstance3D).layers,
			MIRROR_VIEW, "the main scene's mirror draws its own")
	assert_eq((container.get_node("FireTracersInsetMirror") as MeshInstance3D).layers,
			INSET_MIRROR_VIEW, "the weapon Inset's mirror draws its own")

	presenter.draw_tracer_rows(rows)
	var mesh: ArrayMesh = presenter.fire_ribbon_mesh()
	assert_eq(mesh.get_surface_count(), 1)
	if mesh.get_surface_count() != 1:
		presenter.teardown()
		return
	var main_before := _vertices(mesh)
	assert_eq(presenter.fire_mirror_ribbon_mesh(0).get_surface_count(), 0,
			"no mirror camera: no mirror ribbons")

	var mirror := _mirror_camera(_proper_mirror(camera.global_transform, 0.0),
			SubViewport.UPDATE_ALWAYS)
	var inset_mirror := _mirror_camera(_proper_mirror(camera.global_transform, 0.0),
			SubViewport.UPDATE_DISABLED)
	presenter.set_tracer_mirror_cameras(mirror, inset_mirror)
	presenter.draw_tracer_rows(rows)

	assert_eq(_vertices(mesh), main_before, "the scene views' ribbons are unchanged")
	var main_side := _first_side(mesh)
	assert_gt(absf(main_side.dot(camera.global_transform.basis.y)), 0.999,
			"the scene ribbon's side is the scene camera's up")
	var mirror_mesh: ArrayMesh = presenter.fire_mirror_ribbon_mesh(0)
	assert_eq(mirror_mesh.get_surface_count(), 1, "the rendering mirror builds its ribbons")
	if mirror_mesh.get_surface_count() == 1:
		var mirror_side := _first_side(mirror_mesh)
		assert_gt(absf(mirror_side.dot(mirror.global_transform.basis.y)), 0.999,
				"the mirror ribbon's side is the mirror camera's up")
		assert_lt(absf(mirror_side.dot(main_side)), 0.9,
				"so it faces the mirrored eye, not the scene's")
		var material := _surface_material(mirror_mesh, 0)
		assert_not_null(material)
		if material != null:
			assert_eq(material.render_priority, ObjectShaderCache.RENDER_RUNG_TRACER_CAMERA_SIDE,
					"the shared materials keep the frame's rung")
	assert_eq(presenter.fire_mirror_ribbon_mesh(1).get_surface_count(), 0,
			"a mirror that does not render builds nothing")

	# Below the water the mirror draws from the live eye: the same ribbons.
	mirror.global_transform = camera.global_transform
	inset_mirror.get_viewport().render_target_update_mode = SubViewport.UPDATE_ALWAYS
	inset_mirror.global_transform = camera.global_transform
	presenter.draw_tracer_rows(rows)
	assert_eq(_vertices(mesh), main_before)
	if presenter.fire_mirror_ribbon_mesh(0).get_surface_count() == 1:
		assert_eq(_vertices(presenter.fire_mirror_ribbon_mesh(0)), main_before,
				"below the water the mirror's ribbons are the live eye's")
	assert_eq(presenter.fire_mirror_ribbon_mesh(1).get_surface_count(), 1,
			"the Inset's mirror renders: it builds its own")
	presenter.teardown()


# The weapon Inset's scene runs the world scene, the tracer pool's passes among it,
# under the Inset's own block and the projection of its own field, so its ribbons face
# the Inset's eye and take the minimum width its field gives: far off, a zoomed Inset's
# ribbons are as much narrower as its field's cotangent is larger, while the main
# view's stay its own. The Inset's camera is the effect world's second scene camera.
# [orig: Render_WeaponInsetScene @0x5C9740 - the calls @0x5C99CB, @0x5C9DAE, @0x5C9DE9;
# CEffectChannel_RenderRibbon @0x5DB8A0 - the minimum width @0x5DC0D6..0x5DC0F7]
func test_the_inset_scene_builds_its_ribbons_against_its_own_view() -> void:
	var audio := MissionAudio.create(null, null)
	autofree(audio)
	var container := Node3D.new()
	add_child_autofree(container)
	var camera := _ribbon_camera(container)
	camera.look_at(Vector3(0, 1, -300), Vector3.UP)
	var effects := EffectWorld.new()
	add_child_autofree(effects)
	var presenter := EntityPresenter.new()
	add_child_autofree(presenter)
	presenter.setup_passes(container, null, null, audio, effects, null, null, null)
	# A far stdred trail along the views' x axis: the minimum width rules there.
	var rows := PackedFloat32Array([
		1.0, 1.0, 4.0,
		0.0, 1.0, -300.0, 1.0,
		2.0, 1.0, -300.0, 1.0,
		4.0, 1.0, -300.0, 1.0,
		6.0, 1.0, -300.0, 1.0,
	])
	assert_eq((container.get_node("FireTracersInset") as MeshInstance3D).layers, INSET_VIEW,
			"the Inset's ribbons ride the bit only its camera admits")

	presenter.draw_tracer_rows(rows)
	var mesh: ArrayMesh = presenter.fire_ribbon_mesh()
	assert_eq(mesh.get_surface_count(), 1)
	if mesh.get_surface_count() != 1:
		presenter.teardown()
		return
	var main_before := _vertices(mesh)
	assert_eq(presenter.fire_inset_ribbon_mesh().get_surface_count(), 0,
			"no Inset pass: no Inset ribbons")

	# The Inset pass: a zoomed view from the same eye.
	var inset := _mirror_camera(camera.global_transform, SubViewport.UPDATE_ALWAYS)
	inset.fov = 10.0
	effects.set_second_scene_camera(inset)
	presenter.draw_tracer_rows(rows)
	assert_eq(_vertices(mesh), main_before, "the main view's ribbons are unchanged")
	var inset_mesh: ArrayMesh = presenter.fire_inset_ribbon_mesh()
	assert_eq(inset_mesh.get_surface_count(), 1, "the rendering Inset builds its ribbons")
	if inset_mesh.get_surface_count() == 1:
		var main_verts := _vertices(mesh)
		var inset_verts := _vertices(inset_mesh)
		var main_width := (main_verts[0] - main_verts[1]).length()
		var inset_width := (inset_verts[0] - inset_verts[1]).length()
		var main_scale := camera.get_camera_projection().x.x
		var inset_scale := inset.get_camera_projection().x.x
		assert_gt(inset_scale, main_scale * 2.0, "the zoomed field's cotangent is larger")
		assert_almost_eq(inset_width * inset_scale, main_width * main_scale,
				main_width * main_scale * 1e-3, "each ribbon takes its own field's minimum width")
		assert_gt(absf(_first_side(inset_mesh).dot(inset.global_transform.basis.y)), 0.999,
				"and faces the Inset's eye")

	effects.set_second_scene_camera(null)
	presenter.draw_tracer_rows(rows)
	assert_eq(presenter.fire_inset_ribbon_mesh().get_surface_count(), 0,
			"the Inset pass closed: no Inset ribbons")
	presenter.teardown()


# The tracer pool's distortion ribbons (the styles with a +0x828 word: the
# rocket, the AT4, the sniper) publish into the effect world's FrameFX drawer
# and draw in the type-0 row with slot 2 = 256B (retail
# CEffectEmitterPool_RenderDistortionPass, called from FrameFX_DistortionPass
# @ 0x583928); a live channel of such a style is what opens the row (retail
# CEffectEmitterPool_HasDistortionChannels @ 0x5DB7F0). The stock styles have
# no distortion ribbon.
func test_distortion_style_tracers_draw_in_the_framefx_row() -> void:
	var viewport := SubViewport.new()
	viewport.size = Vector2i(128, 128)
	viewport.own_world_3d = true
	viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	add_child_autofree(viewport)
	var container := Node3D.new()
	viewport.add_child(container)
	_ribbon_camera(container)
	# FrameFx installs its terminal effect on the viewport's WorldEnvironment.
	var background := WorldEnvironment.new()
	background.environment = Environment.new()
	background.environment.background_mode = Environment.BG_COLOR
	background.environment.background_color = Color.BLACK
	viewport.add_child(background)
	var frame_fx := FrameFx.new()
	viewport.add_child(frame_fx)
	var effects := EffectWorld.new()
	viewport.add_child(effects)
	effects.attach_distortion_row(frame_fx)
	var audio := MissionAudio.create(null, null)
	autofree(audio)
	# The ribbons face the camera of the presenter's own viewport.
	var presenter := EntityPresenter.new()
	viewport.add_child(presenter)
	presenter.setup_passes(container, null, null, audio, effects, null, null, null)

	presenter.draw_tracer_rows(PackedFloat32Array([
		1.0, 1.0, 3.0,  # stdred
		0.0, 1.0, 0.0, 1.0,
		2.0, 1.0, 0.0, 1.0,
		4.0, 1.0, 0.0, 1.0,
	]))
	var report := effects.get_debug_draw_list_report()
	assert_false(bool(report.get("distortion_present", true)),
			"a stock channel does not open the distortion row")
	assert_eq(int(report.get("distortion_ribbon_indices", -1)), 0)

	presenter.draw_tracer_rows(PackedFloat32Array([
		3.0, 1.0, 3.0,  # rocket
		0.0, 1.0, 0.0, 1.0,
		2.0, 1.0, 0.0, 1.02,
		4.0, 1.0, 0.0, 0.98,
	]))
	report = effects.get_debug_draw_list_report()
	assert_true(bool(report.get("distortion_present", false)),
			"a live rocket channel opens the distortion row")
	assert_eq(int(report.get("distortion_ribbon_indices", -1)), 18,
			"one bridge between two four-vertex distortion cross-sections")

	frame_fx.set_view_effects(0, 0, false, false, 0, false, false, false, false, false, false, false)
	frame_fx.advance_screen_effects()
	for _frame in 4:
		await get_tree().process_frame
	RenderingServer.force_draw(true)
	RenderingServer.force_sync()
	var fx_report := frame_fx.get_backend_report()
	if not bool(fx_report.get("rd_available", false)):
		pending("RenderingDevice unavailable under this Godot renderer")
		presenter.teardown()
		return
	assert_eq(int(fx_report.get("distortion_sets", -1)), 2)
	assert_eq(int(effects.get_debug_draw_list_report().get("distortion_ribbon_draws", -1)), 1,
			"the row drew the ribbon set")
	presenter.teardown()


func test_tracer_rows_without_a_render_camera_draw_nothing() -> void:
	var audio := MissionAudio.create(null, null)
	autofree(audio)
	var container := Node3D.new()
	add_child_autofree(container)
	var presenter := _make_presenter(audio, container)

	presenter.draw_tracer_rows(PackedFloat32Array([
		1.0, 0.0, 3.0,
		0.0, 1.0, 0.0, 1.0,
		2.0, 1.0, 0.0, 1.0,
		4.0, 1.0, 0.0, 1.0,
	]))

	assert_eq(presenter.fire_ribbon_mesh().get_surface_count(), 0,
			"the ribbons face the render camera; none, nothing to face")
	presenter.teardown()


func test_slot_sounds_play_immediately() -> void:
	# Body slot sounds (footsteps/foley/landing/screams) have NO propagation-delay
	# leg — they play the tick they drain [orig: Entity_PlaySound3D_FullVolume
	# @ 0x528e20 direct]. Every fire goes through the finite channel pool.
	var container := Node3D.new()
	add_child_autofree(container)
	var audio := _staged_audio(container)
	var presenter := _make_presenter(audio)

	presenter.present_slot_sounds([
		_slot_sound("FSP_DIRT_L", Vector3(400, 0, 0), 3, 17),
		_slot_sound("FREEFALL", Vector3(1, 0, 0), 3, 44),
		_slot_sound("", Vector3.ZERO, 3, 18),
	])

	var fired := audio.recent_fired_soundsets()
	assert_eq(fired.size(), 2, "empty set name is the id-0 no-op")
	if fired.size() == 2:
		assert_eq(fired[0].set_name, "FSP_DIRT_L")
		assert_true(fired[0].slot, "body slot rows ride the slot_soundset leg")
		assert_true(fired[0].played, "the footstep plays the tick it drains")
		assert_eq(fired[0].position, Vector3(400, 0, 0))
		assert_eq(fired[1].set_name, "FREEFALL")
		assert_true(fired[1].played)
	assert_eq(_voices(container).size(), 2, "each named slot row spawned one positional voice")
	assert_eq(presenter.get_fire_present_stats().sounds, 2,
			"the pass counts every slot row the bank played")
	presenter.teardown()


func test_persistent_sound_emitters_drain_into_the_shared_audio_layer() -> void:
	var container := Node3D.new()
	add_child_autofree(container)
	var audio := _staged_audio(container)
	var presenter := _make_presenter(audio)
	var idle := SoundEmitterRow.make(77, 0x10001, 42, Vector3(10, 0, 0), 0, 0, 30, 12,
			0x10000, 0xFFFF, false, "V_TRUCK_ILP")

	presenter.present_sound_emitters([idle])

	# The registration is a keep-alive intent for the shared loudest-eight
	# emitter table, not a one-shot: it lands at the next audio pass and
	# occupies one physical channel at the row's emitter position.
	assert_true(audio.recent_fired_soundsets().is_empty(),
			"emitter registrations never enter the one-shot ring")
	assert_true(audio.active_ambient_candidate_ids().is_empty(),
			"the intent queues until the audio pass advances the mixer")
	audio.tick(idle.pos, 0.2)
	var ids := audio.active_ambient_candidate_ids()
	assert_eq(ids.size(), 1, "the idle lane registers one voice in the shared emitter mix")
	assert_eq(int(audio.get_perf_counters().active_channels), 1)
	if ids.size() == 1:
		var voice := audio.ambient_player_for_candidate(ids[0])
		assert_not_null(voice, "the registered lane holds a physical channel")
		if voice != null:
			assert_true(voice.position.is_equal_approx(idle.pos),
					"the voice rides the row's emitter position")
			assert_true(voice.playing)
			assert_eq((voice.stream as AudioStreamWAV).loop_mode,
					AudioStreamWAV.LOOP_FORWARD, "the engine emitter is a persistent loop")
	presenter.teardown()
