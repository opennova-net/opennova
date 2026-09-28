extends GutTest

# The viewport pixel leg of the installed launcher/mortar HUD deploy
# (tests/retail/hud_installed_assets_test.gd over InstalledCombatHud): at two
# window sizes the raised HUD changes visible pixels over the background.
# Needs OPENNOVA_JO_DIR with Javelin (Escalation or mod data) and a windowed
# Forward+ run (`scripts/test_godot.sh --suite retail --windowed`: a
# SubViewport readback).

var _rig: InstalledCombatHud


func after_each() -> void:
	if _rig != null:
		_rig.release()
	_rig = null


func test_installed_launcher_and_mortar_hud_draws_visible_pixels() -> void:
	if RenderingServer.get_rendering_device() == null:
		pending("viewport readback needs a windowed Forward+ run")
		return
	_rig = InstalledCombatHud.boot(self)
	if _rig == null:
		return
	for name: String in InstalledCombatHud.NAMES:
		_rig.deploy(self, name)
		for size: Vector2i in [Vector2i(1024, 768), Vector2i(1920, 1080)]:
			_rig.resize(size)
			_rig.presenter.tick()
			await get_tree().process_frame
			await get_tree().process_frame
			RenderingServer.force_draw(true)
			RenderingServer.force_sync()
			var image := _rig.viewport.get_texture().get_image()
			assert_eq(image.get_size(), size)
			var changed := 0
			for y in range(0, size.y, 8):
				for x in range(0, size.x, 8):
					if absf(image.get_pixel(x, y).r - _rig.background.color.r) \
							+ absf(image.get_pixel(x, y).g - _rig.background.color.g) > 0.1:
						changed += 1
			assert_gt(changed, 15, name + " produces visible HUD pixels")
		_rig.lower(self, name)
