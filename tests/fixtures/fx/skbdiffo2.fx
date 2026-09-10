////////////////////////////////////////////////////////////////
//	skbdiffo2.fx — skinned, object-space bump diffuse,
//	multi-textured.
//
//	OpenNova authored set. Fixed-function DOT3 path against the
//	object-space normal map (MDT): light vector packed into
//	COLOR0 per pass, both diffuse textures folded at the end.
////////////////////////////////////////////////////////////////

string EffectInfo <
	string EffectTag = "VS_SKBUMPDIFFOBJ2";
	string EffectName = "Skinned - Object Space - Diffuse Bump, Multi-textured";
	string EffectDesc1 = "Skinned mesh with bumpmapped diffuse lighting";
	string EffectDesc2 = "The normal map must be provided in object space, typically an MDT file.";
>;

#include "_baseinc.fx"
#include "_vsskshared.fx"
#include "_psshared.fx"

////////////////////////////////////////
////	Vertex shaders				////
////////////////////////////////////////
// Directional layer: object-space light vector packed into COLOR0,
// gouraud hemisphere into COLOR1.
VS_OUTPUT vsObjSkinDot3Dir (VS_INPUT_SKIN1 In, uniform int NumBones)
{
	VS_OUTPUT   Out;
	float3      pos;
	float3      norm;
	int4		indexvector;

	CalcSkinWorldPosAndNormal (In, pos, norm, indexvector, NumBones, false);
	Out.Pos = mul(float4(pos.xyz, 1.0f), MatViewProj);
	Out.Spec = HemicolorFromVectorY (norm.y);

	// per-bone model-space light vector
	float3		lightvec_model = SkinModelLightArray[indexvector.x].xyz;

	Out.Diff = ColorFromVector (ObjSpaceFixup * lightvec_model);

	Out.Tex0  = In.Tex0;
	Out.Tex1  = In.Tex0;

	Out.Fog = CalcFogSkinned (pos);

	return Out;
}

// Point-light layer: attenuation scales the packed vector, light color
// rides COLOR1.
VS_OUTPUT vsObjSkinDot3Point (VS_INPUT_SKIN1 In, uniform int NumBones)
{
	VS_OUTPUT   Out;
	float3      pos;
	float3      norm;
	int4		indexvector;

	CalcSkinWorldPosAndNormal (In, pos, norm, indexvector, NumBones, false);
	Out.Pos = mul(float4(pos, 1), MatViewProj);

	// per-bone model-space light position
	float3	lightvec_model		= SkinModelLightArray[indexvector.x].xyz - In.Pos.xyz;

	float	reciplength;
	float	atten = CalcPointLightAttenuationAndLength (lightvec_model, reciplength);

	Out.Diff = ColorFromVectorScale (ObjSpaceFixup*lightvec_model*reciplength, atten);
	Out.Spec = PointLightColor;

	Out.Tex0  = In.Tex0;
	Out.Tex1  = In.Tex0;

	Out.Fog = CalcFogSkinned (pos);

	return Out;
}

shared VERTEXSHADER		vscObjSkinDot3Dir			= compile vs_1_1 vsObjSkinDot3Dir(4);
shared VERTEXSHADER		vscObjSkinDot3Point			= compile vs_1_1 vsObjSkinDot3Point(4);

////////////////////////////////////////
////	Standard					////
////////////////////////////////////////
technique TSkObjDiff
<
bool usevs=true;
bool useps=false;
int ttype=TECHNIQUE_NORMAL;
>
{
	// directional + hemisphere layer
	pass P0
	<
		int fogmode=FOGMODE_SHADERSET;
		int zmode=ZMODE_NORMAL;
		int amode=AMODE_NORMAL;
		int passrules = 0;
		int passsetup = PASSSETUP_SET_OBJSPACELIGHT_DIR;
	>
	{
		VertexShader = (vscObjSkinDot3Dir);
		PixelShader = NULL;

		SetTextureSh(0, TexNormal1,		sampLinearWrap2D)
		SetTextureSh(1, TexDiffuse1,	sampLinearWrap2D)

		RSAlphaMode(FALSE, ONE, ZERO)
		SpecularEnable		= TRUE;

		TextureFactor		= (DirLightColor);

		TSSColor(0, DotProduct3,	Texture, Diffuse)
		TSSAlpha(0, DotProduct3,	Texture, Diffuse)
		TSSColor(1, Modulate,		TFactor, Current)
		TSSAlpha(1, Modulate,		Texture, Diffuse)
		TSSEnd(2)

		FogEnable	= TRUE;
	}

	// one additive pass per point light
	pass P1
	<
		int fogmode=FOGMODE_SHADERADD;
		int zmode=ZMODE_NORMAL;
		int amode=AMODE_NORMAL;
		int passrules = PASSRULE_ONCE_PER_POINTLIGHT;
	>
	{
		VertexShader = (vscObjSkinDot3Point);
		PixelShader = NULL;

		SetTextureSh(0, TexNormal1,		sampLinearWrap2D)
		SetTextureSh(1, TexDiffuse1,	sampLinearWrap2D)

		RSAlphaMode(TRUE, ONE, ONE)
		SpecularEnable	= FALSE;

		TSSColor(0, DotProduct3,	Texture, Diffuse)
		TSSAlpha(0, DotProduct3,	Texture, Diffuse)
		TSSColor(1, Modulate,		Specular, Current)
		TSSAlpha(1, Modulate,		Texture, Diffuse)
		TSSEnd(2)

		FogEnable	= TRUE;
	}

	// one additive beam pass per spotlight
	pass P2
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

	// fold both diffuse textures over the accumulated lighting
	pass P3
	<
		int fogmode=FOGMODE_SHADER;
		int zmode=ZMODE_NORMAL;
		int amode=AMODE_NORMAL;
	>
	{
		VertexShader = (vscSkinPostMultiplyT2);
		PixelShader = NULL;

		SetTextureSh(0, TexDiffuse1,	sampLinearWrap2D)
		SetTextureSh(1, TexDiffuse2,	sampLinearWrap2D)

		RSAlphaMode(TRUE, DESTCOLOR, SRCCOLOR)
		SpecularEnable	= FALSE;

		TSSColor(0, Modulate2x,	Texture, Diffuse)
		TSSAlpha(0, Modulate,	Texture, Diffuse)
		TSSColor(1, Modulate2x,	Texture, Current)
		TSSAlpha(1, Modulate,	Texture, Current)
		TSSEnd(2)

		FogEnable	= TRUE;
	}
}

////////////////////////////////////////
////	Projected shadow			////
////////////////////////////////////////
technique TSkObjDiff_Proj
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
