class_name McpScreenshot
extends RefCounted

## Viewport capture for the screenshot tool. Two halves:
##
##  - capture(): the GPU part — wait for a drawn frame (raced against a frame
##    budget so a minimized window or stalled renderer errors instead of
##    hanging; headless fast-fails), then read the viewport texture. Mirrors
##    the production pattern in modtools/tools/screenshot_capture.gd.
##  - encode(): the pure-CPU part — optional crop, downscale, WebP encode with
##    PNG fallback, and an oversize retry — kept separate so headless tests
##    exercise it with constructed images.

const DRAW_TIMEOUT_FRAMES := 90
const DEFAULT_MAX_DIM := 1280
const DEFAULT_QUALITY := 0.8
const SIZE_RETRY_BYTES := 2 * 1024 * 1024
const MIN_DIM := 64
const MAX_DIM := 4096


## Grab one drawn frame from `viewport` and encode it. opts: region (Rect2i,
## image pixels), region_control (Control — resolved to a region AFTER the
## drawn frame and against the real image size, so layout still settling when
## the capture starts cannot skew the crop), max_dim, format ("webp"|"png"),
## quality. The optional cancellation probe is sampled before capture and
## after every awaited frame. Returns { ok, bytes, mime, width, height,
## warning? } or { ok: false, error }.
static func capture(
	viewport: Viewport,
	opts := {},
	cancel_requested: Callable = Callable()
) -> Dictionary:
	if _cancel_requested(cancel_requested):
		return { "ok": false, "error": "Screenshot capture was cancelled." }
	if DisplayServer.get_name() == "headless":
		return { "ok": false, "error": "No rendering in headless mode — screenshots need the windowed editor." }
	if viewport == null:
		return { "ok": false, "error": "No viewport to capture." }
	var tree := Engine.get_main_loop() as SceneTree
	if tree == null:
		return { "ok": false, "error": "No scene tree to await frames on." }
	var drawn := { "done": false }
	var on_draw := func() -> void: drawn["done"] = true
	RenderingServer.frame_post_draw.connect(on_draw, Object.CONNECT_ONE_SHOT)
	var frames := 0
	while not drawn["done"] and frames < DRAW_TIMEOUT_FRAMES \
			and not _cancel_requested(cancel_requested):
		await tree.process_frame
		frames += 1
	if _cancel_requested(cancel_requested):
		if RenderingServer.frame_post_draw.is_connected(on_draw):
			RenderingServer.frame_post_draw.disconnect(on_draw)
		return { "ok": false, "error": "Screenshot capture was cancelled." }
	if not drawn["done"]:
		if RenderingServer.frame_post_draw.is_connected(on_draw):
			RenderingServer.frame_post_draw.disconnect(on_draw)
		return { "ok": false, "error": "No frame drawn within %d frames — is the editor window minimized?" % DRAW_TIMEOUT_FRAMES }
	var texture := viewport.get_texture()
	var image := texture.get_image() if texture != null else null
	if image == null or image.is_empty():
		return { "ok": false, "error": "Viewport returned no image." }
	var region_control: Variant = opts.get("region_control")
	if region_control is Control and is_instance_valid(region_control):
		opts = opts.duplicate()
		opts["region"] = region_for_control(region_control, image.get_size())
	return encode(image, opts)


static func _cancel_requested(source: Callable) -> bool:
	return source.is_valid() and bool(source.call())


## Crop / downscale / compress an image into MCP-ready bytes. Same opts and
## result shape as capture().
static func encode(image: Image, opts := {}) -> Dictionary:
	if image == null or image.is_empty():
		return { "ok": false, "error": "No image to encode." }
	if opts.has("region"):
		var region: Rect2i = opts["region"]
		region = region.intersection(Rect2i(Vector2i.ZERO, image.get_size()))
		if region.size.x <= 0 or region.size.y <= 0:
			return { "ok": false, "error": "Crop region %s lies outside the %s image." % [opts["region"], image.get_size()] }
		image = image.get_region(region)
	else:
		# get_region copies; without a crop, duplicate so resize/convert below
		# never mutate the caller's image.
		image = image.duplicate()
	var max_dim := clampi(int(opts.get("max_dim", DEFAULT_MAX_DIM)), MIN_DIM, MAX_DIM)
	_shrink_to(image, max_dim)
	if image.get_format() != Image.FORMAT_RGBA8:
		image.convert(Image.FORMAT_RGBA8)
	var format := String(opts.get("format", "webp"))
	var quality := clampf(float(opts.get("quality", DEFAULT_QUALITY)), 0.1, 1.0)
	var encoded := _compress(image, format, quality)
	if (encoded["bytes"] as PackedByteArray).size() > SIZE_RETRY_BYTES and format != "png":
		_shrink_to(image, maxi(int(image.get_width() * 0.75), MIN_DIM))
		encoded = _compress(image, format, minf(quality, 0.6))
	var bytes: PackedByteArray = encoded["bytes"]
	if bytes.is_empty():
		return { "ok": false, "error": "Image encoding failed." }
	var out := {
		"ok": true,
		"bytes": bytes,
		"mime": encoded["mime"],
		"width": image.get_width(),
		"height": image.get_height(),
	}
	if bytes.size() > SIZE_RETRY_BYTES:
		out["warning"] = "Large image: %d KiB even after retry." % (bytes.size() / 1024)
	return out


## Map a Control's on-screen rect to captured-image pixels (the window image
## and the window can differ in size under scaling). Compute AFTER the draw
## await so layout changes mid-capture cannot skew it — capture() does this
## for you when you pass the control as opts.region_control.
static func region_for_control(control: Control, image_size: Vector2i) -> Rect2i:
	var window := control.get_window()
	if window == null:
		return Rect2i(Vector2i.ZERO, image_size)
	var rect := control.get_global_rect()
	var window_size := Vector2(window.size)
	if window_size.x <= 0.0 or window_size.y <= 0.0:
		return Rect2i(Vector2i.ZERO, image_size)
	var scale := Vector2(image_size) / window_size
	return Rect2i(
		Vector2i((rect.position * scale).floor()),
		Vector2i((rect.size * scale).ceil()))


static func _shrink_to(image: Image, max_dim: int) -> void:
	var largest := maxi(image.get_width(), image.get_height())
	if largest <= max_dim:
		return
	var scale := float(max_dim) / float(largest)
	image.resize(
		maxi(int(roundf(image.get_width() * scale)), 1),
		maxi(int(roundf(image.get_height() * scale)), 1),
		Image.INTERPOLATE_LANCZOS)


static func _compress(image: Image, format: String, quality: float) -> Dictionary:
	if format == "png":
		return { "bytes": image.save_png_to_buffer(), "mime": "image/png" }
	var webp := image.save_webp_to_buffer(true, quality)
	if webp.is_empty():
		return { "bytes": image.save_png_to_buffer(), "mime": "image/png" }
	return { "bytes": webp, "mime": "image/webp" }
