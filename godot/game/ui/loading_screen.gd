class_name LoadingScreen
extends Control

## The mission loading screen: the per-mission sidecar image (or the stock
## loadscrn.pcx) stretched over the whole display, the MP session text
## composited over it, and the red progress bar near the bottom. After a
## single-player load with a custom background it also carries the
## start-mission splash: the same background with a blinking centered
## continue line and the cursor arrow, dismissed by any fresh key or mouse
## button.
##
## Structural translation of the witnessed originals:
##  - background + text compositing  [orig: render_loading_screen @ 0x521d10]
##  - session text provider          [orig: HUD_GetLoadingScreenTextByGameType @ 0x51f300]
##  - fullscreen stretch present     [orig: LoadingScreen_DrawEffectFullscreen @ 0x586ba0]
##  - throttle + exact bar draw      [orig: LoadingScreen_UpdateAndPresent @ 0x586be0]
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

## The layout values (image-space text band, server-message fractions/color,
## progress-bar rect/colors, the redraw throttle) are the engine's
## HudPos.LOADING_* constants and loading_* statics — the witnesses live at
## the engine home, engine/runtime/hud hud/loading_screen.h.
## The resource names (the stock background, the two fonts, the server-message
## label key and its literal fallback) are the engine's HudPos.loading_*
## statics too.

var _texture: Texture2D = null
var _has_custom_bg := false
var _in_session := false
var _title := ""          # server name [orig: g_sessionvar_server_name -> title buf @ 0x51f533]
var _mission_name := ""   # [orig: g_sessionvar_mission_name -> mission buf @ 0x51f53a]
var _game_type_text := "" # [orig: LoadingText LTGT_* lookup @ 0x51f3cf]
var _custom_text := ""    # server message body [orig: g_sessionvar_custom_text @ 0x522123]
var _font_small: FontFile = null
var _font_large: FontFile = null

var _progress := 0        # exact real-stage checkpoint
var _last_drawn_progress := -1
var _last_present_ms := -HudPos.LOADING_PRESENT_INTERVAL_MS
var _layout_viewport: Viewport = null
var _layout_base_size := Vector2.ZERO
var _presented_size := Vector2i.ZERO
var _bar_fill: ColorRect = null

## Emitted when the start-mission splash has been dismissed AND its final
## background-only frame has had a frame to render [orig: the post-loop
## present @ 0x520a48-0x520a79 precedes the caller's effect release @ 0x525d45].
signal splash_dismissed

enum SplashState { NONE, ACTIVE, CLOSING }


## One laid-out line of a wrapped text block: the slice to paint and the
## top-left the alignment resolved it to.
class TextBlockLine extends RefCounted:
	var text := ""
	var x := 0.0
	var y := 0.0

	static func make(p_text: String, p_x: float, p_y: float) -> TextBlockLine:
		var row := TextBlockLine.new()
		row.text = p_text
		row.x = p_x
		row.y = p_y
		return row


## A laid-out wrapped text block: the lines to paint, plus `stopped_at` -- 0
## when the whole string was consumed, otherwise the 1-based count of lines
## processed when the box ran out of vertical room.
class TextBlock extends RefCounted:
	var lines: Array[TextBlockLine] = []
	var stopped_at := 0


# The three characters the wrapped-text block treats structurally: a space is
# the wrap point it remembers, a carriage return is the hard break, and a line
# feed is only swallowed when it trails a break
# retail: the 32 / 13 / 10 tests at 0x580f88, 0x580fdf and 0x581128.
const TEXT_SPACE := 32
const TEXT_CARRIAGE_RETURN := 13
const TEXT_LINE_FEED := 10
# The size a .fnt without a fixed size is drawn at.
const DEFAULT_TEXT_SIZE := 16

var _splash_state := SplashState.NONE
var _splash_arrow: Texture2D = null      # newarow1.tga, the menu cursor art
var _splash_font: FontFile = null        # Impac22b.fnt (the large HUD label slot)
var _splash_text := ""                   # LoadingText/LT_Continue ("" -> no line)
var _splash_arrow_anchor := Vector2.ZERO # live cursor pos, viewport px
var _splash_blink_on := true
var _splash_frames_until_emit := 0


