extends SceneTree

# Renderer-facing regression loop for the reported foliage failure:
# near/detail foliage must not render black, and a fixed camera/fixed wind must
# produce identical pixels across consecutive dispatcher submissions.
#
# This is deliberately a probe rather than a GUT test: the assertions consume
# real rasterized viewport bytes and therefore require a non-headless renderer.
# The fixture is asset-free but exercises NovaFoliageDispatcher, its retail
# one-frame detail cache, generated ArrayMeshes, draw pools, ShaderMaterials,
# and the production foliage detail and MODEL shaders.
#
# Run:
#   Godot_v4.6.1-stable_win64_console.exe --path godot \
#     --rendering-method gl_compatibility \
#     -s res://tests/foliage_black_flicker_regression_probe.gd -- <out_dir>

const VIEWPORT_SIZE := Vector2i(320, 240)
const BACKGROUND := Color(0.07, 0.09, 0.13, 1.0)
const CAPTURE_COUNT := 6
const FOREGROUND_DELTA := 6
const FRAME_DELTA := 1
const MIN_FOREGROUND_PIXELS := 300
const MIN_MEAN_LUMA := 32.0
const MAX_EXACT_BLACK_COMPONENT := 64
const MODEL_CONTRACT_DELTA := 2
const MIN_MODEL_CONTRACT_PIXELS := 1000
const MODEL_ALPHA_REF := 128.0 / 255.0

var _foliage_index := 1


func _initialize() -> void:
	call_deferred("_run")


