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
	assert_not_null(mode_alpha, "alpha mode loads the TGA")
	assert_not_null(mode_colour, "colour mode loads the TGA")
	if alpha != null and full != null and mode_alpha != null and mode_colour != null:
		assert_eq(_pixel(alpha), Color8(255, 255, 255, 99), ".ALPHA makes it alpha-only")
		assert_eq(_pixel(full), Color8(10, 20, 30, 99), ".FULL makes it colour")
		assert_eq(_pixel(mode_alpha), Color8(255, 255, 255, 99),
				"alpha mode keeps only the alpha; the material draws the vertex colour")
		assert_eq(_pixel(mode_colour), Color8(10, 20, 30, 99), "colour mode keeps ARGB")
	assert_null(root.load_texture("art.dds", ResourceRoot.TEXTURE_LOADER_HUD_COLOR),
			"the HUD loader reads TGA and PCX only")


# Six HUDSTANCE frames all naming `stance_name` (the stance widget sizes its
# quads off frame 0).
func _stance_layout(dir: String, stance_name: String, extra := PackedStringArray()) -> HudPos:
	# The stance rides WPNGRP on foot; the rows show it (D-HUD-54).
	var lines := PackedStringArray(HudFixture.DECLUTTER_ROWS + ["HUDSTANCEPOS 21 630", "ALPHAFADE 30 50 3"])
	lines.append_array(extra)
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


func _doubled(c: Color) -> Color:
	return Color(minf(c.r8 * 2, 255) / 255.0, minf(c.g8 * 2, 255) / 255.0,
			minf(c.b8 * 2, 255) / 255.0, c.a8 / 255.0)


func test_alpha_mode_art_draws_twice_the_vertex_colour() -> void:
	# The stance art loads in alpha mode, whose material 0xA51 draws ADD(DIFFUSE,
	# DIFFUSE): twice the vertex colour, saturated, the texture's colour unread. A
	# ".FULL" name loads it in colour mode, drawn under 0x651's MODULATE2X(TEXTURE,
	# DIFFUSE): twice texel x vertex colour.
	for stance_name in ["stance.tga", "stance.tga.full"]:
		var dir := _root_dir("hud_alpha_draw")
		TestFs.write_bytes(self, dir.path_join("stance.tga"), TestFs.tga_bytes(Vector2i(2, 2), Color8(10, 20, 30, 255)))
		var hud := HudOverlay.new()
		hud.size = Vector2(1024, 768)
		add_child_autofree(hud)
		hud.configure(_stance_layout(dir, stance_name,
				PackedStringArray(["stanceicon_color 255 80 40 20"])), _loose(dir))
		hud.set_player_state(100, 1.0, 2, 80.0)
		var compiled := hud.get_textured_quad_colors(false)
		var drawn := hud.get_textured_quad_colors(true)
		assert_eq(drawn.size(), 1, "one stance quad")
		if drawn.size() != 1:
			continue
		assert_eq(compiled[0].r8, 80, "the stance tint is the vertex colour")
		# Both materials draw a white texel at twice 0x50.
		assert_eq(drawn[0].r8, 160, "0x50 doubles to 0xA0 (%s)" % stance_name)
		var rows: Array = hud.get_flat_submissions()
		assert_eq(rows.size(), 1)
		if rows.size() != 1:
			continue
		if stance_name == "stance.tga":
			assert_true(drawn[0].is_equal_approx(_doubled(compiled[0])),
					"alpha mode draws twice the vertex colour (%s from %s)" % [drawn[0], compiled[0]])
			assert_eq(rows[0].kind, "rect", "the alpha material folds into the submitted colour")
			assert_eq(rows[0].colors[0].r8, 160)
		else:
			# Colour mode: the raw vertex colour goes to the device, whose 0x651
			# material runs MODULATE2X(TEXTURE, DIFFUSE) on the flagged command
			# (D-HUD-49).
			assert_eq(rows[0].kind, "triangles", "a .FULL name draws a flagged triangle pair")
			for uv: Vector2 in rows[0].uvs:
				assert_gte(uv.y, 8.0, "every vertex carries the MODULATE2X flag")
			assert_eq(rows[0].colors[0].r8, 80, "the device gets the raw vertex colour")


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
	# The stage and plain loaders keep a texture under one key, so a stage row of the
	# name after the plain row draws the plain row's masked texture.
	assert_true(root.load_material_texture("skin.pcx", 0) == upper,
			"a stage row after it takes the plain row's texture")
	assert_eq(_pixel(_loose(dir).load_material_texture("SKIN.PCX", 0)), Color8(10, 20, 200, 255),
			"the stage loader (type 0) loading the name itself never masks")


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