## The background image pick: the image name and whether it is the mission's
## own sidecar (the retail custom-background flag the SP splash gate reads).
class BackgroundPick:
	var name := ""
	var custom := false

	static func make(name: String, custom: bool) -> BackgroundPick:
		var pick := BackgroundPick.new()
		pick.name = name
		pick.custom = custom
		return pick


## Resolve the background image for a mission: the sidecar if present, else the
## stock fallback [orig: FileSystem_FileExists probe @ 0x521db5; fallback
## @ 0x521e20].
static func resolve_background(root: ResourceRoot, mission_file: String) -> BackgroundPick:
	var sidecar := HudPos.loading_sidecar_image_name(mission_file)
	# UI image probes force loose-first for this lookup, independent of /d.
	# [orig: CUIImage_LoadTextureFromFile @ 0x6541ba]
	if root != null and not sidecar.is_empty() and root.has_file(
			sidecar, ResourceRoot.LOOKUP_FORCE_LOOSE_FIRST):
		return BackgroundPick.make(sidecar, true)
	return BackgroundPick.make(HudPos.loading_fallback_image(), false)


## Public texture-load seam for owners/tests; avoids private-state inspection (ADR 0018).
## [orig: CUIImage_LoadTextureFromFile @ 0x6541ba]
static func load_background_texture(root: ResourceRoot, image_name: String) -> Texture2D:
	if root == null or image_name.is_empty():
		return null
	return root.load_texture(image_name, ResourceRoot.LOOKUP_FORCE_LOOSE_FIRST)


## Build the screen for a mission load (LoadingScreenInfo):
##   mission_file — the .bms name driving the sidecar lookup
##   in_session — MP session: draw the text overlay [orig: gate @ 0x521ebe]
##   server_name / mission_name / custom_text — the session variables
##     [orig: SERVERNAME/MISSIONNAME/CUSTOMTEXT @ parse_server_session_variables
##      0x5202f0 / serialize_mission_info_to_datastream 0x523620]
##   game_type — the numeric session game type [orig: GAMETYPE]; not carried
##     (negative) reads as 0
func setup(root: ResourceRoot, info: LoadingScreenInfo) -> void:
	var bg := resolve_background(root, info.mission_file)
	_has_custom_bg = bg.custom
	_texture = load_background_texture(root, bg.name)
	_in_session = info.in_session
	if not _in_session:
		queue_redraw()
		return
	# MP only: the text overlay and its fonts [orig: fonts loaded only on the
	# in-session path @ 0x521eec/0x521f5a].
	_title = info.server_name
	_mission_name = info.mission_name
	_custom_text = info.custom_text
	_game_type_text = _lookup_loading_text(HudPos.loading_gametype_text_key(maxi(info.game_type, 0)), "")
	_font_small = _load_font(root, HudPos.loading_font_small())
	_font_large = _load_font(root, HudPos.loading_font_large())
	queue_redraw()


## Refresh mid-load: a JOINER learns the authoritative session record (server/
## mission names, game type, and the mission file driving the sidecar
## background) from the post-auth S2C 0x7B AFTER the screen is already up.
## Retail fills the same buffers from the connect stream during its load
## [orig: parse_server_session_variables @ 0x5202f0 -> the title/mission bufs
## @ 0x51f533/0x51f53a]. Empty values keep the current ones.
func update_session_info(root: ResourceRoot, info: LoadingScreenInfo) -> void:
	if not info.mission_file.is_empty():
		var bg := resolve_background(root, info.mission_file)
		var texture := load_background_texture(root, bg.name)
		if texture != null:
			_has_custom_bg = bg.custom
			_texture = texture
	if _in_session:
		if not info.server_name.is_empty():
			_title = info.server_name
		if not info.mission_name.is_empty():
			_mission_name = info.mission_name
		if info.game_type >= 0:
			_game_type_text = _lookup_loading_text(
					HudPos.loading_gametype_text_key(info.game_type), _game_type_text)
	queue_redraw()


## Report load progress [orig: the per-stage/per-model calls into
## LoadingScreen_UpdateAndPresent, e.g. Game_StartMission @ 0x52498f..0x525d29].
func set_progress(percent: int) -> void:
	_progress = clampi(percent, 0, 100)
	_sync_bar_fill()


