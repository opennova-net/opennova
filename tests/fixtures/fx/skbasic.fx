////////////////////////////////////////////////////////////////
//	skbasic.fx — skinned, gouraud diffuse.
//
//	OpenNova authored set. The plain skinned surface: matrix-
//	palette position/normal, hemisphere + directional + point
//	lights folded per vertex, one diffuse map at Modulate2x.
////////////////////////////////////////////////////////////////

string EffectInfo <
	string EffectTag = "VS_SKBASIC";
	string EffectName = "Skinned - Basic";
	string EffectDesc1 = "Skinned mesh with diffuse lighting";
	string EffectDesc2 = "";
	bool   EffectAlt_UV = true;
>;

#include "_baseinc.fx"
#include "_vsskshared.fx"
#include "_psshared.fx"

////////////////////////////////////////
////	Vertex shaders				////
////////////////////////////////////////
VS_OUTPUT vsSkinBasic (VS_INPUT_SKIN1 In, uniform int NumBones, uniform int numlights, uniform bool uvxform)
{
	VS_OUTPUT   Out;
	float3      pos;
	float3      norm;
	int4		indexvector;

	CalcSkinWorldPosAndNormal (In, pos, norm, indexvector, NumBones, true);
	Out.Pos = mul(float4(pos.xyz, 1.0f), MatViewProj);

	float3	accumlight = (float3)HemicolorFromVectorY (norm.y) + DirLightColor.xyz * max(0, dot(norm, DirLightVector));
	for (int light=0; light<numlights; light++)
	{
		float3	lightvec	= PointLightCoordArray[light] - pos;
		float	distsqrd	= dot (lightvec, lightvec);
		float	recip		= rsqrt (distsqrd);

		float	atten = PointLightAttenArray[light].x +
						PointLightAttenArray[light].y * distsqrd * recip +
						PointLightAttenArray[light].z * distsqrd;
		accumlight += PointLightColorArray[light] * max(0, dot(normalize (lightvec), norm)) * (1.0 / atten);
	}

	Out.Diff = float4(accumlight,1);
	Out.Spec = 0;

	Out.Tex0  = CalcAnimatedUV(In.Tex0, uvxform);
	Out.Tex1  = CalcAnimatedUV(In.Tex0, uvxform);

	float	fogz = dot(float4(pos.xyz, 1.0f), MatViewProj._13_23_33_43);
	Out.Fog = 1 - (fogz - FogStart) * FogRangeRecip;

	return Out;
}

VertexShader vsSkBasicArray[4] =
{
	compile vs_1_1 vsSkinBasic(4, 0, false),
	compile vs_1_1 vsSkinBasic(4, 1, false),
	compile vs_1_1 vsSkinBasic(4, 2, false),
	compile vs_1_1 vsSkinBasic(4, 3, false)
};

VertexShader vsSkBasicArrayUV[4] =
{
	compile vs_1_1 vsSkinBasic(4, 0, true),
	compile vs_1_1 vsSkinBasic(4, 1, true),
	compile vs_1_1 vsSkinBasic(4, 2, true),
	compile vs_1_1 vsSkinBasic(4, 3, true)
};

