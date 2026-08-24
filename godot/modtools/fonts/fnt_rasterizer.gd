class_name FntRasterizer
extends RefCounted

# Rasterizes a TTF/OTF or system Font into a FntResource (FntMaker-style).
# Editor-only: uses the TextServer glyph cache, which renders glyph bitmaps when a
# real text server is active (i.e. in the running editor, not headless dummy mode).
# The deterministic shelf packing lives in engine/formats/fnt (fnt_pack_shelf) behind
# FntResource.pack_shelf; this script only rasterizes and blits.

# Format facts are aliases of the FntResource binding (engine/formats/fnt) — ENG-4.
const FIRST_CHAR := FntResource.FIRST_CHAR
const GLYPH_COUNT := FntResource.GLYPH_COUNT
const TEX := FntResource.TEXTURE_WIDTH  # pages are square (== TEXTURE_HEIGHT)

const FLAG_BOLD := 1
const FLAG_ITALIC := 2
const FLAG_OUTLINE := 4

# There is no FLAG_SHADOW: it used to write -3 into the header +12 word on the belief
# that the word was a shadow offset. It is the inter-glyph SPACING term -- the drawer's
# shadow comes from a format flag and fixed sub-pixel offsets, not from the font
# [orig: CGameFont_DrawText @ 0x6752c0]. Spacing is a font-wide property the editor
# edits directly; it is not a rasterization flag.


## Build a Font from a source string: a .ttf/.otf/.ttc path, or the family name of an
## installed system font. Null when neither resolves.
##
## SystemFont silently substitutes a default when the family is missing, which would hand back
## the wrong typeface with no error, so the name is checked against the installed set first.
## Shared by the Generate dialog and the workspace's scripted entry so the UI and the MCP
## construct fonts through one path.
static func build_font_source(source: String) -> Font:
	var clean := source.strip_edges()
	if clean.is_empty():
		return null
	if clean.get_extension().to_lower() in ["ttf", "otf", "ttc"]:
		if not FileAccess.file_exists(clean):
			return null
		var ff := FontFile.new()
		return ff if ff.load_dynamic_font(clean) == OK else null
	var wanted := clean.to_lower()
	for installed in OS.get_system_fonts():
		if String(installed).to_lower() == wanted:
			var sf := SystemFont.new()
			sf.font_names = PackedStringArray([String(installed)])
			return sf
	return null


