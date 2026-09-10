////////////////////////////////////////////////////////////////
//	_baseinc.fx — OpenNova authored shader set, shared include.
//
//	Authored from scratch against the witnessed retail effect
//	contract [orig: HLSLEffect_LoadFromFile @ 0x5ae690;
//	HLSLEffect_ParsePassData @ 0x5ae120]. Every identifier the
//	ENGINE resolves is contract and must keep its exact retail
//	name and type: the annotation keys (ttype/usevs/useps/
//	useffplights, fogmode/zmode/amode/passrules/passsetup), the
//	named parameters (MatViewProj, SkinWorldMatrixArray,
//	TexDiffuse1, ...), the vertex input semantics, and the
//	compile-time defines the loader passes (FFPTRANSPOSE,
//	TRILINEAR/ANISO, TEX_UVXFORM, and _ffp.fx's variant set).
//	Everything else (helper names, comments, layout) is ours.
////////////////////////////////////////////////////////////////

////////////////////////////////////////
////	Annotation value enums		////
////////////////////////////////////////
// fogmode bits 0-1 pick the fog color row (scene/gray/black/white),
// bits 2-3 the fog parameter set [orig: CD3DDevice_SetFogAndBlendMode
// @ 0x677740]. The SHADER rows shift by 8 unless the loader compiles
// with homogeneous-Z fog.
#define	FOGMODE_NORMAL		0
#define	FOGMODE_NORMALSET	1
#define	FOGMODE_NORMALADD	2
#define	FOGMODE_NORMALWHT	3
#define	FOGMODE_LITE		4
#define	FOGMODE_LITESET		5
#define	FOGMODE_LITEADD		6
#define	FOGMODE_LITEWHT		7

#ifdef FOG_USING_HOMOZ
#define	FOGMODE_SHADER		0
#define	FOGMODE_SHADERSET	1
#define	FOGMODE_SHADERADD	2
#define	FOGMODE_SHADERWHT	3
#else
#define	FOGMODE_SHADER		8
#define	FOGMODE_SHADERSET	9
#define	FOGMODE_SHADERADD	10
#define	FOGMODE_SHADERWHT	11
#endif

#define	ZMODE_NORMAL		0x00
#define	ZMODE_NOWRITE		0x01
#define	ZMODE_NOREAD		0x02

#define	AMODE_NORMAL		0x00
#define	AMODE_CLIP			0x01
#define	AMODE_DEPTHTEST		0x02

// Technique classes -> the six registry pass blocks
// [orig: HLSLEffect_ParsePassData @ 0x5ae120].
#define	TECHNIQUE_NORMAL		0
#define	TECHNIQUE_PROJSHAD		1
#define	TECHNIQUE_DEPTHMASK		2
#define	TECHNIQUE_CLIP			3
#define	TECHNIQUE_GLOW			4
#define	TECHNIQUE_MATCHTERRAIN	5

// Per-pass scheduling rules (pass metadata bits 1-5).
#define	PASSRULE_POINTLIGHT_VARIATIONS	0x0001
#define	PASSRULE_ONCE_PER_POINTLIGHT	0x0002
#define	PASSRULE_ONCE_PER_SPOTLIGHT		0x0004
#define	PASSRULE_IF_NO_SPOTLIGHTS		0x0008
#define	PASSRULE_IF_SPOTLIGHTS			0x0010

// Per-pass setup requests.
#define	PASSSETUP_SET_OBJSPACELIGHT_DIR	0x0001

// Bias subtracted from the depth-gradient coordinate so a surface
// does not spotlight-shadow itself (front faces write the mask).
#define	DEPTHTEST_8BIT_OFFSET		(1.50f/255.0f)