func _run() -> void:
	var args := OS.get_cmdline_user_args()
	var out_dir := args[0] if not args.is_empty() else "C:/tmp/opennova-foliage-black-flicker"
	DirAccess.make_dir_recursive_absolute(out_dir)

	# Pin every animated/environment input. Any remaining pixel change is draw
	# pool/material/cache instability, not expected wind or time-of-day motion.
	RenderingServer.global_shader_parameter_set(&"opennova_sky_ambient", Vector3(0.35, 0.35, 0.35))
	RenderingServer.global_shader_parameter_set(&"opennova_sun_light", Vector3(0.65, 0.60, 0.55))
	RenderingServer.global_shader_parameter_set(&"opennova_sun_direction", Vector3(0.0, 0.0, 1.0))
	RenderingServer.global_shader_parameter_set(&"opennova_fog_color", Vector3(BACKGROUND.r, BACKGROUND.g, BACKGROUND.b))
	RenderingServer.global_shader_parameter_set(&"opennova_fog_start", 1000.0)
	RenderingServer.global_shader_parameter_set(&"opennova_fog_end", 2000.0)
	RenderingServer.global_shader_parameter_set(&"opennova_fog_type", 1)

	var viewport := SubViewport.new()
	viewport.name = "FoliageRegressionViewport"
	viewport.size = VIEWPORT_SIZE
	viewport.own_world_3d = true
	viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	viewport.render_target_clear_mode = SubViewport.CLEAR_MODE_ALWAYS
	viewport.msaa_3d = Viewport.MSAA_DISABLED
	root.add_child(viewport)

	var world_environment := WorldEnvironment.new()
	var environment := Environment.new()
	environment.background_mode = Environment.BG_COLOR
	environment.background_color = BACKGROUND
	environment.ambient_light_source = Environment.AMBIENT_SOURCE_DISABLED
	world_environment.environment = environment
	viewport.add_child(world_environment)

	var camera := Camera3D.new()
	camera.fov = 58.0
	camera.near = 0.1
	camera.far = 160.0
	viewport.add_child(camera)
	camera.global_position = Vector3(8.0, 9.0, 28.0)
	camera.look_at(Vector3(8.0, 2.0, 7.0), Vector3.UP)
	camera.make_current()

	# Capture the exact empty-scene pixels; foliage is measured against this
	# frame rather than assuming a renderer/color-space background byte.
	await _wait_frames(4)
	var background := viewport.get_texture().get_image()
	if background == null or background.is_empty():
		_fail("viewport did not produce a background image")
		return
	background.save_png(out_dir.path_join("background.png"))

	# MODEL is not an opaque black silhouette in retail. Its fixed-function
	# stage emits black into ONE/ONE blending, preserving the destination color,
	# while the alpha-tested fragments still write depth. Exercise all three
	# parts on the production shader before the dispatcher fixture.
	var contract_failures: Array[String] = []
	var model_contract: Dictionary = await _probe_model_blend_depth_contract(out_dir)
	if not bool(model_contract.get("ok", false)):
		contract_failures.append(String(
			model_contract.get("failure", "MODEL blend/depth contract failed")))

	# D3DCMP_GREATER rejects equality. Exercise the high-detail production
	# shader with a solid RGBA8 alpha byte exactly equal to ALPHAREF 180.
	var detail_alpha_contract: Dictionary = await _probe_detail_alpha_equality_contract(out_dir)
	if not bool(detail_alpha_contract.get("ok", false)):
		contract_failures.append(String(detail_alpha_contract.get(
			"failure", "detail alpha equality contract failed")))
	if not contract_failures.is_empty():
		_fail("; ".join(contract_failures))
		return

	var dispatcher := NovaFoliageDispatcher.new()
	dispatcher.name = "FoliageRegressionDispatcher"
	viewport.add_child(dispatcher)

	var definition := NovaTerrainFoliageDef.new()
	definition.graphic = "procedural_regression_cross"
	definition.match = _foliage_index

	var colormap_data := NovaTerrainData.new()
	colormap_data.colormap = _solid_texture(Color(0.72, 0.90, 0.66, 1.0), 16)
	dispatcher.colormap_source = colormap_data
	dispatcher.height_sampler = Callable(self, "_sample_height")
	dispatcher.foliage_sampler = Callable(self, "_sample_foliage")
	dispatcher.configure_slots(
		[definition],
		[_make_cross_mesh()],
		[_solid_texture(Color(0.92, 0.48, 0.12, 1.0), 8)])

	# Retail fills detail cache misses after the current draw. First call fills;
	# second call creates visible draw meshes. Shader compilation gets four more
	# frames before bytes become part of the verdict.
	dispatcher.render_preview(camera.global_transform)
	dispatcher.render_preview(camera.global_transform)
	_pin_wind(dispatcher)
	await _wait_frames(4)

	var images: Array[Image] = []
	var stats_series: Array[Dictionary] = []
	for frame_index in range(CAPTURE_COUNT):
		dispatcher.render_preview(camera.global_transform)
		_pin_wind(dispatcher)
		await _wait_frames(2)
		var image := viewport.get_texture().get_image()
		if image == null or image.is_empty():
			_fail("capture %d did not produce an image" % frame_index)
			return
		images.append(image)
		image.save_png(out_dir.path_join("detail_%02d.png" % frame_index))
		stats_series.append(dispatcher.get_frame_stats())

	var foreground := _foreground_stats(background, images[0])
	var frame_diffs: Array[Dictionary] = []
	var worst_changed := 0
	var worst_delta := 0
	for index in range(1, images.size()):
		var diff := _image_diff(images[0], images[index])
		frame_diffs.append(diff)
		worst_changed = maxi(worst_changed, int(diff.changed_pixels))
		worst_delta = maxi(worst_delta, int(diff.max_delta))

	var first_stats: Dictionary = stats_series[0]
	var renderer_ok := int(foreground.pixels) >= MIN_FOREGROUND_PIXELS
	var nonblack_ok := renderer_ok and float(foreground.mean_luma) >= MIN_MEAN_LUMA
	var stable_ok := worst_changed == 0
	var batches_ok := int(first_stats.get("detail_high_instances", 0)) > 0 \
		and int(first_stats.get("silhouette_instances", 0)) == 0

	print("FOLIAGE_REGRESSION detail foreground_pixels=%d mean_luma=%.3f max_luma=%.3f" % [
		int(foreground.pixels), float(foreground.mean_luma), float(foreground.max_luma)])
	print("FOLIAGE_REGRESSION detail frame_diffs=", frame_diffs,
		" worst_changed=", worst_changed, " worst_delta=", worst_delta)
	print("FOLIAGE_REGRESSION detail first_stats=", first_stats)
	print("FOLIAGE_REGRESSION detail verdict renderer=", renderer_ok,
		" nonblack=", nonblack_ok, " stable=", stable_ok,
		" detail_only=", batches_ok)

	if not renderer_ok:
		_fail("fixture rendered too few foliage pixels (%d < %d)" % [
			int(foreground.pixels), MIN_FOREGROUND_PIXELS])
		return
	if not batches_ok:
		_fail("fixture did not isolate visible near/detail foliage")
		return
	if not nonblack_ok:
		_fail("BLACK FOLIAGE: near/detail mean luma %.3f < %.3f" % [
			float(foreground.mean_luma), MIN_MEAN_LUMA])
		return
	if not stable_ok:
		_fail("FLICKER: fixed-input consecutive frames changed %d pixels (max delta %d)" % [
			worst_changed, worst_delta])
		return

	# Screenshot forensics identifies the reported black object as the distant
	# MODEL tier: a single exact-RGB(0,0,0) connected component. Switch the SAME
	# production dispatcher and model to a MODEL-only request at view
	# depth >38, capture it, and apply that exact symptom detector.
	var silhouette_anchor := Vector3(8.0, _sample_height(8.0, -12.0), -12.0)
	camera.look_at(silhouette_anchor + Vector3(0.0, 2.0, 0.0), Vector3.UP)
	dispatcher.silhouette_anchors = PackedVector3Array([silhouette_anchor])
	dispatcher.render_frame(camera.global_transform)
	_pin_wind(dispatcher)
	await _wait_frames(4)

	var silhouette_images: Array[Image] = []
	var silhouette_stats_series: Array[Dictionary] = []
	for frame_index in range(CAPTURE_COUNT):
		dispatcher.render_frame(camera.global_transform)
		_pin_wind(dispatcher)
		await _wait_frames(2)
		var image := viewport.get_texture().get_image()
		if image == null or image.is_empty():
			_fail("silhouette capture %d did not produce an image" % frame_index)
			return
		silhouette_images.append(image)
		image.save_png(out_dir.path_join("silhouette_%02d.png" % frame_index))
		silhouette_stats_series.append(dispatcher.get_frame_stats())

	var largest_black := _largest_exact_black_component(silhouette_images[0])
	var silhouette_diffs: Array[Dictionary] = []
	var silhouette_worst_changed := 0
	var silhouette_worst_delta := 0
	for index in range(1, silhouette_images.size()):
		var diff := _image_diff(silhouette_images[0], silhouette_images[index])
		silhouette_diffs.append(diff)
		silhouette_worst_changed = maxi(
			silhouette_worst_changed, int(diff.changed_pixels))
		silhouette_worst_delta = maxi(
			silhouette_worst_delta, int(diff.max_delta))

	var silhouette_stats: Dictionary = silhouette_stats_series[0]
	var silhouette_only_ok := int(silhouette_stats.get("silhouette_instances", 0)) > 0 \
		and int(silhouette_stats.get("detail_high_instances", 0)) == 0 \
		and int(silhouette_stats.get("detail_low_instances", 0)) == 0
	var no_black_blob_ok := largest_black <= MAX_EXACT_BLACK_COMPONENT
	var silhouette_stable_ok := silhouette_worst_changed == 0
	print("FOLIAGE_REGRESSION silhouette largest_exact_black_component=", largest_black,
		" allowed=", MAX_EXACT_BLACK_COMPONENT)
	print("FOLIAGE_REGRESSION silhouette frame_diffs=", silhouette_diffs,
		" worst_changed=", silhouette_worst_changed,
		" worst_delta=", silhouette_worst_delta)
	print("FOLIAGE_REGRESSION silhouette first_stats=", silhouette_stats)
	print("FOLIAGE_REGRESSION silhouette verdict no_black_blob=", no_black_blob_ok,
		" stable=", silhouette_stable_ok, " silhouette_only=", silhouette_only_ok)

	if not silhouette_only_ok:
		_fail("fixture did not isolate distant MODEL foliage")
		return
	if not no_black_blob_ok:
		_fail("BLACK FOLIAGE: largest exact-black connected component %d > %d" % [
			largest_black, MAX_EXACT_BLACK_COMPONENT])
		return
	if not silhouette_stable_ok:
		_fail("FLICKER: fixed-input silhouette frames changed %d pixels (max delta %d)" % [
			silhouette_worst_changed, silhouette_worst_delta])
		return

	print("FOLIAGE_REGRESSION PASS")
	quit(0)


