class_name McpAssetDescribe
extends RefCounted

## Per-format JSON summaries for the describe_asset tool: dispatch on file
## type, load through the same GDExtension classes the workspaces use, and
## return a bounded structure. Kinds without a serializer yet return file
## stats plus an honest pointer at read_file / describe_api instead of
## pretending.
##
## Loads are defensive (errors surfaced via get_last_error) — a bad file
## should produce a useful error string, never a script error.

const TEXT_CLIP := 120


## { name, path, kind, summary, data, truncated? } or { error }. `path` may be
## a bare resource name or absolute path (the shared convention).
static func describe(ctx: McpToolContext, raw_path: String, depth := "summary", limit := 100) -> Dictionary:
	var resolved := resolve(ctx, raw_path)
	if not resolved["ok"]:
		return { "error": resolved["error"] }
	var name := String(resolved["name"])
	var path := String(resolved["path"])
	var full := depth == "full"
	limit = clampi(limit, 1, 2000)
	var kind := _kind_for(ctx, name, path)
	var out := { "name": name, "path": path, "kind": kind }
	match kind:
		"mission":
			_describe_mission(ctx, out, resolved, full, limit)
		"strings":
			_describe_strings(ctx, out, resolved, full, limit)
		"menu":
			_describe_menu(ctx, out, resolved, full, limit)
		"font":
			_describe_font(ctx, out, resolved, full, limit)
		"environment":
			_describe_env(ctx, out, resolved)
		"sound":
			_describe_lwf(ctx, out, resolved, full, limit)
		"object_model":
			_describe_3di(ctx, out, resolved, full, limit)
		"pff":
			_describe_pff(out, path, full, limit)
		_:
			_describe_stats(out, resolved, kind)
	return out


## Resolve a bare name or absolute path against disk and the mounted resource
## root. Returns { ok, name, path, loose: bool } or { ok: false, error }.
static func resolve(ctx: McpToolContext, raw: String) -> Dictionary:
	var clean := raw.strip_edges()
	if clean.is_empty():
		return { "ok": false, "error": "Empty path. Pass a resource name (e.g. \"alpha.bms\") or an absolute path." }
	if FileAccess.file_exists(clean):
		return { "ok": true, "name": clean.get_file(), "path": clean, "loose": true }
	var root: Variant = ctx.root()
	if root != null and root.has_file(clean):
		var resolved := String(root.resolve_file(clean))
		return { "ok": true, "name": clean, "path": resolved if not resolved.is_empty() else clean, "loose": FileAccess.file_exists(resolved) }
	var hint := "No resource directory is mounted — set one in Settings." if root == null \
			else "Not found as an absolute path or in the mounted resource root."
	return { "ok": false, "error": "Could not resolve '%s'. %s List candidates with list_assets." % [clean, hint] }


## Resolve `raw_path` and open it on `loader` (open_file for loose paths,
## open_from_resource_root for archived names) — the shared open path for tools
## that load a document class themselves (analyze_mission). Returns
## { ok, name?, path?, error? }.
static func open_data(loader: Object, ctx: McpToolContext, raw_path: String) -> Dictionary:
	var resolved := resolve(ctx, raw_path)
	if not resolved["ok"]:
		return resolved
	var err := _open_via(loader, ctx, resolved)
	if err != OK:
		var detail := String(loader.get_last_error())
		return { "ok": false, "error": "Failed to open %s (%s). %s" % [resolved["name"], error_string(err), detail] }
	return resolved


## The file's bytes through whichever side resolved it (disk or VFS/PFF).
static func read_bytes(ctx: McpToolContext, resolved: Dictionary) -> PackedByteArray:
	if resolved.get("loose", false):
		return FileAccess.get_file_as_bytes(String(resolved["path"]))
	var root: Variant = ctx.root()
	if root != null:
		return root.read_file(String(resolved["name"]))
	return PackedByteArray()


static func _kind_for(ctx: McpToolContext, name: String, path: String) -> String:
	match name.get_extension().to_lower():
		"bms":
			return "mission"
		"trn":
			return "terrain"
		"tpj":
			return "terrain_project"
		"env":
			return "environment"
		"3di":
			return "object_model"
		"3dp":
			return "object_project"
		"ase":
			return "object_scene"
		"fnt":
			return "font"
		"mnu":
			return "menu"
		"mns":
			return "menu_styles"
		"kda":
			return "credits"
		"sbf":
			return "sbf"
		"lwf":
			return "sound"
		"pff":
			return "pff"
		"bin":
			return _sniff_bin(ctx, name, path)
	return name.get_extension().to_lower()


