// The SIGHTS card's MULTIPLY and MULTIPLY_AT rows (hud_sights_card.gd): the texel doubled under the
// multiply blend, alpha-tested at 128 on the _AT rows. A resource, so its program is made as the
// card's script loads, not when a card first builds its rows, which past a mission load is the
// reveal frame (D-NET-1367).
shader_type canvas_item;
render_mode blend_mul;
uniform bool alpha_test = false;

void fragment() {
	vec4 texel = texture(TEXTURE, UV);
	if (alpha_test && texel.a <= 128.0 / 255.0) {
		discard;
	}
	COLOR = vec4(texel.rgb * 2.0, texel.a);
}
