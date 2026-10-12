extends GutTest

# TerrainTileEntry / TerrainTileInfo carry every byte of a .til record: the whole flags byte (bits past
# the flips and rotate the game draws, Nile's selection mark 0x08 among them) and MED's +10 layer, and a
# save writes MED's header (12 * count + 16 at +8) [orig: JOTACmed MapTileStore_Serialize @ 0x433EC0;
# docs/tiles/til-re.md D-TIL-6].

var _dir := ""


func before_each() -> void:
	_dir = TestFs.cache_dir(self, "tile_entry_bytes")


func after_each() -> void:
	TestFs.remove_dir_recursive(_dir)


func _entry(flags: int, layer: int, index: int) -> TerrainTileEntry:
	var entry := TerrainTileEntry.new()
	entry.set_cell(2, 3)
	entry.set_tile_index(index)
	entry.set_flags(flags)
	entry.set_layer(layer)
	return entry


func test_entry_keeps_the_whole_flags_byte_the_layer_and_the_index() -> void:
	var entry := _entry(0x48, 22, 41)
	assert_eq(entry.get_flags(), 0x48, "the flags byte is kept whole")
	assert_eq(entry.get_layer(), 22, "the layer is kept")
	assert_eq(entry.get_tile_index(), 41, "the tile index reads back")
	assert_eq(entry.tile_index, 41, "the tile index is a property")
	entry.set_layer(70000)
	assert_eq(entry.get_layer(), 65535, "a layer is a 16-bit word")


func test_save_writes_every_record_byte_and_meds_header() -> void:
	var info := TerrainTileInfo.new()
	info.add_entry(_entry(0x48, 22, 41))
	var path := _dir.path_join("bytes.til")
	assert_eq(info.save_to_path(path), OK, "the placement saves")
	var bytes := FileAccess.get_file_as_bytes(path)
	assert_eq(bytes.size(), 28, "one record after the 16-byte header")
	assert_eq(bytes.decode_u32(4), 1, "the count")
	assert_eq(bytes.decode_u32(8), 28, "+8 is MED's 12 * count + 16")
	assert_eq(bytes.decode_u32(12), 0, "+12 is MED's 0")
	assert_eq(bytes[16 + 8], 41, "+8 of the record is the tile index")
	assert_eq(bytes[16 + 9], 0x48, "+9 is the whole flags byte")
	assert_eq(bytes.decode_u16(16 + 10), 22, "+10 is the layer")

	var reloaded := TerrainTileInfo.new()
	assert_eq(reloaded.load_from_path(path), OK, "the placement reloads")
	var back := reloaded.get_entry(0)
	assert_not_null(back, "the record reloads")
	assert_eq(back.get_flags(), 0x48, "the flags byte reloads whole")
	assert_eq(back.get_layer(), 22, "the layer reloads")
	assert_eq(back.get_tile_index(), 41, "the tile index reloads")


func test_dictionary_entries_carry_the_layer_and_the_flags_byte() -> void:
	var info := TerrainTileInfo.new()
	info.set_entries([{"x_fixed": 0, "z_fixed": 0, "tile_index": 5, "flags": 0xF7, "layer": 30}])
	var back := info.get_entry(0)
	assert_eq(back.get_flags(), 0xF7, "a dictionary's flags byte is kept whole")
	assert_eq(back.get_layer(), 30, "a dictionary's layer is kept")
	assert_eq(back.get_tile_index(), 5, "a dictionary's tile index is kept")
