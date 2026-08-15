class_name LoadingScreen
extends Control

## The mission loading screen: the per-mission sidecar image (or the stock
## loadscrn.pcx) stretched over the whole display, the MP session text
## composited over it, and the red progress bar near the bottom. After a
## single-player load with a custom background it also hosts the
## start-mission splash: the same background with a blinking centered
## continue line and the cursor arrow, dismissed by any fresh key or mouse
## button.
##
## Structural translation of the witnessed originals:
##  - background + text compositing  [orig: render_loading_screen @ 0x521d10]
##  - session text provider          [orig: HUD_GetLoadingScreenTextByGameType @ 0x51f300]
##  - fullscreen stretch present     [orig: LoadingScreen_DrawEffectFullscreen @ 0x586ba0]
##  - throttle + creep + bar draw    [orig: LoadingScreen_UpdateAndPresent @ 0x586be0]
##  - bar primitive                  [orig: draw_progress_bar_0 @ 0x5d4c40]
##  - SP start-mission splash        [orig: show_start_mission_splash @ 0x520820]
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

## The layout values (image-space text band, server-message fractions/color,
## progress-bar rect/colors, the redraw throttle) are the engine's
## HudPos.LOADING_* constants and loading_* statics — the witnesses live at
## the engine home, engine/runtime/hud hud/loading_screen.h.
## Fallback label when the gametext table misses
## [orig: GameText_GetStringWithFallback("LoadingText", "LT_SERVERMSG", ...) @ 0x522074].
const MSG_LABEL_FALLBACK := "Message from Game Server"

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
var _last_present_ms := -HudPos.LOADING_PRESENT_INTERVAL_MS

## Emitted when the start-mission splash has been dismissed AND its final
## background-only frame has had a frame to render [orig: the post-loop
## present @ 0x520a48-0x520a79 precedes the caller's effect release @ 0x525d45].
signal splash_dismissed

enum SplashState { NONE, ACTIVE, CLOSING }

var _splash_state := SplashState.NONE
var _splash_arrow: Texture2D = null      # newarow1.tga, the menu cursor art
var _splash_font: FontFile = null        # Impac22b.fnt (the large HUD label slot)
var _splash_text := ""                   # LoadingText/LT_Continue ("" -> no line)
var _splash_arrow_anchor := Vector2.ZERO # live cursor pos, viewport px
var _splash_blink_on := true
var _splash_frames_until_emit := 0


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
## type (the original leaves the line empty). The GAMETYPE -> key table is the
## engine's; the witness lives at the engine home, hud/loading_screen.h
## (HudPos.loading_gametype_text_key).
static func gametype_text_key(game_type: int) -> String:
	return HudPos.loading_gametype_text_key(game_type)


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
	return HudPos.loading_bar_step(displayed, reported)


## The fill rect's horizontal span (left, right) for a bar whose outer frame
## starts at `x` with inner fill width `w` — the original's exact arithmetic:
## fill right edge = pct * (w + 2) / 100 + (left-after-two-insets) + 2, clamped
## to the track, then one final 1px inset [orig: v8 @ 0x5d4c40; fill draw after
## the last inset]. right <= left means an empty fill.
static func bar_fill_span(x: int, w: int, displayed: int) -> Vector2i:
	return HudPos.loading_bar_fill_span(x, w, displayed)


## Resolve the background image for a mission: the sidecar if present, else the
## stock fallback [orig: FileSystem_FileExists probe @ 0x521db5; fallback
## @ 0x521e20]. Returns { "name": String, "custom": bool }.
static func resolve_background(root: ResourceRoot, mission_file: String) -> Dictionary:
	var sidecar := sidecar_image_name(mission_file)
	# UI image probes force loose-first for this lookup, independent of /d.
	# [orig: CUIImage_LoadTextureFromFile @ 0x6541ba]
	if root != null and not sidecar.is_empty() and root.has_file(
			sidecar, ResourceRoot.LOOKUP_FORCE_LOOSE_FIRST):
		return {"name": sidecar, "custom": true}
	return {"name": FALLBACK_IMAGE, "custom": false}


