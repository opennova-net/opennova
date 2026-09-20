extends GutTest

# D-WPN-38: the real presenter/rig must consume the authored six-lane pose.
# No retail files: the normal minimal mission boots the committed person model
# as both gun and arms with explicit hip/ADS rotation in its synthetic DEF.

const HIP := Vector3(22.5, 22.5, 337.5)
const ADS := Vector3(11.25, 45.0, 22.5)
const POS := Vector3(64.0, 32.0, -160.0)
const TPOS := Vector3(32.0, 16.0, -128.0)
var _root := ""
var _world: GameWorld
var _camera: Camera3D
var _presenter: LocalPlayerPresenter


func before_each() -> void:
	_root = WorldFixture.stage_minimal_root("viewmodel_rotation", false, {
		"weapon.def": """weapon "WPN_M4AUTO"
category 1
rank 0
flags sighted
animadm PoseGun
gfx1 PoseGun
pos 64,32,-160,22.5,22.5,337.5
tpos 32,16,-128,11.25,45,22.5
ACTION "IDLE"
ANIM anim_wpn_idle
FUNCTION WPN_STD_IDLE
DELAYEND auto
END
ACTION "SWITCHTO"
ANIM anim_wpn_idle
FUNCTION WPN_STD_SWITCHTO
DELAYSTART 1
DELAYEND 1
END
end
weapon "WPN_FORCEPOSE"
category 1
rank 1
flags sighted
flags forcescoped
animadm PoseGun
gfx1 PoseGun
pos 64,32,-160,22.5,22.5,337.5
tpos 32,16,-128,11.25,45,22.5
ACTION "IDLE"
ANIM anim_wpn_idle
FUNCTION WPN_STD_IDLE
DELAYEND auto
END
ACTION "SWITCHTO"
ANIM anim_wpn_idle
FUNCTION WPN_STD_SWITCHTO
DELAYSTART 1
DELAYEND 1
END
end
""",
		"PoseGun.adm": "anim_reset \"idle.bad\"\nanim_wpn_idle \"idle.bad\"\n",
		"Avatars.def": """define head HEAD
{
graphic US01.3di
camo 0 0 0
voice 1
sex m
}
define body BODY
{
graphic US01.3di
camo 0 0 0
}
define arms ARMS
{
graphic PoseArms.3di
camo 0 0 0
}
nationality 0 NAT
{
alignment good
division 0 DIV
{
combo 1 HEAD BODY ARMS
}
}
""",
	})
	for name in ["US01.3di", "Indo01.3di", "PoseGun.3di", "PoseArms.3di"]:
		assert_eq(DirAccess.copy_absolute(ProjectSettings.globalize_path(
				"res://../fixtures/threedi/synth/person.3di"), _root.path_join(name)), OK)
	for name in ["US01.adm", "E_STAND.adm"]:
		assert_eq(DirAccess.copy_absolute(ProjectSettings.globalize_path(
				"res://../fixtures/anim/soldier.adm"), _root.path_join(name)), OK)
	for name in ["idle.bad", "walk.bad"]:
		assert_eq(DirAccess.copy_absolute(ProjectSettings.globalize_path(
				"res://../fixtures/anim/" + name), _root.path_join(name)), OK)


func _boot(weapon_name: String = "WPN_M4AUTO") -> void:
	_world = WorldFixture.make_world(self)
	var loadout := PlayerSpawnLoadout.new()
	loadout.primary = weapon_name
	loadout.player_class = 8
	_world.set_local_player_spawn_loadout(loadout)
	assert_eq(WorldFixture.load_mission(_world, _root), OK)
	_camera = Camera3D.new()
	add_child_autofree(_camera)
	_presenter = LocalPlayerPresenter.new()
	add_child_autofree(_presenter)
	_presenter.setup(_world, _camera, null, ControlsModel.new())
	_presenter.set_input_override(PlayerMoveIntent.new())
	await get_tree().process_frame
	WorldFixture.step_player_frames(_presenter, _world, _camera, 20)
	assert_true(_world.get_sim().has_local_player())
	assert_eq(_world.get_sim().get_local_player_weapon_name(), weapon_name,
			"the normal spawn path installed the requested DEF")
	assert_not_null(_presenter.viewmodel())
	assert_eq(_presenter.vm_parts().size(), 2, "the normal builder produced gun and arms")
	assert_false(_world.local_player_view().suppress_view_bias, "the fixture landed")


func after_each() -> void:
	if _presenter != null:
		_presenter.teardown()
	if _world != null:
		_world.unload()
	TestFs.remove_dir_recursive(_root)


