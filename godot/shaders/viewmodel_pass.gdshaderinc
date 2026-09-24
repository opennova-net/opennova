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

// The viewmodel's pass fog: retail fogs the gun under the DRY pass, applied
// before the sky dome and the gun; the eye's own pass (underwater included)
// is re-applied only after the gun (retail Render_ProcessMainSceneFrame
// @0x5ca3bf..0x5ca3ce, the gun @0x5ca829, the re-apply @0x5ca82e..0x5ca841;
// engine EnvironmentState::build_viewmodel_fog). range = start, end, type.
global uniform vec3 opennova_viewmodel_fog_color;
global uniform vec3 opennova_viewmodel_fog_range;

// D3D viewport MinZ 0 / MaxZ 0.1 on a [0, 1] depth range. Godot's projection
// is reversed-Z on [0, 1] (near -> 1), so the band is NDC [0.9, 1.0].
const float NOVA_VIEWMODEL_DEPTH_WINDOW = 0.1;

vec4 viewmodel_clip(vec4 view_pos, mat4 projection) {
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

#ifdef OBJ_BLENDED_PASS
// Retail draws the viewmodel after the sky pass and before every world draw
// (retail Render_ProcessMainSceneFrame: the dome @0x5ca81a, the gun @0x5ca829),
// and its blended strips write no depth (the FF blend passes force NOWRITE,
// retail @0x5afc8d..0x5afcaa; ZWRITEENABLE follows the pass flags @0x5da318),
// so every later world draw that passes LESSEQUAL paints over them: a blended
// gun fragment survives only over the gun's own depth band or where nothing
// of the world lies behind it. Godot has already drawn the world's opaques
// when the gun's alpha draws, so the fragment is dropped wherever the opaque
// depth of the view is a world depth (neither the cleared far plane nor the
// gun band).
uniform sampler2D obj_opaque_depth : hint_depth_texture, filter_nearest;

bool viewmodel_world_covers(vec2 screen_uv) {
	float depth = textureLod(obj_opaque_depth, screen_uv, 0.0).r;
	return depth > 0.0 && depth < 1.0 - NOVA_VIEWMODEL_DEPTH_WINDOW;
}
#endif

// The one clip-position writer every vertex stage uses. Godot enables its
// POSITION override for the WHOLE shader as soon as POSITION is written
// anywhere, so a path that does not write it leaves the clip position zero
// and the object vanishes: the plain projection is written explicitly here.
vec4 viewmodel_position(bool viewmodel_pass, vec4 view_pos,
		mat4 projection) {
	return viewmodel_pass ? viewmodel_clip(view_pos, projection)
			: projection * view_pos;
}
