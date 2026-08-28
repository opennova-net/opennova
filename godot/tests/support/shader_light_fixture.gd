class_name ShaderLightFixture
extends RefCounted

## The retail light-byte packing the terrain and foliage shader contract
## tests both derive their expected texture-basis light from: a signed
## direction quantized the way PolyTrn packs it into D3DCOLOR diffuse RGB.


static func quantize_retail_signed_vector(value: Vector3) -> Vector3:
	return Vector3(
		floorf(clampf((value.x + 1.0) * 127.5, 0.0, 255.0)),
		floorf(clampf((value.y + 1.0) * 127.5, 0.0, 255.0)),
		floorf(clampf((value.z + 1.0) * 127.5, 0.0, 255.0))
	) / 255.0


static func gpu_light_byte(retail_getter_direction: Vector3) -> Vector3:
	# PolyTrn packs the getter tuple into D3DCOLOR diffuse RGB as (z, x, y).
	var gpu_diffuse_rgb := Vector3(
		retail_getter_direction.z,
		retail_getter_direction.x,
		retail_getter_direction.y
	)
	return quantize_retail_signed_vector(gpu_diffuse_rgb)