////////////////////////////////////////
////	State-block macros			////
////////////////////////////////////////
#define	TSSColor(s,op,arg1,arg2)		ColorOp[s] = op; ColorArg1[s] = arg1; ColorArg2[s] = arg2;
#define	TSSAlpha(s,op,arg1,arg2)		AlphaOp[s] = op; AlphaArg1[s] = arg1; AlphaArg2[s] = arg2;
#define	TSSEnd(s)						ColorOp[s] = Disable; AlphaOp[s] = Disable;
#define	RSAlphaMode(enable,src,dest)	AlphaBlendEnable = enable; SrcBlend = src; DestBlend = dest;
#define	SetTextureSh(stage,t,s)			Texture[stage] = (t); Sampler[stage] = <s>; TexCoordIndex[stage] = stage; TextureTransformFlags[stage] = DISABLE;
#define	SetProjTexSh(stage,t,s)			Texture[stage] = (t); Sampler[stage] = <s>; TexCoordIndex[stage] = stage; TextureTransformFlags[stage] = PROJECTED;
#define	SetNullTexSh(stage,s)			Texture[stage] = NULL; Sampler[stage] = <s>; TexCoordIndex[stage] = stage; TextureTransformFlags[stage] = DISABLE;

////////////////////////////////////////
////	Fixed constants				////
////////////////////////////////////////
// Object-space normal maps bake a mirrored X; flip the light into the
// same handedness before the per-pixel dot.
static const float3	ObjSpaceFixup = float3(-1,1,1);

static const int	MAX_SKIN_MATRICES = 16;
static const int	MAX_POINTLIGHTS = 4;

// Self-shadow ramp: clamp(dot * mult + add, 0, 1) — a hard-ish terminator
// that keeps bumped surfaces from lighting through themselves.
static const float SelfShadowMult	= 10.0;
static const float SelfShadowAdd	= 0.5;
static const float3	ColorSrcZero	= {0.0f, 0.0f, 0.0f};
static const float3	ColorSrcOne		= {1.0f, 1.0f, 1.0f};

////////////////////////////////////////
////	Engine-bound parameters		////
////////////////////////////////////////
// Names, types and shared-ness are the engine contract: the loader
// resolves them by name and the batch flusher binds them per frame /
// per draw [orig: apply_shader_parameters @ 0x58db80].
shared float4x4	MatViewProj;		// view * proj
shared float3	CameraPos;			// world-space eye

float4x4		MatWorld;			// world matrix
float4x4		MatWorldViewProj;	// world * view * proj
float4x3		MatWorldInvTrans;	// inverse transpose of world

#ifdef FFPTRANSPOSE
shared float4x4	MatTexClipPlane;	// camera-space clip ramp (FFP path)
#else
shared float4x3	MatTexClipPlane;
#endif

#ifdef FFPTRANSPOSE
float4x4	MatTexCoord1;			// authored UV transform
#else
float4x3	MatTexCoord1;
#endif

float4			VecTexClipPlane;	// model-space clip plane (VS path)

shared float	FogStart;
shared float	FogRangeRecip;

////////////////////////////////////////
////	Lighting parameters			////
////////////////////////////////////////
shared float3	DirLightVector	= {0.577, 0.577, 0.577};	// TOWARDS the light

float3			HemiGroundColor	= {0.5, 0, 0};
float3			HemiSkyColor	= {0, 0, 0.5};
float3			AmbientColor	= {0, 0, 0};
float4			DirLightColor	= {1, 1, 0, 1};

float3			PointLightCoord;
float4			PointLightColor;
float3			PointLightAtten;

float4x4		SpotLightProjMatrix;	// spotlight texture projection
float4			VecDepthMaskPlane;

float4			PointLightCoordArray[MAX_POINTLIGHTS];
float4			PointLightColorArray[MAX_POINTLIGHTS];
float4			PointLightAttenArray[MAX_POINTLIGHTS];
int				CurNumPointLights = 0;

float3			SelfLumColor = {1.0f, 1.0f, 1.0f};

float4x4		TexProjMatrix1;			// terrain-match texture projection

////////////////////////////////////////
////	Textures					////
////////////////////////////////////////
texture			TexDiffuse1;
texture			TexDiffuse2;
texture			TexNormal1;
texture			TexCurProj;
shared texture	TexCubeNormalize;
shared texture	TexPhongMap;
shared texture	TexClip1D;
shared texture	TexDepthGradWrite;		// depth gradient, write side
shared texture	TexDepthGradTest;		// depth gradient, test side

////////////////////////////////////////
////	Misc parameters				////
////////////////////////////////////////
float			AlphaGenValue			= 0.5f;
shared float3	ColorSrcGlobalGain		= {1.0f, 1.0f, 1.0f};

