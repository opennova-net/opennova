class_name GameHud
extends Control

## The runtime in-game HUD overlay. A full-rect Control on the gameplay HUD layer
## (a child of the $HUD CanvasLayer, so the shell's _set_hud_visible hides it behind
## menus). It draws the witnessed HUD elements from a hudpos.def layout (NovaHudPos)
## plus a per-frame "info" dict the presenter rebuilds each frame — mirroring the original,
## which rebuilds its per-frame HUD info struct every frame and then dispatches the
## element draws. [orig: HUD_BuildEntityInfo @0x4b8440 -> HUD_RenderOverlays @0x5a7bb0]
##
## Elements: static frame, health bar, stance indicator (cross-faded), the weapon
## cluster (ammo count + weapon name at their hudpos anchors, the HUDCLIPGFX/HUDRNDGFX
## clip indicator, the spreading crosshair), and the triggered-text message feed.
## Deferred: radar/minimap, MP objective status + team tile, parachute/armor icons
## (no entity-flag source yet) — see docs/interface/hud-re.md.

## The user crosshair-style index; the original loads "cross%02d.tga" (index+1) from
## the player config. [orig: HUD_LoadAllTextures @0x59e3d6, style @0x25510dc]
const MIN_CROSSHAIR_STYLE := 0
const MAX_CROSSHAIR_STYLE := 24
const DEFAULT_CHAT_LINES := 8 # HUDCHLINE fallback
const STANCE_FRAME_COUNT := 6
const PlayerViewEffectsScript := preload("res://engine/world/player_view_effects.gd")

var _hudpos: NovaHudPos
var _root: NovaResourceRoot
var _font: FontFile
var _frame_tex: Texture2D
var _crosshair_tex: Texture2D
var _crosshair_style := MIN_CROSSHAIR_STYLE
var _stance_textures: Array[Texture2D] = []
var _stance_offsets: Array[Vector2] = []
var _positions: Dictionary = {}
var _rects: Dictionary = {}
var _colors: Dictionary = {}
var _alpha_fade := Vector3.ZERO
var _chat_lines := DEFAULT_CHAT_LINES

# The equipped weapon's HUD slice (PlayerHudWeaponDef, null = no weapon), its resolved
# WepDes display name, and the loaded HUDCLIPGFX/HUDRNDGFX textures.
var _weapon: PlayerHudWeaponDef = null
var _weapon_display_name := ""
var _clip_tex: Texture2D
var _round_tex: Texture2D

# The standard SIGHTS card: one child control per authored row (order preserved,
# per-row blend mode), drawn BEHIND this control's own elements so the HUD text/bars
# stay readable over the card — the original draws the card at scene end and the HUD
# overlays after [orig: draw_weapon_sight_overlays @0x4dce00 from render_hud_overlay
# @0x5d82da; HUD_RenderAllOverlays runs later in the frame]. Row rects live in the
# virtual 1024x768 design space and scale to the live viewport per draw
# [orig: Viewport_ScaleToVirtualCoords @0x5d2b20].
var _card_rows: Array = []
var _view_effects = null # PlayerViewEffects; preloaded explicitly above


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

var _clip_indicator := HudClipIndicator.new()
var _messages := HudMessages.new()

# Stance cross-fade state. [orig: byte_2723D3C/D3D prev/current + stamp @0x2723D38]
var _stance_prev := 0
var _stance_cur := 0
var _stance_stamp := 0

# Per-frame state, rebuilt by the presenter. Defaults keep the HUD sane before the first update.
var _info: Dictionary = {
	"health_fraction": 1.0,
	"stance": 0,
	"team": 0,
	"objective": "",
	"weapon_active": false,
	"clip": -1,
	"reserve": -1,
	"scope_engaged": false,
	"binoculars_view_active": false,
	"binocular_range": 1,
	"nvg_visible": false,
	"nvg_gain": 0,
	"fov_deg": 80.0,
	"ticks": 0,
}


func set_layout(hudpos: NovaHudPos, root: NovaResourceRoot) -> void:
	_ensure_view_effects()
	_hudpos = hudpos
	_root = root
	_font = null
	_frame_tex = null
	_crosshair_tex = null
	_stance_textures.clear()
	_stance_offsets.clear()
	_positions = {}
	_rects = {}
	_colors = {}
	_alpha_fade = Vector3.ZERO
	_chat_lines = DEFAULT_CHAT_LINES
	_load_assets()
	_view_effects.set_resource_root(root)
	queue_redraw()


