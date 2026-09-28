class_name InstalledCombatHud
extends RefCounted

## The installed combat-HUD rig the retail launcher/mortar HUD test
## (tests/retail/hud_installed_assets_test.gd) and its windowed pixel leg
## (tests/retail/windowed/hud_installed_assets_pixels_test.gd) share: the
## first mounted root carrying Javelin (Escalation or mod data), its three
## launcher/mortar weapon and ammo rows staged over the minimal world, and a
## real GameHudPresenter drawing into a SubViewport. Assets stay in the
## mounted VFS; only those rows are staged, and no copyrighted art is
## committed. Every helper that asserts or pends takes the GutTest so the
## verdict lands in the calling test's report.

const NAMES: Array[String] = ["WPN_JAVELIN", "WPN_STINGER", "WPN_MORTAR"]

var art: ResourceRoot
var weapons: WeaponDatabase
var stage := ""
var world: GameWorld
var sim: Simulation
var viewport: SubViewport
var camera: Camera3D
var background: ColorRect
var ui: Control
var presenter: GameHudPresenter
var hud: HudOverlay
var card: HudSightsCard


## The `kind "name"` block of a shipped definition table, up to the next
## block's header; "" when the table lacks it.
static func definition(source: String, kind: String, name: String) -> String:
	var pattern := RegEx.new()
	pattern.compile('(?mi)^' + kind + '\\s+"?' + name + '"?\\s*$')
	var found := pattern.search(source)
	if found == null:
		return ""
	var next := RegEx.new()
	next.compile('(?mi)^' + kind + '\\s+')
	var end := next.search(source, found.get_end())
	return source.substr(found.get_start(),
			(end.get_start() if end != null else source.length()) - found.get_start())


## Javelin and tanks are Escalation content in the stock install. Mods may
## provide them in the base mount; the first mounted weapon table carrying
## Javelin wins instead of assuming OPENNOVA_JO_DIR implies an expansion.
## Null (after pending) without an install, or without Javelin in any mount.
static func combat_root(test: GutTest) -> ResourceRoot:
	var installed := RetailData.install()
	if installed.is_empty():
		test.pending("OPENNOVA_JO_DIR is required for installed combat HUD validation")
		return null
	var expansions := PackedStringArray([""])
	expansions.append_array(RetailData.expansions())
	for expansion: String in expansions:
		var root := ResourceRoot.new()
		var mounted := root.mount_runtime(installed, expansion)
		test.assert_eq(mounted, OK, "combat HUD assets mount: " + expansion)
		if mounted != OK:
			return null
		var weapons := WeaponDatabase.new()
		var loaded := weapons.load_from_resource_root(root, "weapon.def")
		test.assert_eq(loaded, OK, "combat HUD weapon definitions load: " + expansion)
		if loaded != OK:
			return null
		if weapons.find_weapon("WPN_JAVELIN") >= 0:
			return root
	test.pending("OPENNOVA_JO_DIR needs Escalation or mod data providing Javelin and tanks")
	return null


