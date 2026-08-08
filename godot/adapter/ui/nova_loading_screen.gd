class_name NovaLoadingScreen
extends Control

## The mission loading screen: the per-mission sidecar image (or the stock
## loadscrn.pcx) stretched over the whole display, the MP session text
## composited over it, and the red progress bar near the bottom.
##
## Structural translation of the witnessed originals:
##  - background + text compositing  [orig: render_loading_screen @ 0x521d10]
##  - session text provider          [orig: HUD_GetLoadingScreenTextByGameType @ 0x51f300]
##  - fullscreen stretch present     [orig: LoadingScreen_DrawEffectFullscreen @ 0x586ba0]
##  - throttle + creep + bar draw    [orig: LoadingScreen_UpdateAndPresent @ 0x586be0]
##  - bar primitive                  [orig: draw_progress_bar_0 @ 0x5d4c40]
##
## The original composites text INTO the 800x600 background surface with the
## CGameFont bitmap fonts, then stretches the composite to the backbuffer with
## no aspect preservation [orig: 0x586ba0 rect (0,0,width,height)]. We draw the
## texture stretched and draw the text in image space under the same scale
## transform — the same net result.
##
## Shell-neutral presentation mounted by the game shell around GameWorld loads.
## No world/menu knowledge lives here.

## Fallback background when the mission has no sidecar image
## [orig: "loadscrn.pcx" @ 0x521e20].
const FALLBACK_IMAGE := "loadscrn.pcx"
## The two composited fonts [orig: "Arials18.fnt" @ 0x521eec, "Arial22.fnt" @ 0x521f5a].
const FONT_SMALL := "Arials18.fnt"
const FONT_LARGE := "Arial22.fnt"

## Top text band, in image space [orig: rect (21, 29, right, 500) @ 0x52200f;
## right edge 661 with a custom background, 782 with the stock one @ 0x521ec4].
const BAND_LEFT := 21
const BAND_TOP := 29
const BAND_RIGHT_CUSTOM := 661
const BAND_RIGHT_STOCK := 782
const BAND_BOTTOM := 500

## Server-message block, as fractions of the image size
## [orig: doubles 0.02 @ 0x7D01A0, 0.98 @ 0x7D0198, 0.87 @ 0x7D0190, 0.9 @ 0x7C4878].
const MSG_X_FRAC := 0.02
const MSG_RIGHT_FRAC := 0.98
const MSG_LABEL_Y_FRAC := 0.87
const MSG_BODY_Y_FRAC := 0.9
## The message label color tag [orig: "<c80E0FF>%s:\r\n<cFFFFFF>" @ 0x7D01A8].
const MSG_LABEL_COLOR := Color(0x80 / 255.0, 0xE0 / 255.0, 1.0)
## Fallback label when the gametext table misses
## [orig: GameText_GetStringWithFallback("LoadingText", "LT_SERVERMSG", ...) @ 0x522074].
const MSG_LABEL_FALLBACK := "Message from Game Server"

## Progress bar, in the 1024x768 virtual overlay space
## [orig: x=368 y=732 w=286 h=15 @ 0x586c78-0x586c90, scaled via
## Viewport_ScaleToVirtualCoords @ 0x5d2b20].
const VIRTUAL_SIZE := Vector2(1024, 768)
const BAR_POS := Vector2i(368, 732)
const BAR_SIZE := Vector2i(286, 15)
## Bar colors: border black/gray/black [orig: 0, 0xC0C0C0, 0 @ 0x5d4c40], fill
## red [orig: override color 0xFFEB0000 @ 0x586cd0].
const BAR_BORDER_GRAY := Color8(0xC0, 0xC0, 0xC0)
const BAR_FILL := Color8(0xEB, 0x00, 0x00)

## Redraw throttle [orig: GetTickCount() - last >= 100 @ 0x586c24].
const PRESENT_INTERVAL_MS := 100

var _texture: Texture2D = null
var _has_custom_bg := false
var _in_session := false
var _title := ""          # server name [orig: g_sessionvar_server_name -> title buf @ 0x51f533]
var _mission_name := ""   # [orig: g_sessionvar_mission_name -> mission buf @ 0x51f53a]
var _game_type_text := "" # [orig: LoadingText LTGT_* lookup @ 0x51f3cf]
var _custom_text := ""    # server message body [orig: g_sessionvar_custom_text @ 0x522123]
var _font_small: FontFile = null
var _font_large: FontFile = null