func _probe_detail_alpha_equality_contract(out_dir: String) -> Dictionary:
	var viewport := SubViewport.new()
	viewport.name = "FoliageDetailAlphaContractViewport"
	viewport.size = VIEWPORT_SIZE
	viewport.own_world_3d = true
	viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	viewport.render_target_clear_mode = SubViewport.CLEAR_MODE_ALWAYS
	viewport.msaa_3d = Viewport.MSAA_DISABLED
	root.add_child(viewport)

	var world_environment := WorldEnvironment.new()
	var environment := Environment.new()
	environment.background_mode = Environment.BG_COLOR
	environment.background_color = BACKGROUND
	environment.ambient_light_source = Environment.AMBIENT_SOURCE_DISABLED
	world_environment.environment = environment
	viewport.add_child(world_environment)

	var camera := Camera3D.new()
	camera.fov = 58.0
	camera.near = 0.1
	camera.far = 40.0
	viewport.add_child(camera)
	camera.global_position = Vector3(0.0, 0.0, 10.0)
	camera.look_at(Vector3.ZERO, Vector3.UP)
	camera.make_current()

	await _wait_frames(4)
	var baseline := viewport.get_texture().get_image()
	if baseline == null or baseline.is_empty():
		viewport.queue_free()
		return {"ok": false, "failure": "detail alpha contract viewport produced no baseline"}
	baseline.save_png(out_dir.path_join("detail_alpha_baseline.png"))

	var foliage := MeshInstance3D.new()
	foliage.name = "ProductionDetailHighPass"
	foliage.mesh = _make_model_contract_card_mesh()
	var production_shader: Shader = load("res://shaders/foliage_detail_high.gdshader")
	if production_shader == null:
		viewport.queue_free()
		return {"ok": false, "failure": "could not load production high-detail shader"}
	var production_material := ShaderMaterial.new()
	production_material.shader = production_shader
	production_material.set_shader_parameter(
		&"u_fd_texture",
		_solid_texture(Color(0.92, 0.48, 0.12, 180.0 / 255.0), 16))
	production_material.set_shader_parameter(&"u_has_fd_texture", true)
	production_material.set_shader_parameter(&"u_has_colormap", false)
	production_material.set_shader_parameter(&"u_has_heightfield_normal", false)
	production_material.set_shader_parameter(&"u_has_tile_overlay", false)
	foliage.material_override = production_material
	viewport.add_child(foliage)
	foliage.set_instance_shader_parameter(&"u_fade", 1.0)
	foliage.set_instance_shader_parameter(&"u_wind_phase", 0.0)

	# One byte below the sampled alpha is a fixture-positive control.
	foliage.set_instance_shader_parameter(&"u_alpha_ref", 179.0 / 255.0)
	await _wait_frames(4)
	var accepted_image := viewport.get_texture().get_image()
	accepted_image.save_png(out_dir.path_join("detail_alpha_accepted_control.png"))
	var accepted := _foreground_stats(baseline, accepted_image)

	# Equality must be rejected: retail sets D3DCMP_GREATER for ref 180.
	foliage.set_instance_shader_parameter(&"u_alpha_ref", 180.0 / 255.0)
	await _wait_frames(4)
	var equality_image := viewport.get_texture().get_image()
	equality_image.save_png(out_dir.path_join("detail_alpha_equality.png"))
	var equality := _foreground_stats(baseline, equality_image)

	var fixture_ok := int(accepted.pixels) >= MIN_MODEL_CONTRACT_PIXELS
	var equality_rejected := int(equality.pixels) == 0
	print("FOLIAGE_REGRESSION detail_alpha accepted_control_pixels=",
		int(accepted.pixels), " equality_pixels=", int(equality.pixels))
	print("FOLIAGE_REGRESSION detail_alpha verdict fixture=", fixture_ok,
		" greater_rejects_equality=", equality_rejected)
	viewport.queue_free()

	if not fixture_ok:
		return {
			"ok": false,
			"failure": "detail alpha equality fixture rendered too few positive-control pixels",
		}
	if not equality_rejected:
		return {
			"ok": false,
			"failure": "detail D3DCMP_GREATER equality leaked %d pixels" % int(equality.pixels),
		}
	return {"ok": true}


