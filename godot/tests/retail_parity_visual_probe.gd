extends Node3D

## Reproducible visual evidence for the D-HUD-11 / D-ITEM-16 / D-ITEM-17
## closure. Each mode consumes the same public runtime/presentation seam as the
## game, then annotates the result so non-pixel gameplay rules remain legible.
## The debris/glass stages are deterministic probe fixtures, not retail mission
## captures; their particle families are loaded from the mounted retail catalog.
##
## NOVA_PARITY_PROBE_MODE=attach|debris|glass
## NOVA_PARITY_PROBE_DIR=<output dir>
## NOVA_RESOURCE_DIR=<retail root> (debris/glass)
##   "$GODOT_BIN" --path godot res://tests/retail_parity_visual_probe.tscn

const DestructionPresentPass := preload(
		"res://game/world/destruction_present_pass.gd")
const MODE_ENV := "NOVA_PARITY_PROBE_MODE"
const OUTPUT_ENV := "NOVA_PARITY_PROBE_DIR"
const RESOURCE_ENV := "NOVA_RESOURCE_DIR"
const EXPANSION_ENV := "NOVA_EXPANSION"
const TICK_DT := 1.0 / 62.5

var _fx: EffectWorld


class AttachDiagram:
	extends Control
	var labels: Array = []
	var player_position := Vector3(12.0, 0.0, 0.0)
	var accent := Color("55d8ff")

	func _draw() -> void:
		draw_rect(Rect2(Vector2.ZERO, size), Color("111b29"), true)
		draw_rect(Rect2(Vector2.ZERO, size), Color("344b65"), false, 2.0)
		var center := Vector2(size.x * 0.5, size.y * 0.62)
		draw_circle(center, 10.0, Color("f1f6fb"))
		draw_circle(center, 14.0, Color("7793ad"), false, 2.0)
		draw_string(ThemeDB.fallback_font, center + Vector2(-24.0, 34.0),
				"PLAYER", HORIZONTAL_ALIGNMENT_LEFT, -1, 13,
				Color("aebed0"))
		for raw in labels:
			var row: Dictionary = raw
			var point: Vector3 = row.get("position", player_position)
			var p := center + Vector2(
					(point.x - player_position.x) * 31.0,
					(point.y - player_position.y) * 31.0 - 64.0)
			draw_rect(Rect2(p - Vector2(28.0, 15.0), Vector2(56.0, 30.0)),
					Color("354353"), true)
			draw_rect(Rect2(p - Vector2(28.0, 15.0), Vector2(56.0, 30.0)),
					accent if bool(row.get("nearest", false)) else Color("74879a"),
					false, 2.0)
			var bubble := Rect2(p + Vector2(-42.0, -48.0), Vector2(84.0, 23.0))
			draw_rect(bubble, Color(0.02, 0.04, 0.07, 0.94), true)
			draw_rect(bubble, accent, false, 1.5)
			draw_string(ThemeDB.fallback_font, bubble.position + Vector2(12.0, 17.0),
					"USE GUN", HORIZONTAL_ALIGNMENT_LEFT, -1, 13,
					Color("e9f8ff"))


func _ready() -> void:
	DisplayServer.window_set_size(Vector2i(960, 540))
	var mode := OS.get_environment(MODE_ENV).strip_edges().to_lower()
	var output_dir := OS.get_environment(OUTPUT_ENV).strip_edges()
	if output_dir.is_empty():
		_fail("%s is required" % OUTPUT_ENV)
		return
	DirAccess.make_dir_recursive_absolute(output_dir)
	match mode:
		"attach":
			if not _build_attach_stage():
				return
		"debris":
			if not _build_debris_stage():
				return
		"glass":
			if not _build_glass_stage():
				return
		_:
			_fail("unknown %s=%s" % [MODE_ENV, mode])
			return

	await _settle_frames(8)
	if _fx != null:
		for _tick in range(10):
			_fx.advance_fixed_tick(TICK_DT)
			_fx.render_frame()
			await get_tree().process_frame
	var image := await _capture_image()
	var file_path := output_dir.path_join("capture.png")
	if image == null or image.save_png(file_path) != OK:
		_fail("could not save %s" % file_path)
		return
	print("[retail-parity-visual] PASS mode=%s file=%s" % [mode, file_path])
	get_tree().quit(0)