func _ensure_view_effects() -> void:
	if _view_effects != null:
		return
	_view_effects = PlayerViewEffectsScript.new()
	_view_effects.name = "PlayerViewEffects"
	_view_effects.show_behind_parent = true
	_view_effects.mouse_filter = Control.MOUSE_FILTER_IGNORE
	# Internal children stay out of authored SIGHTS-row traversal and scene ownership,
	# while INTERNAL_MODE_BACK gives the post-process/masks a stable layer below them.
	add_child(_view_effects, false, Node.INTERNAL_MODE_BACK)
	_view_effects.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)


## Select and immediately reload the configured crosshair art. Style 0 is cross01.tga;
## style 24 is cross25.tga, matching the retail options range.
func set_crosshair_style(style: int) -> void:
	_crosshair_style = clampi(style, MIN_CROSSHAIR_STYLE, MAX_CROSSHAIR_STYLE)
	_crosshair_tex = _load_texture("cross%02d.tga" % (_crosshair_style + 1))
	queue_redraw()


## Install the equipped weapon's HUD slice (null = no weapon) plus its resolved
## display name; loads the clip/round graphics and resets the flash state.
## Mirrors the per-frame weapon-def pointer of the original's HUD info struct.
## [orig: HUD_BuildEntityInfo @0x4b8561; textures HUD_LoadAllTextures @0x59e246]
func set_weapon(weapon: PlayerHudWeaponDef, display_name: String) -> void:
	_weapon = weapon
	_weapon_display_name = display_name
	_clip_tex = _load_texture(weapon.clipgfx_texture) if weapon != null else null
	_round_tex = _load_texture(weapon.rndgfx_texture) if weapon != null else null
	_clip_indicator.reset()
	_rebuild_sights_card()
	queue_redraw()


# Build the SIGHTS card rows for the equipped weapon (authored order; add rows get an
# additive canvas material — the blend token map [orig: sub_540180 blend/add/...]).
# Rows draw behind this control's own elements and stay hidden until the card is up.
func _rebuild_sights_card() -> void:
	for row in _card_rows:
		if is_instance_valid(row):
			row.queue_free()
	_card_rows.clear()
	if _weapon == null:
		return
	for entry in _weapon.sights:
		var e: Dictionary = entry
		var tex := _load_texture(String(e.get("texture", "")))
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
		row.show_behind_parent = true
		row.set_anchors_preset(Control.PRESET_FULL_RECT)
		row.mouse_filter = Control.MOUSE_FILTER_IGNORE
		row.visible = false
		add_child(row)
		_card_rows.append(row)


# The simulation reports the original's dynamic selector: Scoped or Sighted at
# settled first-person ADS, with NoCardSwitch suppressing both unless ForceScoped
# overrides it. The FP viewmodel hides on the same bit. [orig:
# Render_ProcessMainSceneFrame @0x5ca299..0x5ca304 / @0x5caaf3..0x5cab15]
func _sync_sights_card() -> void:
	var up: bool = bool(_info.get("scope_card", false)) \
			and not bool(_info.get("binoculars_view_active", false)) \
			and not _card_rows.is_empty()
	for row in _card_rows:
		if is_instance_valid(row):
			row.visible = up


## A mission triggered-text line for the message feed. The original pushes color -1
## = raw ARGB 0xFFFFFFFF (opaque white); the chat drawer's own default handling is
## the D-HUD-6 follow-up. [orig: HUD_DisplayTriggeredText @0x51f190 ->
## Chat_AddDebugMessage(text, -1, 930) @0x51f216]
func push_message(text: String) -> void:
	_messages.push(text, Color.WHITE, int(_info.get("ticks", 0)))
	queue_redraw()


func update_info(info: Dictionary) -> void:
	var stance := int(info.get("stance", 0))
	if stance != _stance_cur:
		# [orig: stance change restamp @0x599f8a]
		_stance_prev = _stance_cur
		_stance_cur = stance
		_stance_stamp = int(info.get("ticks", 0))
	_info = info
	_sync_sights_card()
	if _view_effects != null:
		_view_effects.update_info(info)
	queue_redraw()


func _notification(what: int) -> void:
	if what == NOTIFICATION_RESIZED:
		queue_redraw()


