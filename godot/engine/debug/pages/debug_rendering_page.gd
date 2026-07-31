class_name DebugRenderingPage
extends NovaDebugPage
## Renderer and viewport diagnostics plus one place to operate every real
## OpenNova world overlay. Godot's physics/navigation debug hints are omitted:
## gameplay uses the portable simulation's collision and routing systems, so
## those hints had no runtime geometry to show.

const NovaDebugViewStatus := preload(
		"res://engine/debug/nova_debug_view_status.gd")
const WORLD_OVERLAYS: Array[StringName] = [
	&"show_collision",
	&"show_hit_meshes",
	&"show_round_trails",
	&"show_skeletons",
	&"show_user_points",
	&"show_effect_boxes",
	&"show_portal_faces",
]
var _renderer_label: Label
var _viewport_label: Label
var _viewport_debug_state: Label
var _world_overlay_state: Label


func page_id() -> StringName:
	return &"Rendering"


func page_title() -> String:
	return "Rendering & overlays"


func page_category() -> StringName:
	return CATEGORY_WORLD


func _build() -> void:
	add_theme_constant_override("separation", 6)

	_renderer_label = Label.new()
	_renderer_label.name = "RendererInfo"
	_renderer_label.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	add_child(_renderer_label)

	_viewport_label = Label.new()
	_viewport_label.name = "ViewportInfo"
	_viewport_label.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	add_child(_viewport_label)

	var view_label := Label.new()
	view_label.text = "Viewport diagnostic"
	add_child(view_label)
	add_debug_control(&"viewport_debug_draw")

	_viewport_debug_state = Label.new()
	_viewport_debug_state.name = "ViewportDiagnosticState"
	_viewport_debug_state.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	add_child(_viewport_debug_state)

	var hints_label := Label.new()
	hints_label.text = "World overlays"
	add_child(hints_label)

	var hints_help := Label.new()
	hints_help.name = "WorldOverlayHelp"
	hints_help.text = (
			"OpenNova runtime views. Active views report whether matching "
			+ "world data is actually drawable.")
	hints_help.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	add_child(hints_help)

	for id in WORLD_OVERLAYS:
		add_option_check(id)

	_world_overlay_state = Label.new()
	_world_overlay_state.name = "WorldOverlayState"
	_world_overlay_state.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	add_child(_world_overlay_state)


func refresh() -> void:
	var adapter := ""
	var vendor := ""
	var api := ""
	if RenderingServer.has_method("get_video_adapter_name"):
		adapter = String(RenderingServer.get_video_adapter_name())
	if RenderingServer.has_method("get_video_adapter_vendor"):
		vendor = String(RenderingServer.get_video_adapter_vendor())
	if RenderingServer.has_method("get_video_adapter_api_version"):
		api = String(RenderingServer.get_video_adapter_api_version())
	var renderer := String(ProjectSettings.get_setting(
			"rendering/renderer/rendering_method", "unknown"))
	_renderer_label.text = "Renderer: %s\nGPU: %s%s%s" % [
		renderer,
		adapter if not adapter.is_empty() else "unknown",
		" | " + vendor if not vendor.is_empty() else "",
		" | " + api if not api.is_empty() else "",
	]

	var viewport := get_viewport()
	if viewport == null:
		_viewport_label.text = "No viewport."
		_refresh_diagnostic_state()
		return
	var size := viewport.get_visible_rect().size
	_viewport_label.text = "Viewport: %d x %d\nScale %.2f | MSAA 3D %s" % [
		int(size.x), int(size.y), viewport.scaling_3d_scale,
		str(viewport.msaa_3d),
	]
	_refresh_diagnostic_state()