func test_occlusion_rows_halve_to_512() -> void:
	# Type 7 (the occlusion producer) loads with the same 512 cap as the normal maps
	# (renderer::material_texture_side_cap).
	var dir := _root_dir("ao_cap")
	TestFs.write_bytes(self, dir.path_join("height.tga"), TestFs.tga_bytes(Vector2i(1024, 4), Color8(128, 128, 128, 200)))
	var ao: Texture2D = _loose(dir).load_material_texture("height.tga", 7)
	assert_eq(Vector2i(ao.get_width(), ao.get_height()), Vector2i(512, 2), "the AO texture halves to 512")


# --- The review's further cases ---------------------------------------------

func _mip_coloured_dds() -> PackedByteArray:
	# 4x4 red level 0 with an authored green 2x2 and blue 1x1 below it.
	var data := PackedByteArray()
	for side_colour in [[4, Color.RED], [2, Color.GREEN], [1, Color.BLUE]]:
		var colour: Color = side_colour[1]
		for _texel in side_colour[0] * side_colour[0]:
			data.append_array(PackedByteArray([colour.r8, colour.g8, colour.b8, 255]))
	return Image.create_from_data(4, 4, true, Image.FORMAT_RGBA8, data).save_dds_to_buffer()


func test_archive_self_alpha_takes_the_dds_sibling_first() -> void:
	var dir := _root_dir("archive_dds_first")
	var root := _packed(dir, [
		{"name": "sky.pcx", "bytes": TestPcx.ramp_2x2()},
		{"name": "sky.dds", "bytes": _dds(Color.BLUE)},
	])
	assert_eq(_pixel(root.load_texture("sky.pcx", ResourceRoot.TEXTURE_LOADER_ARCHIVE_SELF_ALPHA)),
			Color.BLUE, "the sky map's .dds wins before the PCX and its luminance")


func test_dds_sibling_against_a_loose_file_follows_the_policy() -> void:
	var dir := _root_dir("policy")
	TestFs.write_bytes(self, dir.path_join("wall.tga"), TestFs.tga_bytes(Vector2i(2, 2), Color.YELLOW))
	WorldFixture.write_pff(self, dir.path_join("resource.pff"), [
		{"name": "wall.dds", "bytes": _dds(Color.BLUE)},
		{"name": "wall.tga", "bytes": TestFs.tga_bytes(Vector2i(2, 2), Color.CYAN)},
	])
	var root := ResourceRoot.new()
	assert_eq(root.mount_runtime(dir), OK)
	var stage := ResourceRoot.TEXTURE_LOADER_STAGE
	assert_eq(_pixel(root.load_texture("wall.tga", stage)), Color.BLUE,
			"a packed session: the .dds sibling wins")
	assert_eq(_pixel(root.load_texture("wall.tga", stage, ResourceRoot.LOOKUP_FORCE_LOOSE_FIRST)),
			Color.YELLOW, "a forced loose-first lookup lets the loose TGA win")
	assert_eq(root.mount_runtime(dir, "", true), OK)
	assert_eq(_pixel(root.load_texture("wall.tga", stage)), Color.YELLOW, "/d: the loose TGA wins")
	assert_eq(_pixel(root.load_texture("wall.tga", stage, ResourceRoot.LOOKUP_FORCE_ARCHIVE_ONLY)),
			Color.BLUE, "an archive-only lookup ignores the loose file and takes the .dds")


