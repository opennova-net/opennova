////////////////////////////////////////////////////////////////
//	skbphongo.fx — skinned, object-space bump phong.
//
//	OpenNova authored set. PS1.1 path via the 2D phong-map
//	lookup: the vertex shader hands object-space light and half
//	vectors down, texm3x2 rows dot them against the object-space
//	normal map (MDT) and read the diffuse/specular response from
//	TexPhongMap. Soldier bodies and heads ride this effect.
////////////////////////////////////////////////////////////////

string EffectInfo <
	string EffectTag = "VS_SKBUMPPHONGOBJ";
	string EffectName = "Skinned - Object Space - Phong Bump";
	string EffectDesc1 = "Skinned mesh with bumpmapped phong lighting";
	string EffectDesc2 = "The normal map must be provided in object space, typically an MDT file.";
>;

#include "_baseinc.fx"
#include "_vsskshared.fx"
#include "_psshared.fx"

////////////////////////////////////////
////	Vertex shaders				////
////////////////////////////////////////
// Directional layer: object-space light + half vectors as the texm3x2
// rows, light color * selfshadow in COLOR0.
VS_OUTPUT_PHONG vsObjSkinPhongDir (VS_INPUT_SKIN1 In, uniform int NumBones)
{
	VS_OUTPUT_PHONG	Out;
	float3			pos;
	float3			norm;
	int4			indexvector;

	CalcSkinWorldPosAndNormal (In, pos, norm, indexvector, NumBones, false);
	Out.Pos = mul(float4(pos.xyz, 1.0f), MatViewProj);

	float3		eyevec_world = normalize (CameraPos - pos);
	float3		halfangle = eyevec_world + DirLightVector;

	float3x3	worldtomodel = transpose((float3x3)SkinWorldMatrixArray[indexvector.x]);
	float3		lightvec_model  = normalize (ObjSpaceFixup * mul (DirLightVector, worldtomodel));
	float3		halfangle_model = normalize (ObjSpaceFixup * mul (halfangle, worldtomodel));

	Out.Tex0  = In.Tex0;
	Out.Tex1  = lightvec_model;
	Out.Tex2  = halfangle_model;
	Out.Tex3  = In.Tex0;

	float	selfshadow = CalcSelfShadowTerm (lightvec_model*ObjSpaceFixup, In.Norm);
	Out.Diff = DirLightColor * selfshadow;
	Out.Spec = float4(1,1,1,1);

	Out.Fog = CalcFogSkinned (pos);

	return Out;
}

// Point-light layer.
VS_OUTPUT_PHONG vsObjSkinPhongPoint (VS_INPUT_SKIN1 In, uniform int NumBones)
{
	VS_OUTPUT_PHONG	Out=(VS_OUTPUT_PHONG)0;
	float3			pos;
	float3			norm;
	int4			indexvector;

	CalcSkinWorldPosAndNormal (In, pos, norm, indexvector, NumBones, false);
	Out.Pos = mul(float4(pos.xyz, 1.0f), MatViewProj);

	// per-bone model-space light position
	float3		lightvec_model	= (SkinModelLightArray[indexvector.x].xyz - In.Pos.xyz);

	float	reciplength;
	float	atten = CalcPointLightAttenuationAndLength (lightvec_model, reciplength);

	lightvec_model *= reciplength;

	float3x3	worldtomodel = transpose((float3x3)SkinWorldMatrixArray[indexvector.x]);
	float3		eyevec_world = CameraPos - pos;
	float3		eyevec_model = normalize (mul (eyevec_world, worldtomodel));

	float3		halfangle_model = normalize (eyevec_model + lightvec_model);

	Out.Tex0  = In.Tex0;
	Out.Tex1  = ObjSpaceFixup * lightvec_model;
	Out.Tex2  = ObjSpaceFixup * halfangle_model;
	Out.Tex3  = In.Tex0;

	float	selfshadow = CalcSelfShadowTerm (lightvec_model, In.Norm);
	Out.Diff = PointLightColor * atten * selfshadow;
	Out.Spec = float4(1,1,1,1);

	Out.Fog = CalcFogSkinned (pos);

	return Out;
}

