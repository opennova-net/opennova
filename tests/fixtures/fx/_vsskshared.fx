////////////////////////////////////////////////////////////////
//	_vsskshared.fx — shared vertex shaders for SKINNED meshes.
//
//	Include file for the OpenNova authored set: the spotlight
//	depth pass, post-multiply/black passes, depth-mask write,
//	terrain-match projection, and the object-space DOT3
//	spotlight pass shared by the object-space effects.
////////////////////////////////////////////////////////////////

// Spotlight beam pass, skinned: vertex N.L in COLOR0, depth-gradient
// coordinate in TEXCOORD0, projected spot texture in TEXCOORD1.
VS_OUTPUT_PROJ2 vsSkinFlatSpotDepth (VS_INPUT_SKIN1 In, uniform int NumBones)
{
	VS_OUTPUT_PROJ2	Out = (VS_OUTPUT_PROJ2)0;
	float3      vertworld;
	float3      norm;
	int4		indexvector;

	CalcSkinWorldPosAndNormal (In, vertworld, norm, indexvector, NumBones, true);
	Out.Pos = mul(float4(vertworld, 1), MatViewProj);

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

	float	fogz = dot(float4(vertworld, 1), MatViewProj._13_23_33_43);
	Out.Fog = 1 - (fogz - FogStart) * FogRangeRecip;

	return Out;
}

// Post passes (skinned): modulate the lit framebuffer by the diffuse
// texture(s), or lay the silhouette down black for the projected shadow.
VS_OUTPUT vsSkinPostMultiplyT1 (VS_INPUT_SKIN1 In, uniform int NumBones, uniform bool uvxform)
{
	VS_OUTPUT   Out;
	float3      pos;
	float3      norm;
	int4		indexvector;

	CalcSkinWorldPosAndNormal (In, pos, norm, indexvector, NumBones, false);
	Out.Pos = mul(float4(pos.xyz, 1.0f), MatViewProj);

	Out.Diff = 0.5;
	Out.Spec = 0;

	Out.Tex0  = CalcAnimatedUV(In.Tex0, uvxform);
	Out.Tex1  = CalcAnimatedUV(In.Tex0, uvxform);

	Out.Fog = CalcFogSkinned (pos);

	return Out;
}

VS_OUTPUT vsSkinPostMultiplyT2 (VS_INPUT_SKIN2 In, uniform int NumBones, uniform bool uvxform)
{
	VS_OUTPUT   Out;
	float3      pos;
	float3      norm;
	int4		indexvector;

	VS_INPUT_SKIN1	alt_in;
	alt_in = (VS_INPUT_SKIN1)In;

	CalcSkinWorldPosAndNormal (alt_in, pos, norm, indexvector, NumBones, false);
	Out.Pos = mul(float4(pos.xyz, 1.0f), MatViewProj);

	Out.Diff = 0.5;
	Out.Spec = 0;

	Out.Tex0  = CalcAnimatedUV(In.Tex0, uvxform);
	Out.Tex1  = CalcAnimatedUV(In.Tex1, uvxform);

	Out.Fog = CalcFogSkinned (pos);

	return Out;
}

VS_OUTPUT vsSkinPostBlackT1 (VS_INPUT_SKIN1 In, uniform int NumBones)
{
	VS_OUTPUT   Out;
	float3      pos;
	float3      norm;
	int4		indexvector;

	CalcSkinWorldPosAndNormal (In, pos, norm, indexvector, NumBones, false);
	Out.Pos = mul(float4(pos.xyz, 1.0f), MatViewProj);

	Out.Diff = 0.0;
	Out.Spec = 0;

	Out.Tex0  = In.Tex0;
	Out.Tex1  = In.Tex0;

	Out.Fog = CalcFogSkinned (pos);

	return Out;
}

