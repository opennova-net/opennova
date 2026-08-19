class_name GameRenderCapture
extends RefCounted

## One deep module for durable comparison evidence. It waits for the requested
## settle frames, samples diagnostics inside one completed-draw signal, reads
## that viewport texture without resampling, and atomically writes a
## lossless PNG plus JSON sidecar under a fixed user-data root.

const SCHEMA := "OpenNovaRenderCaptureV1"
const DEFAULT_ROOT := "user://render-captures"
const PRESENTATION_BEGIN_OPTION := "_presentation_begin"
const PRESENTATION_FINISH_OPTION := "_presentation_finish"


static func capture(
		viewport: Viewport,
		diagnostics_source: Callable,
		opts: Dictionary,
		cancel_requested: Callable = Callable()) -> Dictionary:
	if viewport == null:
		return {"error": "No gameplay viewport is available for render capture."}
	if not diagnostics_source.is_valid():
		return {"error": "No render-diagnostics provider is available."}
	var tree := Engine.get_main_loop() as SceneTree
	if tree == null:
		return {"error": "No scene tree is available for render capture."}
	var presentation_begin: Variant = opts.get(PRESENTATION_BEGIN_OPTION)
	var presentation_finish: Variant = opts.get(PRESENTATION_FINISH_OPTION)
	var has_presentation := presentation_begin is Callable \
			and (presentation_begin as Callable).is_valid()
	if has_presentation != (presentation_finish is Callable \
			and (presentation_finish as Callable).is_valid()):
		return {"error": "Render capture presentation requires paired begin/finish actions."}
	for _frame in range(int(opts.get("settle_frames", 2))):
		if _cancelled(cancel_requested):
			return {"error": "Game render capture was cancelled."}
		await tree.process_frame
	if _cancelled(cancel_requested):
		return {"error": "Game render capture was cancelled."}
	if has_presentation:
		var begin_value: Variant = (presentation_begin as Callable).call()
		var begin_error := int(begin_value) \
				if typeof(begin_value) == TYPE_INT else ERR_UNAVAILABLE
		if begin_error != OK:
			return {"error": "Could not apply render capture presentation: %s." \
					% error_string(begin_error)}
	var captured: Dictionary = await McpScreenshot.capture(viewport, {
		"format": "png",
		"preserve_size": true,
		"_frame_probe": diagnostics_source,
	}, cancel_requested)
	if has_presentation:
		(presentation_finish as Callable).call()
	if not bool(captured.get("ok", false)):
		return {"error": String(captured.get("error", "Game render capture failed."))}
	var diagnostics_value: Variant = captured.get("frame_probe_value")
	if not (diagnostics_value is Dictionary) \
			or (diagnostics_value as Dictionary).is_empty():
		return {"error": "The captured frame has no render diagnostics."}
	var written := write_bundle(
			captured["bytes"],
			int(captured["width"]),
			int(captured["height"]),
			diagnostics_value,
			String(opts.get("label", "render")),
			DEFAULT_ROOT,
			int(captured.get("captured_at_ticks_usec", Time.get_ticks_usec())),
			int(captured.get("process_frame", Engine.get_process_frames())))
	if not bool(written.get("ok", false)):
		return {"error": String(written.get("error", "Could not write render capture."))}
	var bundle: Dictionary = written["bundle"]
	# The PNG rides MCP image content but never structuredContent or the sidecar.
	bundle["image_bytes"] = captured["bytes"]
	return bundle


