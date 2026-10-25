// The additive blend a canvas item draws through (the HUD overlay's additive rows and map
// passes, the SIGHTS card's additive rows): the default texel x colour under an additive blend,
// the program a CanvasItemMaterial of BLEND_MODE_ADD generates. A resource, so its program is
// made where it is first loaded (the HUD's load leg, or the boot), not at the first idle frame a
// CanvasItemMaterial would wait for, which past a mission load is the reveal frame
// (D-NET-NEW-LF-28).
shader_type canvas_item;
render_mode blend_add;