// Depth-mask write, skinned.
VS_OUTPUTDEP vsSkinDepth (VS_INPUT_SKIN1 In, uniform int NumBones)
{
	VS_OUTPUTDEP	Out = (VS_OUTPUTDEP)0;
	float3			pos;
	float3			norm;
	int4			indexvector;

	CalcSkinWorldPosAndNormal (In, pos, norm, indexvector, NumBones, true);
	Out.Pos = mul(float4(pos.xyz, 1.0f), MatViewProj);

	Out.Tex0 = dot (float4(pos,1), VecDepthMaskPlane);

	return Out;
}

// Terrain-match: project the skinned vertex through the terrain page
// projection so the mesh samples the ground it stands on.
VS_OUTPUTDEPFOG vsSkinMatchTerrain (VS_INPUT_SKIN1 In, uniform int NumBones)
{
	VS_OUTPUTDEPFOG	Out = (VS_OUTPUTDEPFOG)0;
	float3			pos;
	float3			norm;
	int4			indexvector;

	CalcSkinWorldPosAndNormal (In, pos, norm, indexvector, NumBones, true);
	Out.Pos = mul(float4(pos.xyz, 1.0f), MatViewProj);

	Out.Tex0 = (float2) mul (float4(pos,1), TexProjMatrix1);

	float	fogz = dot(float4(pos.xyz, 1.0f), MatViewProj._13_23_33_43);
	Out.Fog = 1 - (fogz - FogStart) * FogRangeRecip;

	return Out;
}

// Object-space DOT3 spotlight pass (PS hardware): normal map + depth
// gradient + projected spot + diffuse, light vector in COLOR0.
VS_OUTPUT_PHONGPROJ vsObjSkinDot3SpotDepthPS (VS_INPUT_SKIN1 In, uniform int NumBones)
{
	VS_OUTPUT_PHONGPROJ		Out = (VS_OUTPUT_PHONGPROJ)0;
	float3					vertworld;
	float3					norm;
	int4					indexvector;

	CalcSkinWorldPosAndNormal (In, vertworld, norm, indexvector, NumBones, false);
	Out.Pos = mul(float4(vertworld, 1), MatViewProj);

	// per-bone model-space spotlight position
	float3	lightvec_model		= normalize(SkinModelLightArray[indexvector.x].xyz - In.Pos.xyz);

	float	selfshadowterm	= CalcSelfShadowTerm (lightvec_model, In.Norm);
	Out.Diff = ColorFromVector (lightvec_model*ObjSpaceFixup * selfshadowterm);

	Out.Tex0 = In.Tex0;
	Out.Tex1 = dot (float4(vertworld,1), VecDepthMaskPlane) - DEPTHTEST_8BIT_OFFSET;
	Out.Tex2 = mul (float4(vertworld,1), SpotLightProjMatrix) * float4(1,-1,1,1);
	Out.Tex3 = In.Tex0;
	Out.Spec = float4(1,1,1,1);

	return (Out);
}

shared VERTEXSHADER		vscSkinFlatSpotDepth		= compile vs_1_1 vsSkinFlatSpotDepth(4);

shared VERTEXSHADER		vscSkinPostMultiplyT1		= compile vs_1_1 vsSkinPostMultiplyT1(4, false);
shared VERTEXSHADER		vscSkinPostMultiplyT2		= compile vs_1_1 vsSkinPostMultiplyT2(4, false);
shared VERTEXSHADER		vscSkinPostMultiplyT1UV		= compile vs_1_1 vsSkinPostMultiplyT1(4, true);
shared VERTEXSHADER		vscSkinPostMultiplyT2UV		= compile vs_1_1 vsSkinPostMultiplyT2(4, true);

shared VERTEXSHADER		vscSkinPostBlackT1			= compile vs_1_1 vsSkinPostBlackT1(4);

shared VERTEXSHADER		vscSkDepth					= compile vs_1_1 vsSkinDepth(4);
shared VERTEXSHADER		vscSkMatchTerrain			= compile vs_1_1 vsSkinMatchTerrain(4);

shared VERTEXSHADER		vscObjSkinDot3SpotDepthPS	= compile vs_1_1 vsObjSkinDot3SpotDepthPS(4);
