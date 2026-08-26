shader_type canvas_item;

#include "res://shaders/nova_color.gdshaderinc"

// The dedicated FP SubViewport is a retail offscreen device and therefore
// retains gamma-framebuffer values. It composites after the main terminal
// transfer, so decode that texture once into Canvas/display-linear space.
void fragment() {
	vec4 viewmodel = texture(TEXTURE, UV);
	COLOR = vec4(nova_display_decode_gamma(viewmodel.rgb), viewmodel.a);
}
