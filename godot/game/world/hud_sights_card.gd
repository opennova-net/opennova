class_name HudSightsCard
extends Control

## The standard SIGHTS card: one child control per authored weapon.def SIGHTS
## row (order preserved, per-row blend mode), mounted behind the HudOverlay's
## compiled draw list — the original draws the card at scene end and the HUD
## overlays after [orig: draw_weapon_sight_overlays @0x4dce00 from
## render_hud_overlay @0x5d82da; HUD_RenderAllOverlays runs later in the
## frame]. Row rects live in the virtual 1024x768 design space and scale to the
## live viewport per draw [orig: Viewport_ScaleToVirtualCoords @0x5d2b20].
## Additive rows ride a per-row CanvasItemMaterial — the blend token map
## [orig: sub_540180 blend/add/...].
##
## This stays a shell-side child-control stack (not a HudDrawList element)
## because each row needs its own CanvasItem for its blend mode; the engine
## compiler's sights element is deliberately left unfed by HudOverlay.

class SightRowControl:
	extends Control
	const DESIGN := Vector2(1024, 768)
	var tex: Texture2D
	var rect_v := Rect2()

	func _draw() -> void:
		if tex == null:
			return
		var s := get_viewport_rect().size
		var scale_v := Vector2(s.x / DESIGN.x, s.y / DESIGN.y)
		draw_texture_rect(tex, Rect2(rect_v.position * scale_v, rect_v.size * scale_v), false)

	func _notification(what: int) -> void:
		if what == NOTIFICATION_RESIZED:
			queue_redraw()


var _rows: Array = []
var _card_up := false


## Rebuild the card for the equipped weapon's authored SIGHTS rows
## ({texture,x1,y1,x2,y2,blend,...} dicts in draw order; an empty array clears
## the card). Rows stay hidden until set_card_up(true).
func set_weapon_sights(sights: Array, root: ResourceRoot) -> void:
	for row in _rows:
		if is_instance_valid(row):
			row.queue_free()
	_rows.clear()
	for entry in sights:
		var e: Dictionary = entry
		var tex := _load_texture(root, String(e.get("texture", "")))
		if tex == null:
			continue
		var row := SightRowControl.new()
		row.tex = tex
		var x1 := float(e.get("x1", 0))
		var y1 := float(e.get("y1", 0))
		row.rect_v = Rect2(x1, y1, float(e.get("x2", 0)) - x1, float(e.get("y2", 0)) - y1)
		if int(e.get("blend", 0)) == 1:
			var mat := CanvasItemMaterial.new()
			mat.blend_mode = CanvasItemMaterial.BLEND_MODE_ADD
			row.material = mat
		row.set_anchors_preset(Control.PRESET_FULL_RECT)
		row.mouse_filter = Control.MOUSE_FILTER_IGNORE
		row.visible = _card_up
		add_child(row)
		_rows.append(row)


## The simulation's dynamic card selector verdict (Scoped or Sighted at settled
## first-person ADS), already folded with the binocular suppression by the
## presenter. [orig: Render_ProcessMainSceneFrame @0x5ca299..0x5ca304 /
## @0x5caaf3..0x5cab15]
func set_card_up(up: bool) -> void:
	_card_up = up
	for row in _rows:
		if is_instance_valid(row):
			row.visible = up


func is_card_up() -> bool:
	return _card_up


func row_count() -> int:
	return _rows.size()


static func _load_texture(root: ResourceRoot, name: String) -> Texture2D:
	if root == null or name.is_empty():
		return null
	var bytes := root.read_file(name.get_file())
	if bytes.is_empty():
		return null
	var img := Image.new()
	if img.load_tga_from_buffer(bytes) != OK:
		return null
	return ImageTexture.create_from_image(img)
