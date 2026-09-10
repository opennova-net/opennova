////////////////////////////////////////////////////////////////
//	phongt.fx — rigid, tangent-space bump phong.
//
//	OpenNova authored set. PS1.1 path: world-basis tangent
//	rotation per vertex, cube-normalized light and half vectors
//	into ps11Phong. The first-person rifle rides this effect.
////////////////////////////////////////////////////////////////

string EffectInfo <
	string EffectTag = "VS_PHONGT";
	string EffectName = "Local Space Bump - Phong";
	string EffectDesc1 = "Bump map with Phong diffuse and specular lighting.";
	string EffectDesc2 = "The specular brightness can be adjusted per-pixel using the alpha channel of 'Diffuse1'";
	bool   EffectAlt_UV = true;
>;

#include "_baseinc.fx"
#include "_vsshared.fx"
#include "_psshared.fx"

////////////////////////////////////////
////	Vertex shaders				////
////////////////////////////////////////
// Directional layer: tangent-space light + half vectors to the cube
// normalizer, light color * selfshadow in COLOR0, hemisphere in COLOR1.
VS_OUTPUT_PHONG vsTanPhongDir (const VS_INPUT_SEGTAN In, uniform bool uvxform)
{
	VS_OUTPUT_PHONG		Out = (VS_OUTPUT_PHONG)0;
	BASIS				worldbasis;

	Out.Pos  = mul (In.Pos, MatWorldViewProj);

	float3	eye_vec = CalcNormalizedEyeVector (In.Pos);
	float3	halfangle = DirLightVector + eye_vec;

	worldbasis = CalcWorldBasis (In);
	float3	lightvec_tangent = TransformLightByBasis (DirLightVector, worldbasis);
	float3	halfangle_tangent = TransformLightByBasis (halfangle, worldbasis);

	float	selfshadowterm = CalcSelfShadowTerm (DirLightVector, In);

	Out.Tex0 = CalcAnimatedUV(In.Tex0, uvxform);
	Out.Tex1 = lightvec_tangent;
	Out.Tex2 = halfangle_tangent;
	Out.Tex3 = CalcAnimatedUV(In.Tex0, uvxform);
	Out.Diff = float4((float3)DirLightColor * selfshadowterm, 1);
	Out.Spec = HemicolorFromVectorY (dot(In.Norm, MatWorld._12_22_32_42));

	Out.Fog = CalcFogSegmented (In.Pos);

	return (Out);
}

// Point-light layer.
VS_OUTPUT_PHONG vsTanPhongPoint (const VS_INPUT_SEGTAN In, uniform bool uvxform)
{
	VS_OUTPUT_PHONG		Out = (VS_OUTPUT_PHONG)0;

	Out.Pos  = mul (In.Pos, MatWorldViewProj);

	float3	vertworld	= mul (In.Pos, MatWorld);
	float3	lightvec	= PointLightCoord - vertworld;
	float	atten		= CalcPointLightAttenuation (lightvec);

	float3	eye_vec = CalcEyeVectorNN (In.Pos);
	float3	halfangle = lightvec + eye_vec;

	BASIS	worldbasis = CalcWorldBasis (In);
	float3	lightvec_tangent = TransformLightByBasisNN (lightvec, worldbasis);
	float3	halfangle_tangent = TransformLightByBasisNN (halfangle, worldbasis);

	float	selfshadowterm = CalcSelfShadowTerm (normalize(lightvec), In);

	Out.Tex0 = CalcAnimatedUV(In.Tex0, uvxform);
	Out.Tex1 = lightvec_tangent;
	Out.Tex2 = halfangle_tangent;
	Out.Tex3 = CalcAnimatedUV(In.Tex0, uvxform);
	Out.Diff = float4((float3)PointLightColor * selfshadowterm * atten, 1);
	Out.Spec = float4(0,0,0,0);

	Out.Fog = CalcFogSegmented (In.Pos);

	return (Out);
}

// Spotlight diffuse pass (per-pixel depth test): tangent DOT3 protocol.
VS_OUTPUT_PHONGPROJ vsTanDot3SpotDepthPS (const VS_INPUT_SEGTAN In, uniform bool usedifftex, uniform bool clip, uniform bool uvxform)
{
	VS_OUTPUT_PHONGPROJ		Out = (VS_OUTPUT_PHONGPROJ)0;

	Out.Pos  = mul (In.Pos, MatWorldViewProj);

	float3	vertworld	= mul (In.Pos, MatWorld);
	float3	lightvec	= PointLightCoord - vertworld;

	BASIS		worldbasis = CalcWorldBasis (In);
	float3		lightvec_tangent = TransformLightByBasis (lightvec, worldbasis);
	float		selfshadowterm = CalcSelfShadowTerm (lightvec, In);
	Out.Diff = ColorFromVector (lightvec_tangent) * selfshadowterm;

	Out.Tex0 = CalcAnimatedUV(In.Tex0, uvxform);
	Out.Tex1 = dot (float4(vertworld,1), VecDepthMaskPlane) - DEPTHTEST_8BIT_OFFSET;
	Out.Tex2 = mul (float4(vertworld,1), SpotLightProjMatrix) * float4(1,-1,1,1);
	if (clip)				Out.Tex3 = dot(In.Pos, VecTexClipPlane);
	else if (usedifftex)	Out.Tex3 = CalcAnimatedUV(In.Tex0, uvxform);
	Out.Spec = float4(1,1,1,1);

	return (Out);
}

