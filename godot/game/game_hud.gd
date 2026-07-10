class_name GameHud
extends Control

## The runtime in-game HUD overlay. A full-rect Control on the gameplay HUD layer
## (a child of the $HUD CanvasLayer, so the shell's _set_hud_visible hides it behind
## menus). It draws the witnessed HUD elements from a hudpos.def layout (NovaHudPos)
## plus a per-frame "info" dict the host rebuilds each frame — mirroring the original,
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
const CROSSHAIR_STYLE := 0
const DEFAULT_CHAT_LINES := 8 # HUDCHLINE fallback

var _hudpos: NovaHudPos
var _root: NovaResourceRoot
var _font: FontFile
var _frame_tex: Texture2D
var _crosshair_tex: Texture2D
var _stance_textures: Array[Texture2D] = []
var _stance_offsets: Array[Vector2] = []
var _positions: Dictionary = {}
var _colors: Dictionary = {}
var _alpha_fade := Vector3i.ZERO
var _chat_lines := DEFAULT_CHAT_LINES

# The equipped weapon's HUD slice (PlayerHudWeaponDef, null = no weapon), its resolved
# WepDes display name, and the loaded HUDCLIPGFX/HUDRNDGFX textures.
var _weapon: PlayerHudWeaponDef = null
var _weapon_display_name := ""
var _clip_tex: Texture2D
var _round_tex: Texture2D

var _clip_indicator := HudClipIndicator.new()
var _messages := HudMessages.new()

# Stance cross-fade state. [orig: byte_2723D3C/D3D prev/current + stamp @0x2723D38]
var _stance_prev := 0
var _stance_cur := 0
var _stance_stamp := 0

# Per-frame state, rebuilt by the host. Defaults keep the HUD sane before the first update.
var _info: Dictionary = {
	"health_fraction": 1.0,
	"stance": 0,
	"team": 0,
	"objective": "",
	"weapon_active": false,
	"clip": -1,
	"reserve": -1,
	"scope_engaged": false,
	"fov_deg": 80.0,
	"ticks": 0,
}


func set_layout(hudpos: NovaHudPos, root: NovaResourceRoot) -> void:
	_hudpos = hudpos
	_root = root
	_font = null
	_frame_tex = null
	_crosshair_tex = null
	_stance_textures.clear()
	_stance_offsets.clear()
	_positions = {}
	_colors = {}
	_alpha_fade = Vector3i.ZERO
	_chat_lines = DEFAULT_CHAT_LINES
	_load_assets()
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
	queue_redraw()


## A mission triggered-text line for the message feed. [orig: HUD_DisplayTriggeredText
## @0x51f190 -> Chat_AddDebugMessage @0x4987f0 (default color)]
func push_message(text: String) -> void:
	_messages.push(text, Color(0, 0, 0, 0), int(_info.get("ticks", 0)))
	queue_redraw()


