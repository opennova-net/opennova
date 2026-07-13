extends GutTest
## The foliagemap resolvers, pinned.
##
## Two NovaTerrainData accessors exist because retail has two samplers over
## the same load-remapped buffer:
## - get_foliage_far_mask_world: the flat & 1023 read with an internal z
##   negation [orig: Foliage_SampleFarMapMask @ 0x6066d0 indexes
##   ((x>>16)&1023, (-z>>16)&1023)]. Retail feeds it SOURCE-ATLAS coordinates
##   (its FAR keys pack them @ 0x603f8a); it is NOT world-correct on
##   origin-shifted layouts.
## - get_foliage_index_world: the sector-routed world read, consuming the
##   same Godot world (x, z) as the height samplers [orig:
##   Foliage_SampleFoliageMapMask @ 0x606620]. This is the gate BOTH runtime
##   tiers use.
## These tests pin each accessor's own addressing, the pixel-zero early-out,
## and the definition match remap.

const DVXI5_FIXTURE_RES_DIR := "res://../fixtures/godot/dvxi5"


func _fixture_path(filename: String) -> String:
	return ProjectSettings.globalize_path(DVXI5_FIXTURE_RES_DIR).path_join(filename)


func _load_dvxi5() -> NovaTerrainData:
	var data := NovaTerrainData.new()
	data.set_trn_path(_fixture_path("Dvxi5.trn"))
	if data.load() != OK:
		return null
	return data


## Find an interior source-space point: both coordinate forms resolve it and
## its foliage-map texel is addressable.
func _interior_point(data: NovaTerrainData) -> Vector2:
	for sz in range(64, 8192, 256):
		for sx in range(64, 8192, 256):
			var editor_form := data.world_to_source_coords(float(sx), float(sz))
			if editor_form.x >= 0.0:
				return Vector2(float(sx), float(sz))
	return Vector2(-1, -1)


func test_far_mask_negates_once_onto_the_flat_wrapped_row() -> void:
	var data := _load_dvxi5()
	assert_not_null(data, "Dvxi5 fixture should load.")
	if data == null:
		return
	var p := _interior_point(data)
	assert_gt(p.x, 0.0, "fixture should expose an interior point")

	var map := data.get_foliage_map()
	assert_not_null(map, "fixture should carry a foliage map")

	# Address the texel exactly as the game read resolves it, through the
	# runtime wrap kernel.
	var map_x := _flat_far_map_coord(floori(p.x), map.get_width())
	var map_y := _flat_far_map_coord(floori(p.y), map.get_height())

	# Paint a def-matchable index at that texel (defs come from the fixture
	# when it has any; otherwise pin the index path alone).
	var defs: Array = data.get_foliage_defs()
	var match_index := 7
	var match_slot := -1
	for d in mini(defs.size(), 4):
		if defs[d] != null and int(defs[d].get_match()) > 0:
			match_index = int(defs[d].get_match())
			match_slot = d
			break
	map.clear(0)
	map.paint_circle(map_x, map_y, 1, 1.0, 1.0, match_index)

	# The far-mask accessor negates ONCE internally: handing it -source_z (the
	# retail native arg form) must land on the flat wrapped texel...
	var mask := data.get_foliage_far_mask_world(p.x, -p.y)
	if match_slot >= 0:
		assert_eq(mask, 1 << match_slot,
				"far mask at (x, -z) remaps the flat painted texel through all matches")
	# ...and handing it +source_z must NOT (the z-mirror read).
	var mirrored := data.get_foliage_far_mask_world(p.x, p.y)
	assert_ne(mirrored, mask if match_slot >= 0 else -1,
			"far mask at (x, +z) is the MIRRORED read — it must not see the texel")


func test_pixel_zero_never_matches() -> void:
	var data := _load_dvxi5()
	assert_not_null(data, "Dvxi5 fixture should load.")
	if data == null:
		return
	var p := _interior_point(data)
	var map := data.get_foliage_map()
	var source := data.world_to_source_coords_wrapped(p.x, p.y)
	map.paint_circle(map.map_x_from_heightmap_x(source.x),
			map.map_y_from_heightmap_y(source.y), 1, 1.0, 1.0, 0)
	# [orig: the pixel == 0 early-out @ 0x5ff4e8] — even a def authored with
	# match 0 never gates on an unpainted texel.
	assert_eq(data.get_foliage_far_mask_world(p.x, -p.y), 0,
			"pixel 0 never matches any def")


