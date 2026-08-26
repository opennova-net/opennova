class_name ProbeCapture
extends RefCounted

## The viewport self-readback the capture probes share.


## After the next frame is drawn, read the viewport back and save it as PNG.
## False when the readback yielded no image or the save failed.
static func save_viewport_png(viewport: Viewport, path: String) -> bool:
	await RenderingServer.frame_post_draw
	var img: Image = viewport.get_texture().get_image()
	if img == null:
		return false
	return img.save_png(path) == OK
