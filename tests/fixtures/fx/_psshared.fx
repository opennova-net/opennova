////////////////////////////////////////////////////////////////
//	_psshared.fx — shared ps_1_1 pixel shaders.
//
//	Include file for the OpenNova authored set. The register
//	protocol per shader (which map rides which stage, what the
//	vertex colors carry) is fixed by the vertex shaders that
//	feed them; _bx2 unpacks the 0..1 texture range back to
//	signed vectors for the per-pixel dots.
////////////////////////////////////////

// Bumped diffuse, directional + hemisphere:
//   t0 = normal map, t1 = cube-normalized light vector,
//   t2 = cube-normalized hemi vector, t3 = diffuse map,
//   v0 = lightcolor * falloff, c1 = ambient, c2 = sky - ambient.
shared PIXELSHADER ps11Diffuse =
asm
{
    ps.1.1
	tex t0
	tex t1
	tex t2
	tex t3

	dp3_sat	r0.rgb,	t0_bx2,	t1_bx2
	mul		r0.rgb,	r0,		v0

	dp3		r1.rgb,	t0_bx2,	t2_bx2
	mad		r1.rgb, r1,		c2,		c1

	add		r0.rgb,	r0,		r1
	mul_x2	r0.rgb,	r0,		t3

	+mov	r0.a,	t3.a
};

// Bumped diffuse, single light (the additive point-light pass).
shared PIXELSHADER ps11DiffuseSingle =
asm
{
    ps_1_1
	tex t0
	tex t1
	tex t2
	tex t3

	dp3_sat	r0.rgb,	t0_bx2,	t1_bx2
	mul		r0.rgb,	r0,		v0
	mul_x2	r0.rgb,	r0,		t3
	+mov	r0.a,	t3.a
};

// Bumped diffuse spotlight with per-pixel depth test:
//   t0 = normal map, t1 = depth gradient (A) + rearclip (RGB),
//   t2 = projected spot texture (depth in A), t3 = diffuse map,
//   v0 = light vector * selfshadow, c2 = lightcolor.
shared PIXELSHADER ps11DiffuseDepth =
asm
{
	ps_1_1

	def c0, 0.0f, 0.0f, 0.0f, 0.0f
	def c1, 0.5f, 0.5f, 0.5f, 0.5f

	tex t0
	tex t1
	tex t2
	tex t3

	dp3_sat	r0.rgb,	t0_bx2,	v0_bx2
	+sub	r0.a,	t2.a,	t1.a

	mul		r0.rgb,	r0,		t1
	+add	r0.a,	r0.a,	c1.a

	cnd		r1.rgb,	r0.a,	t2,		c0

	mul		r0.rgb,	r0,		r1
	mul		r0.rgb,	r0,		c2

	mul_x2	r0.rgb,	r0,		t3
	+mov	r0.a,	t3.a
};

// Bumped phong via cube-normalized half vector, specular exponent
// built by repeated squaring ((N.H)^8), brightness from t3.a:
//   t0 = normal map, t1 = light vector, t2 = half vector,
//   t3 = diffuse map (A = specular brightness),
//   v0 = lightcolor * falloff, v1 = hemisphere value, c5 = ambient.
shared PIXELSHADER ps11Phong =
asm
{
    ps_1_1
	tex t0
	tex t1
	tex t2
	tex t3

	dp3_sat	r1,		t0_bx2,	t2_bx2
	dp3_sat	r0.rgb,	t0_bx2,	t1_bx2
	+mul	r1.a,	r1.a,	r1.a

	mul		r0.rgb,	r0,		t3
	+mul	r1.a,	r1.a,	r1.a

	mul		r1.a,	r1.a,	r1.a
	mad		r0.rgb,	r1.a,	t3.a,	r0
	mul		r0.rgb,	r0,		v0

	mad_x2	r0.rgb,	t3,		v1,		r0

	+mul	r0.a,	v0,		t3
};

// Bumped phong through the 2D phong-map lookup (texm3x2):
//   t0 = normal map, t1/t2 = N.L / N.H rows for the lookup,
//   t3 = diffuse map (A = specular control + brightness),
//   v0 = lightcolor * falloff, c5 = ambient.
shared PIXELSHADER ps11Phong2 =
asm
{
	ps.1.1

	def c1, 1.0f, 0.0f, 0.0f, 0.0f
	def c2, 0.0f, 0.0f, 1.0f, 0.0f

	tex			t0
	texm3x2pad	t1, t0_bx2
	texm3x2tex	t2, t0_bx2
	tex			t3

	lrp			r0,		t3.a,	c2,		c1
	dp3_sat		r0,		r0,		t2

	mul			r0,		r0,		t3.a

	mad			r0,		t3,		t2.a,	r0
	mul			r0,		r0,		v0

	mad_x2		r0.rgb,	t3,		c5,		r0
	+mov		r0.a,	v0
};

// Spotlight diffuse-only with per-pixel depth test (phong effects'
// spot pass): register protocol as ps11DiffuseDepth, alpha from
// vertex * texture.
shared PIXELSHADER ps11PhongDepthDiffOnly =
asm
{
	ps_1_1

	def c0, 0.0f, 0.0f, 0.0f, 0.0f
	def c1, 0.5f, 0.5f, 0.5f, 0.5f

	tex t0
	tex t1
	tex t2
	tex t3

	dp3_sat	r0.rgb,	t0_bx2,	v0_bx2
	+sub	r0.a,	t2.a,	t1.a

	mul		r0.rgb,	r0,		t1
	+add	r0.a,	r0.a,	c1.a

	cnd		r1.rgb,	r0.a,	t2,		c0

	mul		r0.rgb,	r0,		r1
	+mul	r0.a,	r0.a,	r1.b

	mul		r0.rgb,	r0,		c2

	mul_x2	r0.rgb,	r0,		t3

	+mul	r0.a,	v0,		t3
};

// Spotlight specular-only with per-pixel depth test:
//   t0 = normal map (A = specular brightness), t1 = half vector,
//   t2 = projected spot (depth in A), t3 = depth gradient,
//   v0 = lightcolor * selfshadow. (N.H)^16 by repeated squaring.
shared PIXELSHADER ps11PhongDepthSpecOnly =
asm
{
	ps_1_1

	def c0, 0.0f, 0.0f, 0.0f, 0.0f
	def c1, 0.5f, 0.5f, 0.5f, 0.5f

	def c2, 1.0f, 1.0f, 1.0f, 1.0f

	tex t0
	tex t1
	tex t2
	tex t3

	dp3_sat	r1,		t0_bx2,	t1_bx2

	sub		r0.rgb,	t2.a,	t3.a
	+mul	r1.a,	r1.a,	r1.a

	mul		r1.rgb,	t2,		t3
	+add	r0.a,	r0.b,	c1.a

	cnd		r0.rgb,	r0.a,	r1,		c0
	+mul	r1.a,	r1.a,	r1.a

	mul		r0.rgb,	r0,		t0.a
	+mul	r1.a,	r1.a,	r1.a

	mul		r0.rgb,	r0,		v0
	+mul	r1.a,	r1.a,	r1.a

	mul_x2	r0.rgb,	r0,		r1.a
	+mul	r0.a,	v0,		t3
};

// Terrain-match: the projected terrain page lit by ambient + sun,
//   t0 = projected terrain texture, c0 = ambient, c1 = sun color.
shared PIXELSHADER ps11TrnMatch =
asm
{
    ps_1_1
	tex		t0
	mad_d2	r0.rgb, t0.a, c1, c0
	mul_x4	r0.rgb, r0, t0
	+mov	r0.a, t0.a
};