func test_all_four_definition_match_values_set_the_same_slot_bit() -> void:
	var data := _load_dvxi5()
	assert_not_null(data, "Dvxi5 fixture should load.")
	if data == null:
		return
	var p := _interior_point(data)
	var map := data.get_foliage_map()
	var map_x := _flat_far_map_coord(floori(p.x), map.get_width())
	var map_y := _flat_far_map_coord(floori(p.y), map.get_height())
	var def := NovaTerrainFoliageDef.new()
	def.graphic = "four_match_fixture"
	def.matches = PackedInt32Array([7, 8, 9, 10])
	data.foliage_defs = [def]
	map.clear(0)

	for match_value in def.matches:
		map.paint_circle(map_x, map_y, 1, 1.0, 1.0, match_value)
		assert_eq(data.get_foliage_far_mask_world(p.x, -p.y), 1,
				"Every authored match byte maps the foliage pixel to slot 0.")


func test_far_mask_uses_flat_wrapped_address_not_model_sector_route() -> void:
	var data := _load_dvxi5()
	assert_not_null(data, "Dvxi5 fixture should load.")
	if data == null:
		return
	var map := data.get_foliage_map()
	var witness := Vector2i(-1, -1)
	var flat_pixel := Vector2i(-1, -1)
	var routed_pixel := Vector2i(-1, -1)
	for native_z in range(-768, 769, 17):
		for world_x in range(-768, 769, 19):
			var routed_source := data.world_to_source_coords_wrapped(world_x, -native_z)
			if routed_source.x < 0.0:
				continue
			var flat := Vector2i(
					_flat_far_map_coord(world_x, map.get_width()),
					_flat_far_map_coord(-native_z, map.get_height()))
			var routed := Vector2i(
					map.map_x_from_heightmap_x(routed_source.x),
					map.map_y_from_heightmap_y(routed_source.y))
			if flat != routed:
				witness = Vector2i(world_x, native_z)
				flat_pixel = flat
				routed_pixel = routed
				break
		if witness.x != -1:
			break
	assert_ne(witness, Vector2i(-1, -1),
			"Fixture exposes an address where FAR flat wrap and MODEL routing differ.")
	if witness == Vector2i(-1, -1):
		return

	map.clear(0)
	map.set_index(flat_pixel.x, flat_pixel.y, 7)
	map.set_index(routed_pixel.x, routed_pixel.y, 0)
	var def := NovaTerrainFoliageDef.new()
	def.match = 7
	data.foliage_defs = [def]

	assert_eq(data.get_foliage_far_mask_world(witness.x, witness.y), 1,
			"the flat accessor reads the source-space (& 1023) FOLIAGEMAP texel.")
	assert_eq(data.get_foliage_index_world(witness.x, -witness.y), 0,
			"the sector-routed world accessor stays independent at the same args.")


func _flat_far_map_coord(value: int, dimension: int) -> int:
	var log2_dimension := 0
	var power := 1
	while power * 2 <= dimension and log2_dimension < 10:
		power *= 2
		log2_dimension += 1
	return (value & 1023) >> maxi(10 - log2_dimension, 0)


func test_wrapped_coords_is_the_runtime_kernel_form() -> void:
	var data := _load_dvxi5()
	assert_not_null(data, "Dvxi5 fixture should load.")
	if data == null:
		return
	var p := _interior_point(data)

	# Interior points: the wrapped (runtime) form and the editor form resolve
	# the same source coords — one kernel, two guard sets.
	var editor_form := data.world_to_source_coords(p.x, p.y)
	var wrapped_form := data.world_to_source_coords_wrapped(p.x, p.y)
	assert_almost_eq(wrapped_form.x, editor_form.x, 0.01,
			"interior x resolves identically through both guard sets")
	assert_almost_eq(wrapped_form.y, editor_form.y, 0.01,
			"interior z resolves identically through both guard sets")

	# Out-of-extent coords: the runtime form WRAPS (& 0xF sector wrap) where
	# the editor form bounds-rejects anything outside the authored extent —
	# the reason the editor's foliage addressing goes through the wrapped
	# form. Whether -p.y falls inside the extent depends on the map's origin,
	# so the invariant pinned here is agreement between the wrapped form and
	# the game's own index read at an arbitrary (here negated) coordinate.
	var wrapped_neg := data.world_to_source_coords_wrapped(p.x, -p.y)
	# The wrap can land in an empty sector (sentinel) but on a full 16-grid
	# map it resolves; either way it must agree with the game's index read.
	var idx_direct := data.get_foliage_index_world(p.x, -p.y)
	if wrapped_neg.x < 0.0:
		assert_eq(idx_direct, 0, "empty wrapped sector reads index 0")
	else:
		var map := data.get_foliage_map()
		var expected := int(map.get_index(map.map_x_from_heightmap_x(wrapped_neg.x),
				map.map_y_from_heightmap_y(wrapped_neg.y)))
		assert_eq(idx_direct, expected,
				"the wrapped form addresses the texel the game index read resolves")