# Builds a fresh FntResource from a font, or null on failure.
static func rasterize(font: Font, px_size: int, flags: int) -> FntResource:
	if font == null or px_size <= 0:
		return null
	var ts := TextServerManager.get_primary_interface()
	if ts == null:
		return null
	var rids: Array = font.get_rids()
	if rids.is_empty():
		return null
	var rid: RID = rids[0]
	if not rid.is_valid():
		return null

	if (flags & FLAG_BOLD) != 0:
		ts.font_set_embolden(rid, 0.07)
	if (flags & FLAG_ITALIC) != 0:
		ts.font_set_transform(rid, Transform2D(Vector2(1.0, 0.0), Vector2(0.22, 1.0), Vector2(0.0, 0.0)))

	var ascent := ts.font_get_ascent(rid, px_size)
	# One cell height for the whole font, so baselines line up and descenders are not
	# clipped by a short cell. Retail's own fonts are uniform this way -- every glyph in
	# Gunpl22b is 19 px, every glyph in Serpen24 is 17.
	var line_h := int(ceil(ascent + ts.font_get_descent(rid, px_size)))
	if line_h <= 0:
		line_h = px_size
	line_h = mini(line_h, TEX)
	var size_v := Vector2i(px_size, 0)
	var tex_cache := {}
	var cells: Array = []
	for i in range(GLYPH_COUNT):
		cells.append(_rasterize_glyph(ts, rid, px_size, size_v, FIRST_CHAR + i, ascent, line_h, tex_cache, flags))

	var sizes := PackedInt32Array()
	for cell in cells:
		var c: Dictionary = cell
		sizes.append(0 if c.is_empty() else int(c["w"]))
		sizes.append(0 if c.is_empty() else int(c["h"]))
	var rects := FntResource.pack_shelf(sizes)
	if rects.is_empty():
		return null
	var page_count := 1
	for i in range(GLYPH_COUNT):
		if rects[i * 5 + 3] > 0:
			page_count = maxi(page_count, rects[i * 5] + 1)

	var res := FntResource.new()
	# Spacing 0: the cells already carry each glyph's true advance (see _rasterize_glyph),
	# and retail advances by rect_width + (spacing - 1), so 0 is what makes the two agree.
	if res.create_blank(page_count, 0) != OK:
		return null

	var page_imgs: Array = []
	for p in range(page_count):
		var img := Image.create(TEX, TEX, false, Image.FORMAT_RGBA8)
		img.fill(Color(1.0, 1.0, 1.0, 0.0))
		page_imgs.append(img)

	for i in range(GLYPH_COUNT):
		var code := FIRST_CHAR + i
		var cell: Dictionary = cells[i]
		var rw := rects[i * 5 + 3]
		var rh := rects[i * 5 + 4]
		if cell.is_empty() or rw <= 0:
			res.set_glyph_rect(code, 0, Rect2i(0, 0, 0, 0))
			continue
		var glyph_img: Image = cell["img"]
		var page := rects[i * 5]
		var rx := rects[i * 5 + 1]
		var ry := rects[i * 5 + 2]
		(page_imgs[page] as Image).blit_rect(glyph_img, Rect2i(0, 0, rw, rh), Vector2i(rx, ry))
		res.set_glyph_rect(code, page, Rect2i(rx, ry, rw, rh))

	for p in range(page_count):
		res.set_page_image(p, page_imgs[p])

	return res


