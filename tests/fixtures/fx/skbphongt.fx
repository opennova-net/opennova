////////////////////////////////////////////////////////////////
//	skbphongt.fx — skinned, tangent-space bump phong.
//
//	OpenNova authored set. PS1.1 path: cube-normalized tangent-
//	space light and half vectors feed ps11Phong; specular
//	exponent by repeated squaring, brightness from the diffuse
//	map's alpha. The arms model rides this effect.
////////////////////////////////////////////////////////////////

string EffectInfo <
	string EffectTag = "VS_SKBUMPPHONGT";
	string EffectName = "Skinned - Local Space - Phong Bump";
	string EffectDesc1 = "Skinned mesh with bumpmapped phong lighting";
	string EffectDesc2 = "The normal map must be provided in local space, typically an MDT file.";
>;

#include "_baseinc.fx"
#include "_vsskshared.fx"
#include "_psshared.fx"

////////////////////////////////////////
////	Vertex shaders				////
////////////////////////////////////////
// Directional layer: tangent-space light + half vectors to the cube
// normalizer, light color in COLOR0, hemisphere in COLOR1.
VS_OUTPUT_PHONG vsTanSkinPhongDir (VS_INPUT_SKINTAN1 In, uniform int NumBones)
{
	VS_OUTPUT_PHONG	Out;
	float3			pos;
	float3			norm;
	int4			indexvector;

	VS_INPUT_SKIN1	alt_in;
	alt_in = (VS_INPUT_SKIN1)In;

	CalcSkinWorldPosAndNormal (alt_in, pos, norm, indexvector, NumBones, false);
	Out.Pos = mul(float4(pos.xyz, 1.0f), MatViewProj);

	float3		eyevec_world = normalize (CameraPos - pos);
	float3		halfangle = eyevec_world + DirLightVector;

	float3x3	worldtomodel = transpose((float3x3)SkinWorldMatrixArray[indexvector.x]);
	float3		lightvec_objspace  = normalize (mul (DirLightVector, worldtomodel));
	float3		halfangle_objspace = normalize (mul (halfangle, worldtomodel));

	float3	lightvec_tangent;
	lightvec_tangent.x = dot (In.Tangent,  lightvec_objspace);
	lightvec_tangent.y = dot (In.Binormal, lightvec_objspace);
	lightvec_tangent.z = dot (In.Norm,     lightvec_objspace);
	Out.Tex1  = lightvec_tangent;

	float3	halfangle_tangent;
	halfangle_tangent.x = dot (In.Tangent,  halfangle_objspace);
	halfangle_tangent.y = dot (In.Binormal, halfangle_objspace);
	halfangle_tangent.z = dot (In.Norm,     halfangle_objspace);
	Out.Tex2  = halfangle_tangent;

	Out.Tex0  = In.Tex0;
	Out.Tex3  = In.Tex0;

	Out.Diff = DirLightColor;
	Out.Spec = HemicolorFromVectorY (norm.y);

	Out.Fog = CalcFogSkinned (pos);

	return Out;
}

// Point-light layer.
VS_OUTPUT_PHONG vsTanSkinPhongPoint (VS_INPUT_SKINTAN1 In, uniform int NumBones)
{
	VS_OUTPUT_PHONG	Out=(VS_OUTPUT_PHONG)0;
	float3			pos;
	float3			norm;
	int4			indexvector;

	VS_INPUT_SKIN1	alt_in;
	alt_in = (VS_INPUT_SKIN1)In;
	CalcSkinWorldPosAndNormal (alt_in, pos, norm, indexvector, NumBones, false);
	Out.Pos = mul(float4(pos.xyz, 1.0f), MatViewProj);

	// per-bone model-space light position
	float3	lightvec_model		= SkinModelLightArray[indexvector.x].xyz - In.Pos.xyz;

	float3	lightvec_tangent;
	lightvec_tangent.x = dot (In.Tangent,  lightvec_model);
	lightvec_tangent.y = dot (In.Binormal, lightvec_model);
	lightvec_tangent.z = dot (In.Norm,     lightvec_model);

	Out.Tex1  = normalize(lightvec_tangent);

	float	reciplength;
	float	atten = CalcPointLightAttenuationAndLength (lightvec_model, reciplength);
	float	selfshadow = CalcSelfShadowTerm (lightvec_model*reciplength, In.Norm);

	Out.Diff = PointLightColor * atten * selfshadow;

	float3x3	worldtomodel = transpose((float3x3)SkinWorldMatrixArray[indexvector.x]);
	float3		eyevec_world = CameraPos - pos;
	float3		eyevec_model = mul (eyevec_world, worldtomodel);

	float3		halfangle_model = normalize(eyevec_model) + lightvec_model * reciplength;
	float3		halfangle_tangent;
	halfangle_tangent.x = dot (In.Tangent,  halfangle_model);
	halfangle_tangent.y = dot (In.Binormal, halfangle_model);
	halfangle_tangent.z = dot (In.Norm,     halfangle_model);
	Out.Tex2  = normalize (halfangle_tangent);

	Out.Tex0  = In.Tex0;
	Out.Tex3  = In.Tex0;

	Out.Spec = float4(0,0,0,0);

	Out.Fog = 1.0;

	return Out;
}

