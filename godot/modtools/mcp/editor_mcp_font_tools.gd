class_name EditorMcpFontTools
extends RefCounted

## Fonts-workspace MCP tools: mint a .fnt, rasterize a typeface into it, and save it under an
## exact filename.
##
## The game dir is filename-addressed — retail loads its boot faces by the hardcoded names in
## HUD_InitAllFonts (Arial12b/14n/14b/16n/16b, Impac22b, Impac38b) and a missing one is a null
## slot that draws nothing at all [orig: HUD_InitAllFonts @ 0x51ee20 -> HUD_LoadFontIntoSlot
## @ 0x580400] — so authoring a face means landing it on one of those names, which is what
## font_save_as does.
##
## Every mutation routes through FontsEditorWorkspace's own seams (generate_from_font_source /
## set_glyph_spacing / save_as_file), the same ones the Generate dialog and the Save As flow
## use. An agent cannot author a font a human could not.
##
## Rasterization needs a live text server (TextServerManager's primary interface renders the
## glyph bitmaps), so these are dead under --headless, where the dummy driver returns nothing.

var service: Node


func _init(mcp_service: Node) -> void:
	service = mcp_service


func register_all(registry: McpToolRegistry) -> void:
	registry.register(McpToolDef.make("font_new",
			"Start a fresh empty .fnt in the Fonts workspace: one 256x256 page, 224 glyph slots (bytes 32..255), every rect 0x0. It is valid but draws nothing — follow with font_generate to fill it.",
			{}), Callable(self, "_tool_new"))
	registry.register(McpToolDef.make("font_generate",
			"Rasterize a typeface into the Fonts workspace document, replacing all 224 glyphs. source is either an installed system font's family name (font_list_system lists them) or a path to a .ttf/.otf/.ttc; an unknown family is an error rather than a silent substitution. px_size is the pixel height. bold/italic/outline are style flags. Each glyph's cell is its ADVANCE width, not its ink box, because the format has no advance table — retail advances by the rect width plus spacing minus one. The result is unsaved and has no path: call font_save_as.",
			{
				"source": { "type": "string", "description": "System font family name, or a .ttf/.otf/.ttc path." },
				"px_size": { "type": "integer", "description": "Pixel height to rasterize at." },
				"bold": { "type": "boolean", "default": false },
				"italic": { "type": "boolean", "default": false },
				"outline": { "type": "boolean", "default": false },
			}, ["source", "px_size"]), Callable(self, "_tool_generate"))
	registry.register(McpToolDef.make("font_set_spacing",
			"Set the font-wide inter-glyph spacing (the header +12 word). Retail advances the text cursor by each glyph's rect width plus this value, minus one. Rasterized faces bake the true advance into their rects and use 0; retail's own fonts ship 0 and -3. Negative tightens.",
			{
				"spacing": { "type": "integer" },
			}, ["spacing"]), Callable(self, "_tool_set_spacing"))
	registry.register(McpToolDef.make("font_save_as",
			"Write the Fonts workspace document to an exact absolute .fnt path and adopt it. Use the retail boot names verbatim when authoring the boot set — Arial12b.fnt, Arial14n.fnt, Arial14b.fnt, Arial16n.fnt, Arial16b.fnt, Impac22b.fnt, Impac38b.fnt — since the engine looks them up by name.",
			{
				"path": { "type": "string", "description": "Absolute path ending in .fnt." },
			}, ["path"]), Callable(self, "_tool_save_as"))
	registry.register(McpToolDef.make("font_state",
			"Read the Fonts workspace document: current path, unsaved state, page/glyph counts, spacing, how many glyphs actually have a non-empty rect, and the rects of a few sample characters. The samples are the quick correctness check — space must be non-zero (a 0x0 space advances by -1 and runs words together) and widths must vary between narrow and wide letters.",
			{
				"sample": { "type": "string", "description": "Characters to report rects for. Default \" iW0Ag\"." },
			}, [], false), Callable(self, "_tool_state"))
	registry.register(McpToolDef.make("font_list_system",
			"List the font families installed on this machine, for font_generate's source. contains filters case-insensitively.",
			{
				"contains": { "type": "string" },
			}, [], false), Callable(self, "_tool_list_system"))


# --- shared guard -----------------------------------------------------------------

# The Fonts workspace. It exists from editor boot (all workspaces are constructed eagerly)
# and its document is built in _init, so none of these tools need the viewport mounted.
func _require_fonts(ctx: McpToolContext) -> Dictionary:
	var ws: Variant = ctx.workspace("fonts")
	if ws == null:
		return { "error": "Fonts workspace unavailable." }
	return { "ws": ws }


# --- tools ------------------------------------------------------------------------

func _tool_new(_args: Dictionary, ctx: McpToolContext) -> Variant:
	var gate := _require_fonts(ctx)
	if gate.has("error"):
		return McpToolResult.error(gate["error"])
	var ws: Variant = gate["ws"]
	var err: int = ws.new_current()
	if err != OK:
		return McpToolResult.error("Could not start a new font: %s" % error_string(err))
	return _state_of(ws, " iW0Ag")


