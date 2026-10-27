extends GutTest

# The post-particle overlay tail through the real world: GameWorld's
# scene_overlay leg gathers the frame and every view's overlay pass (composed
# right after particle pass B, before the frame effects) receives it. The
# underwater murk quad draws per VIEW from that view's own render eye, at or
# below the water, whatever the camera mode [orig:
# Terrain_RenderWorldScene @ 0x5c96c5..0x5c96f5; the tail order and
# its witnesses live in engine/runtime/renderer/scene_overlay.h]. The draw
# checks need the real rendering device:
# windowed/scene_overlay_stage_device_test.gd.

const STAGE_TEST_ROOT := "scene_overlay_stage_test"
# A stock-style sky body: an FF_ST_AD_LUM surface whose RGB generator style
# 113 reads CTRL UPL_INTENSITY (the mglare authoring), staged under the name
# the fixture environment's glare_3di line carries.
const GLARE_FIXTURE := "res://../fixtures/threedi/synth/crate_mtrl0_ad_lum_upl113.3di"


var _staged_dirs: Array[String] = []


func after_each() -> void:
	TestFs.remove_dir_recursive(OS.get_cache_dir().path_join(STAGE_TEST_ROOT))
	for dir in _staged_dirs:
		TestFs.remove_dir_recursive(dir)
	_staged_dirs.clear()


# The overlay tail reads each sky body's surfaces once (SceneOverlayModelSurfaces' geometry, a
# device readback that stalls the frame it lands in): the load reads them where it builds the
# bodies, so the first frame that appends the glare, the glint or the water mirror's redraw of the
# discs reads nothing back (D-NET-1341). No frame runs between the load and the read here.
func test_the_load_reads_the_sky_bodies_surfaces_ahead_of_their_first_frame() -> void:
	var root_dir := WorldFixture.stage_minimal_root("scene_overlay_prime", true)
	_staged_dirs.append(root_dir)
	assert_eq(DirAccess.copy_absolute(ProjectSettings.globalize_path(GLARE_FIXTURE),
			root_dir.path_join("mglare.3di")), OK)
	var world := WorldFixture.make_world(self)
	assert_eq(WorldFixture.load_mission(world, root_dir, "mnml.bms",
			func(mission: MissionData) -> void:
				assert_true(mission.set_header_string("terrain", "Tmap"))), OK)
	var celestial := world.get_celestial_node() as Celestial
	assert_not_null(celestial)
	if celestial == null:
		return
	assert_not_null(celestial.get_overlay_body_node("glare"), "the fixture environment names a glare model")
	assert_gt(world.get_scene_overlay_cached_meshes(), 0,
			"the load read the sky bodies' surfaces before any frame appended them")