func _load_assets() -> void:
	if _hudpos == null or not _hudpos.is_loaded():
		return
	var font_name := _hudpos.get_font_hi()
	if font_name.is_empty():
		font_name = _hudpos.get_font_lo()
	_font = HudText.load_font(_root, font_name)
	var dict := _hudpos.to_dictionary()
	_positions = dict.get("positions", {})
	_rects = dict.get("rects", {})
	_colors = _hudpos.get_colors()
	var misc: Dictionary = dict.get("misc", {})
	_alpha_fade = misc.get("alpha_fade", Vector3.ZERO)
	var chline := int(misc.get("hud_chline", 0))
	if chline > 0:
		_chat_lines = chline

	var frames := _hudpos.get_static_frames()
	if frames.size() > 0:
		_frame_tex = _load_texture(String((frames[0] as Dictionary).get("texture", "")))
	# HUDSTANCE's explicit id addresses the retail slot arrays; file order is irrelevant
	# and a later record for the same id replaces the earlier one.
	var stance_names: Array[String] = []
	stance_names.resize(STANCE_FRAME_COUNT)
	_stance_offsets.resize(STANCE_FRAME_COUNT)
	for raw_stance in _hudpos.get_stances():
		var stance: Dictionary = raw_stance
		var stance_id := int(stance.get("id", -1))
		if stance_id < 0 or stance_id >= STANCE_FRAME_COUNT:
			continue
		stance_names[stance_id] = String(stance.get("texture", ""))
		_stance_offsets[stance_id] = Vector2(stance.get("offset", Vector2i.ZERO))
	_stance_textures.resize(STANCE_FRAME_COUNT)
	for stance_id in STANCE_FRAME_COUNT:
		_stance_textures[stance_id] = _load_texture(stance_names[stance_id])
	_crosshair_tex = _load_texture("cross%02d.tga" % (_crosshair_style + 1))


func _load_texture(name: String) -> Texture2D:
	if _root == null or name.is_empty():
		return null
	var bytes := _root.read_file(name.get_file())
	if bytes.is_empty():
		return null
	var img := Image.new()
	if img.load_tga_from_buffer(bytes) != OK:
		return null
	return ImageTexture.create_from_image(img)


func _draw() -> void:
	if _hudpos == null or not _hudpos.is_loaded():
		return
	# This overlay is a Control parented directly to a CanvasLayer, which does not drive a child
	# Control's layout — so our own `size` can stay (0,0) and every scaled element would collapse.
	# Draw against the viewport rect, which is the real screen the original scales its HUD to.
	# An owner that sizes this Control (tests pin exact geometry) wins over the
	# viewport rect.
	# [orig: overlayCtx @0x24c1420 screen_w/h -> Viewport_ScaleToVirtualCoords @0x5d2b20]
	var surface := size if size.x > 1.0 and size.y > 1.0 else get_viewport_rect().size
	var ticks := int(_info.get("ticks", 0))

	# Static HUD frame background.
	if _frame_tex != null:
		var frames := _hudpos.get_static_frames()
		if frames.size() > 0:
			var fpos: Vector2i = (frames[0] as Dictionary).get("pos", Vector2i.ZERO)
			draw_texture_rect(_frame_tex, HudLayout.scale_rect(Rect2(fpos, _frame_tex.get_size()), surface), false)

	# Health bar.
	var border: Color = _colors.get("health_border", Color(0.35, 0.78, 0.78, 0.78))
	var good: Color = _colors.get("tagcolor_good", Color(0.02, 0.98, 0.05))
	var mid: Color = _colors.get("tagcolor_middle", Color(0.98, 0.65, 0.03))
	var bad: Color = _colors.get("tagcolor_bad", Color(0.69, 0.04, 0.04))
	HudHealthBar.draw(self, Rect2(_hudpos.get_health_rect()),
		float(_info.get("health_fraction", 1.0)), border, good, mid, bad, surface)

	_draw_stance(surface, ticks)
	_draw_weapon_cluster(surface, ticks)
	_draw_heat_bar(surface)
	_draw_power_bar(surface)
	_draw_waypoint_info(surface)
	_draw_objectives_panel(surface)
	_draw_attach_labels()

	# Objective / status line (the MP objective element's anchor; SP mission text goes
	# through the message feed below). [orig: draw_objective_status_text @0x59aa30]
	var objective := String(_info.get("objective", ""))
	if not objective.is_empty() and _font != null:
		var oc: Color = _colors.get("hud_textcolor", Color(0.98, 0.84, 0.02))
		var gp := _pos_of("game_info", Vector4i(512, 40, 0, 0))
		if gp.z == 0:
			HudText.draw_text(self, _font, Vector2(gp.x, gp.y), surface, objective, oc, gp.w)

	# Triggered-text message feed at the chat-line anchor.
	if _font != null:
		var chat := Vector2(_positions.get("chat_text", Vector2i(142, 711)))
		var tc: Color = _colors.get("hud_textcolor", Color(0.98, 0.84, 0.02))
		_messages.draw(self, _font, chat, surface, ticks, _chat_lines,
			HudLayout.DESIGN_WIDTH - chat.x - 4.0, tc)


