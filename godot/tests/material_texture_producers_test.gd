extends GutTest

func test_specialized_material_resources_keep_their_dimensions() -> void:
    var path := OS.get_cache_dir().path_join("opennova_material_producers_%d" % Time.get_ticks_usec())
    DirAccess.make_dir_recursive_absolute(path)
    var tga := PackedByteArray()
    tga.resize(18 + 4 * 4 * 4)
    tga[2] = 2
    tga[12] = 4
    tga[14] = 4
    tga[16] = 32
    tga[17] = 0x28
    for i in range(16):
        tga[18 + i * 4 + 3] = (i * 37 + 11) & 255
    var file := FileAccess.open(path.path_join("height.tga"), FileAccess.WRITE)
    file.store_buffer(tga)
    file.close()
    var resources := ResourceRoot.new()
    assert_eq(resources.set_root_dir(path), OK)
    var horizon = resources.load_material_texture("height.tga", 6)
    assert_true(horizon is Texture3D, "type 6 keeps its sixteen slices")
    if horizon is Texture3D:
        assert_eq(horizon.get_width(), 1)
        assert_eq(horizon.get_height(), 1)
        assert_eq(horizon.get_depth(), 16)
    var ao = resources.load_material_texture("height.tga", 7)
    assert_true(ao is Texture2D, "type 7 is a 2D image")
    assert_ne(horizon, ao, "producer type participates in the cache key")
    if ao is Texture2D:
        assert_eq(ao.get_width(), 4)
        assert_eq(ao.get_height(), 4)
    var missing = resources.load_material_texture("missing.hrz", 17)
    assert_true(missing is Texture2D, "a failed volume load binds the checkerboard")
    assert_eq(missing.get_width(), 128)
    for type in [16, 17, 18]:
        var payload_size := 28 + (16 if type == 16 else (8 if type == 17 else 4))
        var chunk := PackedByteArray()
        chunk.resize(16 + payload_size)
        var tag := "NQ8B" if type == 16 else ("HRZ8" if type == 17 else "AOC8")
        for i in range(4):
            chunk[8 + i] = tag.unicode_at(i)
        chunk.encode_u32(12, payload_size)
        chunk.encode_u32(28, 2)
        chunk.encode_u32(32, 2)
        chunk.encode_u32(36, 2)
        file = FileAccess.open(path.path_join("chunk%d.bin" % type), FileAccess.WRITE)
        file.store_buffer(chunk)
        file.close()
    ResourceRoot.bump_cache_epoch()
    assert_eq(resources.set_root_dir(path), OK)
    for type in [16, 17, 18]:
        var texture = resources.load_material_texture("chunk%d.bin" % type, type)
        assert_true(texture is Texture3D if type == 17 else texture is Texture2D)
        assert_eq(texture.get_width(), 2)
        assert_eq(texture.get_height(), 2)
        if texture is Texture3D:
            assert_eq(texture.get_depth(), 2)
    resources.clear()