## The rig over combat_root(): the three rows staged over the minimal world,
## the world booted and settled for 90 ticks, the presenter with its real
## overlay configured from the installed hudpos.def. Null when the root pends
## (the asserts land on `test`); release() removes the staged rows.
static func boot(test: GutTest) -> InstalledCombatHud:
	var root := combat_root(test)
	if root == null:
		return null
	var rig := InstalledCombatHud.new()
	rig.art = root
	rig.weapons = WeaponDatabase.new()
	test.assert_eq(rig.weapons.load_from_resource_root(root, "weapon.def"), OK)
	var layout := HudPos.new()
	test.assert_eq(layout.load_from_resource_root(root, "hudpos.def"), OK)
	var weapon_source := root.read_file("weapon.def").get_string_from_utf8()
	var ammo_source := root.read_file("ammo.def").get_string_from_utf8()
	rig.stage = HudFixture.stage_root(true)
	var staged_weapons := FileAccess.get_file_as_string(rig.stage.path_join("weapon.def"))
	var staged_ammo := FileAccess.get_file_as_string(rig.stage.path_join("ammo.def"))
	for name: String in NAMES:
		var row := definition(weapon_source, "weapon", name)
		test.assert_false(row.is_empty(), name + " has an installed definition")
		staged_weapons += "\n" + row
	for name: String in NAMES:
		var ammo_name := rig.weapons.get_weapon(rig.weapons.find_weapon(name)).get_round_type()
		var row := definition(ammo_source, "ammo", ammo_name)
		test.assert_false(row.is_empty(), ammo_name + " has an installed ammo definition")
		staged_ammo += "\n" + row
	WorldFixture.write_file(rig.stage.path_join("weapon.def"), staged_weapons)
	WorldFixture.write_file(rig.stage.path_join("ammo.def"), staged_ammo)
	rig.world = WorldFixture.boot_minimal(test, rig.stage)
	rig.world.set_process(false)
	rig.sim = rig.world.get_sim()
	for _i in range(90):
		rig.sim.step()
	rig.viewport = SubViewport.new()
	rig.viewport.size = Vector2i(1024, 768)
	rig.viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	test.add_child_autofree(rig.viewport)
	rig.camera = Camera3D.new()
	rig.viewport.add_child(rig.camera)
	rig.camera.keep_aspect = Camera3D.KEEP_WIDTH
	rig.camera.make_current()
	rig.background = ColorRect.new()
	rig.background.size = rig.viewport.size
	rig.background.color = Color(0.12, 0.18, 0.22)
	rig.viewport.add_child(rig.background)
	rig.ui = Control.new()
	rig.ui.size = rig.viewport.size
	rig.viewport.add_child(rig.ui)
	rig.presenter = GameHudPresenter.new()
	test.add_child_autofree(rig.presenter)
	rig.presenter.setup(rig.world, null, rig.ui)
	rig.presenter.ensure_game_hud()
	rig.presenter.set_hud_detail_level(0)
	rig.hud = rig.presenter.get_game_hud()
	rig.hud.configure(layout, root)
	rig.card = rig.hud.get_node("SightsCard") as HudSightsCard
	return rig


## Select `name`, let its draw settle, raise its sights, aim the camera down
## the sim's view and hand the card the weapon's authored sights: the deploy
## the HUD tests walk per weapon.
func deploy(test: GutTest, name: String) -> void:
	test.assert_true(world.set_local_player_weapon_by_name(name))
	for _i in range(90):
		sim.step()
	presenter.tick()
	test.assert_false(card.is_card_up(), name + " starts lowered")
	test.assert_true(sim.request_local_player_scope_toggle())
	for _i in range(100):
		sim.step()
	var view := sim.get_local_player_view()
	camera.global_position = view.camera_eye
	camera.look_at(view.camera_eye + Simulation.presentation_forward(
			view.camera_yaw_deg, view.camera_pitch_deg), Vector3.UP)
	camera.fov = view.fov_h_deg
	presenter.tick()
	test.assert_true(world.local_player_weapon_view().aimed_shot_available,
			name + " finishes raising")
	var weapon := weapons.get_weapon(weapons.find_weapon(name))
	card.set_weapon_sights(weapon.get_sights(), art)


## Lower the raised sights again and settle the presenter.
func lower(test: GutTest, name: String) -> void:
	test.assert_true(sim.request_local_player_scope_toggle())
	for _i in range(100):
		sim.step()
	presenter.tick()
	test.assert_false(card.is_card_up(), name + " hides its card when lowered")


## Resize the viewport, its background and the UI layer together.
func resize(size: Vector2i) -> void:
	viewport.size = size
	ui.size = size
	background.size = size


## Remove the staged rows (the world and the nodes are autofreed by the test).
func release() -> void:
	if not stage.is_empty():
		TestFs.remove_dir_recursive(stage)
		stage = ""
