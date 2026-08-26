// The first-person viewmodel inside the beauty pass. Retail draws the gun and
// arms FIRST into the same backbuffer through the weapon's own `renderfov`
// projection with the near plane swapped to 0.05 and the D3D viewport depth
// range clamped to [0, 0.1], so the world drawn afterwards can never overlap
// the gun, and the murk/bloom/HUD stages then cover scene and weapon alike
// (retail: Player_RenderFirstPersonViewModel @0x4ded60,
// Render_SwapProjectionNearZ @0x58a8f0, Render_SetViewportDepth01 @0x58a7b0,
// see docs/render/render-order-re.md). Godot has no per-instance projection
// or depth range, so a flagged instance rewrites its clip position here:
// the beauty projection with its focal length scaled to the renderfov, and
// the clip depth remapped into the nearest tenth of the reversed-Z range.
//
// x = focal ratio k = cot(fov_fp_v/2) / cot(fov_beauty_v/2) (both share the
// viewport aspect, so one scalar covers x and y), y = the FP near plane,
// z = the camera far plane; w unused.
global uniform vec4 opennova_viewmodel_projection;
instance uniform bool u_viewmodel_pass = false;

// D3D viewport MinZ 0 / MaxZ 0.1 on a [0, 1] depth range. Godot's projection
// is reversed-Z on [0, 1] (near -> 1), so the band is NDC [0.9, 1.0].
const float NOVA_VIEWMODEL_DEPTH_WINDOW = 0.1;

vec4 nova_viewmodel_clip(vec4 view_pos, mat4 projection) {
	vec4 clip = projection * view_pos;
	float n = opennova_viewmodel_projection.y;
	float f = opennova_viewmodel_projection.z;
	// The reversed-Z, [0, 1]-remapped clip depth of a perspective projection
	// with the near plane swapped to n: n * (z_view + f) / (f - n), which is
	// 1.0 * w at z_view = -n and 0.0 at z_view = -f.
	float z_fp = n * (view_pos.z + f) / (f - n);
	clip.xy *= opennova_viewmodel_projection.x;
	clip.z = (1.0 - NOVA_VIEWMODEL_DEPTH_WINDOW) * clip.w
			+ NOVA_VIEWMODEL_DEPTH_WINDOW * z_fp;
	return clip;
}

// The one clip-position writer every vertex stage uses. Godot enables its
// POSITION override for the WHOLE shader as soon as POSITION is written
// anywhere, so a path that does not write it leaves the clip position zero
// and the object vanishes: the plain projection is written explicitly here.
vec4 nova_viewmodel_position(bool viewmodel_pass, vec4 view_pos,
		mat4 projection) {
	return viewmodel_pass ? nova_viewmodel_clip(view_pos, projection)
			: projection * view_pos;
}