# .bin is overloaded (RTXT string tables vs SCR0/MU01 music scripts); sniff the
# magic.
static func _sniff_bin(ctx: McpToolContext, name: String, path: String) -> String:
	var head := PackedByteArray()
	if FileAccess.file_exists(path):
		var file := FileAccess.open(path, FileAccess.READ)
		if file != null:
			head = file.get_buffer(4)
			file.close()
	elif ctx.root() != null:
		head = (ctx.root().read_file(name) as PackedByteArray).slice(0, 4)
	var magic := head.get_string_from_ascii()
	if magic == "RTXT":
		return "strings"
	if magic == "SCR0" or magic == "MU01":
		return "music_script"
	return "strings"


static func _describe_mission(ctx: McpToolContext, out: Dictionary, resolved: Dictionary, full: bool, limit: int) -> void:
	var mission := MissionData.new()
	var err := _open_via(mission, ctx, resolved)
	if err != OK:
		out["error"] = "Mission failed to load (%s): %s" % [error_string(err), mission.get_last_error()]
		return
	var entities: Array = mission.get_all_entities()
	var counts := {}
	for entity: Dictionary in entities:
		var label := String(entity.get("kind_label", str(entity.get("kind", "?"))))
		counts[label] = int(counts.get(label, 0)) + 1
	var data := {
		"info": mission.get_info(),
		"entity_counts": counts,
		"entity_total": entities.size(),
	}
	data["waypoints"] = mission.get_waypoint_summaries()
	data["logic"] = mission.get_logic_summary()
	if full:
		data["entities"] = entities.slice(0, limit)
		out["truncated"] = entities.size() > limit
	out["data"] = data
	out["summary"] = "Mission '%s' on terrain %s — %d entities." % [
		mission.get_mission_name(), mission.get_terrain_ref(), entities.size()]


static func _describe_strings(ctx: McpToolContext, out: Dictionary, resolved: Dictionary, full: bool, limit: int) -> void:
	var table := RtxtStringFile.new()
	if table.load_from_byte_array(read_bytes(ctx, resolved)) != OK:
		out["error"] = "RTXT table failed to parse."
		return
	var sections: Array = []
	var section_names: PackedStringArray = table.get_section_names()
	for i in range(section_names.size()):
		sections.append({ "index": i, "name": section_names[i], "count": table.get_section_string_count(i) })
	var data := { "sections": sections, "entry_count": table.get_entry_count() }
	if full:
		var dump := {}
		var keys: PackedStringArray = table.get_keys()
		for i in range(mini(keys.size(), limit)):
			dump[keys[i]] = {
				"text": _clip(table.get_string(keys[i])),
				"section": table.get_section_index_for_key(keys[i]),
			}
		data["entries"] = dump
		out["truncated"] = keys.size() > limit
	out["data"] = data
	out["summary"] = "RTXT string table — %d entries in %d sections." % [table.get_entry_count(), section_names.size()]


static func _describe_menu(ctx: McpToolContext, out: Dictionary, resolved: Dictionary, full: bool, limit: int) -> void:
	var doc := MnuDocument.new()
	if doc.load_from_bytes(read_bytes(ctx, resolved)) != OK:
		out["error"] = "Menu document failed to parse."
		return
	var screens: Array = []
	var budget := { "left": limit }
	for screen_id in doc.get_screen_ids():
		var screen := {
			"id": screen_id,
			"name": doc.get_screen_name(screen_id),
		}
		if full:
			screen["tree"] = _menu_node(doc, doc.get_screen_root_id(screen_id), budget)
		screens.append(screen)
	var data := { "menu_size": doc.get_menu_size(), "screen_count": doc.get_screen_count(), "screens": screens }
	if full and budget["left"] <= 0:
		out["truncated"] = true
	out["data"] = data
	out["summary"] = "Menu — %d screen(s)." % doc.get_screen_count()


static func _menu_node(doc: MnuDocument, id: int, budget: Dictionary) -> Variant:
	if budget["left"] <= 0:
		return "<truncated>"
	budget["left"] = int(budget["left"]) - 1
	var node := {
		"id": id,
		"type": doc.get_widget_type_name(doc.get_widget_type(id)),
		"name": doc.get_widget_name(id),
		"rect": doc.get_window_rect(id),
	}
	var text := String(doc.get_widget_text(id))
	if not text.is_empty():
		node["text"] = _clip(text)
	var children: Array = []
	for child_id in doc.get_child_ids(id):
		children.append(_menu_node(doc, child_id, budget))
	if not children.is_empty():
		node["children"] = children
	return node


