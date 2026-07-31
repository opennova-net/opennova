class_name HudCrosshair
extends RefCounted

## The original reticle: one crosshair texture drawn as 5 regions — top/bottom/left/
## right arms plus a center quad — each a triangle strip whose vertex UVs are the
## vertex's normalized position within the (spread-shifted) quad rect. The arms taper:
## their inner vertices sit at the quad midpoint pulled back by 0.1 × half-extent, which
## lands their UVs on the witnessed 0.45 / 0.5 / 0.55 atlas bands (center band
## 0.45..0.55). With spread 0 the regions assemble the full reticle at center.
## [orig: HUD_DrawCrosshair @0x592640 -> HUD_DrawCrosshairCornerQuad @0x590f50]
##
## The texture is NOT per-weapon: the original loads "cross%02d.tga" indexed by the
## user's crosshair-style config (+1). [orig: HUD_LoadAllTextures @0x59e3d6,
## style index @0x25510dc]
##
## Vertex color rides the strip's specular channel in the original; this port passes it
## as the polygon vertex color (the fixed-function blend stage of the HUD shader pass is
## a recorded follow-up — identical for the default white; docs/interface/hud-re.md
## D-HUD-8).

const TAPER := 0.1 # inner-vertex pull-back factor [orig: 0x590f50 all cases]


## The witnessed spread → pixel-offset math, recoil terms included by the caller:
## pixel = (spread_16.16 × screen_w / int(fov_deg)) >> 16, where spread is the ERROR
## table row plus the two recoil accumulators (>>7), all 16.16 degrees, and the
## degrees→binary-angle factors (2^31/180) cancel between spread and fov scale.
## [orig: HUD_DrawCrosshair @0x592b07..0x592bf5 — flt_7D76D0 = 11930464 = 2^31/180
## on both sides; fov = HIWORD(g_cameraFovDeg 16.16)]
static func spread_px_fp16(spread_fp16: int, fov_deg: float,
		screen_w: float) -> float:
	var fov_i := int(fov_deg) # HIWORD truncation of the 16.16 fov register
	if fov_i <= 0:
		return 0.0
	return float(int(float(spread_fp16) * screen_w / float(fov_i)) >> 16)


## Compatibility entry for degree-valued callers. Dynamic weapon spread uses the
## fixed-point entry above so the two accumulator shifts remain lossless.
static func spread_px(spread_deg: float, fov_deg: float, screen_w: float) -> float:
	return spread_px_fp16(int(spread_deg * 65536.0), fov_deg, screen_w)


## Retail's HUD instability sum before projection into pixels. GDScript's signed
## right shift is arithmetic, matching the two x86 SAR instructions.
## [orig: HUD_DrawCrosshair @0x592b07..0x592b28]
static func total_spread_fp16(error_fp16: int, recoil_pitch_bam: int,
		weapon_weight_spread_bam: int) -> int:
	var wrapped := _wrap_i32(error_fp16 + (recoil_pitch_bam >> 7))
	return _wrap_i32(wrapped + (weapon_weight_spread_bam >> 7))


static func _wrap_i32(value: int) -> int:
	return ((value + 0x80000000) & 0xFFFFFFFF) - 0x80000000


## Ordinary on-foot aimed shots hide the reticle. Retail's vehicle/gunner leg
## can explicitly keep it while using the second ERROR triplet.
## [orig: HUD_DrawCrosshair gate @0x592afa]
static func should_draw(aimed_shot_available: bool,
		keep_while_aimed: bool = false) -> bool:
	return not aimed_shot_available or keep_while_aimed


## The ERROR-table row for the crosshair spread: stance (0=prone, 1=crouch, 2=stand)
## plus 3 when the aim is scoped/sighted; forced 2 when swimming/under water, 1 when
## mounted. [orig: HUD_DrawCrosshair @0x592b37..0x592b84 — entity+300 flags
## 0x100=prone, 0x200=crouch]
static func error_row(stance: int, scoped: bool) -> int:
	return clampi(stance, 0, 2) + (3 if scoped else 0)