////////////////////////////////////////
////	Skinning palette			////
////////////////////////////////////////
float4x3		SkinWorldMatrixArray[MAX_SKIN_MATRICES];
float4			SkinModelLightArray[MAX_SKIN_MATRICES];		// per-bone model-space light (vector or coord)

////////////////////////////////////////
////	Vertex IO structures		////
////////////////////////////////////////
struct VS_OUTPUT
{
    float4	Pos  : POSITION;
    float4	Diff : COLOR0;
    float4	Spec : COLOR1;
    float2	Tex0 : TEXCOORD0;
    float2	Tex1 : TEXCOORD1;
    float	Fog  : FOG;
};

struct VS_OUTPUTDEP
{
    float4	Pos  : POSITION;
    float2	Tex0 : TEXCOORD0;
};

struct VS_OUTPUTDEPFOG
{
    float4	Pos  : POSITION;
    float2	Tex0 : TEXCOORD0;
    float	Fog  : FOG;
};

struct VS_OUTPUT_PHONG
{
    float4	Pos  : POSITION;
    float4	Diff : COLOR0;
    float4	Spec : COLOR1;
    float2	Tex0 : TEXCOORD0;
    float3	Tex1 : TEXCOORD1;
    float3	Tex2 : TEXCOORD2;
    float2	Tex3 : TEXCOORD3;
    float	Fog  : FOG;
};

struct VS_OUTPUT_PROJ2
{
	float4	Pos  : POSITION;
	float4	Diff : COLOR0;
	float4	Spec : COLOR1;
	float2	Tex0 : TEXCOORD0;
	float4	Tex1 : TEXCOORD1;
	float	Fog  : FOG;
};

struct VS_OUTPUT_PHONGPROJ
{
	float4	Pos  : POSITION;
	float4	Diff : COLOR0;
	float4	Spec : COLOR1;
	float2	Tex0 : TEXCOORD0;
	float2	Tex1 : TEXCOORD1;
	float4	Tex2 : TEXCOORD2;
	float2	Tex3 : TEXCOORD3;
};

struct VS_OUTPUT_PHONGPROJ2
{
	float4	Pos  : POSITION;
	float4	Diff : COLOR0;
	float2	Tex0 : TEXCOORD0;
	float3	Tex1 : TEXCOORD1;
	float4	Tex2 : TEXCOORD2;
	float2	Tex3 : TEXCOORD3;
};

struct VS_INPUT_SEGTAN
{
	float4	Pos			: POSITION;
	float3	Norm		: NORMAL;
	float2	Tex0		: TEXCOORD0;
	float3	Tangent		: TANGENT;
	float3	Binormal	: BINORMAL;
};

struct VS_INPUT_SEG1
{
	float4	Pos			: POSITION;
	float3	Norm		: NORMAL;
	float2	Tex0		: TEXCOORD0;
};

struct VS_INPUT_SEG2
{
	float4	Pos			: POSITION;
	float3	Norm		: NORMAL;
	float2	Tex0		: TEXCOORD0;
	float2	Tex1		: TEXCOORD1;
};

struct VS_INPUT_SKIN1
{
    float4	Pos				: POSITION;
    float4	BlendWeights	: BLENDWEIGHT;
    float4	BlendIndices	: BLENDINDICES;
    float3	Norm			: NORMAL;
    float2	Tex0			: TEXCOORD0;
};

struct VS_INPUT_SKIN2
{
    float4	Pos				: POSITION;
    float4	BlendWeights	: BLENDWEIGHT;
    float4	BlendIndices	: BLENDINDICES;
    float3	Norm			: NORMAL;
    float2	Tex0			: TEXCOORD0;
    float2	Tex1			: TEXCOORD1;
};

struct VS_INPUT_SKINTAN1
{
    float4	Pos				: POSITION;
    float4	BlendWeights	: BLENDWEIGHT;
    float4	BlendIndices	: BLENDINDICES;
    float3	Norm			: NORMAL;
    float2	Tex0			: TEXCOORD0;
	float3	Tangent			: TANGENT;
	float3	Binormal		: BINORMAL;
};

struct BASIS
{
	float3	vecx;
	float3	vecy;
	float3	vecz;
};

