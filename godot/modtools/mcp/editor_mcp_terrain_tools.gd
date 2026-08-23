class_name EditorMcpTerrainTools
extends RefCounted

## Terrain-workspace MCP tools: read the loaded terrain, shape it with the same brushes the
## viewport drives, and export it.
##
## terrain_brush routes through TerrainEditorBrushSession's own interactive path --
## begin_brush_drag / apply_brush_stroke / end_brush_drag -- which already takes a WORLD-space
## position rather than a viewport raycast. So a dab placed here is byte-for-byte the dab a
## human places by dragging, undo entry included; there is no second brush implementation.
##
## Open a terrain first with open_in_workspace(workspace="terrain", path="<name>.trn"), or mint
## one with terrain_new.

var service: Node


func _init(mcp_service: Node) -> void:
	service = mcp_service


func register_all(registry: McpToolRegistry) -> void:
	registry.register(McpToolDef.make("terrain_state",
			"Read the loaded terrain: name, source path, dirty state, the 1024x1024 source atlas, height statistics over the active region, colormap/blendmap dimensions, which texture slots carry an image (slots with none are SKIPPED on export), the sector grid, and the CDEP violation count. Read-only, and the first call to make before shaping anything.",
			{
				"height_samples": { "type": "integer", "description": "Grid resolution to sample heights on. Default 33." },
			}, [], false), Callable(self, "_tool_state"))
	registry.register(McpToolDef.make("terrain_brush",
			"Apply brush dabs at WORLD x/z, through the same session the viewport drags. tool is raise, lower, smooth, flatten, or color. radius is in source-atlas pixels; strength 0..1. dabs>1 walks a straight line from (x,z) to (x2,z2), which is how a drag is modelled. A colour dab needs color as [r,g,b] 0..1. The whole call is ONE undo step. Heights are re-clamped against the CDEP block constraints, as the interactive brush does.",
			{
				"tool": { "type": "string", "description": "raise | lower | smooth | flatten | color" },
				"x": { "type": "number" },
				"z": { "type": "number" },
				"x2": { "type": "number", "description": "End of a drag; defaults to x." },
				"z2": { "type": "number", "description": "End of a drag; defaults to z." },
				"radius": { "type": "number", "description": "Atlas pixels. Default 32." },
				"strength": { "type": "number", "description": "0..1. Default 0.5." },
				"hardness": { "type": "number", "description": "0..1 edge falloff. Default 0.5." },
				"amount": { "type": "number", "description": "Dab weight, the delta the viewport supplies per frame. Default 1.0." },
				"dabs": { "type": "integer", "description": "Dabs along the line. Default 1." },
				"color": { "type": "array", "items": { "type": "number" }, "description": "[r,g,b] 0..1, for tool=color." },
			}, ["tool", "x", "z"]), Callable(self, "_tool_brush"))
	registry.register(McpToolDef.make("terrain_fill",
			"Flood one whole atlas map with a single value as one undo step: target=color fills the colormap with [r,g,b], target=height levels every height to `height`. This is the base wash you paint on top of, and the only way to change a map's resolution -- a colormap loaded at less than the source atlas size is re-created at full size, which matters because retail quadrant-splits the colormap into four 512x512 tiles and a smaller one cannot split meaningfully.",
			{
				"target": { "type": "string", "description": "color | height" },
				"color": { "type": "array", "items": { "type": "number" }, "description": "[r,g,b] 0..1 for target=color." },
				"height": { "type": "number", "description": "World height for target=height." },
			}, ["target"]), Callable(self, "_tool_fill"))
	registry.register(McpToolDef.make("terrain_export",
			"Export the loaded terrain into out_dir in the retail JO flavour: the .trn, the baked .cpt polydata, and every texture slot that carries an image. Runs to completion before returning. Slots with no image are skipped, so check terrain_state's slots against what the .trn references first.",
			{
				"out_dir": { "type": "string", "description": "Absolute directory." },
				"flavor": { "type": "string", "description": "dfx_jo (default) or the project flavour." },
			}, ["out_dir"]), Callable(self, "_tool_export"))


# --- shared guard -----------------------------------------------------------------

func _require_terrain(ctx: McpToolContext) -> Dictionary:
	var ws: Variant = ctx.workspace("terrain")
	if ws == null:
		return { "error": "Terrain workspace unavailable." }
	var editor: Variant = ws.terrain_editor
	if editor == null:
		return { "error": "The terrain editor is not mounted yet." }
	if editor.is_export_running():
		return { "error": "An export is still running; wait for it to finish." }
	return { "ws": ws, "editor": editor }


# --- tools ------------------------------------------------------------------------