var _reported := 0        # last progress input [orig: this[8] @ 0x586c32]
var _displayed := 0       # smoothed bar value [orig: this[9] @ 0x586c2f]
var _last_drawn_reported := -1
var _last_present_ms := -PRESENT_INTERVAL_MS


## <mission>.bms -> <mission>.pcx: the sidecar image name for a mission file
## [orig: PathRemoveExtension + Path_ReplaceOrAppendExtension(path, "pcx")
## @ 0x521d66/0x521dab; resolution is case-insensitive through the VFS].
static func sidecar_image_name(mission_file: String) -> String:
	var base := mission_file.get_file()
	var ext := base.get_extension()
	if not ext.is_empty():
		base = base.substr(0, base.length() - ext.length() - 1)
	return base + ".pcx"


## The LoadingText key for a numeric session game type, or "" for an unknown
## type (the original leaves the line empty) [orig: switch @ 0x51f30b-0x51f3a6].
static func gametype_text_key(game_type: int) -> String:
	if game_type == 0:
		return "LTGT_DM"
	if game_type == 0x10000:
		return "LTGT_TDM"
	if (game_type & 0xFFFDFFFF) == 0x10020:
		return "LTGT_COOP"
	match game_type:
		0x10001: return "LTGT_TKOTH"
		0x00001: return "LTGT_KOTH"
		0x90002: return "LTGT_SD"
		0x10002: return "LTGT_AD"
		0x10004: return "LTGT_CTF"
		0x10008: return "LTGT_FB"
		0x10010: return "LTGT_AAS"
		0x50010: return "LTGT_CAC"
	return ""


## One smoothing step: catch the displayed value up to the reported progress,
## then creep +1 per draw up to 10 points ahead as the witnessed liveness lead,
## capped at 100 [orig: displayed this[9] += 1 up to min(progress+10, 100) per
## draw, LoadingScreen_UpdateAndPresent @ 0x586c3f]. The original reaches the
## catch-up for free because it pumps UpdateAndPresent at window-message
## frequency — hundreds of calls per load (per-model + the per-subsystem slot++
## 62..69). Our present() is driven by the coarser progress emits (D-LOADSCR-1),
## so a literal +1-only step never leaves ~10 in 8 calls; the displayed value
## must track reported here. The +1 lead-ahead past reported is preserved for
## the per-object pulse phase where multiple draws share one reported value.
static func step_displayed(displayed: int, reported: int) -> int:
	var lead_cap := mini(reported + 10, 100)
	return clampi(maxi(displayed + 1, reported), 0, lead_cap)


## The fill rect's horizontal span (left, right) for a bar whose outer frame
## starts at `x` with inner fill width `w` — the original's exact arithmetic:
## fill right edge = pct * (w + 2) / 100 + (left-after-two-insets) + 2, clamped
## to the track, then one final 1px inset [orig: v8 @ 0x5d4c40; fill draw after
## the last inset]. right <= left means an empty fill.
static func bar_fill_span(x: int, w: int, displayed: int) -> Vector2i:
	@warning_ignore("integer_division")  # the original's integer divide
	var fill_right := x + 4 + displayed * (w + 2) / 100
	fill_right = mini(fill_right, x + w + 4) - 1
	return Vector2i(x + 3, fill_right)


## Resolve the background image for a mission: the sidecar if present, else the
## stock fallback [orig: FileSystem_FileExists probe @ 0x521db5; fallback
## @ 0x521e20]. Returns { "name": String, "custom": bool }.
static func resolve_background(root: NovaResourceRoot, mission_file: String) -> Dictionary:
	var sidecar := sidecar_image_name(mission_file)
	# UI image probes force loose-first for this lookup, independent of /d.
	# [orig: CUIImage_LoadTextureFromFile @ 0x6541ba]
	if root != null and not sidecar.is_empty() and root.has_file(
			sidecar, NovaResourceRoot.LOOKUP_FORCE_LOOSE_FIRST):
		return {"name": sidecar, "custom": true}
	return {"name": FALLBACK_IMAGE, "custom": false}


