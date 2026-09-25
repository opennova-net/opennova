// The distant MODEL foliage depth masks over a person entity. Retail draws a
// crouched/prone person's masks immediately inside its BySide wave, after
// the wave's MATCHTERRAIN pre-pass and before the wave's queued NORMAL
// draws, so every person of that wave (and of the camera-side wave after
// it, whose own pre-pass follows the far masks) depth-tests against them;
// FoliageMaskCompositorEffect rasterizes the same masks before Godot's
// opaque pass: R = the nearest far-wave mask depth, G = the nearest
// camera-wave one. u_foliage_mask_side is the person's wave (0 = not a
// person draw, 1 = far, 2 = camera side); the eye match keeps the mirror and
// every other camera unmasked. Every pass of a person strip reads it: the
// opaque and blended NORMAL outputs and the post-multiply (P3) layer, which
// retail draws inside the same wave flush after the masks. Retail
// Terrain_RenderWorldScene @ 0x5c953e..0x5c9567 (far wave:
// MATCHTERRAIN subpass, then the wave with its masks), @ 0x5c9600..0x5c9647
// (camera wave); Terrain_RenderSectorEntitiesBySide @ 0x5c7ddf..0x5c7f11
// (masks) before render_sector_entity @ 0x5c7ffc.
global uniform sampler2D opennova_foliage_mask_depth;
global uniform bool opennova_foliage_mask_active;
global uniform vec3 opennova_foliage_mask_eye;
instance uniform float u_foliage_mask_side = 0.0;

// 0: no mask in front; 1: a mask of the fragment's own wave is in front, so
// only the pre-pass drawn before it remains; 2: a far-wave mask is in front
// of a camera-wave fragment, which hides its pre-pass too. Reverse-Z: a
// nearer mask stores the larger depth; the D3D LESSEQUAL test passes ties.
int obj_foliage_mask_outcome(vec4 frag_coord, vec3 camera_position_world) {
	if (u_foliage_mask_side < 0.5 || !opennova_foliage_mask_active ||
			distance(camera_position_world, opennova_foliage_mask_eye) >= 0.01) {
		return 0;
	}
	ivec2 size = textureSize(opennova_foliage_mask_depth, 0);
	ivec2 texel = clamp(ivec2(frag_coord.xy), ivec2(0), size - ivec2(1));
	vec2 masks = texelFetch(opennova_foliage_mask_depth, texel, 0).rg;
	if (u_foliage_mask_side < 1.5) {
		return masks.r > frag_coord.z ? 1 : 0;
	}
	if (masks.r > frag_coord.z) {
		return 2;
	}
	return masks.g > frag_coord.z ? 1 : 0;
}
