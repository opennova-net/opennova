extends GutTest

# The native entity query is made before both FP model submits (@0x4DEEB0).
var _root := ""
var _world: GameWorld
var _camera: Camera3D
var _presenter: LocalPlayerPresenter


func before_each() -> void:
	_root = WorldFixture.stage_minimal_root("fp_entity_light", false, {
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


func _boot() -> void:
	_world = WorldFixture.make_world(self)
	var loadout := PlayerSpawnLoadout.new()
	loadout.primary = "WPN_M4AUTO"
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
	_frame(20)
	assert_true(_world.get_sim().has_local_player())
	assert_not_null(_presenter.viewmodel())
	assert_eq(_presenter.vm_parts().size(), 2, "the normal builder produced gun and arms")
	assert_false(_world.local_player_view().suppress_view_bias, "the fixture landed")


func after_each() -> void:
	if _presenter != null:
		_presenter.teardown()
	if _world != null:
		_world.unload()
	TestFs.remove_dir_recursive(_root)


func _frame(count: int = 1, jump: bool = false) -> void:
	var input := PlayerMoveIntent.new()
	input.jump = jump
	_presenter.set_input_override(input)
	for tick in count:
		var frame_input := _presenter.before_world_tick(Simulation.tick_dt(), false, true)
		_world.tick(_camera.global_position, _camera.global_transform,
				Simulation.tick_dt(), frame_input)
		_presenter.after_world_tick()


func test_fp_models_query_the_player_even_after_camera_restamping() -> void:
	await _boot()
	var sim := _world.get_sim()
	var entity_position := sim.get_local_player_position()
	_camera.global_position = entity_position + Vector3(50, 40, 30)
	_presenter.restamp_viewmodel_at_camera()
	var director := EffectLightDirector.new()
	director.setup(_world, Callable(), Callable())
	var scene := director.scene()
	assert_gt(scene.spawn_model_light(ModelLightSpawn.make(entity_position, 0.01)), 0)
	var parts: Array[ObjectModel] = _presenter.vm_parts()
	assert_eq(parts.size(), 2, "normal world construction supplies gun and arms")
	for part in parts:
		# Neither a rendered-model radius nor its camera-relative position is
		# allowed to replace the native player query source.
		part.set_shadow_bound_radii(1000, 1000)
	assert_gt(scene.spawn_model_light(ModelLightSpawn.make(parts[0].global_position, 0.01)), 0)
	var camera_light := parts[0].global_position
	director.render_frame(_camera, parts, -1, false)
	for part in parts:
		var surfaces: Array[Node] = part.find_children("*", "GeometryInstance3D", true, false)
		assert_gt(surfaces.size(), 0)
		for node in surfaces:
			var surface := node as GeometryInstance3D
			assert_eq(float(surface.get_instance_shader_parameter("u_point_light_count")), 1.0)
			var posr: Vector4 = surface.get_instance_shader_parameter("u_point_light_posr_0")
			assert_almost_eq(Vector3(posr.x, posr.y, posr.z), entity_position,
					Vector3.ONE * 0.001, "FP lights use the native player's sphere")
	assert_gt(camera_light.distance_to(entity_position), 30.0)
	_camera.global_position += Vector3(20, 10, 15)
	_presenter.restamp_viewmodel_at_camera()
	director.render_frame(_camera, parts, -1, false)
	for part in parts:
		var surface := part.find_children("*", "GeometryInstance3D", true, false)[0] as GeometryInstance3D
		assert_eq(float(surface.get_instance_shader_parameter("u_point_light_count")), 1.0,
				"render-only camera changes leave the shared entity query unchanged")
