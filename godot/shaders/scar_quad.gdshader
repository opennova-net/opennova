shader_type spatial;
// The impact-scar quad: the one-texture mode the retail drawer applies per
// texture strip — unshaded, the vertex colour (Env_TerrainLightCombined) as the
// modulate, the strip's own TGA with clamp wrap, alpha-tested at ref 128, fog
// on, blend mode 0 (retail: Scar_DrawBatches @0x5ccd10 under
// Scar_RenderAllCaches @0x5CDF70, see docs/world/world-wac-ai-re.md §24.9).
// Both faces draw: retail submits the six vertices without a cull change.
render_mode unshaded, cull_disabled, depth_draw_never;

uniform sampler2D albedo_tex : source_color, repeat_disable, filter_linear_mipmap;

// The presenter's ONE device fold: retail writes no depth bias — the scar quad
// is coplanar with the struck face and the D3D rasteriser's own bias keeps it
// on top. Godot has no polygon offset, so the quad is pulled 0.2% toward the
// eye in view space (convention-independent: works under forward or reverse
// Z) — a displacement of 2 mm at 1 m, 20 cm at 100 m, well under the fog cull.
const float SCAR_VIEW_PULL = 0.998;

void vertex() {
	vec4 view_pos = MODELVIEW_MATRIX * vec4(VERTEX, 1.0);
	view_pos.xyz *= SCAR_VIEW_PULL;
	POSITION = PROJECTION_MATRIX * view_pos;
}

void fragment() {
	vec4 tex = texture(albedo_tex, UV);
	ALBEDO = tex.rgb * COLOR.rgb;
	ALPHA = tex.a;
	// Alpha test ref 128 (D3DRS_ALPHAREF) -> the scissor threshold.
	ALPHA_SCISSOR_THRESHOLD = 0.5;
}