static func _describe_font(ctx: McpToolContext, out: Dictionary, resolved: Dictionary, full: bool, limit: int) -> void:
	var font := FntResource.new()
	if font.load_from_bytes(read_bytes(ctx, resolved)) != OK:
		out["error"] = "FNT font failed to parse."
		return
	var pages: Array = []
	for i in range(font.get_page_count()):
		var image: Image = font.get_page_image(i)
		pages.append(image.get_size() if image != null else Vector2i.ZERO)
	var data := {
		"page_count": font.get_page_count(),
		"glyph_count": font.get_glyph_count(),
		"first_char": font.get_first_char(),
		"shadow_offset": font.get_shadow_offset(),
		"page_sizes": pages,
	}
	if full:
		var glyphs: Array = []
		var first := font.get_first_char()
		for code in range(first, first + mini(font.get_glyph_count(), limit)):
			glyphs.append({
				"code": code,
				"char": char(code),
				"page": font.get_glyph_page(code),
				"rect": font.get_glyph_rect(code),
			})
		data["glyphs"] = glyphs
		out["truncated"] = font.get_glyph_count() > limit
	out["data"] = data
	out["summary"] = "FNT bitmap font — %d glyphs on %d page(s) from char %d." % [
		font.get_glyph_count(), font.get_page_count(), font.get_first_char()]


static func _describe_env(ctx: McpToolContext, out: Dictionary, resolved: Dictionary) -> void:
	var env := EnvFile.new()
	var err: Error = ERR_CANT_OPEN
	if resolved.get("loose", false):
		env.set_source_path(String(resolved["path"]))
		err = env.load()
	elif ctx.root() != null:
		err = env.load_from_resource_root(ctx.root(), String(resolved["name"]))
	if err != OK or not env.is_loaded():
		out["error"] = "Environment failed to load (%s)." % error_string(err)
		return
	var properties := {}
	for prop: Dictionary in ClassDB.class_get_property_list("EnvFile", true):
		var usage := int(prop.get("usage", 0))
		if usage & PROPERTY_USAGE_EDITOR == 0:
			continue
		properties[String(prop["name"])] = env.get(prop["name"])
	out["data"] = {
		"properties": properties,
		"noon_sample": env.interpolate_time_of_day(0.5),
		"sun_direction_noon": env.compute_sun_direction(0.5),
	}
	out["summary"] = "Environment profile (fog, sky, lighting, time-of-day)."


static func _describe_lwf(ctx: McpToolContext, out: Dictionary, resolved: Dictionary, full: bool, limit: int) -> void:
	var lwf := LwfData.new()
	if not lwf.load_bytes(read_bytes(ctx, resolved)):
		out["error"] = "LWF sound profile failed to parse."
		return
	var data := { "set_count": lwf.get_set_count() }
	if full:
		var sets: Array = lwf.get_sets()
		data["sets"] = sets.slice(0, limit)
		out["truncated"] = sets.size() > limit
	out["data"] = data
	out["summary"] = "LWF sound profile — %d trigger set(s)." % lwf.get_set_count()


static func _describe_3di(
		ctx: McpToolContext,
		out: Dictionary,
		resolved: Dictionary,
		full: bool,
		limit: int) -> void:
	var object := ObjectData.new()
	var err := _open_via(object, ctx, resolved)
	if err != OK:
		out["error"] = "3DI model failed to load (%s): %s" % [error_string(err), object.get_last_error()]
		return
	var raw_materials: Array = object.get_materials()
	var raw_lights: Array = object.get_lights()
	var object_summary: Dictionary = object.get_summary()
	var data := {
		"object_name": object.get_object_name(),
		"lod_count": int(object_summary.get("lod_count", 0)),
		"userpoint_count": int(object_summary.get("userpoint_count", 0)),
		"material_count": raw_materials.size(),
		"material_total": raw_materials.size(),
		"light_count": raw_lights.size(),
		"light_total": raw_lights.size(),
		"material_features": _3di_material_feature_counts(raw_materials),
	}
	if full:
		var materials: Array = []
		for index in range(mini(raw_materials.size(), limit)):
			materials.append(_3di_material_summary(
					object, raw_materials[index] as Dictionary))
		var lights: Array = []
		for index in range(mini(raw_lights.size(), limit)):
			lights.append(_3di_light_summary(
					object, raw_lights[index] as Dictionary))
		data["materials"] = materials
		data["material_returned"] = materials.size()
		data["lights"] = lights
		data["light_returned"] = lights.size()
		out["truncated"] = raw_materials.size() > limit \
				or raw_lights.size() > limit
	out["data"] = data
	out["summary"] = "3DI model '%s' — %d material(s), %d authored LGHT record(s), %d LOD(s)." % [
		object.get_object_name(), raw_materials.size(), raw_lights.size(),
		int(object_summary.get("lod_count", 0)),
	]