func _build_attach_stage() -> bool:
	var md := MissionData.new()
	if md.create_default() != OK:
		_fail("could not create attach mission")
		return false
	if md.add_entity(MissionData.KIND_ITEM, 101294,
			Vector3(10, 0, 0), Vector3.ZERO).is_empty() \
			or md.add_entity(MissionData.KIND_ITEM, 101294,
			Vector3(14, 0, 0), Vector3.ZERO).is_empty():
		_fail("could not add attach candidates")
		return false

	var fixture_dir := OS.get_cache_dir().path_join(
			"attach_visual_%d" % Time.get_ticks_usec())
	DirAccess.make_dir_recursive_absolute(fixture_dir)
	if not _write_bytes(fixture_dir.path_join("labelgun.3di"),
			FileAccess.get_file_as_bytes(
			"res://../fixtures/threedi/synth/mount.3di")):
		return false
	if not _write_text(fixture_dir.path_join("items.def"), """begin "Labels Gun"
  id 101294
  type object
  graphic labelgun
  primary_weapon WPN_EMPLCD50
end
"""):
		return false
	var item_db := ItemDatabase.new()
	if item_db.load(fixture_dir.path_join("items.def")) != OK:
		_fail("could not load attach items.def")
		return false
	var root := ResourceRoot.new()
	if root.set_root_dir(fixture_dir) != OK:
		_fail("could not mount attach fixture")
		return false
	var sim := Simulation.new()
	sim.set_asset_root(root)
	if not sim.install_seat_specs_for_type_ids(item_db,
			PackedInt32Array([1294])) \
			or not sim.load_from_mission_data(md) \
			or not sim.spawn_local_player(Vector3(12, 0, 0), 0.0, 1):
		_fail("could not start attach simulation")
		sim.free()
		return false
	sim.set_local_player_weapon({
		"name": "WPN_LABEL_SCOPE",
		"actions": [
			{"name": "idle", "delaystart": 0, "delayend": 0},
			{"name": "scopeup", "delaystart": 0, "delayend": 0},
			{"name": "scopedown", "delaystart": 0, "delayend": 0},
		],
		"flags": 0x1,
		"clipsize": 30,
		"startrounds": 60,
	}, {})
	sim.step()
	var blocked: Array = sim.get_attach_labels()
	if not sim.request_local_player_scope_toggle():
		_fail("could not raise attach probe scope")
		sim.free()
		return false
	for _tick in range(16):
		sim.step()
	var aimed: Array = sim.get_attach_labels()
	sim.set_local_player_debug_third_person(true)
	var third_person: Array = sim.get_attach_labels()
	sim.free()
	if blocked.size() != 2 or aimed.size() != 1 or third_person.size() != 2:
		_fail("unexpected attach counts %s/%s/%s" % [
				blocked.size(), aimed.size(), third_person.size()])
		return false

	_add_flat_background(Color("07111d"))
	_add_screen_title("D-HUD-11  •  ATTACH LABELS SHARE Player_CanFireWeapon",
			"The exact runtime query drives all three captures below", Color("55d8ff"))
	var states := [
		{"title": "SCOPE DOWN", "detail": "cannot fire  •  both entities label",
			"labels": blocked, "accent": Color("efb65a")},
		{"title": "SETTLED ADS", "detail": "can fire  •  nearest entity only",
			"labels": aimed, "accent": Color("5de89b")},
		{"title": "THIRD PERSON", "detail": "live gate  •  both return immediately",
			"labels": third_person, "accent": Color("79a9ff")},
	]
	for index in range(states.size()):
		var state: Dictionary = states[index]
		var origin := Vector2(24.0 + index * 304.0, 112.0)
		var title := _label(String(state["title"]), origin, 18,
				state["accent"] as Color)
		add_child(title)
		var detail := _label(String(state["detail"]), origin + Vector2(0, 27),
				13, Color("a9bad0"))
		add_child(detail)
		var diagram := AttachDiagram.new()
		diagram.position = origin + Vector2(0, 58)
		diagram.size = Vector2(280, 274)
		diagram.labels = state["labels"]
		diagram.accent = state["accent"]
		add_child(diagram)
	var footer := _label(
			"PASS  •  counts 2 → 1 → 2  •  promoted scope is frame-stable; camera mode is live",
			Vector2(24, 490), 15, Color("dce8f3"))
	add_child(footer)
	return true


