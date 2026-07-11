class_name HudMessages
extends RefCounted

## The system/debug message feed the mission's triggered text lands in: a scrolling
## ring of timed lines. WAC/BMS "text" actions resolve their id against the mission
## string table (section "Triggered Text", key "ID%03i") and push the line here for
## ~15 seconds. [orig: HUD_DisplayTriggeredText @0x51f190 -> Chat_AddDebugMessage
## @0x4987f0 (color -1, 930 ticks)]
##
## Faithful shape at the message-line altitude: per-message expiry with the witnessed
## 930-tick life and the >=186-tick expiry stagger between consecutive pushed messages;
## wrapped slots share their source timer and scroll upward (newest at the anchor). The full
## chat pipeline (channels, input line, geometry table, per-line fade curve) is a
## recorded follow-up (docs/interface/hud-re.md D-HUD-6).

const LINE_LIFE_TICKS := 930   # [orig: the 930 literal @0x51f216]
const EXPIRY_STAGGER := 186    # [orig: @0x49894e prev+186 floor]
const LINE_TEXT_MAX := 119     # [orig: @0x49884e 120-byte slots]
const DISPLAY_SLOT_COUNT := 40 # [orig: Chat_RebuildDisplayBuffers @0x498bd0]

var _lines: Array[Dictionary] = [] # {text: String, color: Color, expire: int}


func clear() -> void:
	_lines.clear()


func push(text: String, color: Color, now_ticks: int) -> void:
	if text.is_empty():
		return
	var expire := now_ticks + LINE_LIFE_TICKS
	if not _lines.is_empty():
		expire = maxi(expire, int(_lines[-1]["expire"]) + EXPIRY_STAGGER)
	_lines.append({ "text": text, "color": color, "expire": expire })
	while _lines.size() > DISPLAY_SLOT_COUNT:
		_lines.pop_front()


func live_lines(now_ticks: int) -> Array[Dictionary]:
	var out: Array[Dictionary] = []
	for l in _lines:
		if int(l["expire"]) > now_ticks:
			out.append(l)
	return out


## Build the channel's fixed display buffer after the font and channel width
## are known. Slots include the continuation indent in the 119-character cap.
func display_lines(font: Font, fs: int, now_ticks: int, wrap_width_px: float) -> Array[Dictionary]:
	var rows: Array[Dictionary] = []
	for line in live_lines(now_ticks):
		for text in _wrap(String(line["text"]), font, fs, wrap_width_px):
			rows.append({
				"text": text,
				"color": line["color"],
				"expire": line["expire"],
			})
	while rows.size() > DISPLAY_SLOT_COUNT:
		rows.pop_front()
	return rows


## Draw up to `max_lines` (the hudpos HUDCHLINE count) newest-first from the anchor
## upward, wrapped to `wrap_width_design`. Continuation lines get the original's
## two-space indent. [orig: Chat_AddDebugMessage wrap loop @0x498994]
func draw(ci: CanvasItem, font: Font, anchor_design: Vector2, surface: Vector2,
		now_ticks: int, max_lines: int, wrap_width_design: float, default_color: Color) -> void:
	if ci == null or font == null or max_lines <= 0:
		return

	var fs := 16
	if font is FontFile:
		fs = (font as FontFile).get_fixed_size()
	# Wrap in design units: measured surface width scaled back to the design space.
	var scale_x := surface.x / HudLayout.DESIGN_WIDTH if surface.x > 0.0 else 1.0
	var rows := display_lines(font, fs, now_ticks, wrap_width_design * scale_x)
	if rows.is_empty():
		return

	var scale_y := surface.y / HudLayout.DESIGN_HEIGHT if surface.y > 0.0 else 1.0
	var line_h := font.get_height(fs) / scale_y
	var shown := mini(rows.size(), max_lines)
	var first := rows.size() - shown
	for i in shown:
		var row: Dictionary = rows[first + i]
		var color: Color = row["color"] if row["color"].a > 0.0 else default_color
		var y := anchor_design.y - float(shown - 1 - i) * line_h
		HudText.draw_text(ci, font, Vector2(anchor_design.x, y), surface,
			String(row["text"]), color, HudText.Align.LEFT, fs)


static func _wrap(text: String, font: Font, fs: int, width_px: float) -> PackedStringArray:
	var out := PackedStringArray()
	var remaining := text
	while not remaining.is_empty():
		var prefix := "" if out.is_empty() else "  "
		var take := _fitting_character_count(remaining, prefix, font, fs, width_px)
		if take >= remaining.length():
			out.append(prefix + remaining)
			break

		var piece := remaining.left(take)
		var break_at := piece.rfind(" ")
		if break_at > 0:
			piece = piece.left(break_at)
			remaining = remaining.substr(break_at + 1)
		else:
			remaining = remaining.substr(take)
		out.append(prefix + piece)
	return out


static func _fitting_character_count(text: String, prefix: String, font: Font,
		fs: int, width_px: float) -> int:
	var maximum := mini(text.length(), LINE_TEXT_MAX - prefix.length())
	if font == null or width_px <= 0.0:
		return maximum
	var fitting := 0
	for count in range(1, maximum + 1):
		var candidate := prefix + text.left(count)
		if font.get_string_size(candidate, HORIZONTAL_ALIGNMENT_LEFT, -1, fs).x > width_px:
			break
		fitting = count
	# A glyph wider than the whole channel must still consume one character so
	# malformed/narrow geometry cannot stall rebuilding the display buffer.
	return maxi(fitting, 1)