## Public texture-load seam for owners/tests; avoids private-state inspection (ADR 0018).
## [orig: CUIImage_LoadTextureFromFile @ 0x6541ba]
static func load_background_texture(root: ResourceRoot, image_name: String) -> Texture2D:
	if root == null or image_name.is_empty():
		return null
	return root.load_texture(image_name, ResourceRoot.LOOKUP_FORCE_LOOSE_FIRST)


## Build the screen for a mission load. `info`:
##   mission_file: String — the .bms name driving the sidecar lookup
##   in_session: bool — MP session: draw the text overlay [orig: gate @ 0x521ebe]
##   server_name / mission_name / custom_text: String — the session variables
##     [orig: SERVERNAME/MISSIONNAME/CUSTOMTEXT @ parse_server_session_variables
##      0x5202f0 / serialize_mission_info_to_datastream 0x523620]
##   game_type: int — the numeric session game type [orig: GAMETYPE]
func setup(root: ResourceRoot, info: Dictionary) -> void:
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
func update_session_info(root: ResourceRoot, info: Dictionary) -> void:
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
	var due := now - _last_present_ms >= HudPos.LOADING_PRESENT_INTERVAL_MS \
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
func prepare_for_blocking_load(operation: WorldLoadOperation) -> bool:
	if operation == null or operation.is_cancelled() or not is_inside_tree():
		return false
	# Seed the smoothed bar before the registration frame so that frame submits
	# both the background and a non-empty fill to the canvas draw list.
	present(true)
	await get_tree().process_frame
	if operation.is_cancelled() or not is_inside_tree():
		return false
	await get_tree().process_frame
	if operation.is_cancelled() or not is_inside_tree():
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


## Whether setup resolved the per-mission sidecar art rather than the stock
## fallback — the retail custom-background flag the SP splash gate reads
## [orig: g_loadscreen_has_custom_bg @ 0x24d4dfd, set @ 0x521e94, splash gate
## @ 0x525d38].
func has_custom_background() -> bool:
	return _has_custom_bg


## Public ADR-0018 read seam: the start-mission splash is raised (taking
## input, or presenting its final background-only frame).
func is_splash_active() -> bool:
	return _splash_state != SplashState.NONE


func _ready() -> void:
	# The splash handlers below enable per-frame callbacks by existing; keep
	# both off until the splash actually raises.
	set_process(false)
	set_process_input(false)


## Raise the start-mission splash over the held background: the cursor-arrow
## art, the large HUD label font, and the LoadingText/LT_Continue line, each
## degrading to "element skipped" on a miss exactly like the original's
## unguarded loads [orig: show_start_mission_splash @ 0x520820 — TGA
## @ 0x520871, font slot Impac22b.fnt @ HUD_InitAllFonts 0x51ef4e, text fetch
## @ 0x520975]. Keys pressed DURING the blocking load never dismiss it — the
## original flushes its key queue at entry [orig: Input_ResetKeyQueue
## @ 0x52085b] and Godot delivers no stale press events; held-key autorepeat
## echoes DO dismiss, matching the re-queued autorepeat WM_KEYDOWNs.
## The START_MISSION sound is the owner's to fire; it never gates dismissal
## [orig: the exit tests read only input_mask + the key queue @ 0x520a2d].
## Returns false when no background is held (retail draws no splash without
## the loading-screen effect).
func begin_start_mission_splash(root: ResourceRoot) -> bool:
	if _texture == null or _splash_state != SplashState.NONE:
		return false
	if root != null:
		_splash_arrow = root.load_texture(HudPos.loading_splash_arrow_image(),
				ResourceRoot.LOOKUP_SESSION_DEFAULT)
		_splash_font = _load_font(root, HudPos.loading_splash_continue_font())
	_splash_text = _lookup_loading_text(HudPos.loading_splash_continue_key(), "")
	_splash_state = SplashState.ACTIVE
	_splash_blink_on = _splash_blink_phase()
	if is_inside_tree():
		_splash_arrow_anchor = get_viewport().get_mouse_position()
	set_process_input(true)
	set_process(true)
	queue_redraw()
	return true


