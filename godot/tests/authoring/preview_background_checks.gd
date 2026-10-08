extends RefCounted

## What a preview's device draws behind its picture: the editor's Preview background preference
## (editor/session/preview_background.h), drawn by authoring/preview_backdrop. A 3D picture's (the model's, the
## effect's, the definition's) is the quad "PreviewBackdrop" its shader lays over the whole view at the far plane,
## hidden on Dark (the view's own clear colour, as before the preference); a 2D picture's is a shader over a
## ColorRect it draws anyway (the texture's "Texture", the HUD's "Backdrop"), its own colour on Dark. Each preview's
## GUT file checks its device through here: the default (Grey) as the device first draws, then each background set
## as the editor sets it (set_preview_background), taken by the device at the next pump, live. Test-only, preloaded
## by path (`preload("res://tests/authoring/preview_background_checks.gd")`).

## Each background's shader mode (0 the picture's own, 1 the top-to-bottom fill, 2 the checker) and its top colour
## as it shows on screen.
const MODES := {"dark": 0, "grey": 1, "light": 1, "checker": 2}
const TOPS := {"grey": Vector3(90.0, 90.0, 90.0) / 255.0, "light": Vector3(210.0, 210.0, 210.0) / 255.0}
const BOTTOMS := {"grey": Vector3(60.0, 60.0, 60.0) / 255.0, "light": Vector3(180.0, 180.0, 180.0) / 255.0}


## The backdrop `device` draws: {material, shown, quad} (quad: a 3D picture's), {} for none. A 2D picture's is the
## ColorRect named `rect_name`.
static func read(device: SubViewport, rect_name: String) -> Dictionary:
	if device == null:
		return {}
	var quads := device.find_children("PreviewBackdrop", "MeshInstance3D", true, false)
	if not quads.is_empty():
		var quad := quads[0] as MeshInstance3D
		return {"material": quad.material_override, "shown": quad.visible, "quad": true}
	var rects := device.find_children(rect_name, "ColorRect", true, false)
	if rects.is_empty():
		return {}
	var rect := rects[0] as ColorRect
	return {"material": rect.material, "shown": rect.visible, "quad": false}


## The default and then each background, the device drawing each after the next pump.
static func check_each(test: GutTest, app: Node, seam: RefCounted, path: String, kind: String, rect_name := "") -> void:
	var first := true
	for background: String in ["grey", "light", "checker", "dark", "grey"]:
		if not first:
			var answer: Dictionary = seam.request({"kind": "set_preview_background", "preview_background": background})
			test.assert_true(bool(answer.get("outcome", {}).get("done", false)), str(answer))
		first = false
		app.pump()
		var said := String(seam.state(["preferences"]).get("preferences", {}).get("preview_background", ""))
		test.assert_eq(said, background, "the preference in effect")
		var drawn := read(app.get_viewport_device(path, kind), rect_name)
		test.assert_false(drawn.is_empty(), "%s's device draws a backdrop" % kind)
		if drawn.is_empty():
			return
		var material := drawn["material"] as ShaderMaterial
		test.assert_not_null(material, "%s's backdrop is a shader" % kind)
		if material == null:
			return
		var where := "%s on %s" % [kind, background]
		test.assert_eq(int(material.get_shader_parameter("backdrop_mode")), int(MODES[background]), where)
		if bool(drawn["quad"]):
			test.assert_eq(bool(drawn["shown"]), background != "dark", where + ": the quad shown but on Dark")
		else:
			test.assert_true(bool(drawn["shown"]), where)
		if TOPS.has(background):
			var top: Vector3 = material.get_shader_parameter("backdrop_top")
			var bottom: Vector3 = material.get_shader_parameter("backdrop_bottom")
			test.assert_almost_eq(top, TOPS[background] as Vector3, Vector3.ONE * 0.002, where + ": its top")
			test.assert_almost_eq(bottom, BOTTOMS[background] as Vector3, Vector3.ONE * 0.002, where + ": its bottom")