////////////////////////////////////////
////	Helpers						////
////////////////////////////////////////
// Pack a signed unit vector into 0..1 color range (DOT3 / cube-lookup input).
float4 ColorFromVector (in float3 vec)
{
	return (float4(vec * 0.5 + 0.5, 1));
}

float4 ColorFromVectorScale (in float3 vec, in float3 scale)
{
	return (float4(vec * 0.5 * scale + 0.5, 1));
}

// Tangent basis rotated into world space (rigid meshes).
BASIS CalcWorldBasis (const VS_INPUT_SEGTAN In)
{
	BASIS basis;
	basis.vecz = mul (In.Norm,   (float3x3)MatWorld);
	basis.vecx = mul (In.Tangent,(float3x3)MatWorld);
	basis.vecy = mul (In.Binormal, (float3x3)MatWorld);
	return (basis);
}

// World vector -> tangent space, normalized / not.
float3 TransformLightByBasis (in float3 light, in BASIS basis)
{
	float3 lightout;
	lightout.x = dot (basis.vecx, light);
	lightout.y = dot (basis.vecy, light);
	lightout.z = dot (basis.vecz, light);
	lightout = normalize(lightout);
	return (lightout);
}

float3 TransformLightByBasisNN (in float3 light, in BASIS basis)
{
	float3 lightout;
	lightout.x = dot (basis.vecx, light);
	lightout.y = dot (basis.vecy, light);
	lightout.z = dot (basis.vecz, light);
	return (lightout);
}

// Hemisphere term from the world-space normal's Y.
float4 HemicolorFromVectorY (float y)
{
	float	up = y * 0.5 + 0.5;
	return (float4(lerp(HemiGroundColor, HemiSkyColor, up), 1));
}

float CalcSelfShadowTerm (float3 worldlightvector, const VS_INPUT_SEGTAN In)
{
	float3	worldnormal = mul (In.Norm, (float3x3)MatWorld);
	return (clamp ((dot (worldlightvector, worldnormal) * SelfShadowMult) + SelfShadowAdd, 0, 1));
}

float CalcSelfShadowTerm (float3 xspacelightvector, float3 xspacevertexnormal)
{
	return (clamp ((dot (xspacelightvector, xspacevertexnormal) * SelfShadowMult) + SelfShadowAdd, 0, 1));
}

float3 CalcNormalizedEyeVector (float4 modelspacepos)
{
	float3	vertex_world = mul (modelspacepos, MatWorld);
	float3	vec = CameraPos - vertex_world;
	vec = normalize (vec);
	return (vec);
}

float3 CalcEyeVectorNN (float4 modelspacepos)
{
	float3	vertex_world = mul (modelspacepos, MatWorld);
	float3	vec = CameraPos - vertex_world;
	return (vec);
}

// The classic three-coefficient attenuation, computed without DST.
float CalcPointLightAttenuation (float3 lightvec)
{
	float		distsqrd	= dot (lightvec, lightvec);
	float		recip		= rsqrt (distsqrd);
	float		atten = PointLightAtten.x +
						PointLightAtten.y * distsqrd * recip +
						PointLightAtten.z * distsqrd;
	return (1.0 / atten);
}

float CalcPointLightAttenuationAndLength (float3 lightvec, out float reciplength)
{
	float		distsqrd	= dot (lightvec, lightvec);
	reciplength	= rsqrt (distsqrd);
	float		atten = PointLightAtten.x +
						PointLightAtten.y * distsqrd * reciplength +
						PointLightAtten.z * distsqrd;
	return (1.0 / atten);
}

// Four-bone matrix-palette skinning. The last influence takes the
// remaining weight; indices arrive as D3DCOLOR to survive hardware
// without UBYTE4 vertex support.
void CalcSkinWorldPosAndNormal (in VS_INPUT_SKIN1 In, out float3 pos, out float3 norm, out int4 indexvector, uniform int NumBones, uniform bool skinnormal)
{
	float       lastweight = 0.0f;

	indexvector = D3DCOLORtoUBYTE4 (In.BlendIndices);

	float	BlendWeightsArray[4] = (float[4]) In.BlendWeights;
	int		IndexArray[4]        = (int[4])   indexvector;

	pos = 0.0f;
	norm = 0.0f;

	for (int bone=0; bone<NumBones-1; bone++)
	{
		lastweight = lastweight + BlendWeightsArray[bone];

		pos  += mul(In.Pos, SkinWorldMatrixArray[IndexArray[bone]]) * BlendWeightsArray[bone];
		if (skinnormal)
		{
			norm += mul(In.Norm, SkinWorldMatrixArray[IndexArray[bone]]) * BlendWeightsArray[bone];
		}
	}
	lastweight = 1.0f - lastweight;

	pos  += (mul(In.Pos, SkinWorldMatrixArray[IndexArray[NumBones-1]]) * lastweight);
	if (skinnormal)
	{
		norm += (mul(In.Norm, SkinWorldMatrixArray[IndexArray[NumBones-1]]) * lastweight);
		norm = normalize(norm);
	}
	else
	{
		norm = mul(In.Norm, SkinWorldMatrixArray[IndexArray[0]]);
	}
}