func _probe_model_blend_depth_contract(out_dir: String) -> Dictionary:
	var viewport := SubViewport.new()
	viewport.name = "FoliageModelContractViewport"
	viewport.size = VIEWPORT_SIZE
	viewport.own_world_3d = true
	viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	viewport.render_target_clear_mode = SubViewport.CLEAR_MODE_ALWAYS
	viewport.msaa_3d = Viewport.MSAA_DISABLED
	root.add_child(viewport)

	var world_environment := WorldEnvironment.new()
	var environment := Environment.new()
	environment.background_mode = Environment.BG_COLOR
	environment.background_color = BACKGROUND
	environment.ambient_light_source = Environment.AMBIENT_SOURCE_DISABLED
	world_environment.environment = environment
	viewport.add_child(world_environment)

	var camera := Camera3D.new()
	camera.fov = 58.0
	camera.near = 0.1
	camera.far = 40.0
	viewport.add_child(camera)
	camera.global_position = Vector3(0.0, 0.0, 10.0)
	camera.look_at(Vector3.ZERO, Vector3.UP)
	camera.make_current()

	var card_mesh := _make_model_contract_card_mesh()
	var alpha_texture := _make_model_contract_alpha_texture()

	var foliage := MeshInstance3D.new()
	foliage.name = "ProductionModelPass"
	foliage.mesh = card_mesh
	var production_shader: Shader = load("res://shaders/foliage_silhouette.gdshader")
	if production_shader == null:
		viewport.queue_free()
		return {"ok": false, "failure": "could not load production MODEL shader"}
	var production_material := ShaderMaterial.new()
	production_material.shader = production_shader
	production_material.set_shader_parameter(&"u_fd_texture", alpha_texture)
	production_material.set_shader_parameter(&"u_has_fd_texture", true)
	foliage.material_override = production_material
	viewport.add_child(foliage)
	foliage.set_instance_shader_parameter(&"u_alpha_ref", MODEL_ALPHA_REF)
	foliage.set_instance_shader_parameter(&"u_wind_phase", 0.0)

	# This transparent additive card is farther from the camera than MODEL but
	# has the highest transparent render priority, so it is submitted later.
	# It is the visible witness for accepted-depth and rejected-alpha pixels.
	var late_layer := MeshInstance3D.new()
	late_layer.name = "LateBehindLayer"
	late_layer.mesh = card_mesh
	late_layer.position = Vector3(0.0, 0.0, -1.0)
	var late_material := ShaderMaterial.new()
	var late_shader := Shader.new()
	late_shader.code = """
shader_type spatial;
render_mode unshaded, cull_disabled, blend_add, depth_draw_never;
void fragment() {
	ALBEDO = vec3(0.72, 0.14, 0.04);
	ALPHA = 1.0;
}
"""
	late_material.shader = late_shader
	late_material.render_priority = 127
	late_layer.material_override = late_material
	viewport.add_child(late_layer)

	# Identical mesh, UVs, texture and alpha reference recover the exact
	# accepted-fragment mask without assuming projected screen coordinates.
	var oracle := MeshInstance3D.new()
	oracle.name = "AlphaAcceptedOracle"
	oracle.mesh = card_mesh
	var oracle_material := ShaderMaterial.new()
	var oracle_shader := Shader.new()
	oracle_shader.code = """
shader_type spatial;
render_mode unshaded, cull_disabled, depth_draw_opaque;
uniform sampler2D u_fd_texture : filter_linear_mipmap, repeat_enable;
uniform float u_alpha_ref = 0.5;
void fragment() {
	if (texture(u_fd_texture, UV).a <= u_alpha_ref) {
		discard;
	}
	ALBEDO = vec3(1.0);
}
"""
	oracle_material.shader = oracle_shader
	oracle_material.set_shader_parameter(&"u_fd_texture", alpha_texture)
	oracle_material.set_shader_parameter(&"u_alpha_ref", MODEL_ALPHA_REF)
	oracle.material_override = oracle_material
	viewport.add_child(oracle)

	# A transparent black/no-depth control validates the fixture: the late card
	# must show through every covered texel when MODEL contributes no Z write.
	var no_depth := MeshInstance3D.new()
	no_depth.name = "NoDepthControl"
	no_depth.mesh = card_mesh
	var no_depth_material := ShaderMaterial.new()
	var no_depth_shader := Shader.new()
	no_depth_shader.code = """
shader_type spatial;
render_mode unshaded, cull_disabled, blend_add, depth_draw_never;
uniform sampler2D u_fd_texture : filter_linear_mipmap, repeat_enable;
uniform float u_alpha_ref = 0.5;
void fragment() {
	if (texture(u_fd_texture, UV).a <= u_alpha_ref) {
		discard;
	}
	ALBEDO = vec3(0.0);
	ALPHA = 1.0;
}
"""
	no_depth_material.shader = no_depth_shader
	no_depth_material.set_shader_parameter(&"u_fd_texture", alpha_texture)
	no_depth_material.set_shader_parameter(&"u_alpha_ref", MODEL_ALPHA_REF)
	no_depth.material_override = no_depth_material
	viewport.add_child(no_depth)

	foliage.visible = false
	late_layer.visible = false
	oracle.visible = false
	no_depth.visible = false
	await _wait_frames(4)
	var baseline := viewport.get_texture().get_image()
	if baseline == null or baseline.is_empty():
		viewport.queue_free()
		return {"ok": false, "failure": "MODEL contract viewport produced no baseline"}
	baseline.save_png(out_dir.path_join("model_contract_baseline.png"))

	late_layer.visible = true
	await _wait_frames(3)
	var late_only := viewport.get_texture().get_image()
	late_only.save_png(out_dir.path_join("model_contract_late_only.png"))

	late_layer.visible = false
	oracle.visible = true
	await _wait_frames(3)
	var oracle_image := viewport.get_texture().get_image()
	oracle_image.save_png(out_dir.path_join("model_contract_oracle.png"))

	oracle.visible = false
	foliage.visible = true
	await _wait_frames(3)
	var foliage_only := viewport.get_texture().get_image()
	foliage_only.save_png(out_dir.path_join("model_contract_foliage_only.png"))

	late_layer.visible = true
	await _wait_frames(3)
	var combined := viewport.get_texture().get_image()
	combined.save_png(out_dir.path_join("model_contract_combined.png"))

	foliage.visible = false
	no_depth.visible = true
	await _wait_frames(3)
	var no_depth_combined := viewport.get_texture().get_image()
	no_depth_combined.save_png(out_dir.path_join("model_contract_no_depth.png"))

	var accepted_mask := _difference_mask(baseline, oracle_image, MODEL_CONTRACT_DELTA)
	var card_mask := _difference_mask(baseline, late_only, MODEL_CONTRACT_DELTA)
	var rejected_mask := PackedByteArray()
	rejected_mask.resize(card_mask.size())
	var accepted_pixels := 0
	var rejected_pixels := 0
	for index in range(card_mask.size()):
		if accepted_mask[index] != 0:
			accepted_pixels += 1
		elif card_mask[index] != 0:
			rejected_mask[index] = 1
			rejected_pixels += 1

	var color_changed := _masked_changed_pixels(
		baseline, foliage_only, accepted_mask, MODEL_CONTRACT_DELTA)
	var late_leaked_through_depth := _masked_changed_pixels(
		foliage_only, combined, accepted_mask, MODEL_CONTRACT_DELTA)
	var rejected_mismatch := _masked_changed_pixels(
		late_only, combined, rejected_mask, MODEL_CONTRACT_DELTA)
	var no_depth_mismatch := _masked_changed_pixels(
		late_only, no_depth_combined, card_mask, MODEL_CONTRACT_DELTA)

	var mask_ok := accepted_pixels >= MIN_MODEL_CONTRACT_PIXELS \
		and rejected_pixels >= MIN_MODEL_CONTRACT_PIXELS
	var color_preserved_ok := color_changed == 0
	var depth_written_ok := late_leaked_through_depth == 0
	var alpha_reject_ok := rejected_mismatch == 0
	var fixture_order_ok := no_depth_mismatch == 0
	print("FOLIAGE_REGRESSION model_contract accepted_pixels=", accepted_pixels,
		" rejected_pixels=", rejected_pixels)
	print("FOLIAGE_REGRESSION model_contract color_changed=", color_changed,
		" late_leaked_through_depth=", late_leaked_through_depth,
		" rejected_mismatch=", rejected_mismatch,
		" no_depth_mismatch=", no_depth_mismatch)
	print("FOLIAGE_REGRESSION model_contract verdict mask=", mask_ok,
		" one_one_color=", color_preserved_ok,
		" alpha_test_depth=", depth_written_ok,
		" alpha_reject=", alpha_reject_ok,
		" fixture_order=", fixture_order_ok)
	viewport.queue_free()

	if not mask_ok:
		return {"ok": false, "failure": "MODEL contract fixture did not produce robust accepted/rejected masks"}
	if not fixture_order_ok:
		return {"ok": false, "failure": "MODEL contract fixture did not submit the behind-layer after the no-depth control"}
	if not color_preserved_ok:
		return {
			"ok": false,
			"failure": "MODEL ONE/ONE blend changed %d alpha-accepted destination pixels" % color_changed,
		}
	if not depth_written_ok:
		return {
			"ok": false,
			"failure": "MODEL alpha-accepted fragments failed to depth-occlude %d pixels" % late_leaked_through_depth,
		}
	if not alpha_reject_ok:
		return {
			"ok": false,
			"failure": "MODEL alpha-rejected fragments blocked or changed %d behind-layer pixels" % rejected_mismatch,
		}
	return {"ok": true}