// Spotlight specular pass: half vector to the cube normalizer, projected
// spot + depth gradient for the per-pixel test.
VS_OUTPUT_PHONGPROJ2 vsTanPhongSpecSpotDepth (const VS_INPUT_SEGTAN In, uniform bool uvxform)
{
	VS_OUTPUT_PHONGPROJ2		Out = (VS_OUTPUT_PHONGPROJ2)0;

	Out.Pos  = mul (In.Pos, MatWorldViewProj);

	float3	vertworld	= mul (In.Pos, MatWorld);
	float3	lightvec	= PointLightCoord - vertworld;

	float3	eye_vec = CalcEyeVectorNN (In.Pos);
	float3	halfangle = lightvec + eye_vec;

	BASIS	worldbasis = CalcWorldBasis (In);
	float3	halfangle_tangent = TransformLightByBasisNN (halfangle, worldbasis);

	float	selfshadowterm = CalcSelfShadowTerm (normalize(lightvec), In);

	Out.Tex0 = CalcAnimatedUV(In.Tex0, uvxform);
	Out.Tex1 = halfangle_tangent;
	Out.Tex2 = mul (float4(vertworld,1), SpotLightProjMatrix) * float4(1,-1,1,1);
	Out.Tex3 = dot (float4(vertworld,1), VecDepthMaskPlane) - DEPTHTEST_8BIT_OFFSET;

	Out.Diff = float4((float3)PointLightColor * selfshadowterm, 1);

	return (Out);
}

shared VERTEXSHADER		vscTanPhongDir				= compile vs_1_1 vsTanPhongDir(false);
shared VERTEXSHADER		vscTanPhongPoint			= compile vs_1_1 vsTanPhongPoint(false);
shared VERTEXSHADER		vscTanDot3SpotDepthPS		= compile vs_1_1 vsTanDot3SpotDepthPS(true, false, false);
shared VERTEXSHADER		vscTanPhongSpecSpotDepth	= compile vs_1_1 vsTanPhongSpecSpotDepth(false);

shared VERTEXSHADER		vscTanPhongDirUV			= compile vs_1_1 vsTanPhongDir(true);
shared VERTEXSHADER		vscTanPhongPointUV			= compile vs_1_1 vsTanPhongPoint(true);
shared VERTEXSHADER		vscTanPhongSpecSpotDepthUV	= compile vs_1_1 vsTanPhongSpecSpotDepth(true);

////////////////////////////////////////
////	Standard					////
////////////////////////////////////////
technique TSegTanPhong_Std
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
#ifdef TEX_UVXFORM
		VertexShader = (vscTanPhongDirUV);
#else
		VertexShader = (vscTanPhongDir);
#endif
		PixelShader = (ps11Phong);

		PixelShaderConstantF[5] = (AmbientColor);

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
#ifdef TEX_UVXFORM
		VertexShader = (vscTanPhongPointUV);
#else
		VertexShader = (vscTanPhongPoint);
#endif
		PixelShader = (ps11Phong);

		PixelShaderConstantF[5] = float4(0,0,0,0);

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
		VertexShader = (vscTanDot3SpotDepthPS);
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
#ifdef TEX_UVXFORM
		VertexShader = (vscTanPhongSpecSpotDepthUV);
#else
		VertexShader = (vscTanPhongSpecSpotDepth);
#endif
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
technique TSegTanPhong_Proj
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
		int passrules = 0;
	>
	{
#ifdef TEX_UVXFORM
		VertexShader = (vscPostBlackT1UV);
#else
		VertexShader = (vscPostBlackT1);
#endif
		PixelShader = NULL;

		SetTextureSh(0, TexDiffuse1,	sampLinearWrap2D)

		RSAlphaMode(FALSE, ONE, ZERO)
		SpecularEnable	= FALSE;

		TSSColor(0, SelectArg2,	Texture, Diffuse)
		TSSAlpha(0, Modulate,	Texture, Diffuse)
		TSSEnd(1)

		FogEnable	= TRUE;
	}
}

////////////////////////////////////////
////	Depth mask					////
////////////////////////////////////////
#include "_tdepth.fx"