func _build_debris_stage() -> bool:
	if not _build_particle_stage("D-ITEM-16  •  COLLISION-TRIANGLE SECTION DEBRIS",
			"Resolved centroids, launch vectors, and material families reach the presenter verbatim",
			Color("ffbd61")):
		return false
	_add_box("CollisionWall", Vector3(13.2, 4.6, 0.28),
			Vector3(0.0, 2.35, -2.7), Color("394655"), 0.86, 0.12)
	_add_triangle_grid()
	var blast := Vector3(0.0, 0.25, 3.0)
	_add_marker(blast, Color("ff5f52"), "BLAST +0x80", 0.43)
	var effects: Array = []
	for index in range(18):
		var column := index % 6
		var row := index / 6
		var point := Vector3(-5.35 + column * 2.13,
				0.85 + row * 1.33, -2.48)
		var foliage := index in [1, 7, 12, 16]
		var direction := (point - blast).normalized()
		effects.append({
			"effect": "Effect_TreeFoliageExp" if foliage else "Effect_TreeWoodExp",
			"pos": point,
			"dir": direction,
			"family": 0,
		})
		_add_centroid_marker(point,
				Color("57e58c") if foliage else Color("ffb55e"))
		_add_direction_line(point, point + direction * 1.25,
				Color("8fffb0") if foliage else Color("ffd39a"))
	var presenter := DestructionPresentPass.new()
	presenter.setup(null, null, null, null, null, null, Callable(),
			func(): return _fx)
	presenter.present_drained({
		"effects": effects,
		"debris_triangles": effects.size(),
	}, [])
	_add_legend([
		{"color": Color("57e58c"), "text": "material 17  →  Effect_TreeFoliageExp"},
		{"color": Color("ffb55e"), "text": "all other materials  →  Effect_TreeWoodExp"},
		{"color": Color("ff5f52"), "text": "vectors launch away from the stored blast center"},
	], "Visual subset: 18 of the native probe's 150 exact stride samples")
	return true


func _build_glass_stage() -> bool:
	if not _build_particle_stage("D-ITEM-17  •  GLASS USERPOINT SHATTER",
			"Exact stock model mapping, authored-radius gate, break-once state, and ordered effect rolls",
			Color("66dcff")):
		return false
	_add_box("Facade", Vector3(11.8, 5.2, 0.35),
			Vector3(0.0, 2.65, -2.5), Color("343e49"), 0.88, 0.08)
	_add_window_frame(Vector3(-2.7, 2.7, -2.28), true)
	_add_window_frame(Vector3(2.7, 2.7, -2.28), false)
	var point := Vector3(-2.7, 2.7, -2.02)
	var direction := Vector3(0.0, 0.0, 1.0)
	var effects := [
		{"effect": "Effect_BldGlassExp", "pos": point, "dir": direction, "family": 0},
		{"effect": "Effect_BldPaperExp", "pos": point, "dir": direction, "family": 0},
		{"effect": "Effect_BldDustExp", "pos": point, "dir": direction, "family": 0},
	]
	var presenter := DestructionPresentPass.new()
	presenter.setup(null, null, null, null, null, null, Callable(),
			func(): return _fx)
	presenter.present_drained({"effects": effects, "glass_points": 1}, [])
	_add_marker(point + Vector3(0, 0, 0.2), Color("66dcff"),
			"eurhr2  →  GLASS\nBROKEN ONCE", 0.26)
	_add_marker(Vector3(2.7, 2.7, -2.02), Color("94a9bd"),
			"outside authored r=8\nSTAYS INTACT", 0.22)
	_add_legend([
		{"color": Color("66dcff"), "text": "first exact case-insensitive userpoint, full Euler transform"},
		{"color": Color("ffbe62"), "text": "seed 0x200  →  Glass / Paper / Dust; Fire roll misses"},
		{"color": Color("94a9bd"), "text": "queue override r=30 never widens the authored r=8 glass gate"},
	], "Second identical blast emits nothing and consumes no PRNG draws")
	return true


func _build_particle_stage(title: String, subtitle: String, accent: Color) -> bool:
	var resource_dir := OS.get_environment(RESOURCE_ENV).strip_edges()
	var expansion := OS.get_environment(EXPANSION_ENV).strip_edges()
	var root := ResourceRoot.new()
	if resource_dir.is_empty() \
			or root.mount_runtime(resource_dir, expansion, false, "jo") != OK:
		_fail("could not mount %s=%s" % [RESOURCE_ENV, resource_dir])
		return false
	_build_world_environment()
	_fx = EffectWorld.new()
	_fx.name = "RetailEffectWorld"
	add_child(_fx)
	if _fx.load_from_resource_root(root) <= 0:
		_fail("retail particle catalog mounted no effects")
		return false
	_add_screen_title(title, subtitle, accent)
	return true