## Redraw + present if due: on a real checkpoint change or every 100 ms for
## window-event pumping [orig: LoadingScreen_UpdateAndPresent @ 0x586be0].
## `force` seeds the first frame but never changes the checkpoint.
func present(force := false) -> void:
	if _texture == null:
		return  # no background loaded -> no screen at all [orig: @ 0x586bfd]
	var now := Time.get_ticks_msec()
	# The shared native due rule (interval elapsed or a real checkpoint changed)
	# lives in engine/runtime/hud/loading_screen.h.
	var due := HudPos.loading_present_due(now - _last_present_ms,
			_last_drawn_progress != _progress)
	if not (force or due):
		return
	_last_present_ms = now
	_last_drawn_progress = _progress
	# The original pumps window messages and presents mid-load; process_events
	# + force_draw are the shell-side equivalents so the OS window stays live
	# and the screen refreshes while the load blocks the main loop
	# [orig: Game_PumpWindowMessages @ 0x586be6 + Present @ 0x586d53].
	if is_inside_tree():
		if not GameRuntimeRoot.is_headless():
			DisplayServer.process_events()
			var runtime_root := get_tree().current_scene as GameRuntimeRoot
			if runtime_root != null:
				runtime_root.sync_game_viewport_to_window()
		# A fullscreen event can change the viewport during process_events while
		# the ordinary SceneTree loop is blocked. Refit before this same forced
		# draw so no old-resolution strip survives for a frame.
		_fit_to_viewport()
		# queue_redraw is serviced only on an idle SceneTree frame. The background
		# and frame keep their already-submitted commands; progress and fullscreen
		# coverage update through CanvasItem transforms, which reach the rendering
		# server during this blocked call.
		_sync_bar_fill()
	queue_redraw()
	if is_inside_tree() and not GameRuntimeRoot.is_headless():
		RenderingServer.force_draw(true, 0.0)


## Give a newly mounted Control one complete SceneTree frame to receive layout
## and submit its queued draw before the caller enters synchronous mission
## loading. `process_frame` fires at the start of a frame, so crossing two of
## those signals is what lets one ordinary render/present finish. The forced
## present after that frame keeps the window responsive during the block.
func prepare_for_blocking_load(operation: WorldLoadOperation) -> bool:
	if operation == null or operation.is_cancelled() or not is_inside_tree():
		return false
	# Submit the exact initial checkpoint with the registration frame.
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
	return _progress


## Pixel extent covered by this screen after its transform is applied.
func presented_size() -> Vector2i:
	return _presented_size


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


## Leave the player-paced splash through the same closing-frame state machine
## as a witnessed key/button press. Render probes use this public seam so they
## never synthesize input or mistake the held loading background for gameplay.
func dismiss_start_mission_splash() -> bool:
	if _splash_state != SplashState.ACTIVE:
		return false
	_splash_exit_edge()
	return true


## Public ADR-0018 read seam: the cursor-arrow art decoded (a miss degrades
## to an arrowless splash, mirroring the original's unguarded TGA load).
func has_splash_arrow() -> bool:
	return _splash_arrow != null


func _ready() -> void:
	_layout_viewport = get_viewport()
	set_anchors_preset(Control.PRESET_TOP_LEFT)
	position = Vector2.ZERO
	_fit_to_viewport()
	_create_bar_fill()
	if _layout_viewport != null and not _layout_viewport.size_changed.is_connected(
			_fit_to_viewport):
		_layout_viewport.size_changed.connect(_fit_to_viewport)
	# The splash handlers below enable per-frame callbacks by existing; keep
	# both off until the splash actually raises. Guarded: a splash raised
	# before the screen entered the tree must not be silently disarmed here
	# (it could then never dismiss).
	if _splash_state == SplashState.NONE:
		set_process(false)
		set_process_input(false)


func _exit_tree() -> void:
	if _layout_viewport != null and _layout_viewport.size_changed.is_connected(
			_fit_to_viewport):
		_layout_viewport.size_changed.disconnect(_fit_to_viewport)
	_layout_viewport = null


