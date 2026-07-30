class_name DebugRenderingPage
extends NovaDebugPage
## Renderer and viewport diagnostics. Every knob is a catalog control over a
## public Godot property, so F3 and runtime automation see the same state.

var _renderer_label: Label
var _viewport_label: Label


func page_id() -> StringName:
	return &"Rendering"


func page_title() -> String:
	return "Rendering & physics"


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

	var hints_label := Label.new()
	hints_label.text = "World overlays"
	add_child(hints_label)
	add_debug_control(&"physics_collision_shapes")
	add_debug_control(&"navigation_paths")


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
		return
	var size := viewport.get_visible_rect().size
	_viewport_label.text = "Viewport: %d x %d\nScale %.2f | MSAA 3D %s" % [
		int(size.x), int(size.y), viewport.scaling_3d_scale,
		str(viewport.msaa_3d),
	]