////////////////////////////////////////
////	Standard					////
////////////////////////////////////////
technique TSkBasic
<
bool usevs=true;
bool useps=false;
int ttype=TECHNIQUE_NORMAL;
>
{
	// One pass when no spotlight is live.
	pass P0
	<
		int fogmode=FOGMODE_NORMAL;
		int zmode=ZMODE_NORMAL;
		int amode=AMODE_NORMAL;
		int passrules = PASSRULE_IF_NO_SPOTLIGHTS + PASSRULE_POINTLIGHT_VARIATIONS;
	>
	{
#ifdef TEX_UVXFORM
		VertexShader = (vsSkBasicArrayUV[CurNumPointLights]);
#else
		VertexShader = (vsSkBasicArray[CurNumPointLights]);
#endif
		PixelShader = NULL;

		Texture[0]				= (TexDiffuse1);
		Sampler[0]				= <sampLinearWrap2D>;
		TexCoordIndex[0]		= 0;
		TextureTransformFlags[0]= DISABLE;

		AlphaBlendEnable	= FALSE;
		SpecularEnable		= TRUE;

		TSSColor(0, Modulate2x,	Texture, Diffuse)
		TSSAlpha(0, Modulate,	Texture, Diffuse)
		TSSEnd(1)

		FogEnable	= TRUE;
	}

	// Spotlights force multi-pass: lighting only, beams, texture fold.
	pass P1a
	<
		int fogmode=FOGMODE_NORMAL;
		int zmode=ZMODE_NORMAL;
		int amode=AMODE_NORMAL;
		int passrules = PASSRULE_IF_SPOTLIGHTS + PASSRULE_POINTLIGHT_VARIATIONS;
	>
	{
#ifdef TEX_UVXFORM
		VertexShader = (vsSkBasicArrayUV[CurNumPointLights]);
#else
		VertexShader = (vsSkBasicArray[CurNumPointLights]);
#endif
		PixelShader = NULL;

		Texture[0]				= (TexDiffuse1);
		Sampler[0]				= <sampLinearWrap2D>;
		TexCoordIndex[0]		= 0;
		TextureTransformFlags[0]= DISABLE;

		AlphaBlendEnable	= FALSE;
		SpecularEnable		= TRUE;

		TSSColor(0, SelectArg2,	Texture, Diffuse)
		TSSAlpha(0, Modulate,	Texture, Diffuse)
		TSSEnd(1)

		FogEnable	= TRUE;
	}

	pass P2a
	<
		int fogmode=FOGMODE_SHADERADD;
		int zmode=ZMODE_NORMAL;
		int amode=AMODE_DEPTHTEST;
		int passrules = PASSRULE_ONCE_PER_SPOTLIGHT;
	>
	{
		VertexShader = (vscSkinFlatSpotDepth);
		PixelShader = NULL;

		SetTextureSh(0, TexDepthGradTest,	sampLinearClamp2D)
		SetProjTexSh(1, TexCurProj,			sampLinearClamp2D)

		RSAlphaMode(TRUE, ONE, ONE)
		SpecularEnable	= FALSE;

		TSSColor(0, Modulate,		Texture, Diffuse)
		TSSAlpha(0, SelectArg1,		Texture, Diffuse)
		TSSColor(1, Modulate,		Texture, Current)
		TSSAlpha(1, Subtract,		Texture, Current)
		TSSEnd(2)

		FogEnable	= FALSE;
	}

	pass P3a
	<
		int fogmode=FOGMODE_SHADER;
		int zmode=ZMODE_NORMAL;
		int amode=AMODE_NORMAL;
		int passrules = PASSRULE_IF_SPOTLIGHTS;
	>
	{
#ifdef TEX_UVXFORM
		VertexShader = (vscSkinPostMultiplyT1UV);
#else
		VertexShader = (vscSkinPostMultiplyT1);
#endif
		PixelShader = NULL;

		SetTextureSh(0, TexDiffuse1,	sampLinearWrap2D)

		RSAlphaMode(TRUE, DESTCOLOR, SRCCOLOR)
		SpecularEnable	= FALSE;

		TSSColor(0, Modulate2x,	Texture, Diffuse)
		TSSAlpha(0, Modulate,	Texture, Diffuse)
		TSSEnd(1)

		FogEnable	= TRUE;
	}
}

////////////////////////////////////////
////	Projected shadow			////
////////////////////////////////////////
technique TSkBasic_Proj
<
bool usevs=true;
bool useps=false;
int ttype=TECHNIQUE_PROJSHAD;
>
{
	pass P0
	<
		int fogmode=FOGMODE_SHADERSET;
		int zmode=ZMODE_NORMAL;
		int amode=AMODE_NORMAL;
	>
	{
		VertexShader = (vscSkinPostBlackT1);
		PixelShader = NULL;

		SetTextureSh(0, TexDiffuse1,	sampLinearWrap2D)

		RSAlphaMode(FALSE, ONE, ZERO)
		SpecularEnable = FALSE;

		TSSColor(0, SelectArg2,	Texture, Diffuse)
		TSSAlpha(0, Modulate,	Texture, Diffuse)
		TSSEnd(1)

		FogEnable	= TRUE;
	}
}

////////////////////////////////////////
////	Depth mask + terrain match	////
////////////////////////////////////////
#include "_tskin.fx"