# The stance indicator with the witnessed cross-fade: current frame at base+fade,
# previous frame ghosting at quarter fade, both tinted STANCEICON_COLOR, each frame
# at HUDSTANCEPOS plus its own HUDSTANCE offset plus the centering offset shared from
# frame 0. The whole element hides without an ALPHAFADE ramp or with any of the six
# stance frames unloaded, like the original's gates; the MP team tile underneath is
# deferred with the MP HUD. [orig: HUD_DrawStanceIndicator @0x599f10 (D-HUD-1) —
# ramp+texture gates @0x599f18..0x599f50, shared frame-0 scale @0x599fed..0x59a00a]
func _draw_stance(surface: Vector2, ticks: int) -> void:
	var anchor := Vector2(_hudpos.get_stance_pos())
	if anchor == Vector2.ZERO:
		return
	var ramp := int(_alpha_fade.z * HudFade.SECONDS_TO_TICKS)
	if ramp <= 0:
		return
	if _stance_textures.size() < STANCE_FRAME_COUNT:
		return
	for i in STANCE_FRAME_COUNT:
		if _stance_textures[i] == null:
			return
	var frame0_size := Vector2i(_stance_textures[0].get_size())
	var tint: Color = _colors.get("stanceicon_color", Color.WHITE)
	var base_alpha := int(_alpha_fade.x * HudFade.PERCENT_TO_ALPHA)
	var elapsed := ticks - _stance_stamp
	var cur_a := HudFade.stance_current_alpha(elapsed, ramp, base_alpha)
	var prev_a := HudFade.stance_prev_alpha(elapsed, ramp)

	var cur := _stance_cur
	if cur >= 0 and cur < _stance_textures.size() and _stance_textures[cur] != null:
		HudStance.draw(self, _stance_textures[cur], anchor, surface,
			Color(tint.r, tint.g, tint.b, cur_a / 255.0), _stance_offset(cur), frame0_size)
	if prev_a > 0 and _stance_prev != cur \
			and _stance_prev >= 0 and _stance_prev < _stance_textures.size() \
			and _stance_textures[_stance_prev] != null:
		HudStance.draw(self, _stance_textures[_stance_prev], anchor, surface,
			Color(tint.r, tint.g, tint.b, prev_a / 255.0), _stance_offset(_stance_prev), frame0_size)


# The weapon-coupled cluster: ammo count + weapon name, the clip indicator, and the
# crosshair. Nothing draws without an installed weapon FSM (the original gates every
# weapon element on the info struct's weapon-def pointer). [orig: @0x5939f3 / @0x599a67]
func _draw_weapon_cluster(surface: Vector2, ticks: int) -> void:
	if not bool(_info.get("weapon_active", false)) or _weapon == null:
		return
	var clip := int(_info.get("clip", -1))
	var reserve := int(_info.get("reserve", -1))
	var capacity := _weapon.clipsize
	var wc: Color = _colors.get("weapon_textcolor", Color(0.98, 0.84, 0.02))

	if _font != null:
		HudWeaponText.draw_ammo(self, _font, _pos_of("ammo_count", Vector4i.ZERO),
			clip, reserve, capacity, wc, surface)
		HudWeaponText.draw_weapon_name(self, _font, _pos_of("weapon_name", Vector4i.ZERO),
			_weapon_display_name, wc, surface)

	var tint: Color = _colors.get("stanceicon_color", Color.WHITE)
	_clip_indicator.draw(self, Vector2i(_positions.get("clip", Vector2i.ZERO)), _weapon,
		_clip_tex, _round_tex, clip, reserve, tint, _alpha_fade, ticks, surface)

	_draw_crosshair(surface)


