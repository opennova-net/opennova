#include "res://shaders/texture_filter.gdshaderinc"

// Retail's generated :fd texture stops at a 4x4 terminal mip. Godot Image
// payloads require the 2x2 and 1x1 tail, which the terminal clamp keeps out.
// The :fd texture is loaded with flags 0x100000 and its chain rebuilt in
// place, so it carries no 0x8: the device samples it MIN/MAG LINEAR with
// MIPFILTER POINT whatever game.cfg's texfilter_level says
// [orig: Foliage_LoadDefAssets @ 0x601634..0x601650, @ 0x6019cd..0x6019e4;
// CGfxDevice_ApplyRenderStates @ 0x67e463..0x67e4a7] (engine
// renderer/texture_filter.h, TextureStage::FoliageMask): bilinear on the
// nearest level of the isotropic footprint.
vec4 sample_retail_foliage_fd(sampler2D source, vec2 uv) {
	vec2 dimensions = vec2(textureSize(source, 0));
	float terminal_lod = max(
		floor(log2(max(min(dimensions.x, dimensions.y), 4.0))) - 2.0,
		0.0);
	return sample_point_mip(source, uv, terminal_lod);
}