func _fit_to_viewport() -> void:
	if _layout_viewport == null:
		return
	var target := _layout_viewport.get_visible_rect().size
	if target.x <= 0.0 or target.y <= 0.0:
		return
	set_anchors_preset(Control.PRESET_TOP_LEFT)
	position = Vector2.ZERO
	pivot_offset = Vector2.ZERO
	_presented_size = Vector2i(roundi(target.x), roundi(target.y))
	# Keep one submitted set of draw commands at the initial viewport size and
	# resize it through the direct CanvasItem RID transform below. Unlike
	# queue_redraw(), that reaches RenderingServer while the loader blocks the
	# SceneTree, so a fullscreen event cannot leave an old-resolution strip.
	if _layout_base_size == Vector2.ZERO:
		_layout_base_size = target
		size = target
	var stretch := Vector2(target.x / _layout_base_size.x,
			target.y / _layout_base_size.y)
	scale = stretch
	force_update_transform()
	if is_inside_tree():
		# Control defers its ordinary transform flush with the SceneTree. Push the
		# same local transform straight to the canvas RID so a resize processed
		# inside synchronous loading reaches this force_draw().
		RenderingServer.canvas_item_set_transform(get_canvas_item(), Transform2D(
				Vector2(stretch.x, 0.0), Vector2(0.0, stretch.y), Vector2.ZERO))
	queue_redraw()


func _create_bar_fill() -> void:
	_bar_fill = ColorRect.new()
	_bar_fill.name = "ProgressFill"
	_bar_fill.mouse_filter = Control.MOUSE_FILTER_IGNORE
	_bar_fill.color = HudPos.loading_bar_fill_color()
	_bar_fill.set_anchors_preset(Control.PRESET_TOP_LEFT)
	add_child(_bar_fill)
	_layout_bar_fill()
	_sync_bar_fill()


func _layout_bar_fill() -> void:
	if _bar_fill == null or _layout_base_size == Vector2.ZERO:
		return
	var surface_scale := _layout_base_size / Vector2(
			HudPos.DESIGN_WIDTH, HudPos.DESIGN_HEIGHT)
	var bar_pos := HudPos.loading_bar_pos()
	var bar_size := HudPos.loading_bar_size()
	var x := int(bar_pos.x * surface_scale.x)
	var y := int(bar_pos.y * surface_scale.y)
	var w := int(bar_size.x * surface_scale.x)
	var h := int(bar_size.y * surface_scale.y)
	var full_span := HudPos.loading_bar_fill_span(x, w, 100)
	_bar_fill.position = Vector2(full_span.x, y + 3)
	_bar_fill.size = Vector2(full_span.y - full_span.x, h)
	_bar_fill.pivot_offset = Vector2.ZERO


func _sync_bar_fill() -> void:
	if _bar_fill == null:
		return
	var full_width := int(_bar_fill.size.x)
	if full_width <= 0:
		return
	var x := int(_bar_fill.position.x) - 3
	var span := HudPos.loading_bar_fill_span(x, full_width, _progress)
	var fill_width := maxi(0, span.y - span.x)
	var should_draw := _texture != null and _splash_state == SplashState.NONE \
			and fill_width > 0
	# The rectangle's full draw command is submitted during the two-frame
	# preparation barrier. Only its transform/modulate change during the
	# synchronous loader, so force_draw can present the exact checkpoint
	# without waiting for an idle-frame redraw.
	var fill_scale := float(fill_width) / float(full_width)
	var fill_modulate := Color.WHITE if should_draw \
			else Color(1.0, 1.0, 1.0, 0.0)
	_bar_fill.scale = Vector2(maxf(fill_scale, 0.000001), 1.0)
	_bar_fill.self_modulate = fill_modulate
	_bar_fill.force_update_transform()
	if _bar_fill.is_inside_tree():
		RenderingServer.canvas_item_set_transform(
				_bar_fill.get_canvas_item(),
				Transform2D(Vector2(fill_scale, 0.0), Vector2(0.0, 1.0),
						_bar_fill.position))
		RenderingServer.canvas_item_set_self_modulate(
				_bar_fill.get_canvas_item(), fill_modulate)


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
		_splash_arrow = TgaTexture.load_from_root(root, HudPos.loading_splash_arrow_image())
		_splash_font = _load_font(root, HudPos.loading_splash_continue_font())
	_splash_text = _lookup_loading_text(HudPos.loading_splash_continue_key(), "")
	_splash_state = SplashState.ACTIVE
	_sync_bar_fill()
	_splash_blink_on = _splash_blink_phase()
	if is_inside_tree():
		_splash_arrow_anchor = _viewport_to_local(
				get_viewport().get_mouse_position())
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
		_splash_arrow_anchor = _viewport_to_local(motion.position)
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