## Pure persistence half used by headless tests. `root` is injectable only for
## tests; production always calls through capture() and DEFAULT_ROOT.
static func write_bundle(
		png: PackedByteArray,
		width: int,
		height: int,
		diagnostics: Dictionary,
		label: String,
		root: String = DEFAULT_ROOT,
		timestamp_usec: int = -1,
		captured_process_frame: int = -1) -> Dictionary:
	if png.is_empty() or width <= 0 or height <= 0:
		return {"ok": false, "error": "A render bundle requires a non-empty PNG and positive dimensions."}
	var safe_label := _safe_label(label)
	var stamp := timestamp_usec if timestamp_usec >= 0 else Time.get_ticks_usec()
	var diagnostic_frame := -1
	var frame_value: Variant = diagnostics.get("frame", {})
	if frame_value is Dictionary:
		diagnostic_frame = int((frame_value as Dictionary).get("process", -1))
	if diagnostic_frame < 0:
		return {"ok": false, "error": "Render diagnostics carry no completed process-frame serial."}
	if captured_process_frame >= 0 and captured_process_frame != diagnostic_frame:
		return {"ok": false, "error": (
				"Captured draw frame %d does not match diagnostics frame %d."
				% [captured_process_frame, diagnostic_frame])}
	var process_frame := captured_process_frame \
			if captured_process_frame >= 0 else diagnostic_frame
	var capture_id := "%s-%d-f%d" % [safe_label, stamp, process_frame]
	var absolute_root := ProjectSettings.globalize_path(root).simplify_path()
	var mkdir_error := DirAccess.make_dir_recursive_absolute(absolute_root)
	if mkdir_error != OK:
		return {"ok": false, "error": "Could not create render-capture directory: %s." % error_string(mkdir_error)}
	var png_path := absolute_root.path_join(capture_id + ".png")
	var state_path := absolute_root.path_join(capture_id + ".json")
	var png_hash := _sha256(png)
	var capture_state := {
		"schema": SCHEMA,
		"capture": {
			"id": capture_id,
			"label": safe_label,
			"process_frame": process_frame,
			"captured_at_ticks_usec": stamp,
			"png_path": png_path,
			"png_sha256": png_hash,
			"width": width,
			"height": height,
			"mime": "image/png",
		},
		"diagnostics": McpJson.sanitize(diagnostics),
	}
	var png_error := _write_atomic(png_path, png)
	if png_error != OK:
		return {"ok": false, "error": "Could not write lossless render PNG: %s." % error_string(png_error)}
	var state_bytes := JSON.stringify(capture_state, "\t").to_utf8_buffer()
	var state_error := _write_atomic(state_path, state_bytes)
	if state_error != OK:
		DirAccess.remove_absolute(png_path)
		return {"ok": false, "error": "Could not write render state sidecar: %s." % error_string(state_error)}
	return {
		"ok": true,
		"bundle": {
			"schema": SCHEMA,
			"capture_id": capture_id,
			"artifact": {
				"png_path": png_path,
				"state_path": state_path,
				"width": width,
				"height": height,
				"mime": "image/png",
				"bytes": png.size(),
				"sha256": png_hash,
				"state_sha256": _sha256(state_bytes),
			},
			"diagnostics": diagnostics,
		},
	}


static func _write_atomic(path: String, bytes: PackedByteArray) -> Error:
	var temp := "%s.tmp.%d" % [path, OS.get_process_id()]
	var file := FileAccess.open(temp, FileAccess.WRITE)
	if file == null:
		return FileAccess.get_open_error()
	file.store_buffer(bytes)
	file.flush()
	var error := file.get_error()
	file.close()
	if error != OK:
		DirAccess.remove_absolute(temp)
		return error
	if FileAccess.file_exists(path):
		DirAccess.remove_absolute(temp)
		return ERR_ALREADY_EXISTS
	var rename_error := DirAccess.rename_absolute(temp, path)
	if rename_error != OK:
		DirAccess.remove_absolute(temp)
	return rename_error


static func _sha256(bytes: PackedByteArray) -> String:
	var hash := HashingContext.new()
	if hash.start(HashingContext.HASH_SHA256) != OK:
		return ""
	hash.update(bytes)
	return hash.finish().hex_encode()


static func _safe_label(label: String) -> String:
	var source := label.strip_edges()
	if source.is_empty():
		source = "render"
	var out := ""
	var separator := false
	for character in source:
		var code := String(character).unicode_at(0)
		var accepted := (code >= 48 and code <= 57) \
				or (code >= 65 and code <= 90) \
				or (code >= 97 and code <= 122) \
				or character == "_" or character == "-"
		if accepted:
			out += character
			separator = false
		elif not separator and not out.is_empty():
			out += "-"
			separator = true
	out = out.trim_suffix("-")
	return out.substr(0, 64) if not out.is_empty() else "render"


static func _cancelled(source: Callable) -> bool:
	return source.is_valid() and bool(source.call())
