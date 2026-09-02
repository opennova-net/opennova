class_name HudSightsCard
extends Control

## The standard SIGHTS card: one child control per authored weapon.def SIGHTS
## row (order preserved, per-row blend mode), mounted behind the HudOverlay's
## compiled draw list — the original draws the card at scene end and the HUD
## overlays after [orig: draw_weapon_sight_overlays @0x4dce00 from
## render_hud_overlay @0x5d82da; HUD_RenderAllOverlays runs later in the
## frame]. Row rects live in the virtual 1024x768 design space and scale to the
## live viewport per draw [orig: Viewport_ScaleToVirtualCoords @0x5d2b20].
## Rows carry the six-mode retail material map, including the doubled-source
## multiply equation and the alpha-test variants [orig:
## WeaponDef_CreateBlendNamedMaterial @0x540180; blend decoder @0x680f00;
## CGfxDevice_SetAlphaTestRef(128) @0x5ccdae].
##
## This stays a shell-side child-control stack (not a HudDrawList element)
## because each row needs its own CanvasItem for its blend mode; the engine
## compiler's sights element is deliberately left unfed by HudOverlay.

enum SightBlendMode {
	BLEND,
	ADD,
	BLEND_AT,
	MULTIPLY,
	ADD_AT,
	MULTIPLY_AT,
}

const BLEND_AT_SHADER_CODE := """
shader_type canvas_item;
render_mode blend_mix;

void fragment() {
	vec4 texel = texture(TEXTURE, UV);
	if (texel.a <= 128.0 / 255.0) {
		discard;
	}
	COLOR = texel;
}
"""
const ADD_AT_SHADER_CODE := """
shader_type canvas_item;
render_mode blend_add;

void fragment() {
	vec4 texel = texture(TEXTURE, UV);
	if (texel.a <= 128.0 / 255.0) {
		discard;
	}
	COLOR = texel;
}
"""
const MULTIPLY_SHADER_CODE := """
shader_type canvas_item;
render_mode blend_mul;
uniform bool alpha_test = false;

void fragment() {
	vec4 texel = texture(TEXTURE, UV);
	if (alpha_test && texel.a <= 128.0 / 255.0) {
		discard;
	}
	COLOR = vec4(texel.rgb * 2.0, texel.a);
}
"""

class SightRowControl:
	extends Control
	var tex: Texture2D
	var rect_v := Rect2()

	func _draw() -> void:
		if tex == null:
			return
		# The virtual design space and rect scaling are the engine's
		# (HudPos.DESIGN_* / scale_rect — the witness lives at the engine home,
		# engine/runtime/hud hud/hud_math.h).
		draw_texture_rect(tex,
				HudPos.scale_rect(rect_v, get_viewport_rect().size), false)

	func _notification(what: int) -> void:
		if what == NOTIFICATION_RESIZED:
			queue_redraw()


var _rows: Array = []
var _card_up := false


## Rebuild the card for the equipped weapon's authored SIGHTS rows
## ({texture,x1,y1,x2,y2,blend,...} dicts in draw order; an empty array clears
## the card). Rows stay hidden until set_card_up(true).
func set_weapon_sights(sights: Array[WeaponSightRow], root: ResourceRoot) -> void:
	for row in _rows:
		if is_instance_valid(row):
			row.queue_free()
	_rows.clear()
	for e: WeaponSightRow in sights:
		var tex := _load_texture(root, e.get_texture())
		if tex == null:
			continue
		var row := SightRowControl.new()
		row.tex = tex
		var x1 := float(e.get_x1())
		var y1 := float(e.get_y1())
		row.rect_v = Rect2(x1, y1, float(e.get_x2()) - x1, float(e.get_y2()) - y1)
		var material := _material_for_blend(e.get_blend())
		if material != null:
			row.material = material
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


static func _material_for_blend(blend: int) -> Material:
	match blend:
		SightBlendMode.ADD:
			var material := CanvasItemMaterial.new()
			material.blend_mode = CanvasItemMaterial.BLEND_MODE_ADD
			return material
		SightBlendMode.BLEND_AT:
			return _shader_material(BLEND_AT_SHADER_CODE)
		SightBlendMode.MULTIPLY:
			return _multiply_material(false)
		SightBlendMode.ADD_AT:
			return _shader_material(ADD_AT_SHADER_CODE)
		SightBlendMode.MULTIPLY_AT:
			return _multiply_material(true)
	return null


static func _multiply_material(alpha_test: bool) -> ShaderMaterial:
	var material := _shader_material(MULTIPLY_SHADER_CODE)
	material.set_shader_parameter("alpha_test", alpha_test)
	return material


static func _shader_material(code: String) -> ShaderMaterial:
	var shader := Shader.new()
	shader.code = code
	var material := ShaderMaterial.new()
	material.shader = shader
	return material


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