func update_info(info: Dictionary) -> void:
	var stance := int(info.get("stance", 0))
	if stance != _stance_cur:
		# [orig: stance change restamp @0x599f8a]
		_stance_prev = _stance_cur
		_stance_cur = stance
		_stance_stamp = int(info.get("ticks", 0))
	_info = info
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
	_colors = _hudpos.get_colors()
	var misc: Dictionary = dict.get("misc", {})
	_alpha_fade = misc.get("alpha_fade", Vector3i.ZERO)
	var chline := int(misc.get("hud_chline", 0))
	if chline > 0:
		_chat_lines = chline

	var frames := _hudpos.get_static_frames()
	if frames.size() > 0:
		_frame_tex = _load_texture(String((frames[0] as Dictionary).get("texture", "")))
	for s in _hudpos.get_stances():
		_stance_textures.append(_load_texture(String((s as Dictionary).get("texture", ""))))
		_stance_offsets.append(Vector2((s as Dictionary).get("offset", Vector2i.ZERO)))
	_crosshair_tex = _load_texture("cross%02d.tga" % (CROSSHAIR_STYLE + 1))


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
	# [orig: overlayCtx @0x24c1420 screen_w/h -> Viewport_ScaleToVirtualCoords @0x5d2b20]
	var surface := get_viewport_rect().size
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
# at HUDSTANCEPOS plus its own HUDSTANCE offset. The MP team tile underneath is
# deferred with the MP HUD. [orig: HUD_DrawStanceIndicator @0x599f10 (D-HUD-1)]
func _draw_stance(surface: Vector2, ticks: int) -> void:
	var anchor := Vector2(_hudpos.get_stance_pos())
	if anchor == Vector2.ZERO:
		return
	var tint: Color = _colors.get("stanceicon_color", Color.WHITE)
	var ramp := int(_alpha_fade.z * HudFade.SECONDS_TO_TICKS)
	var base_alpha := int(_alpha_fade.x * HudFade.PERCENT_TO_ALPHA)
	var elapsed := ticks - _stance_stamp
	var cur_a := HudFade.stance_current_alpha(elapsed, ramp, base_alpha) if ramp > 0 else 255
	var prev_a := HudFade.stance_prev_alpha(elapsed, ramp) if ramp > 0 else 0

	var cur := _stance_cur
	if cur >= 0 and cur < _stance_textures.size() and _stance_textures[cur] != null:
		HudStance.draw(self, _stance_textures[cur], anchor, surface,
			Color(tint.r, tint.g, tint.b, cur_a / 255.0), _stance_offset(cur))
	if prev_a > 0 and _stance_prev != cur \
			and _stance_prev >= 0 and _stance_prev < _stance_textures.size() \
			and _stance_textures[_stance_prev] != null:
		HudStance.draw(self, _stance_textures[_stance_prev], anchor, surface,
			Color(tint.r, tint.g, tint.b, prev_a / 255.0), _stance_offset(_stance_prev))


# The weapon-coupled cluster: ammo count + weapon name, the clip indicator, and the
# crosshair. Nothing draws without an installed weapon FSM (the original gates every
# weapon element on the info struct's weapon-def pointer). [orig: @0x5939f3 / @0x599a67]
func _draw_weapon_cluster(surface: Vector2, ticks: int) -> void:
	if not bool(_info.get("weapon_active", false)) or _weapon == null:
		_draw_fallback_reticle(surface)
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


# The spreading crosshair, hidden while the aim is scoped-in (the original draws it
# only when an aimed shot is NOT available — in scope view the scope overlay owns the
# reticle). Spread = ERROR[stance row] over the live fov; the recoil-accumulator terms
# (+0x380/+0x384 >>7) are a recorded follow-up (docs/interface/hud-re.md D-HUD-7).
# [orig: HUD_DrawCrosshair @0x592640]
func _draw_crosshair(surface: Vector2) -> void:
	if bool(_info.get("scope_engaged", false)):
		return
	if _crosshair_tex == null:
		_draw_fallback_reticle(surface)
		return
	var stance_icon := int(_info.get("stance", 0))
	# The icon index (0=stand 1=crouch 2=prone) remaps to the ERROR row order
	# (0=prone 1=crouch 2=stand). [orig: @0x592b37 — entity+300 0x100=prone 0x200=crouch]
	var err_stance := 2
	if stance_icon == 2:
		err_stance = 0
	elif stance_icon == 1:
		err_stance = 1
	var err_deg := _weapon.error_row_deg(HudCrosshair.error_row(err_stance, false))
	var spread := HudCrosshair.spread_px(err_deg, float(_info.get("fov_deg", 80.0)), surface.x)
	HudCrosshair.draw(self, _crosshair_tex, Vector2(512, 384), surface, spread)


# A minimal center cross marking aim while no crosshair art is loadable (art-less
# roots, or no weapon installed yet).
func _draw_fallback_reticle(surface: Vector2) -> void:
	var center := surface * 0.5
	var rc := Color(1, 1, 1, 0.7)
	draw_line(center - Vector2(8, 0), center + Vector2(8, 0), rc, 1.0)
	draw_line(center - Vector2(0, 8), center + Vector2(0, 8), rc, 1.0)


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