## Draw the 5 regions. `center_design`/`size_design` are virtual 1024×768 units (the
## original centers on the projected aim point — the screen center on foot — and uses
## the texture's pixel size as its virtual quad size). `spread` is in the same units
## (already through spread_px). The center quad ignores spread. [orig: corner calls
## @0x592c50..0x592cd2: top (x, y−off), bottom (x, y+off), left (x−off, y),
## right (x+off, y), center (x, y)]
static func draw(ci: CanvasItem, texture: Texture2D, center_design: Vector2,
		surface: Vector2, spread: float = 0.0, color: Color = Color.WHITE) -> void:
	if ci == null or texture == null:
		return
	var size := texture.get_size()
	var offsets: Array[Vector2] = [
		Vector2(0, -spread), Vector2(0, spread),
		Vector2(-spread, 0), Vector2(spread, 0),
		Vector2.ZERO,
	]
	for corner in 5:
		_draw_corner(ci, texture, corner, center_design + offsets[corner], size, surface, color)


# One region: the witnessed 5-vertex strip (4 for the center), taper 0.1, UV =
# normalized vertex position within the quad rect. [orig: HUD_DrawCrosshairCornerQuad
# @0x590f50 cases 0..4; rhw 0.9 / diffuse 1.0 / specular = color]
static func _draw_corner(ci: CanvasItem, texture: Texture2D, corner: int,
		center: Vector2, size: Vector2, surface: Vector2, color: Color) -> void:
	var half := size * 0.5
	var l := center.x - half.x
	var t := center.y - half.y
	var r := center.x + half.x
	var b := center.y + half.y
	var mx := (l + r) * 0.5
	var my := (t + b) * 0.5
	var tx := TAPER * half.x
	var ty := TAPER * half.y

	var strip: PackedVector2Array
	match corner:
		0: # top arm
			strip = PackedVector2Array([Vector2(l, t), Vector2(mx - tx, my - ty),
				Vector2(mx, t), Vector2(mx + tx, my - ty), Vector2(r, t)])
		1: # bottom arm
			strip = PackedVector2Array([Vector2(l, b), Vector2(mx - tx, my + ty),
				Vector2(mx, b), Vector2(mx + tx, my + ty), Vector2(r, b)])
		2: # left arm
			strip = PackedVector2Array([Vector2(l, t), Vector2(mx - tx, my - ty),
				Vector2(l, my), Vector2(mx - tx, my + ty), Vector2(l, b)])
		3: # right arm
			strip = PackedVector2Array([Vector2(r, t), Vector2(mx + tx, my - ty),
				Vector2(r, my), Vector2(mx + tx, my + ty), Vector2(r, b)])
		_: # center quad
			strip = PackedVector2Array([Vector2(mx + tx, my - ty), Vector2(mx - tx, my - ty),
				Vector2(mx + tx, my + ty), Vector2(mx - tx, my + ty)])

	var quad_size := Vector2(r - l, b - t)
	if quad_size.x <= 0.0 or quad_size.y <= 0.0:
		return
	var uvs := PackedVector2Array()
	var points := PackedVector2Array()
	for p in strip:
		uvs.append(Vector2((p.x - l) / quad_size.x, (p.y - t) / quad_size.y))
		points.append(HudLayout.scale_point(p, surface))

	# Triangle-strip order (0,1,2)(2,1,3)(2,3,4); canvas has no culling so the
	# winding flip is irrelevant — emit sequential triangles.
	var tri_count := strip.size() - 2
	for i in tri_count:
		var idx := PackedInt32Array([i, i + 1, i + 2])
		var pts := PackedVector2Array([points[idx[0]], points[idx[1]], points[idx[2]]])
		var uv := PackedVector2Array([uvs[idx[0]], uvs[idx[1]], uvs[idx[2]]])
		var cols := PackedColorArray([color, color, color])
		ci.draw_polygon(pts, cols, uv, texture)
