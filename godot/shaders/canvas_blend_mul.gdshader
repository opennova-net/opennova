// The multiply blend a canvas item draws through (PlayerViewEffects' medic revive tint): the
// default texel x colour under a multiply blend, the program a CanvasItemMaterial of
// BLEND_MODE_MUL generates. A resource, so its program is made where the script that preloads
// it loads (the boot), not at the first idle frame a CanvasItemMaterial would wait for, which
// past a mission load is the reveal frame (D-NET-1367).
shader_type canvas_item;
render_mode blend_mul;