func _sample_height(world_x: float, world_z: float) -> float:
	# Slight deterministic slope exercises CPU terrain bending without placing
	# coplanar geometry that could create fixture-only z fighting.
	return 0.025 * world_x - 0.015 * world_z


func _sample_foliage(_world_x: float, _world_z: float) -> int:
	return _foliage_index


func _pin_wind(dispatcher: NovaFoliageDispatcher) -> void:
	for child in dispatcher.get_children():
		if child is MeshInstance3D and child.visible and (
			child.name.begins_with("FoliageDetailDraw")
			or child.name.begins_with("FoliageModelDraw")):
			child.set_instance_shader_parameter(&"u_wind_phase", 0.0)


func _make_cross_mesh() -> ArrayMesh:
	# Two intersecting vertical cards. They are large enough to make the pixel
	# verdict robust, while still traversing the real full-model expansion path.
	var vertices := PackedVector3Array([
		Vector3(-1.2, 0.0, 0.0), Vector3(1.2, 0.0, 0.0),
		Vector3(1.2, 6.0, 0.0), Vector3(-1.2, 6.0, 0.0),
		Vector3(0.0, 0.0, -1.2), Vector3(0.0, 0.0, 1.2),
		Vector3(0.0, 6.0, 1.2), Vector3(0.0, 6.0, -1.2),
	])
	var normals := PackedVector3Array([
		Vector3(0, 0, 1), Vector3(0, 0, 1), Vector3(0, 0, 1), Vector3(0, 0, 1),
		Vector3(1, 0, 0), Vector3(1, 0, 0), Vector3(1, 0, 0), Vector3(1, 0, 0),
	])
	var uvs := PackedVector2Array([
		Vector2(0, 1), Vector2(1, 1), Vector2(1, 0), Vector2(0, 0),
		Vector2(0, 1), Vector2(1, 1), Vector2(1, 0), Vector2(0, 0),
	])
	var indices := PackedInt32Array([
		0, 1, 2, 0, 2, 3,
		4, 5, 6, 4, 6, 7,
	])
	var arrays := []
	arrays.resize(Mesh.ARRAY_MAX)
	arrays[Mesh.ARRAY_VERTEX] = vertices
	arrays[Mesh.ARRAY_NORMAL] = normals
	arrays[Mesh.ARRAY_TEX_UV] = uvs
	arrays[Mesh.ARRAY_INDEX] = indices
	var mesh := ArrayMesh.new()
	mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays)
	return mesh