static func _3di_material_summary(
		object: ObjectData,
		raw: Dictionary) -> Dictionary:
	var row := raw.duplicate(true)
	var index := int(raw.get("index", -1))
	var info: Dictionary = object.get_material_info(index)
	# Keep the raw authored fields at top level for exact inspection, then group
	# the renderer-relevant interpretation inputs so callers do not need to know
	# ObjectData's flags or conflate an authored bit with final runtime policy.
	row["technique"] = {
		"shader_tag": String(raw.get("shader", "")),
		"family": String(raw.get("shader_family", "unknown")),
		"blend": String(raw.get("shader_blend", "opaque")),
		"normal_space": String(raw.get("normal_space", "none")),
		"shader_flags": int(raw.get("shader_flags", 0)),
		"material_flags": int(raw.get("flags", 0)),
	}
	row["shadow_inputs"] = {
		"alpha_test_enabled": bool(info.get("alpha_test_enabled", false)),
		"alpha_test_byte": int(info.get("alpha_test", 0)),
		"alpha_threshold": float(raw.get("alpha_threshold", 0.0)),
		"alpha_invert": bool(info.get("alpha_invert", false)),
		"two_sided": bool(info.get("two_sided", false)),
	}
	row["glass"] = {
		"authored": bool(raw.get("is_glass", false)),
		"shader_flag": bool(raw.get("is_glass_shader", false)),
		"family": String(raw.get("shader_family", "unknown")),
		"reflect_color": raw.get("reflect_color", Color.BLACK),
	}
	row["emissive"] = {
		"authored_type": int(raw.get("emissive_type", 0)),
		"full": bool(info.get("emissive", false)),
		"shader_luminance": bool(raw.get("is_luminance", false)),
		"glow_capable": bool(raw.get("is_glow_capable", false)),
	}
	return row


static func _3di_light_summary(
		object: ObjectData,
		raw: Dictionary) -> Dictionary:
	var row := raw.duplicate(true)
	var info: Dictionary = object.get_light_info(int(raw.get("index", -1)))
	row["kind"] = "target" if int(info.get("light_type", 0)) == 1 else "point"
	row["affects"] = {
		"objects": not bool(info.get("disable_lightobjects", false)),
		"terrain": not bool(info.get("disable_lightterrain", false)),
		"corona": not bool(info.get("disable_corona", false)),
	}
	return row


static func _3di_material_feature_counts(materials: Array) -> Dictionary:
	var counts := {
		"glass": 0,
		"emissive": 0,
		"alpha": 0,
		"glow_capable": 0,
		"environment": 0,
	}
	for value in materials:
		var material: Dictionary = value
		if bool(material.get("is_glass", false)) \
				or bool(material.get("is_glass_shader", false)):
			counts["glass"] += 1
		if int(material.get("emissive_type", 0)) != 0 \
				or bool(material.get("is_luminance", false)):
			counts["emissive"] += 1
		if bool(material.get("is_alpha", false)):
			counts["alpha"] += 1
		if bool(material.get("is_glow_capable", false)):
			counts["glow_capable"] += 1
		if bool(material.get("uses_environment", false)):
			counts["environment"] += 1
	return counts


static func _describe_pff(out: Dictionary, path: String, full: bool, limit: int) -> void:
	var archive := PffDocument.new()
	if archive.open(path) != OK:
		out["error"] = "PFF archive failed to open."
		return
	var entries: Array = archive.get_entries()
	var data := { "entry_count": entries.size() }
	if full:
		data["entries"] = entries.slice(0, limit)
		out["truncated"] = entries.size() > limit
	out["data"] = data
	out["summary"] = "PFF archive — %d entries." % entries.size()


static func _describe_stats(out: Dictionary, resolved: Dictionary, kind: String) -> void:
	var path := String(resolved["path"])
	var size := -1
	if FileAccess.file_exists(path):
		var file := FileAccess.open(path, FileAccess.READ)
		if file != null:
			size = file.get_length()
			file.close()
	out["data"] = { "size_bytes": size }
	out["summary"] = "%s — no structured describe yet for this kind; read_file returns the raw bytes (describe_api documents the matching Nova* class)." % kind


# open_file for loose paths, open_from_resource_root for archived names.
# Both loader classes routed here (MissionData, ObjectData) expose the full
# open_file / open_from_resource_root / get_last_error trio.
static func _open_via(loader: Object, ctx: McpToolContext, resolved: Dictionary) -> Error:
	if resolved.get("loose", false):
		return loader.open_file(String(resolved["path"]))
	var root: Variant = ctx.root()
	if root != null:
		return loader.open_from_resource_root(root, String(resolved["name"]))
	return ERR_CANT_OPEN


static func _clip(text: String) -> String:
	if text.length() <= TEXT_CLIP:
		return text
	return text.substr(0, TEXT_CLIP) + "…"