# The weapon heat bar: hidden at zero heat, else the HUDHEATBORDER wireframe around
# the HUDHEAT rect plus a stancecolor_bad fill proportional to heat/0x10000 —
# left-to-right for a wide rect, bottom-up for a tall one, inset 1px, with the
# original's +0x8000 round-to-nearest span math. Heat arrives 0..0xFFFF in the
# per-frame info (0 until the weapon heat accumulator is ported — D-HUD-15).
# [orig: HUD_DrawWeaponHeatBar @0x599700 (ex kong "draw_minimap_overlay" misnomer);
#  gate hudInfo+60 @0x59970a; border @0x599787; spans @0x5997a1..0x59981f]
func _draw_heat_bar(surface: Vector2) -> void:
	var heat := int(_info.get("heat", 0))
	if heat <= 0:
		return
	var design: Rect2 = _rects.get("heat", Rect2())
	if design.size.x <= 0.0 and design.size.y <= 0.0:
		return
	var r := HudLayout.scale_rect(design, surface)
	var border: Color = _colors.get("heat_border", Color.WHITE)
	var fill: Color = _colors.get("stancecolor_bad", Color(0.69, 0.04, 0.04))
	draw_rect(r, border, false)
	heat = mini(heat, 0xFFFF)
	if r.size.y <= r.size.x:
		# Horizontal: fill left -> right. [orig: @0x5997f0..0x59981f]
		var span := (int(r.size.x) * heat + 0x8000) >> 16
		draw_rect(Rect2(r.position + Vector2.ONE, Vector2(maxf(span - 2.0, 0.0), r.size.y - 2.0)), fill)
	else:
		# Vertical: fill bottom -> up. [orig: @0x5997b0..0x5997df]
		var vspan := (int(r.size.y) * heat + 0x8000) >> 16
		var top := r.position.y + r.size.y - vspan + 1.0
		draw_rect(Rect2(Vector2(r.position.x + 1.0, top),
			Vector2(r.size.x - 2.0, maxf(r.position.y + r.size.y - 1.0 - top, 0.0))), fill)


# The PowerThrow charge bar at the HUDPOWERBAR rect (x,y,w,h): a wireframe
# outline, an inset fill proportional to the windup, and the percent text 15
# output pixels above. The fill curve is the witnessed windup shape: full during
# the first 31 held ticks (the tap window throws at full power), then restarting
# at 0 and climbing (held-31)/93 to 1. Fill span = (progress_fp16 * w + 0x8000)
# >> 16, drawn (x+1, y+1)-(x+span, y+h-1); everything in the flat 0xFF800000
# half-red the original passes for both rects and the text.
# [orig: HUD_DrawPowerThrowChargeBar @0x599830 (ex kong "HUD_DrawWeaponReloadBar"
#  misnomer): gates @0x59988f (def+8 sign bit + g_fireChargeStartTick + ammo);
#  curve @0x5998ad (1/93 = flt_7CD390, fld1 clamp); fill @0x599964; text
#  "%d%" @0x5999ef at y-15 via HUD_DrawTextLeft_HalfBright @0x5804c0]
func _draw_power_bar(surface: Vector2) -> void:
	if not bool(_info.get("windup_active", false)):
		return
	var design: Rect2 = _rects.get("powerbar", Rect2())
	if design.size.x <= 0.0 or design.size.y <= 0.0:
		return
	var held := int(_info.get("windup_held_ticks", 0))
	var progress := 1.0
	if held >= 31:
		progress = minf(float(held - 31) * (1.0 / 93.0), 1.0)
	var color := Color8(128, 0, 0)  # [orig: the 0xFF800000 constant @0x840b1c]
	var r := HudLayout.scale_rect(design, surface)
	draw_rect(r, color, false)
	var span := (int(progress * 65536.0) * int(r.size.x) + 0x8000) >> 16
	if span > 1:
		draw_rect(Rect2(r.position + Vector2.ONE,
			Vector2(minf(float(span - 1), r.size.x - 2.0), r.size.y - 2.0)), color)
	if _font != null:
		var label_pos := design.position + HudLayout.pixel_delta_to_design(
				Vector2(0, -15), surface)
		HudText.draw_text(self, _font,
			label_pos, surface,
			"%d%%" % int(progress * 100.0), HudText.half_bright(color))