func _make_model_contract_card_mesh() -> ArrayMesh:
	var vertices := PackedVector3Array([
		Vector3(-4.0, -3.0, 0.0), Vector3(4.0, -3.0, 0.0),
		Vector3(4.0, 3.0, 0.0), Vector3(-4.0, 3.0, 0.0),
	])
	var uvs := PackedVector2Array([
		Vector2(0.0, 1.0), Vector2(1.0, 1.0),
		Vector2(1.0, 0.0), Vector2(0.0, 0.0),
	])
	var uv2s := PackedVector2Array([
		Vector2.ZERO, Vector2.ZERO, Vector2.ZERO, Vector2.ZERO,
	])
	var indices := PackedInt32Array([0, 1, 2, 0, 2, 3])
	var arrays := []
	arrays.resize(Mesh.ARRAY_MAX)
	arrays[Mesh.ARRAY_VERTEX] = vertices
	arrays[Mesh.ARRAY_TEX_UV] = uvs
	arrays[Mesh.ARRAY_TEX_UV2] = uv2s
	arrays[Mesh.ARRAY_INDEX] = indices
	var mesh := ArrayMesh.new()
	mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays)
	return mesh


func _make_model_contract_alpha_texture() -> ImageTexture:
	# Three broad vertical alpha regions give accepted, exact-equality, and
	# below-reference coverage. The equality band is the integer MODEL maximum
	# (128/255), so it must fail retail's D3DCMP_GREATER test.
	# RGB is deliberately nonblack: the production MODEL pass must select black
	# diffuse regardless of texture RGB while consuming only the texture alpha.
	var image := Image.create(32, 32, false, Image.FORMAT_RGBA8)
	for y in range(image.get_height()):
		for x in range(image.get_width()):
			var alpha := 0.0
			if x < image.get_width() / 3:
				alpha = 1.0
			elif x < 2 * image.get_width() / 3:
				alpha = MODEL_ALPHA_REF
			image.set_pixel(x, y, Color(0.92, 0.48, 0.12, alpha))
	return ImageTexture.create_from_image(image)


