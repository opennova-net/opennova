extends GutTest

# B12: the shared resource-kind vocabulary (framework/resource_kinds.gd).
# Pins the jump translations every jump surface rides — the ref widget's
# pre-B12 private copy was missing the sbf/music_script rows (a latent
# music-jump bug), so these rows are the regression net.


func test_jump_kind_translates_workspace_owned_spellings() -> void:
	assert_eq(ResourceKinds.jump_kind("object_project"), "object")
	assert_eq(ResourceKinds.jump_kind("object_model"), "object")
	assert_eq(ResourceKinds.jump_kind("object_scene"), "object")
	assert_eq(ResourceKinds.jump_kind("sbf"), "music")
	assert_eq(ResourceKinds.jump_kind("music_script"), "music")


func test_jump_kind_is_identity_for_direct_kinds() -> void:
	assert_eq(ResourceKinds.jump_kind("terrain"), "terrain")
	assert_eq(ResourceKinds.jump_kind("menu_style"), "menu_style")
	assert_eq(ResourceKinds.jump_kind("hudpos"), "hudpos")


func test_label_is_artist_facing() -> void:
	assert_eq(ResourceKinds.label("hudpos"), "HUD layout")
	assert_eq(ResourceKinds.label("menu_style"), "menu style")
	assert_eq(ResourceKinds.label("sbf"), "music")
	assert_eq(ResourceKinds.label("object_model"), "object")
	assert_eq(ResourceKinds.label("something_new"), "resource")