# The waypoint name + distance label at the HUDWPDINFO anchor. Gates: the mission
# ShowWaypoints flag and a live current waypoint (the presenter omits the entry
# otherwise). Field 3 of the token hides only the wireframe BOX (which frames the
# distance number); the element itself has no hide field. Align: 0 = name to the
# right of the distance, 1 = name right-aligned at the anchor with the distance
# shifted left, 2 = name ending left of the anchor. The distance always draws
# right-aligned at its anchor. Color base = hud_textcolor (the original reads the
# master overlay color — the D-HUD-13 base swap applies here too).
# [orig: HUD_DrawWaypointNameAndDistance @0x5947a0; gates @0x5a7daf; box @0x594b0c]
func _draw_waypoint_info(surface: Vector2) -> void:
	var wp: Dictionary = _info.get("waypoint", {})
	if wp.is_empty() or _font == null:
		return
	var name_text := String(wp.get("name", ""))
	var dist_str := "%d" % int(wp.get("distance_m", 0))
	var gp := _pos_of("wpd_info", Vector4i.ZERO)
	if gp == Vector4i.ZERO:
		return
	var anchor := Vector2(gp.x, gp.y)
	var hide_box := gp.z != 0
	var align := gp.w
	var color: Color = _colors.get("hud_textcolor", Color(0.98, 0.84, 0.02))
	var fs := 16
	if _font is FontFile:
		fs = (_font as FontFile).get_fixed_size()
	var dist_w := _font.get_string_size(dist_str, HORIZONTAL_ALIGNMENT_LEFT, -1, fs).x \
			* HudLayout.DESIGN_WIDTH / maxf(surface.x, 1.0)
	var text_h := _font.get_string_size(dist_str, HORIZONTAL_ALIGNMENT_LEFT, -1, fs).y \
			* HudLayout.DESIGN_HEIGHT / maxf(surface.y, 1.0)
	# The distance anchor in design space; the name draws around it per align.
	var dist_anchor := anchor
	var box_left := anchor.x
	var box_right := anchor.x + dist_w + 4.0
	if not name_text.is_empty():
		match align:
			1: # name right-aligned at the anchor; distance shifts left of it
				HudText.draw_text(self, _font, anchor, surface, name_text, color, HudText.Align.RIGHT)
				var name_w := _font.get_string_size(name_text, HORIZONTAL_ALIGNMENT_LEFT, -1, fs).x \
						* HudLayout.DESIGN_WIDTH / maxf(surface.x, 1.0)
				dist_anchor.x = anchor.x - 4.0 - name_w
				box_left = dist_anchor.x - dist_w
				box_right = dist_anchor.x + 4.0
			2: # name left-aligned at the anchor; distance right-aligned just left of it
				HudText.draw_text(self, _font, anchor, surface, name_text, color, HudText.Align.LEFT)
				dist_anchor.x = anchor.x - 4.0
				box_left = dist_anchor.x - dist_w
				box_right = dist_anchor.x + 4.0
			_: # 0: name to the right of the distance number
				HudText.draw_text(self, _font, Vector2(anchor.x + dist_w + 4.0, anchor.y),
					surface, name_text, color, HudText.Align.LEFT)
	if not hide_box:
		var p0 := HudLayout.scale_point(Vector2(box_left, anchor.y - 2.0), surface)
		var p1 := HudLayout.scale_point(Vector2(box_right, anchor.y + text_h - 1.0), surface)
		draw_rect(Rect2(p0, p1 - p0), color, false)
	HudText.draw_text(self, _font, dist_anchor, surface, dist_str, color, HudText.Align.RIGHT)


