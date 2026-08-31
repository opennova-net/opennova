shader_type canvas_item;
render_mode unshaded;

#include "res://shaders/color.gdshaderinc"

// The NVG post follows the retail luma/tint/glow constants documented
// with the first-person NVG path in docs/net/novaworld-net-re.md section 5.40.
// Its input frame has already received the witnessed NVG hemisphere rewrite
// [orig: CTerrainRenderer_BuildLightingShaderConstants @ 0x5c8090;
// docs/render/render-lighting-re.md]. The missing four-frame history is an
// explicit bounded residual, not an uncited approximation hidden in this pass.

uniform sampler2D screen_texture : hint_screen_texture, filter_linear_mipmap;

void fragment() {
	vec3 scene = srgb_encode(
			textureLod(screen_texture, SCREEN_UV, 0.0).rgb);

	// Retail first converts each captured frame to 4x luma, then averages four
	// temporal frames. CanvasItem has no retained four-frame history here, so all
	// four samples intentionally collapse to the current frame. The grayscale,
	// tint, glow threshold, and glow opacity retain the retail shader constants.
	float gray = dot(scene, vec3(0.30, 0.60, 0.10));
	vec3 intensified = vec3(gray * 4.0);
	vec3 temporal = (intensified + intensified + intensified + intensified) * 0.25;
	float grayish = dot(temporal, vec3(0.50, 1.00, 0.50));
	vec3 tinted = grayish * vec3(0.20, 0.90, 0.20) + vec3(0.00, 0.20, 0.00);

	vec3 history_sum = temporal * temporal * 4.0;
	float lum = dot(history_sum, vec3(0.30, 0.60, 0.10));
	float bright = clamp(lum - 0.15, 0.0, 1.0);
	vec3 glow = bright * vec3(0.20, 0.60, 0.20);
	COLOR = vec4(srgb_decode(tinted + glow * 0.16), 1.0);
}