func _tool_generate(args: Dictionary, ctx: McpToolContext) -> Variant:
	var gate := _require_fonts(ctx)
	if gate.has("error"):
		return McpToolResult.error(gate["error"])
	var ws: Variant = gate["ws"]
	var source := String(args.get("source", "")).strip_edges()
	if source.is_empty():
		return McpToolResult.error("source is required: a system font family name or a .ttf/.otf/.ttc path.")
	var px_size := int(args.get("px_size", 0))
	if px_size < 6 or px_size > 128:
		return McpToolResult.error("px_size must be between 6 and 128 (got %d)." % px_size)
	var flags := 0
	if bool(args.get("bold", false)):
		flags |= FntRasterizer.FLAG_BOLD
	if bool(args.get("italic", false)):
		flags |= FntRasterizer.FLAG_ITALIC
	if bool(args.get("outline", false)):
		flags |= FntRasterizer.FLAG_OUTLINE

	ctx.status("Rasterizing %s at %d px" % [source, px_size])
	var err: int = ws.generate_from_font_source(source, px_size, flags)
	if err == ERR_FILE_NOT_FOUND:
		return McpToolResult.error(
				"No font matched '%s'. Give an installed family name (font_list_system) or an existing .ttf/.otf/.ttc path." % source)
	if err != OK:
		return McpToolResult.error(
				"Rasterization failed (%s). A live text server is required — this does not work headless — and very large sizes can overflow the 16-page limit."
				% error_string(err))
	var state := _state_of(ws, String(args.get("sample", " iW0Ag")))
	state["source"] = source
	state["px_size"] = px_size
	return state


func _tool_set_spacing(args: Dictionary, ctx: McpToolContext) -> Variant:
	var gate := _require_fonts(ctx)
	if gate.has("error"):
		return McpToolResult.error(gate["error"])
	var ws: Variant = gate["ws"]
	var err: int = ws.set_glyph_spacing(int(args.get("spacing", 0)))
	if err != OK:
		return McpToolResult.error("No font open to set spacing on — font_new or font_generate first.")
	return _state_of(ws, " iW0Ag")


func _tool_save_as(args: Dictionary, ctx: McpToolContext) -> Variant:
	var gate := _require_fonts(ctx)
	if gate.has("error"):
		return McpToolResult.error(gate["error"])
	var ws: Variant = gate["ws"]
	var path := String(args.get("path", "")).strip_edges()
	if path.is_empty() or not path.is_absolute_path():
		return McpToolResult.error("path must be an absolute file path.")
	if path.get_extension().to_lower() != "fnt":
		return McpToolResult.error("path must end in .fnt (got '%s')." % path.get_file())
	var err: int = ws.save_as_file(path)
	if err != OK:
		return McpToolResult.error("Save failed: %s" % error_string(err))
	ctx.status("Wrote %s" % path)
	var state := _state_of(ws, " iW0Ag")
	state["bytes"] = FileAccess.get_file_as_bytes(path).size()
	return state


func _tool_state(args: Dictionary, ctx: McpToolContext) -> Variant:
	var gate := _require_fonts(ctx)
	if gate.has("error"):
		return McpToolResult.error(gate["error"])
	return _state_of(gate["ws"], String(args.get("sample", " iW0Ag")))


func _tool_list_system(args: Dictionary, _ctx: McpToolContext) -> Variant:
	var filter := String(args.get("contains", "")).strip_edges().to_lower()
	var names := PackedStringArray()
	for family in OS.get_system_fonts():
		if filter.is_empty() or String(family).to_lower().contains(filter):
			names.append(String(family))
	return { "count": names.size(), "families": names }


# --- state readout ----------------------------------------------------------------

# The metrics that decide whether retail will draw this font correctly, cheap enough to return
# from every tool: the sample rects are the quick check that space has a real cell and that
# widths are proportional rather than uniform.
func _state_of(ws: Variant, sample: String) -> Dictionary:
	var path := String(ws.get_current_resource_path())
	var out := {
		"path": path,
		"name": path.get_file() if not path.is_empty() else "(untitled)",
		"unsaved": bool(ws.has_unsaved_changes()),
		"summary": String(ws.get_status_context()),
	}
	var res: FntResource = ws.get_font_resource()
	if res == null:
		out["loaded"] = false
		return out
	out["loaded"] = true
	out["pages"] = res.get_page_count()
	out["glyphs"] = res.get_glyph_count()
	out["glyph_spacing"] = res.get_glyph_spacing()
	var first: int = res.get_first_char()
	var drawn := 0
	var heights := {}
	for i in range(res.get_glyph_count()):
		var r: Rect2i = res.get_glyph_rect(first + i)
		if r.size.x > 0 and r.size.y > 0:
			drawn += 1
			heights[r.size.y] = int(heights.get(r.size.y, 0)) + 1
	out["drawn"] = drawn
	out["cell_heights"] = heights
	var rects := {}
	for ch in sample:
		var code := ch.unicode_at(0)
		if code < first or code >= first + res.get_glyph_count():
			continue
		var r: Rect2i = res.get_glyph_rect(code)
		rects[ch] = { "page": res.get_glyph_page(code), "w": r.size.x, "h": r.size.y }
	out["sample_rects"] = rects
	return out
