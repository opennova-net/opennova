# CDEP per-block range enforcement.
# CDEP packs each row of the 1024×1024 heightmap into 4 blocks of 256 pixels
# with a 4-bit bits_per_delta field, so each block's max-min must fit in 15
# bits raw uint16 (= 32767, = 127.99609375 in editor float units, since the
# raw→float conversion is uint16/256.0).
class_name CDEPConstraint
extends RefCounted

const HM_SIZE := 1024
const BLOCK_WIDTH := 256
const BLOCKS_PER_ROW := HM_SIZE / BLOCK_WIDTH
const MAX_BLOCK_RANGE_RAW := 32767
const MAX_BLOCK_RANGE_FLOAT := 32767.0 / 256.0  # 127.99609375


# Returns Vector2(min, max) of the 256 pixels in block `bx` of row `z`,
# read from a FORMAT_RF heightmap image.
static func block_min_max(image: Image, bx: int, z: int) -> Vector2:
	var x_lo := bx * BLOCK_WIDTH
	var x_hi := x_lo + BLOCK_WIDTH
	var lo := INF
	var hi := -INF
	for x in range(x_lo, x_hi):
		var v := image.get_pixel(x, z).r
		if v < lo: lo = v
		if v > hi: hi = v
	return Vector2(lo, hi)


# Returns the block index range [bx_lo, bx_hi] (inclusive) covering the
# horizontal pixel span [x_lo, x_hi). If the span is empty, returns (-1, -2).
static func block_index_range(x_lo: int, x_hi: int) -> Vector2i:
	if x_hi <= x_lo:
		return Vector2i(-1, -2)
	var bx_lo := clampi(x_lo / BLOCK_WIDTH, 0, BLOCKS_PER_ROW - 1)
	var bx_hi := clampi((x_hi - 1) / BLOCK_WIDTH, 0, BLOCKS_PER_ROW - 1)
	return Vector2i(bx_lo, bx_hi)


# Clamp every 256-pixel block intersecting `rect` back into the per-block
# range limit. Always anchors at the block's current min and clamps any
# pixel above min + MAX_BLOCK_RANGE_FLOAT down — independent of how the
# brush moved things. This means smooth/flatten on a block that was
# *already* over-range will heal it instead of leaving the violation
# alone (which an "only clamp if the stroke extended the range" check
# would do). The float ceiling is rounded down by an extra ULP so the raw
# uint16 conversion (int(value * 256)) cannot land at min_raw + 32768
# from FP slop in pixel writes elsewhere in the pipeline.
static func clamp_blocks_in_rect(image: Image, rect: Rect2i) -> void:
	if rect.size.x <= 0 or rect.size.y <= 0:
		return
	var bx_range := block_index_range(rect.position.x, rect.end.x)
	if bx_range.x < 0:
		return
	var z_lo := clampi(rect.position.y, 0, HM_SIZE - 1)
	var z_hi := clampi(rect.end.y, 0, HM_SIZE)
	for z in range(z_lo, z_hi):
		for bx in range(bx_range.x, bx_range.y + 1):
			_clamp_block_top_down(image, bx, z)


static func _clamp_block_top_down(image: Image, bx: int, z: int) -> void:
	var mm := block_min_max(image, bx, z)
	if mm.y - mm.x <= MAX_BLOCK_RANGE_FLOAT:
		return
	var ceiling := mm.x + MAX_BLOCK_RANGE_FLOAT
	var x_lo := bx * BLOCK_WIDTH
	var x_hi := x_lo + BLOCK_WIDTH
	for x in range(x_lo, x_hi):
		var v := image.get_pixel(x, z).r
		if v > ceiling:
			image.set_pixel(x, z, Color(ceiling, 0, 0, 1))


# Scan every block in the heightmap and return the count of blocks that
# violate the CDEP per-block range limit. Used at load time to warn the user
# and at bake time as a hard guard.
static func count_violations(image: Image) -> int:
	var count := 0
	for z in HM_SIZE:
		for bx in BLOCKS_PER_ROW:
			var mm := block_min_max(image, bx, z)
			if mm.y - mm.x > MAX_BLOCK_RANGE_FLOAT:
				count += 1
	return count


# Walk every block in the heightmap and clamp any over-range block back into
# bounds, keeping the lowest pixel as anchor. Returns the number of blocks
# clamped. Used by the load-time auto-fix dialog.
static func clamp_all_violations(image: Image) -> int:
	var clamped := 0
	for z in HM_SIZE:
		for bx in BLOCKS_PER_ROW:
			var mm := block_min_max(image, bx, z)
			if mm.y - mm.x <= MAX_BLOCK_RANGE_FLOAT:
				continue
			var ceiling := mm.x + MAX_BLOCK_RANGE_FLOAT
			var x_lo := bx * BLOCK_WIDTH
			var x_hi := x_lo + BLOCK_WIDTH
			for x in range(x_lo, x_hi):
				var v := image.get_pixel(x, z).r
				if v > ceiling:
					image.set_pixel(x, z, Color(ceiling, 0, 0, 1))
			clamped += 1
	return clamped
