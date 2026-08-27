class_name ProbeStage
extends SubViewport

## An off-screen stage for a probe's own scene: a SubViewport with its own
## World3D, always rendering, cleared every frame, sized like the capture the
## probe wants, so the live world (or the menu) never bleeds into what it
## captures. The rendering settings mirror the shell viewport's, so a byte
## read here matches a window read. Freed through the context at finish.


## A stage of `size` under the tree root; `mirror` (the shell viewport, when
## given) supplies the rendering settings a window capture would have had.
static func create(ctx: ProbeContext, size: Vector2i, mirror: Viewport = null) -> ProbeStage:
	var stage := ProbeStage.new()
	stage.name = "ProbeStage"
	stage.size = size
	# A fresh World3D assigned directly: get_world_3d() then names the world
	# the stage renders, which auxiliary views (FrameFx's Q3 view) copy.
	# own_world_3d would render a private duplicate and leave get_world_3d()
	# pointing at an empty original.
	stage.world_3d = World3D.new()
	stage.transparent_bg = false
	stage.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	stage.render_target_clear_mode = SubViewport.CLEAR_MODE_ALWAYS
	stage.handle_input_locally = false
	stage.gui_disable_input = true
	if mirror != null:
		stage.mirror_rendering_settings(mirror)
	ctx.tree.root.add_child(stage)
	ctx.defer_restore(func() -> void:
		if is_instance_valid(stage):
			if stage.get_parent() != null:
				stage.get_parent().remove_child(stage)
			stage.free())
	return stage


## The per-viewport rendering knobs a window carries from the project
## settings; a SubViewport starts from defaults, so a capture would differ.
func mirror_rendering_settings(source: Viewport) -> void:
	msaa_2d = source.msaa_2d
	msaa_3d = source.msaa_3d
	screen_space_aa = source.screen_space_aa
	use_taa = source.use_taa
	use_debanding = source.use_debanding
	use_occlusion_culling = source.use_occlusion_culling
	use_hdr_2d = source.use_hdr_2d
	scaling_3d_mode = source.scaling_3d_mode
	scaling_3d_scale = source.scaling_3d_scale
	fsr_sharpness = source.fsr_sharpness
	texture_mipmap_bias = source.texture_mipmap_bias
	anisotropic_filtering_level = source.anisotropic_filtering_level
	mesh_lod_threshold = source.mesh_lod_threshold
	positional_shadow_atlas_size = source.positional_shadow_atlas_size
	positional_shadow_atlas_16_bits = source.positional_shadow_atlas_16_bits
	canvas_item_default_texture_filter = source.canvas_item_default_texture_filter
	canvas_item_default_texture_repeat = source.canvas_item_default_texture_repeat
	snap_2d_transforms_to_pixel = source.snap_2d_transforms_to_pixel
	snap_2d_vertices_to_pixel = source.snap_2d_vertices_to_pixel
	sdf_oversize = source.sdf_oversize
	sdf_scale = source.sdf_scale


## Resize between scenes (a mode that wants a different aspect).
func set_stage_size(new_size: Vector2i) -> void:
	size = new_size


## The scene the probe built (its own WorldEnvironment and Camera3D).
func add_scene(scene: Node) -> void:
	add_child(scene)


## Remove and free the current scene(s) so the next mode starts empty.
func clear_scene() -> void:
	for child in get_children():
		remove_child(child)
		child.free()


## The stage's next drawn frame as an Image (null when nothing rendered).
func capture_image(tree: SceneTree, settle_frames := 0) -> Image:
	for _i in range(settle_frames):
		await tree.process_frame
	await RenderingServer.frame_post_draw
	var texture := get_texture()
	var image: Image = texture.get_image() if texture != null else null
	if image == null or image.is_empty():
		return null
	return image
