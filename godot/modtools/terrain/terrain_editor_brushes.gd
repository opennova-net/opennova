extends RefCounted


static func apply_raise_lower(image: Image, cx: int, cz: int, radius: int, amount: float, hardness: float, clip_rect: Rect2i) -> void:
	_for_each_brush_pixel(image, cx, cz, radius, hardness, clip_rect, func(x: int, z: int, falloff: float) -> void:
		var weighted_falloff := falloff * falloff
		var height := image.get_pixel(x, z).r
		height = clampf(height + amount * weighted_falloff, 0.0, 255.996)
		image.set_pixel(x, z, Color(height, 0, 0, 1))
	)


static func apply_smooth(image: Image, cx: int, cz: int, radius: int, strength: float, hardness: float, clip_rect: Rect2i) -> void:
	var originals := {}
	_for_each_brush_pixel(image, cx, cz, radius, hardness, clip_rect, func(x: int, z: int, _falloff: float) -> void:
		originals[Vector2i(x, z)] = image.get_pixel(x, z).r
	)
	_for_each_brush_pixel(image, cx, cz, radius, hardness, clip_rect, func(x: int, z: int, falloff: float) -> void:
		var sum := 0.0
		var count := 0
		for nz in range(maxi(z - 1, 0), mini(z + 2, image.get_height())):
			for nx in range(maxi(x - 1, 0), mini(x + 2, image.get_width())):
				var key := Vector2i(nx, nz)
				sum += originals.get(key, image.get_pixel(nx, nz).r)
				count += 1
		var height: float = originals[Vector2i(x, z)]
		var t := clampf(strength * falloff, 0.0, 1.0)
		image.set_pixel(x, z, Color(lerpf(height, sum / float(count), t), 0, 0, 1))
	)


static func apply_flatten(image: Image, cx: int, cz: int, radius: int, target_height: float, strength: float, hardness: float, clip_rect: Rect2i) -> void:
	_for_each_brush_pixel(image, cx, cz, radius, hardness, clip_rect, func(x: int, z: int, falloff: float) -> void:
		var height := image.get_pixel(x, z).r
		var t := clampf(strength * falloff, 0.0, 1.0)
		image.set_pixel(x, z, Color(lerpf(height, target_height, t), 0, 0, 1))
	)


static func apply_blend_paint(image: Image, channel: int, cx: int, cz: int, radius: int, strength: float, hardness: float, clip_rect: Rect2i) -> void:
	_for_each_brush_pixel(image, cx, cz, radius, hardness, clip_rect, func(x: int, z: int, falloff: float) -> void:
		var weighted_falloff := falloff * falloff
		var current := image.get_pixel(x, z)
		var red := current.r
		var green := current.g
		var blue := current.b
		var amount := strength * weighted_falloff
		if channel == 0:
			red += amount
		elif channel == 1:
			green += amount
		else:
			blue += amount
		var total := red + green + blue
		if total > 0.001:
			red /= total
			green /= total
			blue /= total
		else:
			red = 1.0 if channel == 0 else 0.0
			green = 1.0 if channel == 1 else 0.0
			blue = 1.0 if channel == 2 else 0.0
		image.set_pixel(x, z, Color(red, green, blue, 1.0))
	)


static func apply_colormap_paint(image: Image, color: Color, cx: int, cz: int, radius: int, strength: float, hardness: float, clip_rect: Rect2i) -> void:
	_for_each_brush_pixel(image, cx, cz, radius, hardness, clip_rect, func(x: int, z: int, falloff: float) -> void:
		var target := color
		var current := image.get_pixel(x, z)
		var t := clampf(strength * falloff * falloff, 0.0, 1.0)
		image.set_pixel(x, z, current.lerp(target, t))
	)


static func apply_colormap_clone(dest: Image, source: Image, src_cx: int, src_cy: int, dst_cx: int, dst_cy: int, radius: int, strength: float, hardness: float, clip_rect: Rect2i) -> void:
	var sw := source.get_width()
	var sh := source.get_height()
	_for_each_brush_pixel(dest, dst_cx, dst_cy, radius, hardness, clip_rect, func(x: int, z: int, falloff: float) -> void:
		var sx := clampi(x - dst_cx + src_cx, 0, sw - 1)
		var sy := clampi(z - dst_cy + src_cy, 0, sh - 1)
		var src_color := source.get_pixel(sx, sy)
		var current := dest.get_pixel(x, z)
		var t := clampf(strength * falloff * falloff, 0.0, 1.0)
		dest.set_pixel(x, z, current.lerp(src_color, t))
	)


static func sample_colormap(image: Image, world_x: float, world_z: float) -> Color:
	var sample_x := clampi(int(world_x), 0, image.get_width() - 1)
	var sample_z := clampi(int(world_z), 0, image.get_height() - 1)
	return image.get_pixel(sample_x, sample_z)


static func sample_flatten_target(image: Image, world_x: float, world_z: float) -> float:
	var sample_x := clampi(int(world_x), 0, image.get_width() - 1)
	var sample_z := clampi(int(world_z), 0, image.get_height() - 1)
	return image.get_pixel(sample_x, sample_z).r


static func _for_each_brush_pixel(image: Image, cx: int, cz: int, radius: int, hardness: float, clip_rect: Rect2i, callback: Callable) -> void:
	var width := image.get_width()
	var height := image.get_height()

	var x_min := maxi(cx - radius, 0)
	var x_max := mini(cx + radius + 1, width)
	var z_min := maxi(cz - radius, 0)
	var z_max := mini(cz + radius + 1, height)

	if clip_rect.size.x > 0 and clip_rect.size.y > 0:
		x_min = maxi(x_min, clip_rect.position.x)
		z_min = maxi(z_min, clip_rect.position.y)
		x_max = mini(x_max, clip_rect.position.x + clip_rect.size.x)
		z_max = mini(z_max, clip_rect.position.y + clip_rect.size.y)

	if x_max <= x_min or z_max <= z_min:
		return

	var radius_f := float(radius)
	if radius_f <= 0.0:
		return
	var radius_sq := radius_f * radius_f

	# Core fraction (0..1): portion of the radius that is fully opaque.
	var core := clampf(hardness, 0.0, 1.0)
	var shoulder := 1.0 - core  # width of the soft ramp, relative to radius

	for z in range(z_min, z_max):
		for x in range(x_min, x_max):
			var dx := float(x - cx)
			var dz := float(z - cz)
			var distance_sq := dx * dx + dz * dz
			if distance_sq > radius_sq:
				continue
			var t := 1.0 - sqrt(distance_sq) / radius_f  # 0 at edge, 1 at center
			var falloff: float
			if shoulder <= 0.0001:
				falloff = 1.0
			elif t >= shoulder:
				falloff = 1.0
			else:
				var u := t / shoulder  # 0..1 inside the soft ramp
				falloff = u * u * (3.0 - 2.0 * u)  # smoothstep
			callback.call(x, z, falloff)
