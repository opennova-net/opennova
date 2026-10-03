extends GutTest

# The game's texture loaders through ResourceRoot (renderer/texture_load_rules.h
# owns the rules and their witnesses, ctest renderer_texture_load_rules the rule
# table): each role opens exactly the file its retail loader opens, decodes it
# through the retail reader and applies the loader's transform. The witness
# record is docs/render/render-material-re.md "Texture loaders".

var _dirs: Array[String] = []


func after_each() -> void:
	for dir in _dirs:
		TestFs.remove_dir_recursive(dir)
	_dirs.clear()


func _root_dir(label: String) -> String:
	var dir := TestFs.cache_dir(self, "texture_loaders_" + label)
	_dirs.append(dir)
	return dir


func _loose(dir: String) -> ResourceRoot:
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(dir), OK)
	return root


func _packed(dir: String, entries: Array) -> ResourceRoot:
	WorldFixture.write_pff(self, dir.path_join("resource.pff"), entries)
	var root := ResourceRoot.new()
	assert_eq(root.mount_runtime(dir), OK)
	return root


# A 1 x 2 true-colour TGA: the first stored row `first`, the second `second`,
# with the image descriptor `descriptor` (0x20 = the top-left origin bit).
func _two_row_tga(first: Color, second: Color, descriptor: int) -> PackedByteArray:
	var bytes := PackedByteArray()
	bytes.resize(18)
	bytes[2] = 2
	bytes.encode_u16(12, 1)
	bytes.encode_u16(14, 2)
	bytes[16] = 24
	bytes[17] = descriptor
	for c: Color in [first, second]:
		bytes.append_array(PackedByteArray([c.b8, c.g8, c.r8]))
	return bytes


func _dds(color: Color, side := 4) -> PackedByteArray:
	var image := Image.create(side, side, false, Image.FORMAT_RGBA8)
	image.fill(color)
	return image.save_dds_to_buffer()


func _pixel(texture: Texture2D, x := 0, y := 0) -> Color:
	var image := texture.get_image()
	if image.is_compressed():
		image.decompress()
	return image.get_pixel(x, y)


# --- 1. The HUD loader -----------------------------------------------------

func test_hud_loader_turns_a_pcx_white_with_its_blue_as_alpha() -> void:
	var dir := _root_dir("hud_pcx")
	TestFs.write_bytes(self, dir.path_join("icon.pcx"), TestPcx.solid_2x2(Color8(10, 20, 200)))
	var texture := _loose(dir).load_texture("icon.pcx", ResourceRoot.TEXTURE_LOADER_HUD_COLOR)
	assert_not_null(texture, "the HUD loader reads a PCX")
	if texture != null:
		assert_eq(_pixel(texture), Color8(255, 255, 255, 200),
				"HUD_LoadImageAsTexture shifts each word left 24 and ORs 0xFFFFFF")


func test_hud_loader_suffixes_override_the_mode() -> void:
	var dir := _root_dir("hud_suffix")
	TestFs.write_bytes(self, dir.path_join("art.tga"), TestFs.tga_bytes(Vector2i(2, 2), Color8(10, 20, 30, 99)))
	var root := _loose(dir)
	var alpha := root.load_texture("art.tga.alpha", ResourceRoot.TEXTURE_LOADER_HUD_COLOR)
	var full := root.load_texture("ART.TGA.FULL", ResourceRoot.TEXTURE_LOADER_HUD_ALPHA)
	var mode_alpha := root.load_texture("art.tga", ResourceRoot.TEXTURE_LOADER_HUD_ALPHA)
	var mode_colour := root.load_texture("art.tga", ResourceRoot.TEXTURE_LOADER_HUD_COLOR)
	assert_not_null(alpha, ".ALPHA is cut off before the file is opened")
	assert_not_null(full, ".FULL is cut off before the file is opened")
	if alpha != null and full != null and mode_alpha != null and mode_colour != null:
		assert_eq(_pixel(alpha), Color8(255, 255, 255, 99), ".ALPHA makes it alpha-only")
		assert_eq(_pixel(full), Color8(10, 20, 30, 99), ".FULL makes it colour")
		assert_eq(_pixel(mode_alpha), Color8(255, 255, 255, 99),
				"alpha mode keeps only the alpha, drawn in the vertex colour")
		assert_eq(_pixel(mode_colour), Color8(10, 20, 30, 99), "colour mode keeps ARGB")
	assert_null(root.load_texture("art.dds", ResourceRoot.TEXTURE_LOADER_HUD_COLOR),
			"the HUD loader reads TGA and PCX only")


