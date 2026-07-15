shader_type spatial;
render_mode unshaded, cull_disabled, depth_draw_never;

// LOW detail pass: alpha reference 8 with depth writes disabled. Retail's
// near secondary draw selects strict D3DCMP_LESS while primary LOW uses
// D3DCMP_LESSEQUAL; Godot's spatial shader API has no compare-equality toggle.
// [orig: Foliage_SetupFarSlotDraw @ 0x6008fc..0x600912;
// SetRenderState wrapper @ 0x67cac0..0x67caea].
#include "res://shaders/foliage_detail.gdshaderinc"
