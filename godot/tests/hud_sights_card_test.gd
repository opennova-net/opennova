extends GutTest

const HudSightsCardScript := preload("res://game/world/hud_sights_card.gd")

var _temp_dirs: Array[String] = []


func after_each() -> void:
	for dir_path in _temp_dirs:
		for file_name in DirAccess.get_files_at(dir_path):
			DirAccess.remove_absolute(dir_path.path_join(file_name))
		DirAccess.remove_absolute(dir_path)
	_temp_dirs.clear()


func test_multiplyat_selects_retail_material_contract() -> void:
	var root := _make_sight_root()
	var card := HudSightsCardScript.new()
	add_child_autofree(card)
	_configure_multiplyat(card, root)
	assert_eq(card.row_count(), 1)
	var sight_row := card.get_child(0) as Control
	assert_not_null(sight_row)
	var material := sight_row.material as ShaderMaterial
	assert_not_null(material, "multiplyat selects a shader material")
	if material != null:
		assert_true(bool(material.get_shader_parameter("alpha_test")),
				"multiplyat enables the retail alpha test")


func test_multiplyat_preserves_scene_detail_and_discards_transparent_texels() -> void:
	if RenderingServer.get_rendering_device() == null:
		pending("pixel readback requires a windowed Forward+ RenderingDevice run")
		return
	var root := _make_sight_root()
	var viewport := SubViewport.new()
	viewport.size = Vector2i(64, 32)
	viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	add_child_autofree(viewport)

	var background := Control.new()
	background.size = Vector2(64, 32)
	viewport.add_child(background)
	for y in range(0, 32, 16):
		var stripe := ColorRect.new()
		stripe.position = Vector2(0, y)
		stripe.size = Vector2(64, 16)
		stripe.color = Color(0.25, 0.25, 0.25) if y == 0 else Color(0.75, 0.75, 0.75)
		background.add_child(stripe)

	var card := HudSightsCardScript.new()
	card.size = Vector2(64, 32)
	viewport.add_child(card)
	_configure_multiplyat(card, root)
	await get_tree().process_frame
	await get_tree().process_frame
	RenderingServer.force_draw(true)
	RenderingServer.force_sync()

	var image := viewport.get_texture().get_image()
	assert_not_null(image)
	if image == null:
		return
	# The opaque left texel is neutral 128 gray for retail's witnessed
	# 2*source*destination equation. Both background levels must survive.
	assert_almost_eq(image.get_pixel(16, 8).r, 0.25, 0.03,
			"neutral multiplyat keeps the dark scene sample")
	assert_almost_eq(image.get_pixel(16, 24).r, 0.75, 0.03,
			"neutral multiplyat keeps the bright scene sample")
	# The right texel has black RGB but zero alpha. The AT flag must discard it,
	# rather than allowing multiplicative blending to black out the scene.
	assert_almost_eq(image.get_pixel(48, 8).r, 0.25, 0.03,
			"multiplyat discards a transparent dark texel")
	assert_almost_eq(image.get_pixel(48, 24).r, 0.75, 0.03,
			"multiplyat preserves detail behind transparent texels")


func _configure_multiplyat(card: Control, root: ResourceRoot) -> void:
	var sights: Array[WeaponSightRow] = [WeaponSightRow.make("multiplyat.tga", 0, 0, 1024, 768, 5)]
	card.set_weapon_sights(sights, root)
	card.set_card_up(true)


func _make_sight_root() -> ResourceRoot:
	var dir_path := OS.get_temp_dir().path_join(
			"hud_sights_card_%d" % Time.get_ticks_usec())
	assert_eq(DirAccess.make_dir_recursive_absolute(dir_path), OK)
	_temp_dirs.append(dir_path)
	# Two uncompressed BGRA texels: opaque neutral gray, then transparent black.
	var bytes := PackedByteArray()
	bytes.resize(18 + 2 * 4)
	bytes[2] = 2
	bytes[12] = 2
	bytes[14] = 1
	bytes[16] = 32
	bytes[17] = 0x28
	bytes[18] = 128
	bytes[19] = 128
	bytes[20] = 128
	bytes[21] = 255
	bytes[22] = 0
	bytes[23] = 0
	bytes[24] = 0
	bytes[25] = 0
	var texture_file := FileAccess.open(
			dir_path.path_join("multiplyat.tga"), FileAccess.WRITE)
	assert_not_null(texture_file)
	texture_file.store_buffer(bytes)
	texture_file.close()
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(dir_path), OK)
	return root