# Six HUDSTANCE frames all naming `stance_name` (the stance widget sizes its
# quads off frame 0).
func _stance_layout(dir: String, stance_name: String) -> HudPos:
	var lines := PackedStringArray(["HUDSTANCEPOS 21 630", "ALPHAFADE 30 50 3"])
	for i in range(6):
		lines.append("HUDSTANCE %d 10 11 %s S%d" % [i, stance_name, i])
	TestFs.write_text(self, dir.path_join("hudpos.def"), "\n".join(lines))
	var layout := HudPos.new()
	assert_eq(layout.load(dir.path_join("hudpos.def")), OK)
	return layout


func test_hud_overlay_draws_pcx_and_suffixed_stance_art() -> void:
	for stance_name in ["stance.pcx", "stance.tga.full"]:
		var dir := _root_dir("hud_stance")
		if stance_name.ends_with(".pcx"):
			TestFs.write_bytes(self, dir.path_join("stance.pcx"), TestPcx.solid_2x2(Color.WHITE))
		else:
			TestFs.write_bytes(self, dir.path_join("stance.tga"), TestFs.tga_bytes(Vector2i(2, 2)))
		var hud := HudOverlay.new()
		hud.size = Vector2(1024, 768)
		add_child_autofree(hud)
		hud.configure(_stance_layout(dir, stance_name), _loose(dir))
		hud.set_player_state(100, 1.0, 2, 80.0)
		assert_eq(hud.get_draw_list_stats().quads, 1,
				"the HUD loader loads stance art named %s" % stance_name)


# --- 2. Model texture type 1 -----------------------------------------------

func test_type_one_upper_case_pcx_turns_white_with_its_blue_as_alpha() -> void:
	var dir := _root_dir("type_one")
	TestFs.write_bytes(self, dir.path_join("SKIN.PCX"), TestPcx.solid_2x2(Color8(10, 20, 200)))
	TestFs.write_bytes(self, dir.path_join("cloth.pcx"), TestPcx.solid_2x2(Color8(10, 20, 200)))
	var root := _loose(dir)
	var upper: Texture2D = root.load_material_texture("SKIN.PCX", 1)
	var lower: Texture2D = root.load_material_texture("cloth.pcx", 1)
	assert_eq(_pixel(upper), Color8(255, 255, 255, 200),
			"Texture_LoadAndRegister masks a name holding .PCX as written")
	assert_eq(_pixel(lower), Color8(10, 20, 200, 255), "a lower-case .pcx stays opaque colour")
	assert_eq(_pixel(root.load_material_texture("SKIN.PCX", 0)), Color8(10, 20, 200, 255),
			"the stage loader (type 0) never masks")


# --- 3. The TGA reader -----------------------------------------------------

func test_tga_rows_always_flip_whatever_the_origin_bit_says() -> void:
	var dir := _root_dir("tga_flip")
	TestFs.write_bytes(self, dir.path_join("top.tga"), _two_row_tga(Color.RED, Color.GREEN, 0x20))
	TestFs.write_bytes(self, dir.path_join("bottom.tga"), _two_row_tga(Color.RED, Color.GREEN, 0x00))
	var root := _loose(dir)
	for name in ["top.tga", "bottom.tga"]:
		var model: Texture2D = root.load_material_texture(name, 0)
		assert_eq(_pixel(model, 0, 0), Color.GREEN,
				"%s: the second stored row draws on top (a model row)" % name)
		var direct := root.load_texture(name, ResourceRoot.TEXTURE_LOADER_TGA)
		assert_eq(_pixel(direct, 0, 0), Color.GREEN, "%s: the TGA reader flips" % name)
		assert_eq(_pixel(direct, 0, 1), Color.RED, "%s: the first stored row at the bottom" % name)


# --- 4. No alternate names -------------------------------------------------

