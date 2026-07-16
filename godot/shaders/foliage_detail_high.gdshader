shader_type spatial;
render_mode unshaded, cull_disabled, blend_mix, depth_draw_always;

// Near detail pass: alpha reference 180 with depth writes enabled, blended
// SRCALPHA/INVSRCALPHA like every retail detail draw. Alpha-test survivors
// write depth; discarded texels leave none.
// [orig: Foliage_SetupFarSlotDraw @ 0x6008d4;
// Foliage_LoadDefAssets technique block @ 0x60141f..0x601427]
#include "res://shaders/foliage_detail.gdshaderinc"
