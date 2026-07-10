class_name HudMessages
extends RefCounted

## The system/debug message feed the mission's triggered text lands in: a scrolling
## ring of timed lines. WAC/BMS "text" actions resolve their id against the mission
## string table (section "Triggered Text", key "ID%03i") and push the line here for
## ~15 seconds. [orig: HUD_DisplayTriggeredText @0x51f190 -> Chat_AddDebugMessage
## @0x4987f0 (color -1, 930 ticks)]
##
## Faithful shape at the message-line altitude: per-line expiry with the witnessed
## 930-tick life and the >=186-tick expiry stagger between consecutive lines; lines
## word-wrap to the channel width and scroll upward (newest at the anchor). The full
## chat pipeline (channels, input line, geometry table, per-line fade curve) is a
## recorded follow-up (docs/interface/hud-re.md D-HUD-6).

const LINE_LIFE_TICKS := 930   # [orig: the 930 literal @0x51f216]
const EXPIRY_STAGGER := 186    # [orig: @0x49894e prev+186 floor]
const LINE_TEXT_MAX := 119     # [orig: @0x49884e 120-byte slots]

var _lines: Array[Dictionary] = [] # {text: String, color: Color, expire: int}


func clear() -> void:
	_lines.clear()


func push(text: String, color: Color, now_ticks: int) -> void:
	if text.is_empty():
		return
	var expire := now_ticks + LINE_LIFE_TICKS
	if not _lines.is_empty():
		expire = maxi(expire, int(_lines[-1]["expire"]) + EXPIRY_STAGGER)
	_lines.append({ "text": text.left(LINE_TEXT_MAX), "color": color, "expire": expire })


func live_lines(now_ticks: int) -> Array[Dictionary]:
	var out: Array[Dictionary] = []
	for l in _lines:
		if int(l["expire"]) > now_ticks:
			out.append(l)
	return out


## Draw up to `max_lines` (the hudpos HUDCHLINE count) newest-first from the anchor
## upward, wrapped to `wrap_width_design`. Continuation lines get the original's
## two-space indent. [orig: Chat_AddDebugMessage wrap loop @0x498994]
func draw(ci: CanvasItem, font: Font, anchor_design: Vector2, surface: Vector2,
		now_ticks: int, max_lines: int, wrap_width_design: float, default_color: Color) -> void:
	if ci == null or font == null or max_lines <= 0:
		return
	var live := live_lines(now_ticks)
	if live.is_empty():
		return

	var fs := 16
	if font is FontFile:
		fs = (font as FontFile).get_fixed_size()
	# Wrap in design units: measured surface width scaled back to the design space.
	var scale_x := surface.x / HudLayout.DESIGN_WIDTH if surface.x > 0.0 else 1.0
	var rows: Array[Dictionary] = [] # {text, color}
	for l in live:
		var wrapped := _wrap(String(l["text"]), font, fs, wrap_width_design * scale_x)
		for i in wrapped.size():
			var t: String = wrapped[i] if i == 0 else "  " + wrapped[i]
			rows.append({ "text": t, "color": l["color"] })

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
	if width_px <= 0.0 or font.get_string_size(text, HORIZONTAL_ALIGNMENT_LEFT, -1, fs).x <= width_px:
		return PackedStringArray([text])
	var out := PackedStringArray()
	var line := ""
	for word in text.split(" ", false):
		var probe := word if line.is_empty() else line + " " + word
		if not line.is_empty() and font.get_string_size(probe, HORIZONTAL_ALIGNMENT_LEFT, -1, fs).x > width_px:
			out.append(line)
			line = word
		else:
			line = probe
	if not line.is_empty():
		out.append(line)
	return out