func _solid_texture(color: Color, size: int) -> ImageTexture:
	var image := Image.create(size, size, false, Image.FORMAT_RGBA8)
	image.fill(color)
	return ImageTexture.create_from_image(image)


func _foreground_stats(background: Image, rendered: Image) -> Dictionary:
	var pixels := 0
	var luma_sum := 0.0
	var max_luma := 0.0
	for y in range(rendered.get_height()):
		for x in range(rendered.get_width()):
			var base := background.get_pixel(x, y)
			var color := rendered.get_pixel(x, y)
			var delta := maxi(maxi(absi(color.r8 - base.r8), absi(color.g8 - base.g8)),
				absi(color.b8 - base.b8))
			if delta <= FOREGROUND_DELTA:
				continue
			var luma := 0.2126 * float(color.r8) + 0.7152 * float(color.g8) + 0.0722 * float(color.b8)
			pixels += 1
			luma_sum += luma
			max_luma = maxf(max_luma, luma)
	return {
		"pixels": pixels,
		"mean_luma": luma_sum / float(pixels) if pixels > 0 else 0.0,
		"max_luma": max_luma,
	}


func _image_diff(a: Image, b: Image) -> Dictionary:
	var changed := 0
	var max_delta := 0
	for y in range(a.get_height()):
		for x in range(a.get_width()):
			var ca := a.get_pixel(x, y)
			var cb := b.get_pixel(x, y)
			var delta := maxi(maxi(absi(ca.r8 - cb.r8), absi(ca.g8 - cb.g8)),
				maxi(absi(ca.b8 - cb.b8), absi(ca.a8 - cb.a8)))
			max_delta = maxi(max_delta, delta)
			if delta > FRAME_DELTA:
				changed += 1
	return {"changed_pixels": changed, "max_delta": max_delta}