# The toggled MISSION OBJECTIVES panel: a backing box at the witnessed anchor
# (x=15, y=240 design), the gametext header, then one row per shown win
# condition — a checkbox that gains a checkmark when the subgoal is won, the
# row text dimming to gray on completion (the witnessed +0xFF808081 color fold
# collapses white -> 0x808080 gray at full alpha). The presenter feeds resolved rows
# ({text, done}); an empty array hides the panel (the retail toggle's off
# state). Stand-ins recorded as D-HUD-18: the exact checkbox line geometry
# (the ten draw_clipped_2d_line calls decompile with elided operands) and the
# retail label-box texture ride Godot rects.
# [orig: HUD_DrawWinConditions @0x5ba940 — gate @0x5be14a (dword_24C18CC),
#  anchor @0x5be153 (x=15, y=+0xF0), header STROVER_MISSIONOBJECTIVES
#  @0x5ba986, box HUD_DrawLabelBox @0x5baaba, gray fold @0x5bac86]
func _draw_objectives_panel(surface: Vector2) -> void:
	var rows: Array = _info.get("objectives", [])
	if rows.is_empty() or _font == null:
		return
	var header := "MISSION OBJECTIVES"
	var t: RtxtStringFile = NovaStrings.get_table("gametext")
	if t != null and t.has_string_in_section("Overlays", "STROVER_MISSIONOBJECTIVES"):
		header = t.get_string_in_section("Overlays", "STROVER_MISSIONOBJECTIVES")
	var fs := 16
	if _font is FontFile:
		fs = (_font as FontFile).get_fixed_size()
	var row_h := _font.get_string_size("M", HORIZONTAL_ALIGNMENT_LEFT, -1, fs).y \
			* HudLayout.DESIGN_HEIGHT / maxf(surface.y, 1.0)
	var x := 15.0
	var y := 240.0
	# Panel width tracks the widest line, like the original's measure pass.
	var max_w := _font.get_string_size(header, HORIZONTAL_ALIGNMENT_LEFT, -1, fs).x \
			* HudLayout.DESIGN_WIDTH / maxf(surface.x, 1.0)
	for raw in rows:
		var w := _font.get_string_size(String((raw as Dictionary).get("text", "")),
				HORIZONTAL_ALIGNMENT_LEFT, -1, fs).x * HudLayout.DESIGN_WIDTH / maxf(surface.x, 1.0)
		max_w = maxf(max_w, w)
	var panel := Rect2(Vector2(x, y - 6.0),
			Vector2(max_w + 72.0, (rows.size() + 1) * (row_h + 4.0) + 48.0))
	var p0 := HudLayout.scale_point(panel.position, surface)
	var p1 := HudLayout.scale_point(panel.position + panel.size, surface)
	draw_rect(Rect2(p0, p1 - p0), Color(0, 0, 0, 0.5))
	draw_rect(Rect2(p0, p1 - p0), Color.WHITE, false)
	HudText.draw_text(self, _font, Vector2(x + 24.0, y), surface, header, Color.WHITE)
	var row_y := y + row_h + 10.0
	for raw in rows:
		var row: Dictionary = raw
		var done := bool(row.get("done", false))
		# The checkbox at the row head; the checkmark only when won.
		var b0 := HudLayout.scale_point(Vector2(x + 26.0, row_y + 2.0), surface)
		var b1 := HudLayout.scale_point(Vector2(x + 26.0 + 12.0, row_y + 14.0), surface)
		draw_rect(Rect2(b0, b1 - b0), Color.WHITE, false)
		if done:
			var m0 := HudLayout.scale_point(Vector2(x + 28.0, row_y + 8.0), surface)
			var m1 := HudLayout.scale_point(Vector2(x + 31.0, row_y + 12.0), surface)
			var m2 := HudLayout.scale_point(Vector2(x + 40.0, row_y + 2.0), surface)
			draw_polyline(PackedVector2Array([m0, m1, m2]), Color.WHITE, 2.0)
		var color := Color(0.5, 0.5, 0.5) if done else Color.WHITE
		HudText.draw_text(self, _font, Vector2(x + 48.0, row_y - 2.0), surface,
			String(row.get("text", "")), color)
		row_y += row_h + 4.0


# The floating seat/armory attach labels, presenter-projected to screen pixels: each entry
# {screen: Vector2, text: String, nearest: bool}. The nearest candidate draws the full
# HUD text color; every other label the witnessed dim transform. Distance/LOS/occupancy
# selection happened sim-side; the presenter dropped behind-camera points at projection.
# [orig: draw_vehicle_seat_and_armory_labels @0x5a3290 — called unconditionally by
#  HUD_RenderOverlays @0x5a7daa; nearest full `alpha` color @0x5a362d, others
#  ((rgb & 0xFEFEFE) | 0xFE000001) >> 1 @0x5a364e]
func _draw_attach_labels() -> void:
	var labels: Array = _info.get("attach_labels", [])
	if labels.is_empty() or _font == null:
		return
	var base: Color = _colors.get("hud_textcolor", Color(0.98, 0.84, 0.02))
	for raw in labels:
		var l: Dictionary = raw
		var screen: Vector2 = l.get("screen", Vector2.INF)
		if screen == Vector2.INF:
			continue
		var color := base if bool(l.get("nearest", false)) else HudAttachLabels.dim(base)
		HudAttachLabels.draw(self, _font, screen, String(l.get("text", "")), color)