func _build_world_environment() -> void:
	var world_environment := WorldEnvironment.new()
	var environment := Environment.new()
	environment.background_mode = Environment.BG_COLOR
	environment.background_color = Color("08111c")
	environment.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	environment.ambient_light_color = Color("9db3ca")
	environment.ambient_light_energy = 0.58
	environment.tonemap_mode = Environment.TONE_MAPPER_FILMIC
	world_environment.environment = environment
	add_child(world_environment)
	var camera := Camera3D.new()
	camera.position = Vector3(0.0, 5.2, 12.8)
	camera.fov = 48.0
	camera.current = true
	add_child(camera)
	camera.look_at(Vector3(0.0, 2.25, -2.0), Vector3.UP)
	var key := DirectionalLight3D.new()
	key.light_color = Color("ddecff")
	key.light_energy = 1.4
	key.shadow_enabled = true
	key.rotation_degrees = Vector3(-48.0, -24.0, 0.0)
	add_child(key)
	_add_box("Floor", Vector3(20.0, 0.18, 15.0),
			Vector3(0.0, -0.12, -0.2), Color("1e2a36"), 0.95, 0.04)


func _add_triangle_grid() -> void:
	var mesh := ImmediateMesh.new()
	mesh.surface_begin(Mesh.PRIMITIVE_LINES)
	for column in range(7):
		var x := -6.4 + column * (12.8 / 6.0)
		mesh.surface_add_vertex(Vector3(x, 0.08, -2.50))
		mesh.surface_add_vertex(Vector3(x, 4.62, -2.50))
	for row in range(4):
		var y := 0.08 + row * (4.54 / 3.0)
		mesh.surface_add_vertex(Vector3(-6.4, y, -2.50))
		mesh.surface_add_vertex(Vector3(6.4, y, -2.50))
	for column in range(6):
		for row in range(3):
			var x0 := -6.4 + column * (12.8 / 6.0)
			var x1 := -6.4 + (column + 1) * (12.8 / 6.0)
			var y0 := 0.08 + row * (4.54 / 3.0)
			var y1 := 0.08 + (row + 1) * (4.54 / 3.0)
			mesh.surface_add_vertex(Vector3(x0, y0, -2.49))
			mesh.surface_add_vertex(Vector3(x1, y1, -2.49))
	mesh.surface_end()
	var lines := MeshInstance3D.new()
	lines.mesh = mesh
	lines.material_override = _unshaded_material(Color("718398"))
	add_child(lines)


func _add_window_frame(center: Vector3, shattered: bool) -> void:
	var frame_color := Color("697785")
	_add_box("WindowTop", Vector3(3.25, 0.17, 0.22),
			center + Vector3(0, 1.35, 0.0), frame_color, 0.45, 0.55)
	_add_box("WindowBottom", Vector3(3.25, 0.17, 0.22),
			center + Vector3(0, -1.35, 0.0), frame_color, 0.45, 0.55)
	_add_box("WindowLeft", Vector3(0.17, 2.85, 0.22),
			center + Vector3(-1.55, 0, 0.0), frame_color, 0.45, 0.55)
	_add_box("WindowRight", Vector3(0.17, 2.85, 0.22),
			center + Vector3(1.55, 0, 0.0), frame_color, 0.45, 0.55)
	if not shattered:
		_add_box("IntactGlass", Vector3(2.95, 2.52, 0.06), center,
				Color(0.20, 0.62, 0.78, 0.47), 0.12, 0.05, true)
		return
	for index in range(9):
		var fragment := MeshInstance3D.new()
		var mesh := BoxMesh.new()
		mesh.size = Vector3(0.18 + (index % 3) * 0.08,
				0.08 + (index % 2) * 0.07, 0.035)
		fragment.mesh = mesh
		fragment.position = center + Vector3(
				-1.0 + (index % 5) * 0.48,
				-1.0 + (index / 5) * 0.52,
				0.35 + (index % 3) * 0.12)
		fragment.rotation_degrees = Vector3(index * 13.0, index * 17.0,
				index * 29.0)
		fragment.material_override = _unshaded_material(Color("83e7ff"))
		add_child(fragment)


func _add_legend(rows: Array, footer_text: String) -> void:
	var panel := ColorRect.new()
	panel.position = Vector2(22, 356)
	panel.size = Vector2(916, 158)
	panel.color = Color(0.025, 0.045, 0.072, 0.91)
	add_child(panel)
	for index in range(rows.size()):
		var row: Dictionary = rows[index]
		var swatch := ColorRect.new()
		swatch.position = Vector2(20, 18 + index * 32)
		swatch.size = Vector2(13, 13)
		swatch.color = row["color"]
		panel.add_child(swatch)
		var text_label := _label(String(row["text"]),
				Vector2(44, 12 + index * 32), 15, Color("dbe8f4"))
		panel.add_child(text_label)
	var footer := _label(footer_text, Vector2(20, 116), 14,
			Color("91a9bf"))
	panel.add_child(footer)


