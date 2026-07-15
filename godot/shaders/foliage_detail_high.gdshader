shader_type spatial;
render_mode unshaded, cull_disabled, depth_draw_opaque;

// Near detail pass: alpha reference 180 with depth writes enabled
// [orig: Foliage_SetupFarSlotDraw @ 0x6008d4].
#include "res://shaders/foliage_detail.gdshaderinc"