func _input(event: InputEvent) -> void:
	if _splash_state != SplashState.ACTIVE:
		return
	var motion := event as InputEventMouseMotion
	if motion != null:
		# The arrow tracks the live cursor [orig: xLeft/yTop @ 0x3342e48/
		# 0x3342e4c ARE the cursor position, written by
		# Input_DispatchMouseEvent @ 0x761470]. Motion never dismisses.
		_splash_arrow_anchor = motion.position
		queue_redraw()
		return
	var key := event as InputEventKey
	if key != null and key.pressed:
		# Echo (autorepeat) presses dismiss too — the original's autorepeat
		# WM_KEYDOWNs land in the same dequeued queue
		# [orig: Input_QueueKeyEvent @ 0x760c10 -> dequeue @ 0x520a36].
		accept_event()
		_splash_exit_edge()
		return
	var button := event as InputEventMouseButton
	if button != null and button.pressed:
		accept_event()
		_splash_exit_edge()


func _process(_delta: float) -> void:
	if _splash_state == SplashState.ACTIVE:
		# A held mouse button dismisses without a fresh press — the original
		# exit reads the live button mask each frame [orig: input_mask
		# @ 0x3342e50 (WindowProc button bits), test @ 0x520a2d].
		if Input.get_mouse_button_mask() != 0:
			_splash_exit_edge()
			return
		var phase := _splash_blink_phase()
		if phase != _splash_blink_on:
			_splash_blink_on = phase
			queue_redraw()
		return
	if _splash_state == SplashState.CLOSING:
		if _splash_frames_until_emit > 0:
			_splash_frames_until_emit -= 1
			return
		_splash_state = SplashState.NONE
		set_process(false)
		splash_dismissed.emit()


# The 512 ms two-phase pulse the continue line rides
# [orig: GetTickCount() & 0x200 selects the color @ 0x5209b0-0x5209be].
func _splash_blink_phase() -> bool:
	return (Time.get_ticks_msec() & HudPos.SPLASH_BLINK_MASK_MS) != 0


func _splash_exit_edge() -> void:
	if _splash_state != SplashState.ACTIVE:
		return
	_splash_state = SplashState.CLOSING
	set_process_input(false)
	# One background-only frame before releasing to the owner [orig: the
	# post-loop present without text or arrow @ 0x520a48-0x520a79, ahead of
	# the caller's effect release @ 0x525d45].
	_splash_frames_until_emit = 1
	queue_redraw()
	if not is_inside_tree() or DisplayServer.get_name() == "headless":
		# No live renderer to wait on: finish on the next process tick.
		_splash_frames_until_emit = 0


func _draw() -> void:
	if _texture == null:
		return
	# Background: stretched to the full display, no aspect preservation; drawn
	# unmodulated — the original's 0xFF7F7F7F modulate is the MODULATE2X
	# neutral (docs/interface/loading-screen-re.md D-LOADSCR-6)
	# [orig: LoadingScreen_DrawEffectFullscreen rect (0,0,width,height) @ 0x586ba0].
	draw_texture_rect(_texture, Rect2(Vector2.ZERO, size), false)
	if _splash_state != SplashState.NONE:
		# Splash frames carry the background plus (while taking input) the
		# continue line and the arrow — no bar, no session text; the closing
		# frame is the original's final background-only present
		# [orig: loop draw @ 0x520993-0x520a28; final @ 0x520a48-0x520a79].
		if _splash_state == SplashState.ACTIVE:
			_draw_splash_overlay()
		return
	if _in_session:
		_draw_session_text()
	_draw_progress_bar()


