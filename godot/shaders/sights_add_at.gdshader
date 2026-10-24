// The SIGHTS card's ADD_AT rows (hud_sights_card.gd): the texel under the additive blend,
// alpha-tested at 128. A resource, so its program is made as the card's script loads, not when a
// card first builds its rows, which past a mission load is the reveal frame (D-NET-NEW-LF-28).
shader_type canvas_item;
render_mode blend_add;

void fragment() {
	vec4 texel = texture(TEXTURE, UV);
	if (texel.a <= 128.0 / 255.0) {
		discard;
	}
	COLOR = texel;
}