# Rasterize one glyph into an ADVANCE-WIDE cell.
#
# The .fnt format carries no advance table: retail advances the cursor by
# `rect_width + (glyph_spacing - 1) * scale` and measures the same way
# [orig: CGameFont_MeasureText @ 0x674e70; CGameFont_DrawText @ 0x6752c0], so the glyph RECT
# is the advance. Retail's shipped fonts confirm it -- Serpen24 gives 'i' 6 px and 'W' 17 px,
# which are advances, not ink extents. Emitting the ink's bounding box instead (what this did
# before) drops every side bearing and draws each glyph hard against its neighbour.
static func _rasterize_glyph(ts: TextServer, rid: RID, size_i: int, size_v: Vector2i, code: int, ascent: float, line_h: int, tex_cache: Dictionary, flags: int) -> Dictionary:
	var gi := ts.font_get_glyph_index(rid, size_i, code, 0)
	if gi == 0:
		# The face has no glyph for this codepoint. Returning nothing lands it a 0x0 rect, which
		# retail draws as a -1 advance -- the next character backs up onto the previous one. Give
		# it the blank cell a space gets instead, sized by the face's .notdef advance (glyph 0)
		# or, when the face reports none, its space.
		var fallback_w := int(ceil(ts.font_get_glyph_advance(rid, size_i, 0).x))
		if fallback_w <= 0:
			var space_gi := ts.font_get_glyph_index(rid, size_i, 0x20, 0)
			if space_gi != 0:
				fallback_w = int(ceil(ts.font_get_glyph_advance(rid, size_i, space_gi).x))
		if fallback_w <= 0:
			return {}
		var blank_w := mini(fallback_w + 1, TEX)
		var blank_cell := Image.create(blank_w, line_h, false, Image.FORMAT_RGBA8)
		blank_cell.fill(Color(1.0, 1.0, 1.0, 0.0))
		return {"img": blank_cell, "w": blank_w, "h": line_h}
	# +1 because the font emits spacing 0 and retail subtracts one from every advance.
	var cell_w := int(ceil(ts.font_get_glyph_advance(rid, size_i, gi).x)) + 1

	var cov: Image = null
	var left_pad := 0
	var top_pad := 0
	ts.font_render_glyph(rid, size_v, gi)
	var tex_idx := ts.font_get_glyph_texture_idx(rid, size_v, gi)
	if tex_idx >= 0:
		var uv := ts.font_get_glyph_uv_rect(rid, size_v, gi)
		var gw := int(round(uv.size.x))
		var gh := int(round(uv.size.y))
		if gw > 0 and gh > 0:
			if not tex_cache.has(tex_idx):
				tex_cache[tex_idx] = ts.font_get_texture_image(rid, size_v, tex_idx)
			var atlas: Image = tex_cache[tex_idx]
			if atlas != null:
				var region := Rect2i(int(round(uv.position.x)), int(round(uv.position.y)), gw, gh)
				region = region.intersection(Rect2i(0, 0, atlas.get_width(), atlas.get_height()))
				if region.size.x > 0 and region.size.y > 0:
					cov = atlas.get_region(region)
					var off := ts.font_get_glyph_offset(rid, size_v, gi)
					left_pad = maxi(0, int(round(off.x)))
					top_pad = maxi(0, int(round(ascent + off.y)))

	# A glyph with no ink -- space, and anything the face draws blank -- still needs a cell, or
	# it lands a 0x0 rect and advances by -1, running words together. Retail sizes its space
	# like any other glyph (9x19 in Gunpl22b, 7x17 in Serpen24).
	if cov == null:
		if cell_w <= 0:
			return {}
		cell_w = mini(cell_w, TEX)
		var blank := Image.create(cell_w, line_h, false, Image.FORMAT_RGBA8)
		blank.fill(Color(1.0, 1.0, 1.0, 0.0))
		return {"img": blank, "w": cell_w, "h": line_h}

	var cw := cov.get_width()
	var ch := cov.get_height()
	var uses_alpha := _uses_alpha(cov)
	# The cell holds the advance, but never less than the ink it has to contain.
	cell_w = mini(maxi(cell_w, left_pad + cw), TEX)
	var cell_h := mini(maxi(line_h, top_pad + ch), TEX)
	var cell := Image.create(cell_w, cell_h, false, Image.FORMAT_RGBA8)
	cell.fill(Color(1.0, 1.0, 1.0, 0.0))
	for yy in range(ch):
		var ty := top_pad + yy
		if ty >= cell_h:
			break
		for xx in range(cw):
			var tx := left_pad + xx
			if tx >= cell_w:
				break
			var c := cov.get_pixel(xx, yy)
			var a: float = c.a if uses_alpha else maxf(c.r, maxf(c.g, c.b))
			if a > 0.0:
				cell.set_pixel(tx, ty, Color(1.0, 1.0, 1.0, a))
	if (flags & FLAG_OUTLINE) != 0:
		cell = _dilate(cell)
	return {"img": cell, "w": cell_w, "h": cell_h}


static func _uses_alpha(img: Image) -> bool:
	var w := img.get_width()
	var h := img.get_height()
	for y in range(h):
		for x in range(w):
			if img.get_pixel(x, y).a < 0.99:
				return true
	return false


static func _dilate(img: Image) -> Image:
	var w := img.get_width()
	var h := img.get_height()
	var out := Image.create(w, h, false, Image.FORMAT_RGBA8)
	out.fill(Color(1.0, 1.0, 1.0, 0.0))
	for y in range(h):
		for x in range(w):
			var m := 0.0
			for dy in range(-1, 2):
				for dx in range(-1, 2):
					var nx := x + dx
					var ny := y + dy
					if nx >= 0 and ny >= 0 and nx < w and ny < h:
						m = maxf(m, img.get_pixel(nx, ny).a)
			if m > 0.0:
				out.set_pixel(x, y, Color(1.0, 1.0, 1.0, m))
	return out