// Spotlight diffuse pass (per-pixel depth test): tangent DOT3 protocol.
VS_OUTPUT_PHONGPROJ vsTanSkinDot3SpotDepthPS (VS_INPUT_SKINTAN1 In, uniform int NumBones)
{
	VS_OUTPUT_PHONGPROJ		Out = (VS_OUTPUT_PHONGPROJ)0;
	float3					vertworld;
	float3					norm;
	int4					indexvector;

	VS_INPUT_SKIN1	alt_in;
	alt_in = (VS_INPUT_SKIN1)In;

	CalcSkinWorldPosAndNormal (alt_in, vertworld, norm, indexvector, NumBones, false);
	Out.Pos = mul(float4(vertworld, 1), MatViewProj);

	// per-bone model-space spotlight position
	float3	lightvec_model		= SkinModelLightArray[indexvector.x].xyz - In.Pos.xyz;

	float3	lightvec_tangent;
	lightvec_tangent.x = dot (In.Tangent,  lightvec_model);
	lightvec_tangent.y = dot (In.Binormal, lightvec_model);
	lightvec_tangent.z = dot (In.Norm,     lightvec_model);

	float	selfshadowterm	= CalcSelfShadowTerm (normalize(lightvec_model), In.Norm);
	Out.Diff = ColorFromVector (normalize(lightvec_tangent) * selfshadowterm);

	Out.Tex0 = In.Tex0;
	Out.Tex1 = dot (float4(vertworld,1), VecDepthMaskPlane) - DEPTHTEST_8BIT_OFFSET;
	Out.Tex2 = mul (float4(vertworld,1), SpotLightProjMatrix) * float4(1,-1,1,1);
	Out.Tex3 = In.Tex0;
	Out.Spec = float4(1,1,1,1);

	return (Out);
}

// Spotlight specular pass: half vector to the cube normalizer, projected
// spot + depth gradient for the per-pixel test.
VS_OUTPUT_PHONGPROJ2 vsTanSkinPhongSpecSpotDepth (VS_INPUT_SKINTAN1 In, uniform int NumBones)
{
	VS_OUTPUT_PHONGPROJ2	Out = (VS_OUTPUT_PHONGPROJ2)0;
	float3					vertworld;
	float3					norm;
	int4					indexvector;

	VS_INPUT_SKIN1	alt_in;
	alt_in = (VS_INPUT_SKIN1)In;
	CalcSkinWorldPosAndNormal (alt_in, vertworld, norm, indexvector, NumBones, false);
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
	float3		halfangle_tangent;
	halfangle_tangent.x = dot (In.Tangent,  halfangle_model);
	halfangle_tangent.y = dot (In.Binormal, halfangle_model);
	halfangle_tangent.z = dot (In.Norm,     halfangle_model);

	Out.Tex0  = In.Tex0;
	Out.Tex1  = normalize (halfangle_tangent);
	Out.Tex2 = mul (float4(vertworld,1), SpotLightProjMatrix) * float4(1,-1,1,1);
	Out.Tex3 = dot (float4(vertworld,1), VecDepthMaskPlane) - DEPTHTEST_8BIT_OFFSET;

	return Out;
}

shared VERTEXSHADER		vscTanSkinPhongDir				= compile vs_1_1 vsTanSkinPhongDir(4);
shared VERTEXSHADER		vscTanSkinPhongPoint			= compile vs_1_1 vsTanSkinPhongPoint(4);
shared VERTEXSHADER		vscTanSkinDot3SpotDepthPS		= compile vs_1_1 vsTanSkinDot3SpotDepthPS(4);
shared VERTEXSHADER		vscTanSkinPhongSpecSpotDepth	= compile vs_1_1 vsTanSkinPhongSpecSpotDepth(4);

////////////////////////////////////////
////	Standard					////
////////////////////////////////////////
technique TSkTanPhong_Std
<
bool usevs=true;
bool useps=true;
int ttype=TECHNIQUE_NORMAL;
>
{
	// diffusemap*dirlight + specular + hemisphere ambient
	pass P0
	<
		int fogmode=FOGMODE_SHADER;
		int zmode=ZMODE_NORMAL;
		int amode=AMODE_NORMAL;
		int passrules = 0;
	>
	{
		VertexShader = (vscTanSkinPhongDir);
		PixelShader = (ps11Phong);

		PixelShaderConstantF[5]	= (AmbientColor);

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
		VertexShader = (vscTanSkinPhongPoint);
		PixelShader = (ps11Phong);

		PixelShaderConstantF[5]	= float4(0,0,0,0);

		SetTextureSh(0, TexNormal1,			sampLinearWrap2D)
		SetTextureSh(1, TexCubeNormalize,	sampLinearWrap3D)
		SetTextureSh(2, TexCubeNormalize,	sampLinearWrap3D)
		SetTextureSh(3, TexDiffuse1,		sampLinearWrap2D)

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
		VertexShader = (vscTanSkinDot3SpotDepthPS);
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
		VertexShader = (vscTanSkinPhongSpecSpotDepth);
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
technique TSkTanPhong_Proj
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
