extends GutTest

const HudSightsCardScript := preload("res://game/world/hud_sights_card.gd")

var _temp_dirs: Array[String] = []


func after_each() -> void:
	for dir_path in _temp_dirs:
		TestFs.remove_dir_recursive(dir_path)
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
	var bytes := TestFs.tga_bytes(Vector2i(2, 1), Color8(128, 128, 128, 255))
	for i in range(22, 26):
		bytes[i] = 0
	TestFs.write_bytes(self, dir_path.path_join("multiplyat.tga"), bytes)
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(dir_path), OK)
	return root