# The blinking centered continue line + the cursor arrow
# [orig: show_start_mission_splash @ 0x520820].
func _draw_splash_overlay() -> void:
	if _splash_font != null and not _splash_text.is_empty():
		# Centered at virtual (512, 730) of the 1024x768 overlay space, in
		# the large HUD label font at its width/800 slot scale, half-bright,
		# color pulsing on the 512 ms tick bit. Top-anchored like the block
		# draws; glyph metrics ride the FontFile view (D-LOADSCR-2)
		# [orig: HUD_DrawTextAtVirtualPos(ctx, 512, 730, 0, text,
		# g_hudLabelFontLarge, color, mode=2 centered) @ 0x5209da; centered
		# dispatch HUD_DrawTextCentered_HalfBright @ 0x580680; slot scale
		# (w << 16) / 800 @ 0x51ef62].
		var s := size / Vector2(HudPos.DESIGN_WIDTH, HudPos.DESIGN_HEIGHT)
		var pos := Vector2(HudPos.SPLASH_CONTINUE_X * s.x,
				HudPos.SPLASH_CONTINUE_Y * s.y)
		var fscale := size.x / float(HudPos.SPLASH_FONT_SCALE_BASE_W)
		var fs := _splash_font.get_fixed_size()
		if fs <= 0:
			fs = 16
		var color := HudPos.loading_splash_continue_color(_splash_blink_on)
		var text_w := _splash_font.get_string_size(_splash_text,
				HORIZONTAL_ALIGNMENT_LEFT, -1, fs).x
		draw_set_transform(pos, 0.0, Vector2(fscale, fscale))
		draw_string(_splash_font,
				Vector2(-text_w * 0.5, _splash_font.get_ascent(fs)),
				_splash_text, HORIZONTAL_ALIGNMENT_LEFT, -1, fs, color)
		draw_set_transform(Vector2.ZERO, 0.0, Vector2.ONE)
	if _splash_arrow != null:
		# The arrow is the cursor art, top-left at the live cursor position
		# (identity in viewport px — the original keeps the cursor in 640x480
		# space and scales by w/640, h/480 to the same point), sized
		# tga_dims * (viewport / 800x600) [orig: pos @ 0x520920-0x520942;
		# size @ 0x52089d/0x5208ae].
		var asize := Vector2(
				_splash_arrow.get_width() * size.x
					/ float(HudPos.SPLASH_ARROW_SCALE_BASE_W),
				_splash_arrow.get_height() * size.y
					/ float(HudPos.SPLASH_ARROW_SCALE_BASE_H))
		draw_texture_rect(_splash_arrow, Rect2(_splash_arrow_anchor, asize), false)


