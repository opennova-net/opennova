// The retail color pipeline is GAMMA-SPACE end to end: texture bytes enter
// the TSS/shader math raw, every combine (MODULATE2X, lighting saturate, fog
// mix, blending) runs on those gamma-encoded values, and the framebuffer byte
// IS the displayed value. Witnessed exhaustively: no D3DSAMP_SRGBTEXTURE(15)
// at any device sampler-state site (the full sweep saw only ADDRESSU/V/W,
// MAG/MIN/MIPFILTER, MIPMAPLODBIAS, MAXMIPLEVEL, MAXANISOTROPY
// [orig: CD3DDevice_InitializeDisplay @ 0x679c1b..0x679d29;
//  CGfxDevice_ApplyRenderStates @ 0x67e3ec..0x67e4f3]), no
// D3DRS_SRGBWRITEENABLE(194) at any SetRenderState site, no sRGB pass state
// in the 44-file shipped .fx corpus, and the display ramp is the identity at
// the default gamma 1.0 [orig: GLib_SetGammaRamp @ 0x677be0 - ramp[i] =
// (i/255)^gamma; default 1.0f @ 0x84f354].
//
// The host renders linear HDR with an sRGB-encoding blit, so the witnessed-FF
// shaders sample textures RAW (no source_color hint), run the witnessed math
// in gamma space, and write ALBEDO through this exact inverse of the blit's
// transfer curve - the displayed byte then equals the computed gamma-space
// byte (divergence D-RMAT-7, docs/render/render-material-re.md). Framebuffer
// BLENDING still happens on the encoded values - that bounded residual is
// D-RMAT-8. The object-model composer embeds this same function body
// (libs/renderer/src/object_shader_template.cpp - keep them identical).
vec3 nova_gamma_to_linear(vec3 gamma_rgb) {
	vec3 c = clamp(gamma_rgb, vec3(0.0), vec3(1.0));
	return mix(pow((c + vec3(0.055)) * (1.0 / 1.055), vec3(2.4)),
			c * (1.0 / 12.92), lessThan(c, vec3(0.04045)));
}