// Spotlight specular pass: half vector to the cube normalizer, projected
// spot + depth gradient for the per-pixel test.
VS_OUTPUT_PHONGPROJ2 vsObjSkinPhongSpecSpotDepth (VS_INPUT_SKIN1 In, uniform int NumBones)
{
	VS_OUTPUT_PHONGPROJ2	Out = (VS_OUTPUT_PHONGPROJ2)0;
	float3					vertworld;
	float3					norm;
	int4					indexvector;

	CalcSkinWorldPosAndNormal (In, vertworld, norm, indexvector, NumBones, false);
	Out.Pos = mul(float4(vertworld, 1), MatViewProj);

	// per-bone model-space spotlight position
	float3	lightvec_model		= SkinModelLightArray[indexvector.x].xyz - In.Pos.xyz;
	float3	lightvec_model_norm	= normalize(lightvec_model);

	float	selfshadowterm = CalcSelfShadowTerm (lightvec_model_norm, In.Norm);

	Out.Diff = PointLightColor * selfshadowterm;

	float3x3	worldtomodel = transpose((float3x3)SkinWorldMatrixArray[indexvector.x]);
	float3		eyevec_world = CameraPos - vertworld;
	float3		eyevec_model = mul (eyevec_world, worldtomodel);

	float3		halfangle_model = normalize(eyevec_model) + lightvec_model_norm;

	Out.Tex0  = In.Tex0;
	Out.Tex1  = normalize (halfangle_model) * ObjSpaceFixup;
	Out.Tex2 = mul (float4(vertworld,1), SpotLightProjMatrix) * float4(1,-1,1,1);
	Out.Tex3 = dot (float4(vertworld,1), VecDepthMaskPlane) - DEPTHTEST_8BIT_OFFSET;

	return Out;
}

shared VERTEXSHADER		vscObjSkinPhongDir				= compile vs_1_1 vsObjSkinPhongDir(4);
shared VERTEXSHADER		vscObjSkinPhongPoint			= compile vs_1_1 vsObjSkinPhongPoint(4);
shared VERTEXSHADER		vscObjSkinPhongSpecSpotDepth	= compile vs_1_1 vsObjSkinPhongSpecSpotDepth(4);

////////////////////////////////////////
////	Standard					////
////////////////////////////////////////
technique TSkObjPhong_Std
<
bool usevs=true;
bool useps=true;
int ttype=TECHNIQUE_NORMAL;
>
{
	// diffusemap*dirlight + specular + flat ambient
	pass P0
	<
		int fogmode=FOGMODE_SHADER;
		int zmode=ZMODE_NORMAL;
		int amode=AMODE_NORMAL;
		int passrules = 0;
	>
	{
		VertexShader = (vscObjSkinPhongDir);
		PixelShader = (ps11Phong2);

		PixelShaderConstantF[5]	= (AmbientColor);

		SetTextureSh(0, TexNormal1,		sampLinearWrap2D)
		SetNullTexSh(1,					sampLinearWrap2D)
		SetTextureSh(2,	TexPhongMap,	sampLinearClamp2D)
		SetTextureSh(3, TexDiffuse1,	sampLinearWrap2D)

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
		VertexShader = (vscObjSkinPhongPoint);
		PixelShader = (ps11Phong2);

		PixelShaderConstantF[5]	= float4(0,0,0,0);

		SetTextureSh(0, TexNormal1,		sampLinearWrap2D)
		SetNullTexSh(1,					sampLinearWrap2D)
		SetTextureSh(2,	TexPhongMap,	sampLinearClamp2D)
		SetTextureSh(3, TexDiffuse1,	sampLinearWrap2D)

		RSAlphaMode(TRUE, ONE, ONE)
		SpecularEnable	= FALSE;

		FogEnable	= TRUE;
	}

	// spotlight diffuse-only, per-pixel depth test
	pass P2
	<
		int fogmode=FOGMODE_SHADERADD;
		int zmode=ZMODE_NORMAL;
		int amode=AMODE_DEPTHTEST;
		int passrules = PASSRULE_ONCE_PER_SPOTLIGHT;
	>
	{
		VertexShader = (vscObjSkinDot3SpotDepthPS);
		PixelShader = (ps11PhongDepthDiffOnly);

		PixelShaderConstantF[2] = (PointLightColor);

		SetTextureSh(0, TexNormal1,			sampLinearWrap2D)
		SetTextureSh(1, TexDepthGradTest,	sampLinearClamp2D)
		SetProjTexSh(2, TexCurProj,			sampLinearClamp2D)
		SetTextureSh(3, TexDiffuse1,		sampLinearWrap2D)

		RSAlphaMode(TRUE, ONE, ONE)
		SpecularEnable	= FALSE;
		FogEnable	= FALSE;
	}

	// spotlight specular-only, per-pixel depth test
	pass P3
	<
		int fogmode=FOGMODE_SHADERADD;
		int zmode=ZMODE_NORMAL;
		int amode=AMODE_NORMAL;
		int passrules = PASSRULE_ONCE_PER_SPOTLIGHT;
	>
	{
		VertexShader = (vscObjSkinPhongSpecSpotDepth);
		PixelShader = (ps11PhongDepthSpecOnly);

		SetTextureSh(0, TexNormal1,			sampLinearWrap2D)
		SetTextureSh(1, TexCubeNormalize,	sampLinearWrap3D)
		SetProjTexSh(2, TexCurProj,			sampLinearClamp2D)
		SetTextureSh(3, TexDepthGradTest,	sampLinearClamp2D)

		RSAlphaMode(TRUE, ONE, ONE)
		SpecularEnable	= FALSE;
		FogEnable	= FALSE;
	}
}

////////////////////////////////////////
////	Projected shadow			////
////////////////////////////////////////
technique TSkObjPhong_Proj
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