func test_no_loader_reads_an_alternate_name() -> void:
	var dir := _root_dir("alternates")
	var png := Image.create(2, 2, false, Image.FORMAT_RGBA8)
	png.fill(Color.GREEN)
	TestFs.write_bytes(self, dir.path_join("wall.png"), png.save_png_to_buffer())
	TestFs.write_bytes(self, dir.path_join("wall_O.tga"), TestFs.tga_bytes(Vector2i(2, 2)))
	TestFs.write_bytes(self, dir.path_join("wall.jpg"), png.save_jpg_to_buffer())
	var root := _loose(dir)
	for loader in [ResourceRoot.TEXTURE_LOADER_STAGE, ResourceRoot.TEXTURE_LOADER_ARCHIVE,
			ResourceRoot.TEXTURE_LOADER_TGA, ResourceRoot.TEXTURE_LOADER_FILE,
			ResourceRoot.TEXTURE_LOADER_HUD_COLOR, ResourceRoot.TEXTURE_LOADER_MENU]:
		assert_null(root.load_texture("wall.tga", loader),
				"loader %d opens wall.tga only: no _O twin, no png/jpg" % loader)
	assert_null(root.load_texture("wall.png", ResourceRoot.TEXTURE_LOADER_STAGE),
			"no game loader but the menus' reads a PNG")
	assert_not_null(root.load_texture("wall.png", ResourceRoot.TEXTURE_LOADER_MENU),
			"the menu texture loader reads a PNG")


# --- 5. The other loaders the role table names -----------------------------

func test_archive_loader_tries_the_dds_sibling_and_names_its_alpha() -> void:
	var dir := _root_dir("archive")
	var root := _packed(dir, [
		{"name": "sky.pcx", "bytes": TestPcx.ramp_2x2()},
		{"name": "wake.tga", "bytes": TestFs.tga_bytes(Vector2i(2, 2))},
		{"name": "wake.dds", "bytes": _dds(Color.BLUE)},
	])
	var self_alpha := root.load_texture("sky.pcx", ResourceRoot.TEXTURE_LOADER_ARCHIVE_SELF_ALPHA)
	var no_alpha := root.load_texture("sky.pcx", ResourceRoot.TEXTURE_LOADER_ARCHIVE)
	# Ramp index 3 = (3, 3, 3): luminance (85 * 9) >> 8 = 2.
	assert_eq(_pixel(self_alpha, 1, 1), Color8(3, 3, 3, 2),
			"a PCX naming itself as alpha takes its palette luminance")
	assert_eq(_pixel(no_alpha, 1, 1), Color8(3, 3, 3, 255), "an empty alpha name leaves it opaque")
	assert_eq(_pixel(root.load_texture("wake.tga", ResourceRoot.TEXTURE_LOADER_ARCHIVE)),
			Color.BLUE, "the .dds sibling wins")


func test_file_loader_reads_any_other_name_as_pcx() -> void:
	var dir := _root_dir("file")
	TestFs.write_bytes(self, dir.path_join("icons.bmp"), TestPcx.solid_2x2(Color8(10, 20, 30)))
	var texture := _loose(dir).load_texture("icons.bmp", ResourceRoot.TEXTURE_LOADER_FILE)
	assert_not_null(texture, "Texture_LoadFromFile_0 hands a non-.TGA name to the PCX reader")
	if texture != null:
		assert_eq(_pixel(texture), Color8(10, 20, 30, 255))


func test_cine_fade_loader_order() -> void:
	var dir := _root_dir("cine")
	TestFs.write_bytes(self, dir.path_join("win.dds"), _dds(Color.BLUE))
	TestFs.write_bytes(self, dir.path_join("lose.pcx"), TestFs.tga_bytes(Vector2i(2, 2), Color.RED))
	var root := _loose(dir)
	assert_eq(_pixel(root.load_texture("win.tga", ResourceRoot.TEXTURE_LOADER_CINE_FADE)), Color.BLUE,
			"a missing .tga reads its .dds")
	assert_eq(_pixel(root.load_texture("lose.pcx", ResourceRoot.TEXTURE_LOADER_CINE_FADE)), Color.RED,
			"a name the PCX reader rejects goes to the TGA reader")


func test_normal_maps_halve_to_512() -> void:
	var dir := _root_dir("normal_cap")
	TestFs.write_bytes(self, dir.path_join("big.mdt"), TestFs.tga_bytes(Vector2i(1024, 4), Color8(128, 128, 255, 255)))
	TestFs.write_bytes(self, dir.path_join("small.mdt"), TestFs.tga_bytes(Vector2i(512, 4), Color8(128, 128, 255, 255)))
	var root := _loose(dir)
	var big: Texture2D = root.load_material_texture("big.mdt", 4)
	assert_eq(Vector2i(big.get_width(), big.get_height()), Vector2i(512, 2),
			"a normal map loads with the 512 cap: both sides halve while either exceeds it")
	var small: Texture2D = root.load_material_texture("small.mdt", 4)
	assert_eq(Vector2i(small.get_width(), small.get_height()), Vector2i(512, 4), "at the cap nothing halves")