## Public texture-load seam for owners/tests; avoids private-state inspection (ADR 0018).
## [orig: CUIImage_LoadTextureFromFile @ 0x6541ba]
static func load_background_texture(root: NovaResourceRoot, image_name: String) -> Texture2D:
	if root == null or image_name.is_empty():
		return null
	return root.load_texture(image_name, NovaResourceRoot.LOOKUP_FORCE_LOOSE_FIRST)


## Build the screen for a mission load. `info`:
##   mission_file: String — the .bms name driving the sidecar lookup
##   in_session: bool — MP session: draw the text overlay [orig: gate @ 0x521ebe]
##   server_name / mission_name / custom_text: String — the session variables
##     [orig: SERVERNAME/MISSIONNAME/CUSTOMTEXT @ parse_server_session_variables
##      0x5202f0 / serialize_mission_info_to_datastream 0x523620]
##   game_type: int — the numeric session game type [orig: GAMETYPE]
func setup(root: NovaResourceRoot, info: Dictionary) -> void:
	set_anchors_preset(Control.PRESET_FULL_RECT)
	var bg := resolve_background(root, String(info.get("mission_file", "")))
	_has_custom_bg = bool(bg["custom"])
	_texture = load_background_texture(root, String(bg["name"]))
	_in_session = bool(info.get("in_session", false))
	if not _in_session:
		queue_redraw()
		return
	# MP only: the text overlay and its fonts [orig: fonts loaded only on the
	# in-session path @ 0x521eec/0x521f5a].
	_title = String(info.get("server_name", ""))
	_mission_name = String(info.get("mission_name", ""))
	_custom_text = String(info.get("custom_text", ""))
	_game_type_text = _lookup_loading_text(gametype_text_key(int(info.get("game_type", 0))), "")
	_font_small = _load_font(root, FONT_SMALL)
	_font_large = _load_font(root, FONT_LARGE)
	queue_redraw()


## Refresh mid-load: a JOINER learns the authoritative session record (server/
## mission names, game type, and the mission file driving the sidecar
## background) from the post-auth S2C 0x7B AFTER the screen is already up.
## Retail fills the same buffers from the connect stream during its load
## [orig: parse_server_session_variables @ 0x5202f0 -> the title/mission bufs
## @ 0x51f533/0x51f53a]. Empty values keep the current ones.
func update_session_info(root: NovaResourceRoot, info: Dictionary) -> void:
	var mission_file := String(info.get("mission_file", ""))
	if not mission_file.is_empty():
		var bg := resolve_background(root, mission_file)
		var texture := load_background_texture(root, String(bg["name"]))
		if texture != null:
			_has_custom_bg = bool(bg["custom"])
			_texture = texture
	if _in_session:
		var title := String(info.get("server_name", ""))
		if not title.is_empty():
			_title = title
		var mission_name := String(info.get("mission_name", ""))
		if not mission_name.is_empty():
			_mission_name = mission_name
		if int(info.get("game_type", -1)) >= 0:
			_game_type_text = _lookup_loading_text(
					gametype_text_key(int(info.get("game_type", 0))), _game_type_text)
	queue_redraw()


## Report load progress [orig: the per-stage/per-model calls into
## LoadingScreen_UpdateAndPresent, e.g. Game_StartMission @ 0x52498f..0x525d29].
func set_progress(percent: int) -> void:
	_reported = clampi(percent, 0, 100)


## Redraw + present if due: at most every 100 ms, and only when the reported
## value changed or the displayed value still trails it
## [orig: LoadingScreen_UpdateAndPresent @ 0x586be0]. `force` seeds the first
## frame. Outside a live rendering context this only updates the smoothing.
func present(force := false) -> void:
	if _texture == null:
		return  # no background loaded -> no screen at all [orig: @ 0x586bfd]
	var now := Time.get_ticks_msec()
	# Draw when 100 ms elapsed, the reported value changed, or the displayed
	# value still trails it — the trailing case redraws unthrottled, so the bar
	# catches a jump quickly, then creeps ahead at the 100 ms cadence
	# [orig: elapsed >= 100 || this[8] != progress || this[9] < progress @ 0x586c24].
	var due := now - _last_present_ms >= PRESENT_INTERVAL_MS \
		or _last_drawn_reported != _reported or _displayed < _reported
	if not (force or due):
		return
	_last_present_ms = now
	_last_drawn_reported = _reported
	_displayed = step_displayed(_displayed, _reported)
	queue_redraw()
	# The original pumps window messages and presents mid-load; process_events
	# + force_draw are the shell-side equivalents so the OS window stays live
	# and the screen refreshes while the load blocks the main loop
	# [orig: Game_PumpWindowMessages @ 0x586be6 + Present @ 0x586d53].
	if is_inside_tree() and DisplayServer.get_name() != "headless":
		DisplayServer.process_events()
		RenderingServer.force_draw(true, 0.0)


