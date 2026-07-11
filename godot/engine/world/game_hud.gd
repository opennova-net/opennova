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
const MIN_CROSSHAIR_STYLE := 0
const MAX_CROSSHAIR_STYLE := 24
const DEFAULT_CHAT_LINES := 8 # HUDCHLINE fallback
const STANCE_FRAME_COUNT := 6

var _hudpos: NovaHudPos
var _root: NovaResourceRoot
var _font: FontFile
var _frame_tex: Texture2D
var _crosshair_tex: Texture2D
var _crosshair_style := MIN_CROSSHAIR_STYLE
var _stance_textures: Array[Texture2D] = []
var _stance_offsets: Array[Vector2] = []
var _positions: Dictionary = {}
var _colors: Dictionary = {}
var _alpha_fade := Vector3.ZERO
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
	_alpha_fade = Vector3.ZERO
	_chat_lines = DEFAULT_CHAT_LINES
	_load_assets()
	queue_redraw()


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
	queue_redraw()


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
	# An embedded ONED host supplies a nonzero Control size; that panel wins over
	# the enclosing editor viewport so HUD geometry stays clipped to play.
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


# The spreading crosshair. Witnessed visibility: it draws when an aimed shot is NOT
# available [orig: the gate @0x592afa — !Player_CanFireWeapon() (plus vehicle
# autoaim/scoped-gunner keep-up legs, unported); Player_CanFireWeapon @0x5cf780
# returns the SETTLED sight view: g_weaponScopeActive is promoted only when the
# scope ease completes (@0x4de4f7)] — so it stays up from the hip and through the
# whole ADS ease, and yields once fully sighted (D-HUD-9 CLOSED; the magnified scope
# overlay itself is still the recorded hud-re deferral). Spread = ERROR[stance row]
# over the live fov. The +3 sighted rows key on the SAME CanFire predicate
# [orig: row += 3·CanFire @0x592b87 — NOT "scoped"], so on foot they are unreachable
# (the crosshair only draws while !CanFire); the reachable-at-CanFire cases are
# vehicle autoaim / scoped gunner — unported with vehicles. Stance-row overrides
# still deferred: airborne/swimming → standing (@0x592b65), mounted → crouch
# (@0x592b6c). The recoil-accumulator terms (+0x380/+0x384 >>7) are D-HUD-7.
# [orig: HUD_DrawCrosshair @0x592640]
func _draw_crosshair(surface: Vector2) -> void:
	var scoped := bool(_info.get("scope_engaged", false))
	# The settled-sights hide [orig: !Player_CanFireWeapon @0x5cf780 via @0x592afa;
	# scope_fraction < 1 = the ease still interpolating = scopeActive still 0].
	if scoped and float(_info.get("scope_fraction", 1.0)) >= 1.0:
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
	# Hip rows only: the crosshair draws only while !CanFire, and the +3 select keys
	# on CanFire itself [orig: @0x592b87] — never reachable here on foot.
	var err_deg := _weapon.error_row_deg(HudCrosshair.error_row(err_stance, false))
	var spread := HudCrosshair.spread_px(err_deg, float(_info.get("fov_deg", 80.0)), surface.x)
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
