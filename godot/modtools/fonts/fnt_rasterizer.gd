class_name FntRasterizer
extends RefCounted

# Rasterizes a TTF/OTF or system Font into a NovaFntResource (FntMaker-style).
# Editor-only: uses the TextServer glyph cache, which renders glyph bitmaps when a
# real text server is active (i.e. in the running editor, not headless dummy mode).
# Pure-data packing is deterministic and lives in _pack().

const FIRST_CHAR := 32
const GLYPH_COUNT := 224
const TEX := 256
const PAD := 1
const MAX_PAGES := 16

const FLAG_BOLD := 1
const FLAG_ITALIC := 2
const FLAG_OUTLINE := 4
const FLAG_SHADOW := 8


# Builds a fresh NovaFntResource from a font, or null on failure.
static func rasterize(font: Font, px_size: int, flags: int) -> NovaFntResource:
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
	var size_v := Vector2i(px_size, 0)
	var tex_cache := {}
	var cells: Array = []
	for i in range(GLYPH_COUNT):
		cells.append(_rasterize_glyph(ts, rid, px_size, size_v, FIRST_CHAR + i, ascent, tex_cache, flags))

	var packed := _pack(cells)
	if packed.is_empty():
		return null
	var rects: Array = packed["rects"]
	var page_count: int = maxi(1, int(packed["pages"]))

	var res := NovaFntResource.new()
	var shadow := -3 if (flags & FLAG_SHADOW) != 0 else 0
	if res.create_blank(page_count, shadow) != OK:
		return null

	var page_imgs: Array = []
	for p in range(page_count):
		var img := Image.create(TEX, TEX, false, Image.FORMAT_RGBA8)
		img.fill(Color(1.0, 1.0, 1.0, 0.0))
		page_imgs.append(img)

	for i in range(GLYPH_COUNT):
		var code := FIRST_CHAR + i
		var cell: Dictionary = cells[i]
		var r: Dictionary = rects[i]
		if cell.is_empty() or r.is_empty():
			res.set_glyph_rect(code, 0, Rect2i(0, 0, 0, 0))
			continue
		var glyph_img: Image = cell["img"]
		var page: int = r["page"]
		var dst := Vector2i(int(r["x"]), int(r["y"]))
		(page_imgs[page] as Image).blit_rect(glyph_img, Rect2i(0, 0, int(r["w"]), int(r["h"])), dst)
		res.set_glyph_rect(code, page, Rect2i(int(r["x"]), int(r["y"]), int(r["w"]), int(r["h"])))

	for p in range(page_count):
		res.set_page_image(p, page_imgs[p])

	return res


static func _rasterize_glyph(ts: TextServer, rid: RID, size_i: int, size_v: Vector2i, code: int, ascent: float, tex_cache: Dictionary, flags: int) -> Dictionary:
	var gi := ts.font_get_glyph_index(rid, size_i, code, 0)
	if gi == 0:
		return {}
	ts.font_render_glyph(rid, size_v, gi)
	var tex_idx := ts.font_get_glyph_texture_idx(rid, size_v, gi)
	if tex_idx < 0:
		return {}
	var uv := ts.font_get_glyph_uv_rect(rid, size_v, gi)
	var gw := int(round(uv.size.x))
	var gh := int(round(uv.size.y))
	if gw <= 0 or gh <= 0:
		return {}
	var off := ts.font_get_glyph_offset(rid, size_v, gi)
	if not tex_cache.has(tex_idx):
		tex_cache[tex_idx] = ts.font_get_texture_image(rid, size_v, tex_idx)
	var atlas: Image = tex_cache[tex_idx]
	if atlas == null:
		return {}
	var region := Rect2i(int(round(uv.position.x)), int(round(uv.position.y)), gw, gh)
	region = region.intersection(Rect2i(0, 0, atlas.get_width(), atlas.get_height()))
	if region.size.x <= 0 or region.size.y <= 0:
		return {}
	var cov := atlas.get_region(region)
	var cw := cov.get_width()
	var ch := cov.get_height()
	var uses_alpha := _uses_alpha(cov)
	var top_pad := maxi(0, int(round(ascent + off.y)))
	var cell_h := top_pad + ch
	if cell_h > TEX:
		cell_h = TEX
	var cell := Image.create(cw, cell_h, false, Image.FORMAT_RGBA8)
	cell.fill(Color(1.0, 1.0, 1.0, 0.0))
	for yy in range(ch):
		var ty := top_pad + yy
		if ty >= cell_h:
			break
		for xx in range(cw):
			var c := cov.get_pixel(xx, yy)
			var a: float = c.a if uses_alpha else maxf(c.r, maxf(c.g, c.b))
			if a > 0.0:
				cell.set_pixel(xx, ty, Color(1.0, 1.0, 1.0, a))
	if (flags & FLAG_OUTLINE) != 0:
		cell = _dilate(cell)
	return {"img": cell, "w": cw, "h": cell_h}


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


# Deterministic shelf packer. cells: per-glyph {img,w,h} or {} for empty.
# Returns { rects: Array of {page,x,y,w,h} ({} for empty), pages: int } or {} on overflow.
static func _pack(cells: Array) -> Dictionary:
	var rects: Array = []
	var page := 0
	var cx := PAD
	var cy := PAD
	var shelf_h := 0
	for cell in cells:
		var c: Dictionary = cell
		if c.is_empty():
			rects.append({})
			continue
		var w: int = mini(int(c["w"]), TEX - 2 * PAD)
		var h: int = mini(int(c["h"]), TEX - 2 * PAD)
		if cx + w + PAD > TEX:
			cx = PAD
			cy += shelf_h + PAD
			shelf_h = 0
		if cy + h + PAD > TEX:
			page += 1
			cx = PAD
			cy = PAD
			shelf_h = 0
			if page >= MAX_PAGES:
				return {}
		rects.append({"page": page, "x": cx, "y": cy, "w": w, "h": h})
		cx += w + PAD
		shelf_h = maxi(shelf_h, h)
	return {"rects": rects, "pages": page + 1}
