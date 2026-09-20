class_name ProbeCapture
extends RefCounted

## The viewport self-readback the capture probes share.


## Pixels whose RGB moved by more than `threshold` (channels summed) between
## two same-size captures, skipping the top `skip_rows` (a caption band whose
## BEFORE/AFTER label change is intentional); -1 when the images cannot be
## compared.
static func changed_pixels(reference: Image, candidate: Image, skip_rows: int,
		threshold := 0.12) -> int:
	if reference == null or candidate == null \
			or reference.get_size() != candidate.get_size():
		return -1
	var changed := 0
	for y_value in range(skip_rows, reference.get_height()):
		for x_value in range(reference.get_width()):
			var before := reference.get_pixel(x_value, y_value)
			var after := candidate.get_pixel(x_value, y_value)
			if absf(before.r - after.r) + absf(before.g - after.g) \
					+ absf(before.b - after.b) > threshold:
				changed += 1
	return changed


## True when any pixel carries a colour channel above the near-black floor
## (24/255): the "did this surface present a frame" test.
static func has_visible_color(source: Image) -> bool:
	var image: Image = source.duplicate()
	image.convert(Image.FORMAT_RGBA8)
	var bytes: PackedByteArray = image.get_data()
	for offset in range(0, bytes.size(), 4):
		if bytes[offset] > 24 or bytes[offset + 1] > 24 or bytes[offset + 2] > 24:
			return true
	return false


## After the next frame is drawn, read the viewport back and save it as PNG.
## False when the readback yielded no image or the save failed.
static func save_viewport_png(viewport: Viewport, path: String) -> bool:
	await RenderingServer.frame_post_draw
	var img: Image = viewport.get_texture().get_image()
	if img == null:
		return false
	return img.save_png(path) == OK