func _difference_mask(a: Image, b: Image, threshold: int) -> PackedByteArray:
	var mask := PackedByteArray()
	mask.resize(a.get_width() * a.get_height())
	for y in range(a.get_height()):
		for x in range(a.get_width()):
			var ca := a.get_pixel(x, y)
			var cb := b.get_pixel(x, y)
			var delta := maxi(maxi(absi(ca.r8 - cb.r8), absi(ca.g8 - cb.g8)),
				absi(ca.b8 - cb.b8))
			if delta > threshold:
				mask[y * a.get_width() + x] = 1
	return mask


func _masked_changed_pixels(
	a: Image, b: Image, mask: PackedByteArray, threshold: int
) -> int:
	var changed := 0
	for y in range(a.get_height()):
		for x in range(a.get_width()):
			if mask[y * a.get_width() + x] == 0:
				continue
			var ca := a.get_pixel(x, y)
			var cb := b.get_pixel(x, y)
			var delta := maxi(maxi(absi(ca.r8 - cb.r8), absi(ca.g8 - cb.g8)),
				absi(ca.b8 - cb.b8))
			if delta > threshold:
				changed += 1
	return changed


func _largest_exact_black_component(image: Image) -> int:
	# Eight-connected, matching the screenshot measurement. Building the mask
	# once keeps the loop deterministic and avoids color reads during traversal.
	var width := image.get_width()
	var height := image.get_height()
	var mask := PackedByteArray()
	mask.resize(width * height)
	for y in range(height):
		for x in range(width):
			var color := image.get_pixel(x, y)
			if color.r8 <= 1 and color.g8 <= 1 and color.b8 <= 1:
				mask[y * width + x] = 1

	var largest := 0
	var queue := PackedInt32Array()
	for start in range(mask.size()):
		if mask[start] == 0:
			continue
		mask[start] = 0
		queue.clear()
		queue.append(start)
		var cursor := 0
		while cursor < queue.size():
			var current := queue[cursor]
			cursor += 1
			var cx := current % width
			var cy := current / width
			for dy in range(-1, 2):
				for dx in range(-1, 2):
					if dx == 0 and dy == 0:
						continue
					var nx := cx + dx
					var ny := cy + dy
					if nx < 0 or nx >= width or ny < 0 or ny >= height:
						continue
					var neighbor := ny * width + nx
					if mask[neighbor] == 0:
						continue
					mask[neighbor] = 0
					queue.append(neighbor)
		largest = maxi(largest, queue.size())
	return largest


func _wait_frames(count: int) -> void:
	for _index in range(count):
		await process_frame


func _fail(message: String) -> void:
	push_error("FOLIAGE_REGRESSION FAIL: " + message)
	quit(1)