func _viewport_to_local(point: Vector2) -> Vector2:
	return Vector2(
			point.x / scale.x if not is_zero_approx(scale.x) else point.x,
			point.y / scale.y if not is_zero_approx(scale.y) else point.y)


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
	if not is_inside_tree() or GameRuntimeRoot.is_headless():
		# No live renderer to wait on: finish on the next process tick.
		_splash_frames_until_emit = 0


func _draw() -> void:
	if _texture == null:
		return
	# Background: stretched to the full display, no aspect preservation.
	# Unmodulated — the original's 0xFF7F7F7F modulate is the MODULATE2X
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
		var label := _lookup_loading_text(HudPos.loading_msg_label_key(),
				HudPos.loading_msg_label_fallback()) + ":"
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


## The wrapped-text block, ported line-break rule for line-break rule.
##
## `left`..`right` is the box the text measures against, `skip_lines` drops the
## first N wrapped lines (they are still consumed and cost no vertical space),
## and drawing stops once the next line would pass `bottom` -- unless
## `top == bottom`, which disables the vertical clip entirely. Returns 0 when
## the whole string was consumed, otherwise the 1-based count of lines
## processed when it ran out of room.
##
## retail: render_draw_wrapped_text_block_ex @ 0x580eb0. The two kerning
## parameters offset the first (`use_kerning_start`) and the wrapped
## (`use_kerning_wrap`) lines by the font's tab-width field font+0x168; every
## loading-screen call site passes `use_kerning_start = 0` (@0x521fee,
## @0x52201a, @0x52203c, @0x5220bd) and the field is 0 unless
## GText_SetLineSpacing @ 0x674720 sets one, so the offset folds out here --
## D-LOADSCR-2 carries the residual with the rest of the CGameFont metrics.
static func wrap_text_lines(font: Font, font_size: int, text: String,
		max_width: int) -> PackedStringArray:
	var out := PackedStringArray()
	if font == null or text.is_empty() or max_width <= 0:
		return out
	var n := text.length()
	var line_start := 0
	var i := 0
	var last_space := 0  # absolute index; 0 means "no usable space on this line"
	var width := 0       # loop-carried: a NUL/end step re-uses the last measure
	while i <= n:
		var ch := text.unicode_at(i) if i < n else 0
		if ch == TEXT_SPACE:
			last_space = i
		if ch != 0:
			# Measure the whole prefix up to and INCLUDING this character, the
			# way retail NUL-terminates one past it and re-measures the line.
			var span := i - line_start + 1
			width = (int(font.get_string_size(text.substr(line_start, span),
					HORIZONTAL_ALIGNMENT_LEFT, -1, font_size).x) if span > 0 else 0)
		var break_at := i
		if width <= max_width:
			# Only '\r' and the terminator break a line that still fits; a '\n'
			# is an ordinary glyph unless it trails a break.
			if ch != TEXT_CARRIAGE_RETURN and ch != 0:
				i += 1
				continue
		elif last_space != 0:
			# Overflow with a space on this line: break at the LAST space.
			break_at = last_space
		# Overflow with no space breaks at the current character, which is then
		# dropped with every other break character.
		out.append(text.substr(line_start, break_at - line_start))
		if break_at >= n:
			return out
		line_start = break_at + 1
		if line_start < n and text.unicode_at(line_start) == TEXT_LINE_FEED:
			line_start += 1  # a '\n' right after the break is swallowed
		i = break_at + 1
		last_space = 0
		width = 0
	return out