# The spreading crosshair. Witnessed visibility: it draws when an aimed shot is NOT
# available [orig: the gate @0x592afa — !Player_CanFireWeapon() (plus the
# vehicle/scoped-gunner keep-up leg); Player_CanFireWeapon @0x5cf780
# returns the SETTLED sight view: g_weaponScopeActive is promoted only when the
# scope ease completes (@0x4de4f7)] — so it stays up from the hip and through the
# whole ADS ease, and yields once fully sighted (D-HUD-9 CLOSED; the magnified scope
# overlay itself is still the recorded hud-re deferral). Spread = ERROR[stance row]
# + (recoil >> 7) + (weapon weight >> 7), over the live fov. The +3 sighted rows
# key on the SAME CanFire predicate
# [orig: row += 3·CanFire @0x592b87 — NOT "scoped"], so on foot they are unreachable
# (the crosshair only draws while !CanFire); the reachable-at-CanFire cases use
# the modeled vehicle/gunner keep-up proxy. Stance-row overrides
# are applied sim-side: airborne/swimming → standing (@0x592b65), mounted → crouch
# (@0x592b6c).
# [orig: HUD_DrawCrosshair @0x592640]
func _draw_crosshair(surface: Vector2) -> void:
	if bool(_info.get("binoculars_view_active", false)):
		return
	var scoped := bool(_info.get("scope_engaged", false))
	# The sim supplies Player_CanFireWeapon's settled/mode/mount verdict. Retain
	# the old scope-derived fallback for test producers that predate the field.
	# [orig: !Player_CanFireWeapon @0x5cf780 via @0x592afa]
	var aimed := bool(_info.get("aimed_shot_available",
			scoped and float(_info.get("scope_fraction", 1.0)) >= 1.0))
	if not HudCrosshair.should_draw(aimed,
			bool(_info.get("keep_crosshair_while_aimed", false))):
		return
	if _crosshair_tex == null:
		return
	var stance_icon := int(_info.get("stance", 0))
	# The icon index (0=stand 1=crouch 2=prone) remaps to the ERROR row order
	# (0=prone 1=crouch 2=stand). [orig: @0x592b37 — entity+300 0x100=prone 0x200=crouch]
	var err_stance := 2
	if stance_icon == 2:
		err_stance = 0
	elif stance_icon == 1:
		err_stance = 1
	# Hip rows are the ordinary on-foot visible case; vehicle/gunner override paths
	# can keep the crosshair visible with the +3 aimed-shot triplet selected.
	# [orig: @0x592b87]
	# The bridge keeps the authored ERROR row and both signed SAR terms in exact
	# fixed point. The static-row fallback supports old/test producers only.
	# [orig: HUD_DrawCrosshair @0x592b07..0x592bf5]
	var spread_fp16: int
	if _info.has("hud_spread_fp16"):
		spread_fp16 = int(_info["hud_spread_fp16"])
	else:
		var err_deg := _weapon.error_row_deg(
				HudCrosshair.error_row(err_stance, false))
		spread_fp16 = int(err_deg * 65536.0)
	var spread := HudCrosshair.spread_px_fp16(
			spread_fp16, float(_info.get("fov_deg", 80.0)), surface.x)
	HudCrosshair.draw(self, _crosshair_tex, _crosshair_center(surface), surface, spread)


# The crosshair anchor: the projected aim point mapped into the 1024×768 design space
# (Viewport_ScreenToVirtual is ×1024/width) — in third person / spectate, where the
# orbit-pitched chase camera does not look along the aim. First person arrives as
# Vector2.INF and PINS the exact design center [orig: @0x5928a0 — the original never
# projects in 1P; the projected branch is @0x592910..0x59293c]. (D-HUD-10 CLOSED.)
# [orig: @0x592c50..0x592cd2 offsets around the virtual-space projected aim;
# Viewport_ScreenToVirtual @0x5d2c70]
func _crosshair_center(surface: Vector2) -> Vector2:
	var aim: Vector2 = _info.get("aim_screen", Vector2.INF)
	if aim == Vector2.INF or surface.x <= 0.0 or surface.y <= 0.0:
		return Vector2(512, 384)
	return Vector2(aim.x * HudLayout.DESIGN_WIDTH / surface.x,
			aim.y * HudLayout.DESIGN_HEIGHT / surface.y)

func _stance_offset(idx: int) -> Vector2:
	return _stance_offsets[idx] if idx >= 0 and idx < _stance_offsets.size() else Vector2.ZERO


# Read a cached element position as the 4-field (x, y, hidden, align) record.
func _pos_of(key: String, fallback: Vector4i) -> Vector4i:
	if not _positions.has(key):
		return fallback
	var v = _positions[key]
	if v is Vector4i:
		return v
	if v is Vector2i:
		return Vector4i(v.x, v.y, 0, 0)
	return fallback
