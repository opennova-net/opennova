extends GutTest

# The water's beauty strip on the device (D-NET-1353): while the march keeps its row count,
# the strip's surface is rewritten in place (Water::_rewrite_strip_in_place over
# WaterCore::strip_surface_bytes), its device buffers kept, as retail writes the strip into the
# one dynamic vertex buffer it keeps (engine formats/env/env_water_render.h, the batch contract,
# carries the witness). The rewrite must hold the surface a rebuild of the same march holds: the
# same positions, colour bytes, UVs, customs and indices, and the same AABB. A server without a
# RenderingDevice rebuilds (the dummy keeps no buffers): a tests/windowed/ script, run by
# `scripts/test_godot.sh --windowed` (a headless run pends it).

const TICK := 1.0 / 62.0


func _rd_available() -> bool:
	return RenderingServer.get_rendering_device() != null


# water_test.gd's fixture: the march is a function of viewport pixels, so it runs in a
# code-fixed SubViewport whose own camera the water resolves.
func _make_water_fixture() -> Dictionary:
	var strip_vp := SubViewport.new()
	strip_vp.size = Vector2i(1024, 600)
	add_child_autofree(strip_vp)
	var water: Node = Water.new()
	strip_vp.add_child(water)
	water.water_height = 7.0
	var cam := Camera3D.new()
	strip_vp.add_child(cam)
	cam.global_position = Vector3(100.3, 27.0, -33.7)
	cam.make_current()
	return {"water": water, "camera": cam}


func test_the_rewritten_strip_is_the_rebuilt_strip() -> void:
	if not _rd_available():
		pending("RenderingDevice unavailable under this Godot renderer")
		return
	var fixture := _make_water_fixture()
	var water: Node = fixture["water"]
	var cam: Camera3D = fixture["camera"]
	cam.rotation_degrees = Vector3(-12.0, 20.0, 0.0)
	water.advance_frame(TICK)
	var mesh := water.get_mesh_instance().mesh as ArrayMesh
	assert_eq(mesh.get_surface_count(), 1, "the first march builds the strip's surface")
	assert_eq(water.get_strip_surface_rewrites(), 0, "a first surface is built, not rewritten")
	# Small turns keep the row count; the first one that does rewrites the surface in place.
	for step in 12:
		cam.rotation_degrees = Vector3(-12.0, 20.0 + 0.25 * float(step + 1), 0.0)
		water.advance_frame(TICK)
		if water.get_strip_surface_rewrites() > 0:
			break
	var rewrites: int = water.get_strip_surface_rewrites()
	assert_gt(rewrites, 0, "a turn that keeps the rows rewrites in place")
	if rewrites == 0:
		return
	var rewritten := mesh.surface_get_arrays(0)
	var rewritten_aabb := RenderingServer.mesh_get_custom_aabb(mesh.get_rid())
	# The same view again through a rebuild: a hidden frame clears the strip, the next one
	# marches the same rows and builds a new surface from the arrays.
	water.visible = false
	water.advance_frame(TICK)
	water.visible = true
	water.advance_frame(TICK)
	assert_eq(water.get_strip_surface_rewrites(), rewrites, "the cleared strip is rebuilt")
	assert_eq(mesh.get_surface_count(), 1)
	var rebuilt := mesh.surface_get_arrays(0)
	for slot in [Mesh.ARRAY_VERTEX, Mesh.ARRAY_COLOR, Mesh.ARRAY_TEX_UV, Mesh.ARRAY_CUSTOM0,
			Mesh.ARRAY_CUSTOM1, Mesh.ARRAY_CUSTOM2, Mesh.ARRAY_INDEX]:
		assert_true(rewritten[slot] == rebuilt[slot],
				"array %d: the rewrite holds what a rebuild of the same march holds" % slot)
	assert_eq(rewritten_aabb, mesh.get_aabb(), "the rewrite's AABB is the rebuild's")
	assert_eq(RenderingServer.mesh_get_custom_aabb(mesh.get_rid()), AABB(),
			"a rebuilt surface keeps its own AABB")