func test_dds_reader_decodes_by_content() -> void:
	var dir := _root_dir("dds_content")
	var png := Image.create(2, 2, false, Image.FORMAT_RGBA8)
	png.fill(Color.GREEN)
	var root := _packed(dir, [
		{"name": "pic.dds", "bytes": png.save_png_to_buffer()},
		{"name": "flip.dds", "bytes": _two_row_tga(Color.RED, Color.GREEN, 0x20)},
		{"name": "mips.dds", "bytes": _mip_coloured_dds()},
	])
	var stage := ResourceRoot.TEXTURE_LOADER_STAGE
	var pic := root.load_texture("pic.tga", stage)
	assert_not_null(pic, "a PNG under the .dds sibling's name decodes, as D3DX sniffs it")
	if pic != null:
		assert_eq(_pixel(pic), Color.GREEN)
	var flip := root.load_texture("flip.dds", stage)
	assert_not_null(flip, "a TGA under a .dds name decodes")
	if flip != null:
		assert_eq(_pixel(flip, 0, 0), Color.RED, "D3DX's TGA codec honours the origin bit")
	var mips := root.load_texture("mips.dds", stage)
	assert_not_null(mips)
	if mips != null:
		var image := mips.get_image()
		assert_true(image.has_mipmaps(), "a DDS keeps its mip chain")
		var level1 := image.get_mipmap_offset(1)
		var data := image.get_data()
		assert_eq(Color8(data[level1], data[level1 + 1], data[level1 + 2]), Color8(0, 255, 0),
				"the authored level 1 survives")


# The rest of D3DX's content sniff: a PPM, a PFM and a headerless DIB under a
# .dds name decode too (D-RMAT-17; the codecs' witnesses ride
# renderer/d3dx_image_codecs.h, ctest renderer_d3dx_image_codecs).
# [orig: D3DXTex::CImage::Load @ 0x6DF1DC, the codec order @ 0x6DF212..0x6DF242]
func test_dds_reader_decodes_ppm_pfm_and_dib() -> void:
	var dir := _root_dir("dds_codecs")
	var ppm := "P6\n2 1\n255\n".to_ascii_buffer()
	ppm.append_array(PackedByteArray([255, 0, 0, 0, 0, 255]))
	# A grey PFM, little-endian by its negative scale: one texel of 0.5.
	var pfm := "Pf\n1 1\n-1.0\n".to_ascii_buffer()
	var texel := PackedByteArray()
	texel.resize(4)
	texel.encode_float(0, 0.5)
	pfm.append_array(texel)
	# A 1 x 1 24-bit BITMAPINFOHEADER with no file header: one green pixel (B, G, R)
	# and the row's pad byte.
	var dib := PackedByteArray()
	dib.resize(40)
	dib.encode_u32(0, 40)
	dib.encode_s32(4, 1)
	dib.encode_s32(8, 1)
	dib.encode_u16(12, 1)
	dib.encode_u16(14, 24)
	dib.append_array(PackedByteArray([0, 255, 0, 0]))
	var root := _packed(dir, [
		{"name": "ppm.dds", "bytes": ppm},
		{"name": "pfm.dds", "bytes": pfm},
		{"name": "dib.dds", "bytes": dib},
	])
	var stage := ResourceRoot.TEXTURE_LOADER_STAGE
	var ppm_texture := root.load_texture("ppm.dds", stage)
	assert_not_null(ppm_texture, "a P6 PPM under a .dds name decodes")
	if ppm_texture != null:
		assert_eq(_pixel(ppm_texture, 0, 0), Color.RED)
		assert_eq(_pixel(ppm_texture, 1, 0), Color.BLUE)
	var pfm_texture := root.load_texture("pfm.dds", stage)
	assert_not_null(pfm_texture, "a grey PFM under a .dds name decodes")
	if pfm_texture != null:
		assert_eq(_pixel(pfm_texture), Color8(128, 128, 128),
				"0.5 through D3DX's 8-bit encode: trunc(127.5 + 0.5)")
	var dib_texture := root.load_texture("dib.dds", stage)
	assert_not_null(dib_texture, "a headerless DIB under a .dds name decodes")
	if dib_texture != null:
		assert_eq(_pixel(dib_texture), Color.GREEN)


