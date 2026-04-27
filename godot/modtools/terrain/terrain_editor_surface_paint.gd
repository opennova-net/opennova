extends RefCounted

const HM_SIZE := 1024
const DEFAULT_SURFACE_INDEX := 1

const SURFACE_TYPES := [
	{"id": 0, "label": "Null", "color": Color8(0, 0, 0)},
	{"id": 1, "label": "Dirt", "color": Color8(153, 118, 61)},
	{"id": 2, "label": "Grass", "color": Color8(0, 210, 0)},
	{"id": 3, "label": "Snow", "color": Color8(204, 239, 244)},
	{"id": 4, "label": "Cement", "color": Color8(152, 152, 152)},
	{"id": 5, "label": "Sand", "color": Color8(255, 255, 0)},
	{"id": 6, "label": "Packed Dirt", "color": Color8(255, 128, 0)},
	{"id": 7, "label": "(Unused)", "color": Color8(0, 0, 255)},
	{"id": 8, "label": "(Unused)", "color": Color8(255, 0, 0)},
	{"id": 9, "label": "Mud", "color": Color8(114, 64, 0)},
	{"id": 10, "label": "Ice", "color": Color8(160, 190, 219)},
	{"id": 11, "label": "(Unused)", "color": Color8(161, 0, 161)},
	{"id": 12, "label": "Rock/Stone", "color": Color8(255, 0, 186)},
	{"id": 13, "label": "Wood", "color": Color8(158, 78, 0)},
	{"id": 14, "label": "Metal", "color": Color8(0, 201, 203)},
	{"id": 15, "label": "(Unused)", "color": Color8(255, 255, 255)},
]


static func get_surface_types() -> Array:
	return SURFACE_TYPES.duplicate(true)


static func get_surface_label(index: int) -> String:
	for entry in SURFACE_TYPES:
		if int(entry["id"]) == index:
			return String(entry["label"])
	return "Custom %d" % index


static func get_surface_color(index: int, palette: PackedByteArray = PackedByteArray()) -> Color:
	if palette.size() >= (index + 1) * 3 and index >= 0:
		var offset := index * 3
		return Color8(palette[offset], palette[offset + 1], palette[offset + 2])
	for entry in SURFACE_TYPES:
		if int(entry["id"]) == index:
			return entry["color"]
	var gray := clampi(index, 0, 255)
	return Color8(gray, gray, gray)


static func get_palette_bytes(state: Dictionary) -> PackedByteArray:
	return state.get("palette", PackedByteArray())


static func get_index(state: Dictionary, x: int, y: int) -> int:
	var width := int(state.get("width", 0))
	var height := int(state.get("height", 0))
	if x < 0 or y < 0 or x >= width or y >= height:
		return 0
	var indices: PackedByteArray = state.get("indices", PackedByteArray())
	var offset := y * width + x
	if offset < 0 or offset >= indices.size():
		return 0
	return int(indices[offset])


static func map_x_from_heightmap_x(hm_x: float, map_width: int) -> int:
	if map_width <= 0:
		return -1
	var clamped := clampf(hm_x, 0.0, float(HM_SIZE) - 0.001)
	return clampi(int(floor(clamped * float(map_width) / float(HM_SIZE))), 0, map_width - 1)


static func map_y_from_heightmap_y(hm_y: float, map_height: int) -> int:
	if map_height <= 0:
		return -1
	var clamped := clampf(hm_y, 0.0, float(HM_SIZE) - 0.001)
	return clampi(int(floor(clamped * float(map_height) / float(HM_SIZE))), 0, map_height - 1)


static func paint_circle(state: Dictionary, center_x: int, center_y: int, radius: int, hardness: float, strength: float, index: int) -> bool:
	var width := int(state.get("width", 0))
	var height := int(state.get("height", 0))
	var indices: PackedByteArray = state.get("indices", PackedByteArray())
	if width <= 0 or height <= 0 or indices.size() < width * height:
		return false
	if radius <= 0 or strength <= 0.0:
		return false

	var x_min := maxi(center_x - radius, 0)
	var y_min := maxi(center_y - radius, 0)
	var x_max := mini(center_x + radius + 1, width)
	var y_max := mini(center_y + radius + 1, height)
	if x_max <= x_min or y_max <= y_min:
		return false

	var radius_f := float(radius)
	var radius_sq := radius_f * radius_f
	var core := clampf(hardness, 0.0, 1.0)
	var shoulder := 1.0 - core
	var threshold := clampf(strength, 0.0, 1.0)
	var next_index := clampi(index, 0, 255)
	var changed := false

	for y in range(y_min, y_max):
		for x in range(x_min, x_max):
			var dx := float(x - center_x)
			var dy := float(y - center_y)
			var dist_sq := dx * dx + dy * dy
			if dist_sq > radius_sq:
				continue
			var distance := sqrt(dist_sq)
			var t := 1.0 - distance / radius_f
			var falloff := 1.0
			if shoulder > 0.0001 and t < shoulder:
				falloff = _smoothstep(t / shoulder)
			if falloff * threshold < 0.5:
				continue
			var offset := y * width + x
			if indices[offset] == next_index:
				continue
			indices[offset] = next_index
			changed = true

	if changed:
		state["indices"] = indices
	return changed


static func _smoothstep(t: float) -> float:
	var u := clampf(t, 0.0, 1.0)
	return u * u * (3.0 - 2.0 * u)
