shader_type spatial;
render_mode unshaded, cull_disabled, blend_mix, depth_draw_never;

// LOW detail pass: alpha reference 8, no depth writes, blended
// SRCALPHA/INVSRCALPHA. The near secondary draw additionally sets
// u_high_pass_cutoff to the HIGH reference: retail runs it under strict
// D3DCMP_LESS so it only lands where the HIGH pass left no depth, and the
// cutoff discard reproduces that texel selection.
// [orig: Foliage_SetupFarSlotDraw @ 0x6008fc..0x600912;
// Foliage_LoadDefAssets technique block @ 0x60141f..0x601427]
#include "res://shaders/foliage_detail.gdshaderinc"