# D3DX's HDR codec under a .dds name: decoded straight into the image's one buffer,
# made only once the scanlines describe the image. A 16384 x 16384 header over 64 KiB
# of bare words fails its size step, before any buffer, and reads as no texture
# [orig: D3DXTex_LoadHDRFromMemory @0x6DEA53; the flat scanline @0x6DEF26..0x6DF00A].
func test_dds_reader_decodes_hdr_into_one_buffer() -> void:
	var dir := _root_dir("dds_hdr")
	var hdr := "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 1 +X 3\n".to_ascii_buffer()
	hdr.append_array(PackedByteArray([10, 20, 30, 128, 40, 50, 60, 128, 70, 80, 90, 128]))
	var huge := "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 16384 +X 16384\n".to_ascii_buffer()
	var words := PackedByteArray()
	words.resize(16384 * 4)
	words.fill(0x10)
	huge.append_array(words)
	var root := _packed(dir, [
		{"name": "flat.dds", "bytes": hdr},
		{"name": "huge.dds", "bytes": huge},
	])
	var stage := ResourceRoot.TEXTURE_LOADER_STAGE
	var flat := root.load_texture("flat.dds", stage)
	assert_not_null(flat, "a flat HDR under a .dds name decodes")
	if flat != null:
		assert_eq(flat.get_width(), 3)
		# (byte + 0.5) * 2^(128 - 136) through D3DX's 8-bit encode: 40 -> 40.
		assert_eq(_pixel(flat, 0, 0), Color8(40, 50, 60), "the first word is held, never written")
		assert_eq(_pixel(flat, 2, 0), Color8(0, 0, 0), "the last pixel stays unwritten")
	assert_null(root.load_texture("huge.dds", stage),
			"a header its data cannot describe is no texture, and no 1 GiB buffer")


func test_names_differing_in_case_stay_apart() -> void:
	# A load by name keeps the decode its files and readers resolved: ".MDT" is tested
	# as written, so "x.mdt" takes the .dds sibling and "x.MDT" the TGA reader. (Retail's
	# stage loader keeps one texture under "x.mdt:1" for both; the material rows below
	# share it, the loads by name do not yet.)
	var dir := _root_dir("case_keys")
	var root := _packed(dir, [
		{"name": "x.dds", "bytes": _dds(Color.BLUE)},
		{"name": "x.mdt", "bytes": TestFs.tga_bytes(Vector2i(2, 2), Color.RED)},
	])
	var stage := ResourceRoot.TEXTURE_LOADER_STAGE
	assert_eq(_pixel(root.load_texture("x.MDT", stage)), Color.RED, "x.MDT reads the MDT")
	assert_eq(_pixel(root.load_texture("x.mdt", stage)), Color.BLUE, "x.mdt reads the .dds")
	assert_eq(_pixel(root.load_texture("x.MDT", stage)), Color.RED, "and x.MDT stays the MDT")


# The texture registry (renderer/texture_registry.h, ctest renderer_texture_registry): a
# material row's loader keeps its texture under the row's name and its loader's suffix,
# looked up without case before any file opens, so the first row of a key to load
# decides the texture every later row of it draws.
func test_material_rows_differing_in_case_share_the_first_load() -> void:
	var entries := [
		{"name": "x.dds", "bytes": _dds(Color.BLUE)},
		{"name": "x.mdt", "bytes": TestFs.tga_bytes(Vector2i(2, 2), Color.RED)},
	]
	var upper_first := _packed(_root_dir("rows_case_upper"), entries)
	var mdt := upper_first.load_material_texture("x.MDT", 0)
	assert_eq(_pixel(mdt), Color.RED, "an x.MDT row first reads the MDT")
	assert_true(upper_first.load_material_texture("x.mdt", 0) == mdt, "an x.mdt row after it takes that texture")
	var lower_first := _packed(_root_dir("rows_case_lower"), entries)
	var dds := lower_first.load_material_texture("x.mdt", 0)
	assert_eq(_pixel(dds), Color.BLUE, "an x.mdt row first reads the .dds")
	assert_true(lower_first.load_material_texture("x.MDT", 0) == dds, "an x.MDT row after it takes the .dds")


