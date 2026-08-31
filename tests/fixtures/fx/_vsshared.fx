////////////////////////////////////////////////////////////////
//	_vsshared.fx — shared vertex shaders for RIGID meshes.
//
//	Include file for the OpenNova authored set: the flat gouraud
//	base (hemi + directional + N point lights), the spotlight
//	depth pass, the post-multiply/black passes, and the
//	depth-mask write. Rigid counterparts of _vsskshared.fx.
////////////////////////////////////////////////////////////////

// Base gouraud shade: hemisphere + directional + numlights point lights,
// all folded into COLOR0. Used when spotlights force the multi-pass path.
VS_OUTPUT vsFlatBaseHemi (const VS_INPUT_SEG1 In, uniform int numlights, uniform bool clip)
{
	VS_OUTPUT	Out = (VS_OUTPUT)0;

	Out.Pos  = mul (In.Pos, MatWorldViewProj);

	float3	norm = mul (In.Norm, MatWorldInvTrans);
	float3	vertworld = mul (In.Pos, MatWorld);

	float3	accumlight = (float3)HemicolorFromVectorY (norm.y) + DirLightColor.xyz * max(0, dot(norm, DirLightVector));
	for (int light=0; light<numlights; light++)
	{
		float3	lightvec	= PointLightCoordArray[light] - vertworld;
		float	distsqrd	= dot (lightvec, lightvec);
		float	recip		= rsqrt (distsqrd);

		float	atten = PointLightAttenArray[light].x +
						PointLightAttenArray[light].y * distsqrd * recip +
						PointLightAttenArray[light].z * distsqrd;
		accumlight += PointLightColorArray[light] * max(0, dot(normalize (lightvec), norm)) * (1.0 / atten);
	}

	Out.Diff = float4(accumlight,1);
	Out.Spec = 0;
	Out.Tex0 = In.Tex0;
	if (clip)
		Out.Tex1 = dot(In.Pos, VecTexClipPlane);
	else
		Out.Tex1 = In.Tex0;

	Out.Fog = CalcFogSegmented (In.Pos);

	return Out;
}

// Spotlight beam pass: vertex N.L in COLOR0, depth-gradient coordinate in
// TEXCOORD0, projected spot texture in TEXCOORD1.
VS_OUTPUT_PROJ2 vsFlatSpotDepth (const VS_INPUT_SEGTAN In, uniform bool clip)
{
	VS_OUTPUT_PROJ2	Out = (VS_OUTPUT_PROJ2)0;

	Out.Pos  = mul (In.Pos, MatWorldViewProj);

	float3	vertworld	= mul (In.Pos, MatWorld);
	float3	lightvec	= PointLightCoord - vertworld;
	float	reciplength;
	float	atten		= CalcPointLightAttenuationAndLength (lightvec, reciplength);
	float3	L			= lightvec * reciplength;
	float3	N			= mul (In.Norm, (float3x3)MatWorld);
	float	NdotL		= dot(N, L);

	// distance attenuation rides the depth-mask texture, not the vertex color
	Out.Diff = PointLightColor * clamp (NdotL, 0, 1);

	Out.Tex0 = dot (float4(vertworld,1), VecDepthMaskPlane) - DEPTHTEST_8BIT_OFFSET;
	Out.Tex1 = mul (float4(vertworld,1), SpotLightProjMatrix) * float4(1,-1,1,1);

	return Out;
}

// Post passes: modulate the lit framebuffer by the diffuse texture(s)
// (0.5 vertex color + Modulate2x = identity on the texture), or lay the
// silhouette down black for the projected-shadow render.
VS_OUTPUT vsPostMultiplyT1 (const VS_INPUT_SEG1 In, uniform bool clip, uniform bool uvxform)
{
	VS_OUTPUT	Out = (VS_OUTPUT)0;

	Out.Pos  = mul (In.Pos, MatWorldViewProj);
	Out.Diff = float4(0.5, 0.5, 0.5, 1);
	Out.Spec = 0;
	Out.Tex0 = CalcAnimatedUV(In.Tex0, uvxform);
	if (clip)
		Out.Tex1 = dot(In.Pos, VecTexClipPlane);
	else
		Out.Tex1 = CalcAnimatedUV(In.Tex0, uvxform);

	Out.Fog = CalcFogSegmented (In.Pos);

	return Out;
}

VS_OUTPUT vsPostMultiplyT2 (const VS_INPUT_SEG2 In, uniform bool clip, uniform bool uvxform)
{
	VS_OUTPUT	Out = (VS_OUTPUT)0;

	Out.Pos  = mul (In.Pos, MatWorldViewProj);
	Out.Diff = float4(0.5, 0.5, 0.5, 1);
	Out.Spec = 0;
	Out.Tex0 = CalcAnimatedUV(In.Tex0, uvxform);
	Out.Tex1 = In.Tex1;

	Out.Fog = CalcFogSegmented (In.Pos);

	return Out;
}

VS_OUTPUT vsPostBlackT1 (const VS_INPUT_SEG1 In, uniform bool clip, uniform bool uvxform)
{
	VS_OUTPUT	Out = (VS_OUTPUT)0;

	Out.Pos  = mul (In.Pos, MatWorldViewProj);
	Out.Diff = float4(0,0,0,1);
	Out.Spec = 0;
	Out.Tex0 = CalcAnimatedUV(In.Tex0, uvxform);
	if (clip)
		Out.Tex1 = dot(In.Pos, VecTexClipPlane);
	else
		Out.Tex1 = CalcAnimatedUV(In.Tex0, uvxform);

	Out.Fog = CalcFogSegmented (In.Pos);

	return Out;
}

// Depth-mask write: project the vertex onto the mask plane ramp.
VS_OUTPUTDEP vsDepth (const VS_INPUT_SEG1 In)
{
	VS_OUTPUTDEP	Out = (VS_OUTPUTDEP)0;

	Out.Pos  = mul (In.Pos, MatWorldViewProj);

	float3	vertworld = mul (In.Pos, MatWorld);

	Out.Tex0 = dot (float4(vertworld,1), VecDepthMaskPlane);

	return Out;
}

shared VERTEXSHADER		vscFlatSpotDepth		= compile vs_1_1 vsFlatSpotDepth (false);

shared VERTEXSHADER		vscFlatBaseHemiArray[4] =
{
	compile vs_1_1 vsFlatBaseHemi(0, false),
	compile vs_1_1 vsFlatBaseHemi(1, false),
	compile vs_1_1 vsFlatBaseHemi(2, false),
	compile vs_1_1 vsFlatBaseHemi(3, false)
};

shared VERTEXSHADER		vscPostMultiplyT1		= compile vs_1_1 vsPostMultiplyT1 (false, false);
shared VERTEXSHADER		vscPostMultiplyT2		= compile vs_1_1 vsPostMultiplyT2 (false, false);
shared VERTEXSHADER		vscPostBlackT1			= compile vs_1_1 vsPostBlackT1 (false, false);

shared VERTEXSHADER		vscPostMultiplyT1UV		= compile vs_1_1 vsPostMultiplyT1 (false, true);
shared VERTEXSHADER		vscPostMultiplyT2UV		= compile vs_1_1 vsPostMultiplyT2 (false, true);
shared VERTEXSHADER		vscPostBlackT1UV		= compile vs_1_1 vsPostBlackT1 (false, true);

shared VERTEXSHADER		vscDepth				= compile vs_1_1 vsDepth ();