// Linear fog factor from the projected depth row.
float CalcFogSkinned (float3 worldpos)
{
	float	fogz = dot(float4(worldpos.xyz, 1.0f), MatViewProj._13_23_33_43);
	return (1 - (fogz - FogStart) * FogRangeRecip);
}

float CalcFogSegmented (float4 modelpos)
{
	float	fogz = dot(modelpos, MatWorldViewProj._13_23_33_43);
	return (1 - (fogz - FogStart) * FogRangeRecip);
}

// Authored UV transform, applied only in the #UV twin compile.
float2 CalcAnimatedUV (float2 uv_in, uniform bool uvxform)
{
	if (uvxform)
	{
		return (mul(float3(uv_in,1), (float3x2)MatTexCoord1));
	}
	else
	{
		return (uv_in);
	}
}

////////////////////////////////////////
////	Samplers					////
////////////////////////////////////////
// The loader picks the filter tier by define [orig: HLSLEffect_LoadFromFile
// @ 0x5ae690, HLSLEffect_TextureFilterMode @ 0x27e5698].
#ifdef ANISO

sampler sampLinearWrap2D = sampler_state
{
	MinFilter = ANISOTROPIC;
	MagFilter = LINEAR;
	MipFilter = LINEAR;
	MaxAnisotropy = 2;
	AddressU = Wrap;		AddressV = Wrap;
};
sampler sampLinearWrap3D = sampler_state
{
	MinFilter = LINEAR;
	MagFilter = LINEAR;
	MipFilter = POINT;
	AddressU = Wrap;		AddressV = Wrap;		AddressW = Wrap;
};
sampler sampLinearClamp2D = sampler_state
{
	MinFilter = ANISOTROPIC;
	MagFilter = LINEAR;
	MipFilter = LINEAR;
	MaxAnisotropy = 2;
	AddressU = Clamp;		AddressV = Clamp;
};

#elif defined ( TRILINEAR )

sampler sampLinearWrap2D = sampler_state
{
	MinFilter = LINEAR;
	MagFilter = LINEAR;
	MipFilter = LINEAR;
	MaxAnisotropy = 2;
	AddressU = Wrap;		AddressV = Wrap;
};
sampler sampLinearWrap3D = sampler_state
{
	MinFilter = LINEAR;
	MagFilter = LINEAR;
	MipFilter = POINT;
	AddressU = Wrap;		AddressV = Wrap;		AddressW = Wrap;
};
sampler sampLinearClamp2D = sampler_state
{
	MinFilter = LINEAR;
	MagFilter = LINEAR;
	MipFilter = LINEAR;
	MaxAnisotropy = 2;
	AddressU = Clamp;		AddressV = Clamp;
};

#else

sampler sampLinearWrap2D = sampler_state
{
	MinFilter = LINEAR;
	MagFilter = LINEAR;
	MipFilter = POINT;
	AddressU = Wrap;		AddressV = Wrap;
};
sampler sampLinearWrap3D = sampler_state
{
	MinFilter = LINEAR;
	MagFilter = LINEAR;
	MipFilter = POINT;
	AddressU = Wrap;		AddressV = Wrap;		AddressW = Wrap;
};
sampler sampLinearClamp2D = sampler_state
{
	MinFilter = LINEAR;
	MagFilter = LINEAR;
	MipFilter = POINT;
	MaxAnisotropy = 2;
	AddressU = Clamp;		AddressV = Clamp;
};

#endif