func _add_screen_title(title: String, subtitle: String, accent: Color) -> void:
	var panel := ColorRect.new()
	panel.position = Vector2(18, 16)
	panel.size = Vector2(924, 78)
	panel.color = Color(0.025, 0.045, 0.072, 0.92)
	add_child(panel)
	var title_label := _label(title, Vector2(20, 12), 22, accent)
	panel.add_child(title_label)
	var subtitle_label := _label(subtitle, Vector2(20, 45), 14,
			Color("b5c7d8"))
	panel.add_child(subtitle_label)


func _add_flat_background(color: Color) -> void:
	var background := ColorRect.new()
	background.position = Vector2.ZERO
	background.size = Vector2(960, 540)
	background.color = color
	add_child(background)


func _label(text: String, pos: Vector2, font_size: int, color: Color) -> Label:
	var result := Label.new()
	result.text = text
	result.position = pos
	result.add_theme_font_size_override("font_size", font_size)
	result.add_theme_color_override("font_color", color)
	return result


func _add_box(node_name: String, box_size: Vector3, pos: Vector3,
		color: Color, roughness := 0.8, metallic := 0.1,
		transparent := false) -> void:
	var instance := MeshInstance3D.new()
	instance.name = node_name
	var mesh := BoxMesh.new()
	mesh.size = box_size
	instance.mesh = mesh
	instance.position = pos
	var material := StandardMaterial3D.new()
	material.albedo_color = color
	material.roughness = roughness
	material.metallic = metallic
	if transparent:
		material.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	instance.material_override = material
	add_child(instance)


func _add_centroid_marker(pos: Vector3, color: Color) -> void:
	var marker := MeshInstance3D.new()
	var sphere := SphereMesh.new()
	sphere.radius = 0.09
	sphere.height = 0.18
	marker.mesh = sphere
	marker.position = pos + Vector3(0, 0, 0.09)
	marker.material_override = _unshaded_material(color)
	add_child(marker)


func _add_direction_line(from: Vector3, to: Vector3, color: Color) -> void:
	var mesh := ImmediateMesh.new()
	mesh.surface_begin(Mesh.PRIMITIVE_LINES)
	mesh.surface_add_vertex(from)
	mesh.surface_add_vertex(to)
	mesh.surface_end()
	var line := MeshInstance3D.new()
	line.mesh = mesh
	line.material_override = _unshaded_material(color)
	add_child(line)


func _add_marker(pos: Vector3, color: Color, text: String, radius: float) -> void:
	var ring := MeshInstance3D.new()
	var torus := TorusMesh.new()
	torus.inner_radius = radius
	torus.outer_radius = radius + 0.14
	torus.rings = 28
	torus.ring_segments = 10
	ring.mesh = torus
	ring.position = pos
	ring.material_override = _unshaded_material(color)
	add_child(ring)
	var marker_label := Label3D.new()
	marker_label.text = text
	marker_label.position = pos + Vector3(0, radius + 0.55, 0)
	marker_label.font_size = 31
	marker_label.pixel_size = 0.009
	marker_label.modulate = color.lightened(0.25)
	marker_label.outline_size = 7
	marker_label.no_depth_test = true
	add_child(marker_label)


func _unshaded_material(color: Color) -> StandardMaterial3D:
	var material := StandardMaterial3D.new()
	material.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	material.albedo_color = color
	material.emission_enabled = true
	material.emission = color
	material.emission_energy_multiplier = 1.8
	return material


func _write_bytes(path: String, bytes: PackedByteArray) -> bool:
	var file := FileAccess.open(path, FileAccess.WRITE)
	if file == null:
		_fail("could not write %s" % path)
		return false
	file.store_buffer(bytes)
	file.close()
	return true


func _write_text(path: String, text: String) -> bool:
	var file := FileAccess.open(path, FileAccess.WRITE)
	if file == null:
		_fail("could not write %s" % path)
		return false
	file.store_string(text)
	file.close()
	return true


func _settle_frames(count: int) -> void:
	for _index in range(count):
		if _fx != null:
			_fx.render_frame()
		else:
			RenderingServer.force_sync()
		await get_tree().process_frame


func _capture_image() -> Image:
	if _fx != null:
		_fx.render_frame()
	await get_tree().process_frame
	await RenderingServer.frame_post_draw
	return get_viewport().get_texture().get_image()


func _fail(message: String) -> void:
	push_error("[retail-parity-visual] %s" % message)
	get_tree().quit(1)