## Give a newly mounted Control one complete SceneTree frame to receive layout
## and submit its queued draw before the caller enters synchronous mission
## loading. `process_frame` fires at the start of a frame, so crossing two of
## those signals is what lets one ordinary render/present finish. The forced
## present after that frame keeps the window responsive during the block.
func prepare_for_blocking_load() -> bool:
	if not is_inside_tree():
		return false
	# Seed the smoothed bar before the registration frame so that frame submits
	# both the background and a non-empty fill to the canvas draw list.
	present(true)
	await get_tree().process_frame
	if not is_inside_tree():
		return false
	await get_tree().process_frame
	if not is_inside_tree():
		return false
	present(true)
	return true


func displayed_progress() -> int:
	return _displayed


## Public ADR-0018 read seam for whether the visible session overlay is enabled.
func has_session_overlay() -> bool:
	return _in_session


## Public ADR-0018 read seam for the visible composed strings, ordered as server
## title, mission, game-type label, and custom message.
func session_overlay_lines() -> PackedStringArray:
	return PackedStringArray([_title, _mission_name, _game_type_text, _custom_text])


## Whether setup resolved and decoded loading art. Owners and tests should not
## inspect the screen's private texture resource directly (ADR 0018).
func has_background() -> bool:
	return _texture != null


func _draw() -> void:
	if _texture == null:
		return
	# Background: stretched to the full display, no aspect preservation; drawn
	# unmodulated — the original's 0xFF7F7F7F modulate is the MODULATE2X
	# neutral (docs/interface/loading-screen-re.md D-LOADSCR-6)
	# [orig: LoadingScreen_DrawEffectFullscreen rect (0,0,width,height) @ 0x586ba0].
	draw_texture_rect(_texture, Rect2(Vector2.ZERO, size), false)
	if _in_session:
		_draw_session_text()
	_draw_progress_bar()


# The MP text overlay, drawn in image space under the image's stretch scale
# (the original composites into the texture before stretching, so text scales
# with the image) [orig: render_loading_screen @ 0x521fe1-0x522123].
func _draw_session_text() -> void:
	var tex_size := Vector2(_texture.get_width(), _texture.get_height())
	if tex_size.x <= 0.0 or tex_size.y <= 0.0:
		return
	draw_set_transform(Vector2.ZERO, 0.0, size / tex_size)
	var band_right := BAND_RIGHT_CUSTOM if _has_custom_bg else BAND_RIGHT_STOCK
	var band_width := band_right - BAND_LEFT
	# Title / mission / game type share one band: left-, center- and
	# right-aligned [orig: alignment 3/4/5 @ 0x52200f/0x52202e/0x522050 ->
	# left/center/right @ 0x58106c/0x581071]. All white (<cFFFFFF> @ 0x51f42d).
	if _font_large != null:
		_draw_wrapped(_font_large, _title, BAND_LEFT, BAND_TOP, band_width,
			BAND_BOTTOM, HORIZONTAL_ALIGNMENT_LEFT, Color.WHITE)
		_draw_wrapped(_font_large, _mission_name, BAND_LEFT, BAND_TOP, band_width,
			BAND_BOTTOM, HORIZONTAL_ALIGNMENT_CENTER, Color.WHITE)
		_draw_wrapped(_font_large, _game_type_text, BAND_LEFT, BAND_TOP, band_width,
			BAND_BOTTOM, HORIZONTAL_ALIGNMENT_RIGHT, Color.WHITE)
	# The server message block [orig: gate on a non-empty CUSTOMTEXT @ 0x52205f;
	# label "<c80E0FF>%s:" at (0.02w, 0.87h), body from 0.90h, box to (0.98w, h)].
	if _font_small != null and not _custom_text.is_empty():
		var label := _lookup_loading_text("LT_SERVERMSG", MSG_LABEL_FALLBACK) + ":"
		var mx := int(MSG_X_FRAC * tex_size.x)
		var mw := int(MSG_RIGHT_FRAC * tex_size.x) - mx
		_draw_wrapped(_font_small, label, mx, int(MSG_LABEL_Y_FRAC * tex_size.y),
			mw, int(tex_size.y), HORIZONTAL_ALIGNMENT_LEFT, MSG_LABEL_COLOR)
		_draw_wrapped(_font_small, _custom_text, mx, int(MSG_BODY_Y_FRAC * tex_size.y),
			mw, int(tex_size.y), HORIZONTAL_ALIGNMENT_LEFT, Color.WHITE)
	draw_set_transform(Vector2.ZERO, 0.0, Vector2.ONE)


