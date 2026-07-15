// Retail's generated :fd texture stops at a 4x4 terminal mip. Godot Image
// payloads require the 2x2 and 1x1 tail. The reference configuration uses
// bilinear filtering with point-selected mip levels (texfilter_level 0), so
// select the nearest retail level explicitly instead of trilinearly blending
// the alpha-test mask, and clamp the host-only tail out of the selection.
vec4 sample_retail_foliage_fd(sampler2D source, vec2 uv) {
	vec2 dimensions = vec2(textureSize(source, 0));
	vec2 dx = dFdx(uv);
	vec2 dy = dFdy(uv);
	float footprint = max(length(dx * dimensions), length(dy * dimensions));
	float requested_lod = max(log2(max(footprint, 1.0)), 0.0);
	float terminal_lod = max(
		floor(log2(max(min(dimensions.x, dimensions.y), 4.0))) - 2.0,
		0.0);
	float selected_lod = clamp(
		floor(requested_lod + 0.5), 0.0, terminal_lod);
	return textureLod(source, uv, selected_lod);
}
