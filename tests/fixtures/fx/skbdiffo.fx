////////////////////////////////////////////////////////////////
//	skbdiffo.fx — skinned, object-space bump diffuse.
//
//	OpenNova authored set. PS1.1 path: the vertex shader hands
//	the object-space light and hemisphere vectors down as cube-
//	normalized texture coordinates and ps11Diffuse does the
//	per-pixel dots against the object-space normal map (MDT).
////////////////////////////////////////////////////////////////

string EffectInfo <
	string EffectTag = "VS_SKBUMPDIFFOBJ";
	string EffectName = "Skinned - Object Space - Diffuse Bump";
	string EffectDesc1 = "Skinned mesh with bumpmapped diffuse lighting";
	string EffectDesc2 = "The normal map must be provided in object space, typically an MDT file.";
>;

#include "_baseinc.fx"
#include "_vsskshared.fx"
#include "_psshared.fx"

////////////////////////////////////////
////	Vertex shaders				////
////////////////////////////////////////
// Directional layer: object-space light + hemi vectors to the cube
// normalizer, light color * selfshadow in COLOR0.
VS_OUTPUT_PHONG vsObjSkinDot3DirPS (VS_INPUT_SKIN1 In, uniform int NumBones)
{
	VS_OUTPUT_PHONG		Out = (VS_OUTPUT_PHONG)0;
	float3				pos;
	float3				norm;
	int4				indexvector;

	CalcSkinWorldPosAndNormal (In, pos, norm, indexvector, NumBones, false);
	Out.Pos = mul(float4(pos, 1), MatViewProj);

	// per-bone model-space light vector
	float3		lightvec_model = SkinModelLightArray[indexvector.x].xyz;

	float3x3	worldtomodel = transpose((float3x3)SkinWorldMatrixArray[indexvector.x]);
	float3		hemivec_world = float3(0,1,0);
	float3		hemivec_model = mul (hemivec_world, worldtomodel);

	Out.Tex0 = In.Tex0;
	Out.Tex1 = lightvec_model * ObjSpaceFixup;
	Out.Tex2 = hemivec_model * ObjSpaceFixup;
	Out.Tex3 = In.Tex0;

	float	selfshadowterm = CalcSelfShadowTerm (lightvec_model, In.Norm);

	Out.Diff = DirLightColor * selfshadowterm;
	Out.Spec = float4(1,1,1,1);

	Out.Fog = CalcFogSkinned (pos);

	return Out;
}

// Point-light layer: attenuated color in COLOR0, light vector to the
// cube normalizer.
VS_OUTPUT_PHONG vsObjSkinDot3PointPS (VS_INPUT_SKIN1 In, uniform int NumBones)
{
	VS_OUTPUT_PHONG		Out = (VS_OUTPUT_PHONG)0;
	float3				pos;
	float3				norm;
	int4				indexvector;

	CalcSkinWorldPosAndNormal (In, pos, norm, indexvector, NumBones, false);
	Out.Pos = mul(float4(pos, 1), MatViewProj);

	// per-bone model-space light position
	float3	lightvec_model	= SkinModelLightArray[indexvector.x].xyz - In.Pos.xyz;

	float	atten = CalcPointLightAttenuation (lightvec_model);
	float	selfshadowterm	= CalcSelfShadowTerm (normalize(lightvec_model), In.Norm);
	Out.Diff = PointLightColor * selfshadowterm * atten;

	Out.Tex0 = In.Tex0;
	Out.Tex1 = lightvec_model * ObjSpaceFixup;
	Out.Tex3 = In.Tex0;
	Out.Spec = float4(1,1,1,1);

	Out.Fog = CalcFogSkinned (pos);

	return Out;
}

shared VERTEXSHADER		vscObjSkinDot3DirPS			= compile vs_1_1 vsObjSkinDot3DirPS(4);
shared VERTEXSHADER		vscObjSkinDot3PointPS		= compile vs_1_1 vsObjSkinDot3PointPS(4);

////////////////////////////////////////
////	Standard					////
////////////////////////////////////////
technique TSkinObjDiff_Std
<
bool usevs=true;
bool useps=true;
int ttype=TECHNIQUE_NORMAL;
>
{
	// directional + hemisphere layer, diffuse folded in-pass
	pass P0
	<
		int fogmode=FOGMODE_SHADER;
		int zmode=ZMODE_NORMAL;
		int amode=AMODE_NORMAL;
		int passrules = 0;
		int passsetup = PASSSETUP_SET_OBJSPACELIGHT_DIR;
	>
	{
		VertexShader = (vscObjSkinDot3DirPS);
		PixelShader = (ps11Diffuse);

		PixelShaderConstantF[1] = (AmbientColor);
		PixelShaderConstantF[2] = (HemiSkyColor-AmbientColor);

		SetTextureSh(0, TexNormal1,			sampLinearWrap2D)
		SetTextureSh(1, TexCubeNormalize,	sampLinearWrap3D)
		SetTextureSh(2, TexCubeNormalize,	sampLinearWrap3D)
		SetTextureSh(3, TexDiffuse1,		sampLinearWrap2D)

		RSAlphaMode(FALSE, ONE, ZERO)
		SpecularEnable	= FALSE;

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
		VertexShader = (vscObjSkinDot3PointPS);
		PixelShader = (ps11DiffuseSingle);

		SetTextureSh(0, TexNormal1,			sampLinearWrap2D)
		SetTextureSh(1, TexCubeNormalize,	sampLinearWrap3D)
		SetTextureSh(2, TexCubeNormalize,	sampLinearWrap3D)
		SetTextureSh(3, TexDiffuse1,		sampLinearWrap2D)

		RSAlphaMode(TRUE, ONE, ONE)
		SpecularEnable		= FALSE;

		FogEnable	= TRUE;
	}

	// one additive beam pass per spotlight, per-pixel depth test
	pass P2
	<
		int fogmode=FOGMODE_SHADERADD;
		int zmode=ZMODE_NORMAL;
		int amode=AMODE_NORMAL;
		int passrules = PASSRULE_ONCE_PER_SPOTLIGHT;
	>
	{
		VertexShader = (vscObjSkinDot3SpotDepthPS);
		PixelShader = (ps11DiffuseDepth);

		PixelShaderConstantF[2] = (PointLightColor);

		SetTextureSh(0, TexNormal1,			sampLinearWrap2D)
		SetTextureSh(1, TexDepthGradTest,	sampLinearClamp2D)
		SetProjTexSh(2, TexCurProj,			sampLinearClamp2D)
		SetTextureSh(3, TexDiffuse1,		sampLinearWrap2D)

		RSAlphaMode(TRUE, ONE, ONE)
		SpecularEnable	= FALSE;
		FogEnable	= FALSE;
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