func _assert_rig(rotation_deg: Vector3, fraction: float, check_position: bool = true) -> void:
	var model := _presenter.viewmodel()
	if model == null:
		fail_test("no actual viewmodel to inspect")
		return
	# Independent axis map: source yaw/pitch/roll -> camera y/x/-z, YXZ.
	var bias := Basis.from_euler(Vector3(deg_to_rad(rotation_deg.y),
			deg_to_rad(rotation_deg.x), -deg_to_rad(rotation_deg.z)))
	var rig_rotation := _presenter.viewmodel_rig().get_player_viewmodel_rot()
	var axis_map := Basis.from_euler(Vector3(deg_to_rad(rig_rotation.x),
			deg_to_rad(rig_rotation.y), deg_to_rad(rig_rotation.z)))
	var actual := _camera.global_transform.affine_inverse() * model.global_transform
	var expected_basis := bias * axis_map
	assert_true(actual.basis.x.distance_to(expected_basis.x) < 0.00001, "actual rig X basis")
	assert_true(actual.basis.y.distance_to(expected_basis.y) < 0.00001, "actual rig Y basis")
	assert_true(actual.basis.z.distance_to(expected_basis.z) < 0.00001, "actual rig Z basis")
	if check_position:
		var view := POS.lerp(TPOS, fraction) / 256.0
		var size := _world.get_viewport().get_visible_rect().size
		if 3.0 * size.x <= 4.0 * size.y:
			view.z -= 1280.0 / 65536.0
		var offset := Vector3(-view.y, view.z, -view.x)
		assert_true(actual.origin.distance_to(bias * offset) < 0.002,
				"the same ADS rotation turns the actual viewmodel offset")


func test_authored_ads_rotation_reaches_real_rig_and_camera_restamp() -> void:
	await _boot()
	_assert_rig(HIP, 0.0)
	assert_true(_world.get_sim().request_local_player_scope_toggle())
	WorldFixture.step_player_frames(_presenter, _world, _camera)
	# Original BAM lanes: decreasing yaw advances by delta/15; increasing
	# pitch and wrap-crossing roll snap immediately because velocity is unsigned.
	_assert_rig(Vector3(21.75, 45.0, 22.5), 1.0 / 15.0)
	WorldFixture.step_player_frames(_presenter, _world, _camera, 16)
	_assert_rig(ADS, 1.0)
	_camera.global_transform = Transform3D(Basis.from_euler(Vector3(0.3, -0.5, 0.2)),
			Vector3(30.0, 20.0, -40.0))
	_presenter.restamp_viewmodel_at_camera()
	_assert_rig(ADS, 1.0)


func test_airborne_actual_rig_keeps_hip_cant_then_restores_current_ads_pose() -> void:
	await _boot()
	assert_true(_world.get_sim().request_local_player_scope_toggle())
	WorldFixture.step_player_frames(_presenter, _world, _camera, 3)
	_assert_rig(Vector3(20.25, 45.0, 22.5), 3.0 / 15.0)
	WorldFixture.step_player_frames(_presenter, _world, _camera, 1, true)
	assert_true(_world.local_player_view().suppress_view_bias, "jump suppresses actual bias")
	_assert_rig(HIP, 0.0, false)
	WorldFixture.step_player_frames(_presenter, _world, _camera, 4)
	assert_true(_world.local_player_view().suppress_view_bias, "the player remains airborne")
	_assert_rig(HIP, 0.0, false)
	for tick in range(120):
		WorldFixture.step_player_frames(_presenter, _world, _camera)
		if not _world.local_player_view().suppress_view_bias:
			break
	assert_false(_world.local_player_view().suppress_view_bias, "the player landed")
	WorldFixture.step_player_frames(_presenter, _world, _camera, 20)
	_assert_rig(ADS, 1.0)


func test_forced_scope_equip_reaches_actual_ads_rig_without_toggle() -> void:
	await _boot("WPN_FORCEPOSE")
	var database := _world.get_weapon_database()
	var forced_def := database.get_weapon(database.find_weapon("WPN_FORCEPOSE"))
	assert_eq(forced_def.flags, 0x20000002, "both authored flag lines reached the DEF")
	assert_true(_world.local_player_view().scope_engaged)
	assert_almost_eq(_world.local_player_view().scope_fraction, 1.0, 0.001)
	# Player_MountWeaponSlot promotes ForceScoped itself and schedules Setup(1).
	# The normal spawn/install + presenter route must consume it without input.
	_assert_rig(ADS, 1.0)
	assert_false(_world.get_sim().request_local_player_scope_toggle(), "forced equip stays pinned")
	WorldFixture.step_player_frames(_presenter, _world, _camera, 3)
	_assert_rig(ADS, 1.0)