func _refresh_diagnostic_state() -> void:
	var draw_state := _ctx.session.get_control_state(&"viewport_debug_draw") \
			if _ctx != null and _ctx.session != null else null
	if draw_state == null or not draw_state.available:
		_viewport_debug_state.text = (
				draw_state.reason
				if draw_state != null and not draw_state.reason.is_empty()
				else "No render viewport is available.")
	else:
		var draw_index := clampi(
				int(draw_state.value),
				0,
				NovaDebugCatalog.VIEWPORT_DRAW_CHOICES.size() - 1)
		var draw_name := NovaDebugCatalog.VIEWPORT_DRAW_CHOICES[draw_index]
		_viewport_debug_state.text = (
				"Normal scene shading."
				if draw_index == 0
				else "SELECTED - %s\n%s" % [
					draw_name, _viewport_debug_help(draw_index)])

	var world := _ctx.world() if _ctx != null else null
	var report := {}
	if world != null and world.has_method("get_debug_view_statuses"):
		var value: Variant = world.get_debug_view_statuses()
		if value is Array:
			for entry_v in value:
				if entry_v is NovaDebugViewStatus:
					var entry := entry_v as NovaDebugViewStatus
					report[entry.id] = entry
	var active_lines := PackedStringArray()
	for id in WORLD_OVERLAYS:
		var state := _ctx.session.get_control_state(id) \
				if _ctx != null and _ctx.session != null else null
		var entry := report.get(id) as NovaDebugViewStatus
		var enabled := (
				entry.enabled
				if entry != null
				else state != null and bool(state.value))
		if not enabled:
			continue
		var installed := entry != null and entry.installed
		var count := entry.drawable_count if entry != null else -1
		var detail := entry.reason if entry != null else ""
		if detail.is_empty():
			if not installed:
				detail = "waiting for a loaded view"
			elif count == 0:
				detail = "no matching data is currently drawable"
			elif count > 0:
				detail = "%d drawable items" % count
			else:
				detail = "view installed"
		active_lines.append("%s — ACTIVE · %s" % [
			_world_overlay_label(id),
			detail,
		])
	_world_overlay_state.text = (
			"No world overlays active."
			if active_lines.is_empty()
			else "\n".join(active_lines))


func _world_overlay_label(id: StringName) -> String:
	match id:
		&"show_collision":
			return "Collision"
		&"show_hit_meshes":
			return "Hit meshes"
		&"show_round_trails":
			return "Round trails"
		&"show_skeletons":
			return "Skeletons"
		&"show_user_points":
			return "User points"
		&"show_effect_boxes":
			return "Effect bounds"
		&"show_portal_faces":
			return "Portal faces"
	return String(id)


func _viewport_debug_help(draw_index: int) -> String:
	match draw_index:
		1:
			return "Shows material color without scene lighting."
		2:
			return "Shows lighting without material color."
		3:
			return "Repeated draws appear brighter; hot areas reveal overdraw."
		4:
			return "Shows rendered triangle edges; unsupported surfaces may stay solid."
		5:
			return "Shows screen-space normals; empty areas have no rendered geometry."
		6, 7, 8:
			return "Requires VoxelGI data; black or unchanged areas have no VoxelGI contribution."
		9:
			return "Requires shadow-casting lights; empty areas have no shadow-atlas data."
		10, 14:
			return "Requires a shadowed directional light; an empty view means no matching shadow data."
		11:
			return "Shows the renderer luminance buffer used by exposure."
		12:
			return "This view requires SSAO to be enabled; black or unchanged output means no SSAO buffer contributes."
		13:
			return "This view requires SSIL to be enabled; black or unchanged output means no SSIL buffer contributes."
		15:
			return "Requires active decals; an empty view means the decal atlas has no visible contribution."
		16, 17:
			return "Requires SDFGI to be enabled; black or unchanged output means no SDFGI data contributes."
		18:
			return "Requires an active GI path; empty areas have no GI buffer contribution."
		19:
			return "Disables mesh LOD so geometry stays at its highest available detail."
		20:
			return "Requires Forward+ clustered omni lights; empty clusters show no matching lights."
		21:
			return "Requires Forward+ clustered spot lights; empty clusters show no matching lights."
		22:
			return "Requires Forward+ clustered decals; empty clusters show no matching decals."
		23:
			return "Requires Forward+ reflection probes; empty clusters show no matching probes."
		24:
			return "Shows registered renderer occluders; an empty view means none contribute."
		25:
			return "Shows motion vectors; static or unsupported geometry can remain empty."
		26:
			return "Shows a renderer-internal buffer; its contents depend on the active renderer."
	return "The selected renderer diagnostic is shown in the world beside this dock."
