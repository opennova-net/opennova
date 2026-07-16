// Retail's generated :fd texture stops at a 4x4 terminal mip. Godot Image
// payloads require the 2x2 and 1x1 tail. The device-global texfilter mode
// applies to every stage [orig: CGfxDevice_ApplyRenderStates per-stage loop
// @ 0x67e3b5..0x67e50e], and the reference machine runs the anisotropic
// mode, so the foliage :fd mask samples anisotropically like the terrain
// inputs; only the synthetic Godot tail is clamped out of the selection.
vec4 sample_retail_foliage_fd(sampler2D source, vec2 uv) {
	vec2 dimensions = vec2(textureSize(source, 0));
	vec2 dx = dFdx(uv);
	vec2 dy = dFdy(uv);
	float footprint = max(length(dx * dimensions), length(dy * dimensions));
	float requested_lod = max(log2(max(footprint, 1.0)), 0.0);
	float terminal_lod = max(
		floor(log2(max(min(dimensions.x, dimensions.y), 4.0))) - 2.0,
		0.0);
	float gradient_scale = exp2(min(terminal_lod - requested_lod, 0.0));
	return textureGrad(source, uv, dx * gradient_scale, dy * gradient_scale);
}
