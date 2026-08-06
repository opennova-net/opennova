class_name HudWeaponText
extends RefCounted

## The weapon-coupled HUD text pair: the ammo counter at AMMOCOUNTPOS and the weapon
## display name at HUDWEAPONNAME, both drawn half-bright in WEAPON_TEXTCOLOR with the
## token's alignment. [orig: hud_draw_weapon_ammo_and_name @0x5939d0 — the three
## alignment paths route through HUD_DrawTextRightAligned_HalfBright @0x580850 /
## center @0x580680 / left @0x5804c0]
##
## Positions are the 4-field (x, y, hidden, align) form: hidden != 0 disables the
## element. [orig: gates @0x5939f3 (ammo) / @0x593b1c (name)]


## "clip/reserve" when a magazine weapon (clip valid, capacity >= 2), plain "reserve"
## otherwise; nothing when reserve or capacity is the -1 sentinel (no ammo model —
## knives etc.). [orig: @0x593a33..0x593ab0]
static func format_ammo(clip: int, reserve: int, capacity: int) -> String:
	if reserve == -1 or capacity == -1:
		return ""
	if clip != -1 and capacity >= 2:
		return "%d/%d" % [clip, reserve]
	return "%d" % reserve


static func draw_ammo(ci: CanvasItem, font: Font, pos: Vector4i, clip: int, reserve: int,
		capacity: int, color: Color, surface: Vector2) -> void:
	if ci == null or font == null or pos.z != 0:
		return
	var text := format_ammo(clip, reserve, capacity)
	if text.is_empty():
		return
	HudText.draw_text(ci, font, Vector2(pos.x, pos.y), surface, text,
		HudText.half_bright(color), pos.w)


## The display name resolved from the gametext "WepDes" section by the raw weapon id;
## a miss draws nothing (the original's GameText miss is the empty string). On surfaces
## 640 wide or narrower the x nudges −4 (left) / +4 (right). [orig: @0x593b36..0x593bf5;
## GameText_GetString @0x51ebd0 -> "WepDes" @0x593b7f]
static func draw_weapon_name(ci: CanvasItem, font: Font, pos: Vector4i, name: String,
		color: Color, surface: Vector2) -> void:
	if ci == null or font == null or pos.z != 0 or name.is_empty():
		return
	var x := pos.x
	if surface.x <= 640.0:
		if pos.w == 0:
			x -= 4
		elif pos.w == 1:
			x += 4
	HudText.draw_text(ci, font, Vector2(x, pos.y), surface, name,
		HudText.half_bright(color), pos.w)