func _tool_state(args: Dictionary, ctx: McpToolContext) -> Variant:
	var gate := _require_terrain(ctx)
	if gate.has("error"):
		return McpToolResult.error(gate["error"])
	return _state_of(gate["ws"], gate["editor"], clampi(int(args.get("height_samples", 33)), 3, 129))


func _tool_brush(args: Dictionary, ctx: McpToolContext) -> Variant:
	var gate := _require_terrain(ctx)
	if gate.has("error"):
		return McpToolResult.error(gate["error"])
	var editor: Variant = gate["editor"]
	var session: Variant = editor._brush_session

	var tool_name := String(args.get("tool", "")).strip_edges().to_lower()
	var tool_id := _tool_id(tool_name)
	if tool_id < 0:
		return McpToolResult.error("tool must be raise, lower, smooth, flatten or color (got '%s')." % tool_name)

	if tool_name == "color":
		var rgb: Variant = args.get("color", null)
		if not (rgb is Array) or (rgb as Array).size() < 3:
			return McpToolResult.error("tool=color needs color as [r,g,b] with values 0..1.")
		var a := rgb as Array
		session.paint_color = Color(
				clampf(float(a[0]), 0.0, 1.0),
				clampf(float(a[1]), 0.0, 1.0),
				clampf(float(a[2]), 0.0, 1.0), 1.0)

	var x := float(args.get("x", 0.0))
	var z := float(args.get("z", 0.0))
	var x2 := float(args.get("x2", x))
	var z2 := float(args.get("z2", z))
	var dabs := clampi(int(args.get("dabs", 1)), 1, 512)

	session.current_tool = tool_id
	session.brush_radius = clampf(float(args.get("radius", 32.0)), 1.0, 512.0)
	session.brush_strength = clampf(float(args.get("strength", 0.5)), 0.0, 1.0)
	session.brush_hardness = clampf(float(args.get("hardness", 0.5)), 0.0, 1.0)
	var amount := clampf(float(args.get("amount", 1.0)), -10.0, 10.0)

	var source: Image = editor._editable_image_for_kind(session.history_kind_for_tool(tool_id))
	session.begin_brush_drag(source, false)
	var changed := { "changed_heightmap": false, "changed_blendmap": false, "changed_colormap": false }
	for i in range(dabs):
		var t: float = 0.0 if dabs == 1 else float(i) / float(dabs - 1)
		var hit := Vector3(lerpf(x, x2, t), 0.0, lerpf(z, z2, t))
		var r: Dictionary = session.apply_brush_stroke(amount, hit, true, editor.terrain_mesh, editor._data)
		for key in changed:
			changed[key] = bool(changed[key]) or bool(r.get(key, false))
	session.end_brush_drag(source)

	if not (changed["changed_heightmap"] or changed["changed_blendmap"] or changed["changed_colormap"]):
		return McpToolResult.error(
				"The dab landed outside every active sector — nothing was painted. terrain_state's sector grid and origin say where the authored region is.")
	editor.invalidate_after_brush_run(changed)
	ctx.status("%s x%d at (%.0f, %.0f)" % [tool_name, dabs, x, z])
	var state := _state_of(gate["ws"], editor, 17)
	state["applied"] = changed
	return state


func _tool_fill(args: Dictionary, ctx: McpToolContext) -> Variant:
	var gate := _require_terrain(ctx)
	if gate.has("error"):
		return McpToolResult.error(gate["error"])
	var editor: Variant = gate["editor"]
	var target := String(args.get("target", "")).strip_edges().to_lower()

	if target == "height":
		if not args.has("height"):
			return McpToolResult.error("target=height needs a height value.")
		var h := float(args["height"])
		var ok: bool = editor.apply_bulk_edit(TerrainEditHistory.Kind.HEIGHTMAP,
				func(image: Image) -> Rect2i:
					image.fill(Color(h, 0.0, 0.0, 1.0))
					return Rect2i(0, 0, image.get_width(), image.get_height()))
		if not ok:
			return McpToolResult.error("The height fill did not apply.")
		return _state_of(gate["ws"], editor, 17)

	if target != "color":
		return McpToolResult.error("target must be color or height (got '%s')." % target)

	var rgb: Variant = args.get("color", null)
	if not (rgb is Array) or (rgb as Array).size() < 3:
		return McpToolResult.error("target=color needs color as [r,g,b] with values 0..1.")
	var a := rgb as Array
	var col := Color(clampf(float(a[0]), 0.0, 1.0), clampf(float(a[1]), 0.0, 1.0),
			clampf(float(a[2]), 0.0, 1.0), 1.0)

	# A colormap smaller than the source atlas cannot be quadrant-split into the four 512x512
	# tiles retail expects, so a fill is also where an undersized one gets re-created at full
	# resolution. Undo cannot span that (the history stores sub-rects of one image), so the
	# resize is done first, outside the undoable step.
	var resized := false
	var current: Image = editor._colormap_image
	if current == null or current.get_width() < TerrainData.ATLAS_SIZE:
		var fresh := Image.create(TerrainData.ATLAS_SIZE, TerrainData.ATLAS_SIZE, false, Image.FORMAT_RGB8)
		fresh.fill(col)
		editor._document.set_colormap_image(editor._get_material(), fresh)
		editor.is_dirty = true
		resized = true
	else:
		var ok2: bool = editor.apply_bulk_edit(TerrainEditHistory.Kind.COLORMAP,
				func(image: Image) -> Rect2i:
					image.fill(col)
					return Rect2i(0, 0, image.get_width(), image.get_height()))
		if not ok2:
			return McpToolResult.error("The colour fill did not apply.")

	var state := _state_of(gate["ws"], editor, 17)
	state["resized_colormap"] = resized
	return state