func test_material_rows_of_one_name_share_the_first_load() -> void:
	# A type-0 row (the stage loader: the .dds beside the name first) and a type-1 row
	# (the plain loader: the named file) print one key, so the second row of a name
	# draws the first row's texture, in either order and any case.
	var entries := [
		{"name": "wall.dds", "bytes": _dds(Color.BLUE)},
		{"name": "wall.tga", "bytes": TestFs.tga_bytes(Vector2i(2, 2), Color.RED)},
	]
	var stage_first := _packed(_root_dir("rows_stage_first"), entries)
	var stage := stage_first.load_material_texture("wall.tga", 0)
	assert_eq(_pixel(stage), Color.BLUE, "the stage row loads the .dds")
	assert_true(stage_first.load_material_texture("WALL.TGA", 1) == stage,
			"a plain row of the name after it draws the stage row's .dds, never the TGA")
	assert_true(stage_first.load_material_texture("wall.tga", 2) == stage, "so does a detail row")
	var plain_first := _packed(_root_dir("rows_plain_first"), entries)
	var plain := plain_first.load_material_texture("wall.tga", 1)
	assert_eq(_pixel(plain), Color.RED, "the plain row loads the TGA")
	assert_true(plain_first.load_material_texture("wall.tga", 0) == plain,
			"the stage row after it draws the plain row's TGA, never the .dds")


func test_material_rows_of_other_loaders_keep_their_own() -> void:
	# A normal-map row's key is the name and ":BA:1": a stage row of the same file is
	# another texture. A failed load keeps nothing, so a later row of the key loads by
	# its own loader.
	var root := _packed(_root_dir("rows_other_loaders"), [
		{"name": "brick.tga", "bytes": TestFs.tga_bytes(Vector2i(4, 4), Color8(90, 70, 61, 128))},
		{"name": "sign.tgaz", "bytes": TestFs.tga_bytes(Vector2i(2, 2), Color.GREEN)},
	])
	var diffuse := root.load_material_texture("brick.tga", 0)
	var normal := root.load_material_texture("brick.tga", 4)
	assert_false(diffuse == normal, "a stage row and a normal-map row of one file are two textures")
	assert_true(root.load_material_texture("BRICK.TGA", 5) == normal, "a type-5 row shares the type-4 row's")
	# The stage rule cuts "sign.tgaz" to "sign.tga", which is not there: the checkerboard,
	# kept under no key. The plain rule opens the whole name.
	assert_eq(root.load_material_texture("sign.tgaz", 0).get_width(), 128, "the stage row draws the checkerboard")
	var plain := root.load_material_texture("sign.tgaz", 1)
	assert_eq(_pixel(plain), Color.GREEN, "the plain row after it loads its own file")
	assert_true(root.load_material_texture("sign.tgaz", 0) == plain, "and the stage row then draws that")


func test_loose_mount_resolves_a_qualified_name_by_its_file() -> void:
	var dir := _root_dir("qualified")
	TestFs.write_bytes(self, dir.path_join("flat.tga"), TestFs.tga_bytes(Vector2i(2, 2), Color.RED))
	var texture := _loose(dir).load_texture("sub/flat.tga", ResourceRoot.TEXTURE_LOADER_TGA)
	assert_not_null(texture, "a loose mount keeps a qualified query's file name")


func test_particle_graphics_read_the_loose_tga_folder_first() -> void:
	var dir := _root_dir("particle")
	DirAccess.make_dir_recursive_absolute(dir.path_join("tga"))
	TestFs.write_bytes(self, dir.path_join("tga").path_join("spark.tga"), TestFs.tga_bytes(Vector2i(2, 2), Color.RED))
	var root := _packed(dir, [
		{"name": "spark.tga", "bytes": TestFs.tga_bytes(Vector2i(2, 2), Color.BLUE)},
		{"name": "ember.tga", "bytes": TestFs.tga_bytes(Vector2i(2, 2), Color.BLUE)},
	])
	var particle := ResourceRoot.TEXTURE_LOADER_PARTICLE
	assert_eq(_pixel(root.load_texture("spark.tga", particle)), Color.RED,
			"the loose tga folder wins without /d")
	assert_eq(_pixel(root.load_texture("ember.tga", particle)), Color.BLUE,
			"a graphic not in the folder reads the mounted file")