## The laid-out block: every line the drawer will paint, already placed, plus
## the line count the walk stopped at.
##
## Lines before `skip_lines` are consumed without being placed AND without
## costing vertical space; a line that measures empty advances HALF a line
## instead of a whole one.
##
## retail: the drawing half of render_draw_wrapped_text_block_ex @ 0x580eb0 --
## the alignment fold (4 = centred on left + width/2 @0x5810ab, 5 = right
## aligned on rect_right @0x581094, else left @0x58107f), the half advance
## `extent >> 1` @0x580f40/@0x581005 against the full advance @0x5810d7, the
## skip_lines jump @0x580ffb, and the ordering of the end-of-text return
## @0x581155 ahead of the bottom test @0x58111e.
static func layout_text_block(font: Font, font_size: int, text: String,
		left: int, top: int, right: int, bottom: int,
		align: HorizontalAlignment, skip_lines := 0) -> TextBlock:
	var out := TextBlock.new()
	var width := right - left
	if font == null or text.is_empty() or width <= 0:
		return out
	# The line pitch is the 'I' character's own extent
	# retail: CGameFont_GetCharExtent(font, 'I', ...) @ 0x580f2b.
	var line_h := int(font.get_height(font_size))
	var half_h := line_h >> 1
	var cursor := top
	var lines := wrap_text_lines(font, font_size, text, width)
	for i in lines.size():
		var line := lines[i]
		if i >= skip_lines:
			var line_w := (font.get_string_size(line, HORIZONTAL_ALIGNMENT_LEFT,
					-1, font_size).x if not line.is_empty() else 0.0)
			if line_w <= 0.0:
				cursor += half_h
			else:
				var px := float(left)
				if align == HORIZONTAL_ALIGNMENT_CENTER:
					px = float(left + (width >> 1)) - line_w * 0.5
				elif align == HORIZONTAL_ALIGNMENT_RIGHT:
					px = float(right) - line_w
				out.lines.append(TextBlockLine.make(line, px, float(cursor)))
				cursor += line_h
		# The terminating break reports "consumed"; only a line that still has
		# text behind it can run the box out of room, and top == bottom disables
		# the vertical clip entirely.
		if i == lines.size() - 1:
			return out
		if bottom != top and cursor + line_h > bottom:
			out.stopped_at = i + 1
			return out
	return out


# Word-wrapped text in a box, painted from the laid-out block. `width` is the
# band width (retail's rect_right - rect_left); `y` is the line TOP, so each
# baseline adds the ascent.
# Line metrics ride the FontFile view of the .fnt; exact CGameFont glyph
# spacing is the standing follow-up (docs/interface/loading-screen-re.md
# D-LOADSCR-2, shared with hud-re.md).
func _draw_wrapped(font: FontFile, text: String, x: int, y: int, width: int,
		bottom: int, align: HorizontalAlignment, color: Color,
		skip_lines := 0) -> int:
	if font == null:
		return 0
	var fs := font.get_fixed_size()
	if fs <= 0:
		fs = DEFAULT_TEXT_SIZE
	var block := layout_text_block(font, fs, text, x, y, x + width, bottom,
			align, skip_lines)
	var ascent := font.get_ascent(fs)
	for row in block.lines:
		draw_string(font, Vector2(row.x, row.y + ascent), row.text,
				HORIZONTAL_ALIGNMENT_LEFT, -1, fs, color)
	return block.stopped_at


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
	# The fill is the ProgressFill child. Its complete red rectangle is
	# submitted before loading blocks, then scaled to the exact native span by
	# _sync_bar_fill() at each real checkpoint.


# LoadingText lookup against the registered gametext table; a miss returns the
# fallback, never the "??..??" marker (the original returns the raw entry or
# its literal fallback) [orig: GameText_GetStringWithFallback @ 0x51eb90;
# TextResource_FindEntryBySectionAndKey(g_TextGameText, "LoadingText", key) @ 0x51f3cf].
func _lookup_loading_text(key: String, fallback: String) -> String:
	return Strings.lookup_or(Strings.TABLE_GAMETEXT, Strings.SECTION_LOADING_TEXT, key, fallback)


func _load_font(root: ResourceRoot, name: String) -> FontFile:
	if root == null:
		return null
	var res: FntResource = root.load_font(name)
	if res == null:
		return null
	return res.to_font_file()