func _tool_export(args: Dictionary, ctx: McpToolContext) -> Variant:
	var gate := _require_terrain(ctx)
	if gate.has("error"):
		return McpToolResult.error(gate["error"])
	var editor: Variant = gate["editor"]
	var out_dir := String(args.get("out_dir", "")).strip_edges()
	if out_dir.is_empty() or not out_dir.is_absolute_path():
		return McpToolResult.error("out_dir must be an absolute directory path.")
	var flavor: int = editor.ExportFlavor.DFX_JO
	ctx.status("Exporting terrain to %s" % out_dir)
	var err: int = editor.export_terrain(out_dir, flavor)
	if err != OK:
		return McpToolResult.error("Export failed: %s" % error_string(err))
	var written := PackedStringArray()
	var dir := DirAccess.open(out_dir)
	if dir != null:
		for f in dir.get_files():
			written.append(String(f))
	written.sort()
	return { "out_dir": out_dir, "files": written }


# --- readout ----------------------------------------------------------------------

func _tool_id(name: String) -> int:
	match name:
		"raise": return TerrainEditorBrushSession.Tool.RAISE
		"lower": return TerrainEditorBrushSession.Tool.LOWER
		"smooth": return TerrainEditorBrushSession.Tool.SMOOTH
		"flatten": return TerrainEditorBrushSession.Tool.FLATTEN
		"color": return TerrainEditorBrushSession.Tool.PAINT_COLORMAP
	return -1


func _state_of(ws: Variant, editor: Variant, samples: int) -> Dictionary:
	var out := {
		"terrain_name": String(editor.get_terrain_name_value()),
		"path": String(ws.get_current_resource_path()),
		"unsaved": bool(ws.has_unsaved_changes()),
		"atlas_size": TerrainData.ATLAS_SIZE,
		"height_revision": editor.get_height_revision(),
		"can_undo": bool(editor.can_undo()),
	}
	var hm: Image = editor._heightmap_image
	var cm: Image = editor._colormap_image
	var bm: Image = editor._blendmap_image
	if hm != null:
		out["heightmap"] = "%dx%d" % [hm.get_width(), hm.get_height()]
	if cm != null:
		out["colormap"] = "%dx%d" % [cm.get_width(), cm.get_height()]
		out["colormap_full_size"] = cm.get_width() >= TerrainData.ATLAS_SIZE
	if bm != null:
		out["blendmap"] = "%dx%d" % [bm.get_width(), bm.get_height()]

	# Height statistics over the WORLD extent, sampled through the same live-surface reader the
	# re-ground pass uses -- the baked CPT goes stale the moment a height edit lands.
	var lo := INF
	var hi := -INF
	var total := 0.0
	var count := 0
	var extent: float = float(TerrainData.ATLAS_SIZE) * 0.5
	for iz in range(samples):
		for ix in range(samples):
			var wx: float = lerpf(-extent, extent, float(ix) / float(samples - 1))
			var wz: float = lerpf(-extent, extent, float(iz) / float(samples - 1))
			var h: float = editor.sample_height_world(wx, wz)
			if is_nan(h):
				continue
			lo = minf(lo, h)
			hi = maxf(hi, h)
			total += h
			count += 1
	if count > 0:
		out["height"] = {
			"min": snappedf(lo, 0.01),
			"max": snappedf(hi, 0.01),
			"mean": snappedf(total / float(count), 0.01),
			"relief": snappedf(hi - lo, 0.01),
			"sampled": count,
		}
	if editor._data != null:
		out["cdep_violations"] = editor._data.cdep_count_violations()
	return out