# The MP text overlay, drawn in image space under the image's stretch scale
# (the original composites into the texture before stretching, so text scales
# with the image) [orig: render_loading_screen @ 0x521fe1-0x522123].
func _draw_session_text() -> void:
	var tex_size := Vector2(_texture.get_width(), _texture.get_height())
	if tex_size.x <= 0.0 or tex_size.y <= 0.0:
		return
	draw_set_transform(Vector2.ZERO, 0.0, size / tex_size)
	var band_right: int = HudPos.LOADING_BAND_RIGHT_CUSTOM if _has_custom_bg \
			else HudPos.LOADING_BAND_RIGHT_STOCK
	var band_width: int = band_right - HudPos.LOADING_BAND_LEFT
	# Title / mission / game type share one band: left-, center- and
	# right-aligned [orig: alignment 3/4/5 @ 0x52200f/0x52202e/0x522050 ->
	# left/center/right @ 0x58106c/0x581071]. All white (<cFFFFFF> @ 0x51f42d).
	if _font_large != null:
		_draw_wrapped(_font_large, _title, HudPos.LOADING_BAND_LEFT,
			HudPos.LOADING_BAND_TOP, band_width,
			HudPos.LOADING_BAND_BOTTOM, HORIZONTAL_ALIGNMENT_LEFT, Color.WHITE)
		_draw_wrapped(_font_large, _mission_name, HudPos.LOADING_BAND_LEFT,
			HudPos.LOADING_BAND_TOP, band_width,
			HudPos.LOADING_BAND_BOTTOM, HORIZONTAL_ALIGNMENT_CENTER, Color.WHITE)
		_draw_wrapped(_font_large, _game_type_text, HudPos.LOADING_BAND_LEFT,
			HudPos.LOADING_BAND_TOP, band_width,
			HudPos.LOADING_BAND_BOTTOM, HORIZONTAL_ALIGNMENT_RIGHT, Color.WHITE)
	# The server message block [orig: gate on a non-empty CUSTOMTEXT @ 0x52205f;
	# the fraction/color witnesses live at the engine home, hud/loading_screen.h].
	if _font_small != null and not _custom_text.is_empty():
		var label := _lookup_loading_text("LT_SERVERMSG", MSG_LABEL_FALLBACK) + ":"
		var mx := int(HudPos.loading_msg_x_frac() * tex_size.x)
		var mw := int(HudPos.loading_msg_right_frac() * tex_size.x) - mx
		_draw_wrapped(_font_small, label, mx,
			int(HudPos.loading_msg_label_y_frac() * tex_size.y),
			mw, int(tex_size.y), HORIZONTAL_ALIGNMENT_LEFT,
			HudPos.loading_msg_label_color())
		_draw_wrapped(_font_small, _custom_text, mx,
			int(HudPos.loading_msg_body_y_frac() * tex_size.y),
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


# The progress bar, scaled from the HudPos.DESIGN_* virtual overlay space onto
# the display; the 1px border/inset steps stay in real pixels (the rect/color
# witnesses live at the engine home, hud/loading_screen.h)
# [orig: LoadingScreen_UpdateAndPresent @ 0x586c78 + draw_progress_bar_0 @ 0x5d4c40].
func _draw_progress_bar() -> void:
	var s := size / Vector2(HudPos.DESIGN_WIDTH, HudPos.DESIGN_HEIGHT)
	var bar_pos := HudPos.loading_bar_pos()
	var bar_size := HudPos.loading_bar_size()
	var x := int(bar_pos.x * s.x)
	var y := int(bar_pos.y * s.y)
	var w := int(bar_size.x * s.x)
	var h := int(bar_size.y * s.y)
	# Layered filled rects, each inset 1px: black, gray, black track, then the
	# fill [orig: draw_progress_bar_0 — outer spans w+6/h+6, three inset border
	# draws, fill right edge = pct*(inner width)/100 + left + 2, one last inset].
	draw_rect(Rect2(x, y, w + 6, h + 6), Color.BLACK)
	draw_rect(Rect2(x + 1, y + 1, w + 4, h + 4), HudPos.loading_bar_border_gray())
	draw_rect(Rect2(x + 2, y + 2, w + 2, h + 2), Color.BLACK)
	var span := bar_fill_span(x, w, _displayed)
	if span.y > span.x:
		draw_rect(Rect2(span.x, y + 3, span.y - span.x, h), HudPos.loading_bar_fill_color())


# LoadingText lookup against the registered gametext table; a miss returns the
# fallback, never the "??..??" marker (the original returns the raw entry or
# its literal fallback) [orig: GameText_GetStringWithFallback @ 0x51eb90;
# TextResource_FindEntryBySectionAndKey(g_TextGameText, "LoadingText", key) @ 0x51f3cf].
func _lookup_loading_text(key: String, fallback: String) -> String:
	if key.is_empty():
		return fallback
	var table: RtxtStringFile = Strings.get_table("gametext")
	if table == null or not table.has_string_in_section("LoadingText", key):
		return fallback
	return table.get_string_in_section("LoadingText", key)


func _load_font(root: ResourceRoot, name: String) -> FontFile:
	if root == null:
		return null
	var res: FntResource = root.load_font(name)
	if res == null:
		return null
	return res.to_font_file()
