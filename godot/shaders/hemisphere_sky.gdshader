shader_type sky;
render_mode use_quarter_res_pass;

// ADR 0043 (modern presentation): the hemisphere ambient source. The .env
// TOD palette's sky/ground colors (post-modulator, NVG-rewritten — the same
// values the retired fixed-function hemisphere lights carried) become the
// radiance the scene's ambient and reflected light sample. The visible
// background stays the ClearColor Environment's BG_COLOR (the witnessed frame
// clear) plus the authored sky dome; this sky exists only for
// ambient_light_source = SKY / reflected_light_source = SKY.
// source_color: the witnessed bytes set as Colors decode to the linear scene
// domain (ADR 0043 linear-scene amendment).
uniform vec3 u_hemi_sky : source_color = vec3(0.329412, 0.345098, 0.349020);
uniform vec3 u_hemi_ground : source_color = vec3(0.25, 0.23, 0.20);

void sky() {
	COLOR = mix(u_hemi_ground, u_hemi_sky,
			clamp(EYEDIR.y * 0.5 + 0.5, 0.0, 1.0));
}