# Word-wrapped text in a box: lines wrap at `width`, drawing stops at `bottom`
# [orig: render_draw_wrapped_text_block_ex @ 0x580eb0 — wraps at the last
# space, advances one line height, stops when the next line passes rect_bottom].
# Line metrics ride the FontFile view of the .fnt; exact CGameFont glyph
# spacing is the standing follow-up (docs/interface/loading-screen-re.md
# D-LOADSCR-2, shared with hud-re.md).
func _draw_wrapped(font: FontFile, text: String, x: int, y: int, width: int,
		bottom: int, align: HorizontalAlignment, color: Color) -> void:
	if text.is_empty():
		return
	var fs := font.get_fixed_size()
	if fs <= 0:
		fs = 16
	var line_h := font.get_height(fs)
	var max_lines := maxi(1, int((bottom - y) / line_h)) if bottom > y else 1
	draw_multiline_string(font, Vector2(x, y + font.get_ascent(fs)), text,
		align, width, fs, max_lines, color)


# The progress bar, scaled from the 1024x768 virtual overlay space onto the
# display; the 1px border/inset steps stay in real pixels
# [orig: LoadingScreen_UpdateAndPresent @ 0x586c78 + draw_progress_bar_0 @ 0x5d4c40].
func _draw_progress_bar() -> void:
	var s := size / VIRTUAL_SIZE
	var x := int(BAR_POS.x * s.x)
	var y := int(BAR_POS.y * s.y)
	var w := int(BAR_SIZE.x * s.x)
	var h := int(BAR_SIZE.y * s.y)
	# Layered filled rects, each inset 1px: black, gray, black track, then the
	# fill [orig: draw_progress_bar_0 — outer spans w+6/h+6, three inset border
	# draws, fill right edge = pct*(inner width)/100 + left + 2, one last inset].
	draw_rect(Rect2(x, y, w + 6, h + 6), Color.BLACK)
	draw_rect(Rect2(x + 1, y + 1, w + 4, h + 4), BAR_BORDER_GRAY)
	draw_rect(Rect2(x + 2, y + 2, w + 2, h + 2), Color.BLACK)
	var span := bar_fill_span(x, w, _displayed)
	if span.y > span.x:
		draw_rect(Rect2(span.x, y + 3, span.y - span.x, h), BAR_FILL)


# LoadingText lookup against the registered gametext table; a miss returns the
# fallback, never the "??..??" marker (the original returns the raw entry or
# its literal fallback) [orig: GameText_GetStringWithFallback @ 0x51eb90;
# TextResource_FindEntryBySectionAndKey(g_TextGameText, "LoadingText", key) @ 0x51f3cf].
func _lookup_loading_text(key: String, fallback: String) -> String:
	if key.is_empty():
		return fallback
	var table: RtxtStringFile = NovaStrings.get_table("gametext")
	if table == null or not table.has_string_in_section("LoadingText", key):
		return fallback
	return table.get_string_in_section("LoadingText", key)


func _load_font(root: NovaResourceRoot, name: String) -> FontFile:
	if root == null:
		return null
	var res := root.load_font(name)
	if res == null or not res.has_method("to_font_file"):
		return null
	return res.to_font_file()
