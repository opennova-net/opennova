// The shaders (ADR 0046 d7/d8, the blank factories): OpenNova's own HLSL effects, written here from
// scratch for the contract the original renderer reads an effect by, never from its shipped effects.
//
// The renderer registers its effects as it starts [orig: HLSLEffect_InitAndLoadAll @ 0x5B0080]: _ffp.fx,
// opened by name and compiled once for each of its twelve fixed-function tags and their #UV twins
// [orig: HLSLEffect_InitFixedFunctionShaders @ 0x5AF790], then every other .fx of the mounted archives
// whose name does not start with '_', each under its EffectTag [orig: HLSLEffect_LoadAllFromPFFArchive
// @ 0x5AFED0 -> HLSLEffect_LoadFromFile @ 0x5AE690]. A model's material names a tag; one no effect
// registered draws with the registry's first entry, and with none registered at all (a game with no
// _ffp.fx and no effect) that entry holds no pass and no model draws [orig: Material_ConvertDefinition
// @ 0x5B0664..0x5B0672; CRenderBatchQueue_FlushBatches @ 0x5DA220..0x5DA22B].
//
// What the contract fixes, and every effect here keeps:
// - The EffectInfo annotations: EffectTag, EffectName, EffectAlt_UV (a TEX_UVXFORM twin registered as
//   the tag and #UV) [orig: HLSLEffect_LoadFromFile @ 0x5AE899..0x5AE9BC, the twin @ 0x5AEA03].
// - The defines the loader passes: FFPTRANSPOSE always, TRILINEAR or ANISO for the texture filter,
//   TEX_UVXFORM for a twin; _ffp.fx's TEX_SINGLE/TEX_MULTIPLE, SELFLUM, BLEND_NONE/ALPHA/ADD [orig:
//   HLSLEffect_LoadFromFile @ 0x5AE6D1..0x5AE726; HLSLEffect_InitFixedFunctionShaders @ 0x5AFB33..0x5AFB9E].
// - The technique annotations usevs, useps, useffplights and ttype (NORMAL 0, PROJSHAD 1, CLIP 3,
//   MATCHTERRAIN 5), the first admitted technique of each kind taking its slot, a missing CLIP falling
//   back to NORMAL; the pass annotations fogmode, zmode, amode, passrules [orig:
//   HLSLEffect_ParsePassData @ 0x5AE120; HLSLEffect_LoadFromFile @ 0x5AEB33..0x5AEBC8].
// - The parameters the renderer sets by name [orig: HLSLEffect_LoadFromFile @ 0x5AF101..0x5AF6AB]. Those
//   it sets once a frame through whichever effect draws first are shared through the effect pool, and
//   every effect declares them alike: the view and fog [orig: Material_ApplyShaderParameters @
//   0x58E0BD..0x58E26F], the sun's direction and the water mirror's clip matrix [orig:
//   Render_SetupEntityLightingAndShaderConstants @ 0x5D98D6..0x5D99A6], and the eight system
//   textures it binds [orig: Material_ApplyShaderParameters @ 0x58DEAE..0x58E021]. TexAngleMap is left
//   out: its slot is bound @ 0x58E023..0x58E04E from Render_AngleMapTexture, which nothing creates
//   (that read is its one reference), so an effect sharing it hands the renderer a texture that is not
//   there (GTexture_UpdateFrameStats @ 0x684515 then faults).
// - A lamp pass (passrules 2, once per point light, added) reads PointLightCoord (world space),
//   PointLightColor and PointLightAtten (its constant, linear and squared fall-off terms) [orig:
//   CRenderBatchQueue_FlushBatches @ 0x5DA84F..0x5DAA07; Light_GetPointLightParams @ 0x5A9180].
// - The water mirror's CLIP technique tests alpha at 128 (amode 1) against TexClip1D, by the camera-space
//   plane MatTexClipPlane (fixed function) or the model-space VecTexClipPlane (a vertex shader) [orig:
//   CRenderBatchQueue_FlushBatches @ 0x5DA384..0x5DA399; Render_SetupEntityLightingAndShaderConstants @
//   0x5D99B5..0x5D9A2D]; the skinned effects have none (a person is never clipped).
// - MATCHTERRAIN, a crouching or prone person's pre-pass, reads the terrain page under them as
//   TexCurProj through TexProjMatrix1 [orig: CRenderBatchQueue_FlushBatches @ 0x5DA775..0x5DA841].
// The spot-light passes and DEPTHMASK are not written: their one producer, the spot projector, is never
// called in JO (docs/render/render-lighting-re.md, LightPool_SpawnSpotProjectorEffect @ 0x5A9FD0).
//
// The look is the original's fixed-function family: a texture lit by the sun over the sky's hemisphere,
// times two (MODULATE2X), the Phong shaders' highlight strength in the diffuse texture's alpha, a detail
// texture on the second UV set at two times, mid grey neutral.
#include "blank_makers.h"

#include <initializer_list>
#include <utility>

#include <base/io/strutil.h>
#include <editor/documents/text_types.h>
#include <editor/project/project_files.h>
#include <formats/scr/scr.h>
#include <runtime/renderer/shader_effect_info.h>

namespace opennova::editor {

namespace {

constexpr const char kShared[] = R"fx(// What the renderer sets once a frame through whichever effect draws first: shared through the
// effect pool, and declared alike in every effect so each reaches them all.
shared float4x4 MatViewProj;
shared float3 CameraPos;
shared float FogStart;
shared float FogRangeRecip;
shared float3x3 MatCamToWorldRot;
shared float3x3 MatRotSpecular;
shared float4x4 MatTexClipPlane;
shared float3 DirLightVector;
shared float3 ColorSrcGlobalGain;
shared texture TexCubeNormalize;
shared texture TexCubeEnvironment;
shared texture TexCubeRotSpecular;
shared texture TexPhongMap;
shared texture TexClip1D;
shared texture TexSpot2D;
shared texture TexDepthGradWrite;
shared texture TexDepthGradTest;
)fx";

constexpr const char kFilters[] = R"fx(#if defined(ANISO)
#define ON_MIN_FILTER MinFilter = ANISOTROPIC; MaxAnisotropy = 2; MipFilter = LINEAR;
#elif defined(TRILINEAR)
#define ON_MIN_FILTER MinFilter = LINEAR; MipFilter = LINEAR;
#else
#define ON_MIN_FILTER MinFilter = LINEAR; MipFilter = POINT;
#endif
)fx";

constexpr const char kClipSampler[] = R"fx(// The water mirror's clip: the texture is clear left of its middle and solid right of it, and the
// alpha test keeps what reads solid.
sampler ClipSampler = sampler_state {
	Texture = <TexClip1D>; MinFilter = POINT; MagFilter = POINT; MipFilter = NONE; AddressU = CLAMP; AddressV = CLAMP;
};
)fx";

constexpr const char kFfp[] = R"fx(// _ffp.fx: OpenNova's fixed-function effect.
//
// The renderer opens this file by name as it starts and compiles it once for each of its
// fixed-function shader tags: single or multi texture (TEX_SINGLE, TEX_MULTIPLE), self-lit or lit
// (SELFLUM), opaque, alpha-blended or additive (BLEND_NONE, BLEND_ALPHA, BLEND_ADD), each again with
// TEX_UVXFORM for an animated UV (FF_ST_OP, FF_MT_AB_LUM, FF_ST_OP#UV ...). Without it no model draws.
// Made by the OpenNova Editor (engine/editor/blank/blank_shader.cpp).

string EffectInfo <
	string EffectName = "OpenNova fixed function";
	bool EffectAlt_UV = true;
>;

@SHARED@
// Set for each draw.
float4x4 MatWorld;
float4x4 MatTexCoord1;
float3 AmbientColor;
float3 SelfLumColor;
float AlphaGenValue;
texture TexDiffuse1;
texture TexDiffuse2;

@FILTERS@
sampler DiffuseSampler1 = sampler_state {
	Texture = <TexDiffuse1>; ON_MIN_FILTER MagFilter = LINEAR; AddressU = WRAP; AddressV = WRAP;
};
sampler DiffuseSampler2 = sampler_state {
	Texture = <TexDiffuse2>; ON_MIN_FILTER MagFilter = LINEAR; AddressU = WRAP; AddressV = WRAP;
};
@CLIP_SAMPLER@
#if defined(BLEND_ALPHA)
#define ON_BLEND AlphaBlendEnable = TRUE; SrcBlend = SRCALPHA; DestBlend = INVSRCALPHA;
#define ON_FOG 1
#elif defined(BLEND_ADD)
#define ON_BLEND AlphaBlendEnable = TRUE; SrcBlend = ONE; DestBlend = ONE;
#define ON_FOG 2
#else
#define ON_BLEND AlphaBlendEnable = FALSE; SrcBlend = ONE; DestBlend = ZERO;
#define ON_FOG 0
#endif

#if defined(FFPTRANSPOSE)
#define ON_WORLD WorldTransform[0] = (transpose(MatWorld));
#define ON_UV_MATRIX(stage) TextureTransform[stage] = (transpose(MatTexCoord1));
#define ON_CLIP_MATRIX TextureTransform[1] = (transpose(MatTexClipPlane));
#else
#define ON_WORLD WorldTransform[0] = (MatWorld);
#define ON_UV_MATRIX(stage) TextureTransform[stage] = (MatTexCoord1);
#define ON_CLIP_MATRIX TextureTransform[1] = (MatTexClipPlane);
#endif

#if defined(TEX_UVXFORM)
#define ON_UV(stage) TexCoordIndex[stage] = stage; TextureTransformFlags[stage] = COUNT2; ON_UV_MATRIX(stage)
#else
#define ON_UV(stage) TexCoordIndex[stage] = stage; TextureTransformFlags[stage] = DISABLE;
#endif

#if defined(SELFLUM)
// Self-lit: the lights add nothing and the colour is the material's own glow.
#define ON_MATERIAL MaterialAmbient = float4(0, 0, 0, 0); MaterialDiffuse = float4(0, 0, 0, 0); \
	MaterialEmissive = (float4(SelfLumColor * ColorSrcGlobalGain, 0));
#else
// Lit by the renderer's sun, sky and lamp lights over the ambient floor.
#define ON_MATERIAL MaterialAmbient = float4(0, 0, 0, 0); MaterialDiffuse = (float4(1, 1, 1, AlphaGenValue)); \
	MaterialEmissive = (float4(AmbientColor, 0));
#endif

technique OpenNovaFixedFunction <
	bool usevs = false;
	bool useps = false;
	bool useffplights = true;
	int ttype = 0;
> {
	pass Lit <
		int fogmode = ON_FOG;
		int zmode = 0;
		int amode = 0;
	> {
		VertexShader = NULL;
		PixelShader = NULL;
		ON_WORLD
		ON_BLEND
		ON_MATERIAL
		Lighting = TRUE;
		SpecularEnable = FALSE;
		FogEnable = TRUE;
		Sampler[0] = (DiffuseSampler1);
		ON_UV(0)
		ColorOp[0] = MODULATE2X; ColorArg1[0] = TEXTURE; ColorArg2[0] = DIFFUSE;
		AlphaOp[0] = MODULATE; AlphaArg1[0] = TEXTURE; AlphaArg2[0] = DIFFUSE;
#if defined(TEX_MULTIPLE)
		Sampler[1] = (DiffuseSampler2);
		ON_UV(1)
		ColorOp[1] = MODULATE2X; ColorArg1[1] = TEXTURE; ColorArg2[1] = CURRENT;
		AlphaOp[1] = MODULATE; AlphaArg1[1] = TEXTURE; AlphaArg2[1] = CURRENT;
		ColorOp[2] = DISABLE;
		AlphaOp[2] = DISABLE;
#else
		ColorOp[1] = DISABLE;
		AlphaOp[1] = DISABLE;
#endif
	}
}

// In the water mirror, where the model reaches below the water: the part under the plane is cut.
technique OpenNovaFixedFunctionClipped <
	bool usevs = false;
	bool useps = false;
	int ttype = 3;
> {
	pass Lit <
		int fogmode = ON_FOG;
		int zmode = 0;
		int amode = 1;
	> {
		VertexShader = NULL;
		PixelShader = NULL;
		ON_WORLD
		ON_BLEND
		ON_MATERIAL
		Lighting = TRUE;
		SpecularEnable = FALSE;
		FogEnable = TRUE;
		Sampler[0] = (DiffuseSampler1);
		ON_UV(0)
		ColorOp[0] = MODULATE2X; ColorArg1[0] = TEXTURE; ColorArg2[0] = DIFFUSE;
		AlphaOp[0] = MODULATE; AlphaArg1[0] = TEXTURE; AlphaArg2[0] = DIFFUSE;
		Sampler[1] = (ClipSampler);
		TexCoordIndex[1] = CAMERASPACEPOSITION;
		TextureTransformFlags[1] = COUNT2;
		ON_CLIP_MATRIX
		ColorOp[1] = SELECTARG2; ColorArg1[1] = TEXTURE; ColorArg2[1] = CURRENT;
		AlphaOp[1] = MODULATE; AlphaArg1[1] = TEXTURE; AlphaArg2[1] = CURRENT;
		ColorOp[2] = DISABLE;
		AlphaOp[2] = DISABLE;
	}
}

// The projected shadow: the model's silhouette in black, its texture's alpha kept.
technique OpenNovaFixedFunctionShadow <
	bool usevs = false;
	bool useps = false;
	int ttype = 1;
> {
	pass Silhouette <
		int fogmode = ON_FOG;
		int zmode = 0;
		int amode = 0;
	> {
		VertexShader = NULL;
		PixelShader = NULL;
		ON_WORLD
		ON_BLEND
		MaterialAmbient = float4(0, 0, 0, 0);
		MaterialDiffuse = (float4(0, 0, 0, AlphaGenValue));
		MaterialEmissive = float4(0, 0, 0, 0);
		Lighting = TRUE;
		SpecularEnable = FALSE;
		FogEnable = TRUE;
		Sampler[0] = (DiffuseSampler1);
		ON_UV(0)
		ColorOp[0] = SELECTARG2; ColorArg1[0] = TEXTURE; ColorArg2[0] = DIFFUSE;
		AlphaOp[0] = MODULATE; AlphaArg1[0] = TEXTURE; AlphaArg2[0] = DIFFUSE;
		ColorOp[1] = DISABLE;
		AlphaOp[1] = DISABLE;
	}
}
)fx";

constexpr const char kObjHead[] = R"fx(// @FILE@: OpenNova's @WHAT@ effect, the shader tag @TAG@.
//
// The renderer loads every effect in the game's archives as it starts and registers each under its
// EffectTag; a model's material names the tag. Made by the OpenNova Editor
// (engine/editor/blank/blank_shader.cpp).

string EffectInfo <
	string EffectTag = "@TAG@";
	string EffectName = "@NAME@";@ALT_UV@
>;

@SHARED@
// Set for each draw: the model's place, the sun (DirLightVector points at it), the sky and ground
// light, the lamp a lamp pass draws, the textures.
float4x4 MatWorld;
float4x4 MatWorldViewProj;
float4x4 MatTexCoord1;
float4 VecTexClipPlane;
float4 DirLightColor;
float3 HemiGroundColor;
float3 HemiSkyColor;
float3 PointLightCoord;
float4 PointLightColor;
float3 PointLightAtten;
texture TexDiffuse1;
texture TexDiffuse2;
texture TexNormal1;
@SKIN_DECL@
@FILTERS@
sampler DiffuseSampler1 = sampler_state {
	Texture = <TexDiffuse1>; ON_MIN_FILTER MagFilter = LINEAR; AddressU = WRAP; AddressV = WRAP;
};
sampler DiffuseSampler2 = sampler_state {
	Texture = <TexDiffuse2>; ON_MIN_FILTER MagFilter = LINEAR; AddressU = WRAP; AddressV = WRAP;
};
sampler NormalSampler = sampler_state {
	Texture = <TexNormal1>; MinFilter = LINEAR; MagFilter = LINEAR; MipFilter = LINEAR; AddressU = WRAP; AddressV = WRAP;
};
// Any direction to its unit vector, as a colour: the renderer's normalisation cube.
sampler NormalizeSampler = sampler_state {
	Texture = <TexCubeNormalize>; MinFilter = LINEAR; MagFilter = LINEAR; MipFilter = NONE;
	AddressU = CLAMP; AddressV = CLAMP; AddressW = CLAMP;
};
@CLIP_SAMPLER@@PAGE_SAMPLER@
// What a vertex hands the lighting pixel: the two UV sets, the surface frame in world space, the
// way to the eye, the way to the lamp, the up of the vertex normal and the light's fade.
struct LitVertex {
	float4 position : POSITION;
	float4 uv : TEXCOORD0;
	float3 tangent : TEXCOORD1;
	float3 binormal : TEXCOORD2;
	float3 normal : TEXCOORD3;
	float3 to_eye : TEXCOORD4;
	float3 to_lamp : TEXCOORD5;
	float2 shade : TEXCOORD6;
	float fog : FOG;
};

// What a vertex hands the fixed-function stages: its light, its UV sets.
struct ShadedVertex {
	float4 position : POSITION;
	float4 light : COLOR0;
	float2 uv0 : TEXCOORD0;
	float2 uv1 : TEXCOORD1;
	float fog : FOG;
};

float2 on_uv(float2 uv)
{
#if defined(TEX_UVXFORM)
	return mul(float3(uv, 1), (float3x2)MatTexCoord1);
#else
	return uv;
#endif
}

// The fog factor from the depth the projection gives a vertex.
float on_fog(float depth)
{
	return saturate(1 - (depth - FogStart) * FogRangeRecip);
}

// The sky light a surface facing `up` (the world y of its normal) receives.
float3 on_hemisphere(float up)
{
	return lerp(HemiGroundColor, HemiSkyColor, saturate(up * 0.5 + 0.5));
}

// A light's fade across the geometric terminator, so a face turned away is not lit through a normal
// map leaning toward the light.
float on_terminator(float3 normal, float3 to_light)
{
	return saturate(dot(normal, to_light) * 10 + 0.5);
}

// The lamp's fall-off with distance (its constant, linear and squared terms).
float on_lamp_falloff(float distance)
{
	return 1 / (PointLightAtten.x + PointLightAtten.y * distance + PointLightAtten.z * distance * distance);
}

float3 on_unit(float3 v)
{
	return texCUBE(NormalizeSampler, v).rgb * 2 - 1;
}

float3 on_bumped_normal(float2 uv, float3 tangent, float3 binormal, float3 normal)
{
	float3 n = tex2D(NormalSampler, uv).rgb * 2 - 1;
	return normalize(n.x * tangent + n.y * binormal + n.z * normal);
}

float on_pow8(float x)
{
	x = x * x;
	x = x * x;
	return x * x;
}
)fx";

constexpr const char kStaticPlace[] = R"fx(
struct ModelVertex {
	float4 position : POSITION;
	float3 normal : NORMAL;
	float2 uv0 : TEXCOORD0;
	float2 uv1 : TEXCOORD1;
	float3 tangent : TANGENT;
	float3 binormal : BINORMAL;
};

// A rigid model: its world matrix places it.
void on_place(ModelVertex v, out float4 position, out float3 world, out float3x3 frame)
{
	position = mul(v.position, MatWorldViewProj);
	world = mul(v.position, MatWorld).xyz;
	frame = (float3x3)MatWorld;
}
)fx";

constexpr const char kSkinDecl[] = R"fx(
// The bones of the strip being drawn, as the renderer uploads its palette; the terrain page under a
// crouching or prone person and its projection.
float4x3 SkinWorldMatrixArray[16];
texture TexCurProj;
float4x4 TexProjMatrix1;
)fx";

constexpr const char kSkinPlace[] = R"fx(
struct ModelVertex {
	float4 position : POSITION;
	float3 weights : BLENDWEIGHT;
	float4 bones : BLENDINDICES;
	float3 normal : NORMAL;
	float2 uv0 : TEXCOORD0;
	float2 uv1 : TEXCOORD1;
	float3 tangent : TANGENT;
	float3 binormal : BINORMAL;
};

// A skinned model: four bones blend the position (the fourth weight is what the stored three leave);
// the surface frame turns with the first bone.
void on_place(ModelVertex v, out float4 position, out float3 world, out float3x3 frame)
{
	int4 bone = D3DCOLORtoUBYTE4(v.bones);
	float last = 1 - (v.weights.x + v.weights.y + v.weights.z);
	world = mul(v.position, SkinWorldMatrixArray[bone.x]) * v.weights.x
	      + mul(v.position, SkinWorldMatrixArray[bone.y]) * v.weights.y
	      + mul(v.position, SkinWorldMatrixArray[bone.z]) * v.weights.z
	      + mul(v.position, SkinWorldMatrixArray[bone.w]) * last;
	position = mul(float4(world, 1), MatViewProj);
	frame = (float3x3)SkinWorldMatrixArray[bone.x];
}
)fx";

constexpr const char kPageSampler[] = R"fx(// The terrain page under the person.
sampler PageSampler = sampler_state {
	Texture = <TexCurProj>; MinFilter = LINEAR; MagFilter = LINEAR; MipFilter = NONE; AddressU = CLAMP; AddressV = CLAMP;
};
)fx";

constexpr const char kVertices[] = R"fx(
// The sun pass's vertex (`lamp` false) or a lamp pass's (true).
LitVertex on_lit_vertex(ModelVertex v, uniform bool lamp)
{
	LitVertex o;
	float3 world;
	float3x3 frame;
	on_place(v, o.position, world, frame);
	o.uv = float4(on_uv(v.uv0), v.uv1);
	o.tangent = mul(v.tangent, frame);
	o.binormal = mul(v.binormal, frame);
	o.normal = mul(v.normal, frame);
	o.to_eye = CameraPos - world;
	float3 n = normalize(o.normal);
	if (lamp) {
		float3 to_lamp = PointLightCoord - world;
		float distance = length(to_lamp);
		o.to_lamp = to_lamp / distance;
		o.shade = float2(n.y, on_lamp_falloff(distance) * on_terminator(n, o.to_lamp));
	} else {
		o.to_lamp = DirLightVector;
		o.shade = float2(n.y, on_terminator(n, DirLightVector));
	}
	o.fog = on_fog(o.position.z);
	return o;
}

// The light at the vertex, for an adapter without pixel shaders.
ShadedVertex on_shaded_vertex(ModelVertex v, uniform bool lamp)
{
	ShadedVertex o;
	float3 world;
	float3x3 frame;
	on_place(v, o.position, world, frame);
	float3 n = normalize(mul(v.normal, frame));
	float3 light;
	if (lamp) {
		float3 to_lamp = PointLightCoord - world;
		float distance = length(to_lamp);
		light = PointLightColor.rgb * saturate(dot(n, to_lamp / distance)) * on_lamp_falloff(distance);
	} else {
		light = on_hemisphere(n.y) + DirLightColor.rgb * saturate(dot(n, DirLightVector));
	}
	o.light = float4(saturate(light), 1);
	o.uv0 = on_uv(v.uv0);
	o.uv1 = v.uv1;
	o.fog = on_fog(o.position.z);
	return o;
}

ShadedVertex on_shadow_vertex(ModelVertex v)
{
	ShadedVertex o;
	float3 world;
	float3x3 frame;
	on_place(v, o.position, world, frame);
	o.light = float4(0, 0, 0, 1);
	o.uv0 = on_uv(v.uv0);
	o.uv1 = v.uv1;
	o.fog = on_fog(o.position.z);
	return o;
}
)fx";

constexpr const char kClipVertex[] = R"fx(
// The water mirror's clip coordinate: the height above the plane the renderer gives in model space.
ShadedVertex on_clipped_vertex(ModelVertex v)
{
	ShadedVertex o = on_shaded_vertex(v, false);
	o.uv1 = float2(dot(v.position, VecTexClipPlane), 0.5);
	return o;
}
)fx";

constexpr const char kPageFuncs[] = R"fx(
struct PageVertex {
	float4 position : POSITION;
	float2 page : TEXCOORD0;
	float fog : FOG;
};

// A crouching or prone person drawn first in the colour of the ground under them.
PageVertex on_page_vertex(ModelVertex v)
{
	PageVertex o;
	float3 world;
	float3x3 frame;
	on_place(v, o.position, world, frame);
	o.page = mul(float4(world, 1), TexProjMatrix1).xy;
	o.fog = on_fog(o.position.z);
	return o;
}

// The page's colour, lit by the sky and, where its alpha says the sun reaches, by the sun.
float4 on_page_pixel(PageVertex p) : COLOR
{
	float4 page = tex2D(PageSampler, p.page);
	return float4(page.rgb * (HemiSkyColor + DirLightColor.rgb * page.a) * 2, 1);
}
)fx";

constexpr const char kPhongPixel[] = R"fx(
// Lit by the light with a Phong highlight (the diffuse texture's alpha its strength) and, in the sun
// pass, by the sky over the vertex normal, as the original's Phong family lights.
float4 on_lit_pixel(LitVertex p, uniform bool lamp) : COLOR
{
	float4 base = tex2D(DiffuseSampler1, p.uv.xy);
	float3 n = on_bumped_normal(p.uv.xy, p.tangent, p.binormal, p.normal);
	float diffuse = saturate(dot(n, p.to_lamp));
	float highlight = on_pow8(saturate(dot(n, on_unit(p.to_lamp + on_unit(p.to_eye)))));
	float specular = diffuse > 0 ? highlight * base.a : 0;
	float3 color = lamp ? PointLightColor.rgb : DirLightColor.rgb;
	float3 lit = (base.rgb * diffuse + specular) * color * p.shade.y;
	if (!lamp)
		lit += base.rgb * on_hemisphere(p.shade.x);
	return float4(lit * 2, base.a);
}
)fx";

constexpr const char kDiffusePixel[] = R"fx(
// Lit by the light over the normal map and, in the sun pass, by the sky over the mapped normal, as the
// original's bump diffuse family lights.
float4 on_lit_pixel(LitVertex p, uniform bool lamp) : COLOR
{
	float4 base = on_base(p.uv);
	float3 n = on_bumped_normal(p.uv.xy, p.tangent, p.binormal, p.normal);
	float3 color = lamp ? PointLightColor.rgb : DirLightColor.rgb;
	float3 light = color * saturate(dot(n, p.to_lamp)) * p.shade.y;
	if (!lamp)
		light += on_hemisphere(n.y);
	return float4(base.rgb * light * 2, base.a);
}
)fx";

constexpr const char kBaseSingle[] = R"fx(
float4 on_base(float4 uv)
{
	return tex2D(DiffuseSampler1, uv.xy);
}
)fx";

constexpr const char kBaseDetail[] = R"fx(
// The base texture times the tiling detail on the second UV set, mid grey neutral.
float4 on_base(float4 uv)
{
	float4 base = tex2D(DiffuseSampler1, uv.xy);
	float4 detail = tex2D(DiffuseSampler2, uv.zw);
	return float4(base.rgb * detail.rgb * 2, base.a * detail.a);
}
)fx";

constexpr const char kStagesSingle[] = R"fx(		Sampler[0] = (DiffuseSampler1);
		ColorOp[0] = MODULATE2X; ColorArg1[0] = TEXTURE; ColorArg2[0] = DIFFUSE;
		AlphaOp[0] = SELECTARG1; AlphaArg1[0] = TEXTURE; AlphaArg2[0] = DIFFUSE;
		ColorOp[1] = DISABLE;
		AlphaOp[1] = DISABLE;
)fx";

constexpr const char kStagesDetail[] = R"fx(		Sampler[0] = (DiffuseSampler1);
		ColorOp[0] = MODULATE2X; ColorArg1[0] = TEXTURE; ColorArg2[0] = DIFFUSE;
		AlphaOp[0] = SELECTARG1; AlphaArg1[0] = TEXTURE; AlphaArg2[0] = DIFFUSE;
		Sampler[1] = (DiffuseSampler2);
		ColorOp[1] = MODULATE2X; ColorArg1[1] = TEXTURE; ColorArg2[1] = CURRENT;
		AlphaOp[1] = MODULATE; AlphaArg1[1] = TEXTURE; AlphaArg2[1] = CURRENT;
		ColorOp[2] = DISABLE;
		AlphaOp[2] = DISABLE;
)fx";

constexpr const char kTechLit[] = R"fx(
// The sun and the sky, then one pass for each lamp near the model, added.
technique OpenNovaLit <
	bool usevs = true;
	bool useps = true;
	int ttype = 0;
> {
	pass Sun <
		int fogmode = 8;
		int zmode = 0;
		int amode = 0;
		int passrules = 0;
	> {
		VertexShader = compile vs_2_0 on_lit_vertex(false);
		PixelShader = compile ps_2_0 on_lit_pixel(false);
		AlphaBlendEnable = FALSE;
		SpecularEnable = FALSE;
		FogEnable = TRUE;
	}
	pass Lamp <
		int fogmode = 10;
		int zmode = 0;
		int amode = 0;
		int passrules = 2;
	> {
		VertexShader = compile vs_2_0 on_lit_vertex(true);
		PixelShader = compile ps_2_0 on_lit_pixel(true);
		AlphaBlendEnable = TRUE; SrcBlend = ONE; DestBlend = ONE;
		SpecularEnable = FALSE;
		FogEnable = TRUE;
	}
}

// The same light at the vertices, for an adapter without pixel shaders.
technique OpenNovaShaded <
	bool usevs = true;
	bool useps = false;
	int ttype = 0;
> {
	pass Sun <
		int fogmode = 8;
		int zmode = 0;
		int amode = 0;
		int passrules = 0;
	> {
		VertexShader = compile vs_1_1 on_shaded_vertex(false);
		PixelShader = NULL;
		AlphaBlendEnable = FALSE;
		SpecularEnable = FALSE;
		FogEnable = TRUE;
@STAGES@	}
	pass Lamp <
		int fogmode = 10;
		int zmode = 0;
		int amode = 0;
		int passrules = 2;
	> {
		VertexShader = compile vs_1_1 on_shaded_vertex(true);
		PixelShader = NULL;
		AlphaBlendEnable = TRUE; SrcBlend = ONE; DestBlend = ONE;
		SpecularEnable = FALSE;
		FogEnable = TRUE;
@STAGES@	}
}
)fx";

constexpr const char kTechClip[] = R"fx(
// In the water mirror, where the model reaches below the water: lit at the vertices, the part under
// the plane cut.
technique OpenNovaClipped <
	bool usevs = true;
	bool useps = false;
	int ttype = 3;
> {
	pass Sun <
		int fogmode = 8;
		int zmode = 0;
		int amode = 1;
	> {
		VertexShader = compile vs_1_1 on_clipped_vertex();
		PixelShader = NULL;
		AlphaBlendEnable = FALSE;
		SpecularEnable = FALSE;
		FogEnable = TRUE;
		Sampler[0] = (DiffuseSampler1);
		ColorOp[0] = MODULATE2X; ColorArg1[0] = TEXTURE; ColorArg2[0] = DIFFUSE;
		AlphaOp[0] = SELECTARG2; AlphaArg1[0] = TEXTURE; AlphaArg2[0] = DIFFUSE;
		Sampler[1] = (ClipSampler);
		ColorOp[1] = SELECTARG2; ColorArg1[1] = TEXTURE; ColorArg2[1] = CURRENT;
		AlphaOp[1] = SELECTARG1; AlphaArg1[1] = TEXTURE; AlphaArg2[1] = CURRENT;
		ColorOp[2] = DISABLE;
		AlphaOp[2] = DISABLE;
	}
}
)fx";

constexpr const char kTechPage[] = R"fx(
// A crouching or prone person: the ground's colour first, which the person's own pass covers.
technique OpenNovaGroundMatch <
	bool usevs = true;
	bool useps = true;
	int ttype = 5;
> {
	pass Ground <
		int fogmode = 8;
		int zmode = 0;
		int amode = 0;
	> {
		VertexShader = compile vs_2_0 on_page_vertex();
		PixelShader = compile ps_2_0 on_page_pixel();
		AlphaBlendEnable = FALSE;
		SpecularEnable = FALSE;
		FogEnable = TRUE;
	}
}
)fx";

constexpr const char kTechShadow[] = R"fx(
// The projected shadow: the model's silhouette in black, its texture's alpha kept.
technique OpenNovaShadow <
	bool usevs = true;
	bool useps = false;
	int ttype = 1;
> {
	pass Silhouette <
		int fogmode = 9;
		int zmode = 0;
		int amode = 0;
	> {
		VertexShader = compile vs_1_1 on_shadow_vertex();
		PixelShader = NULL;
		AlphaBlendEnable = FALSE;
		SpecularEnable = FALSE;
		FogEnable = TRUE;
		Sampler[0] = (DiffuseSampler1);
		ColorOp[0] = SELECTARG2; ColorArg1[0] = TEXTURE; ColorArg2[0] = DIFFUSE;
		AlphaOp[0] = MODULATE; AlphaArg1[0] = TEXTURE; AlphaArg2[0] = DIFFUSE;
		ColorOp[1] = DISABLE;
		AlphaOp[1] = DISABLE;
	}
}
)fx";

// Each @NAME@ of `text` replaced by its value.
std::string fill(std::string text, std::initializer_list<std::pair<const char *, std::string>> values) {
	for (const auto &[token, value] : values) {
		const std::string marker = std::string("@") + token + "@";
		for (size_t at = text.find(marker); at != std::string::npos; at = text.find(marker, at + value.size()))
			text.replace(at, marker.size(), value);
	}
	return text;
}

// The object effects the editor makes, one for each shader tag of the base game's models: the family it
// lights as and what it reads.
struct ObjectEffect {
	const char *tag;
	const char *name; // EffectName
	const char *what; // the header's words
	bool skinned;     // the skinned layout, its bones in SkinWorldMatrixArray
	bool phong;       // the Phong highlight, its strength the diffuse texture's alpha
	bool detail;      // the detail texture on the second UV set
};
constexpr ObjectEffect kObjectEffects[] = {
	{"VS_PHONGT", "OpenNova: normal-mapped Phong", "normal-mapped Phong", false, true, false},
	{"VS_DOT3DIFF2", "OpenNova: normal-mapped diffuse with detail", "normal-mapped diffuse with a detail texture", false,
	 false, true},
	{"VS_SKBUMPPHONGT", "OpenNova: skinned normal-mapped Phong", "skinned normal-mapped Phong", true, true, false},
	{"VS_SKBUMPDIFFT", "OpenNova: skinned normal-mapped diffuse", "skinned normal-mapped diffuse", true, false, false},
};

const ObjectEffect *object_effect(const std::string &tag) {
	for (const ObjectEffect &effect : kObjectEffects)
		if (strutil::iequals(tag, effect.tag)) return &effect;
	return nullptr;
}

std::string object_text(const ObjectEffect &effect, const std::string &file) {
	std::string text = fill(kObjHead, {{"FILE", file},
	                                   {"WHAT", effect.what},
	                                   {"TAG", effect.tag},
	                                   {"NAME", effect.name},
	                                   {"ALT_UV", effect.skinned ? "" : "\n\tbool EffectAlt_UV = true;"},
	                                   {"SHARED", kShared},
	                                   {"FILTERS", kFilters},
	                                   {"SKIN_DECL", effect.skinned ? kSkinDecl : ""},
	                                   {"CLIP_SAMPLER", effect.skinned ? "" : kClipSampler},
	                                   {"PAGE_SAMPLER", effect.skinned ? kPageSampler : ""}});
	text += effect.skinned ? kSkinPlace : kStaticPlace;
	text += kVertices;
	text += effect.skinned ? kPageFuncs : kClipVertex;
	if (effect.phong) text += kPhongPixel;
	else text += std::string(effect.detail ? kBaseDetail : kBaseSingle) + kDiffusePixel;
	text += fill(kTechLit, {{"STAGES", effect.detail ? kStagesDetail : kStagesSingle}});
	text += effect.skinned ? kTechPage : kTechClip;
	text += kTechShadow;
	return text;
}

bool shader_bytes(const std::string &text, std::vector<uint8_t> &out) {
	// The shader loader's form, the text CR LF as every shipped effect's (formats/scr scr_shader_encode).
	out = scr::scr_shader_encode(blank_crlf(text));
	return true;
}

} // namespace

const std::vector<std::string> &blank_shader_tags() {
	static const std::vector<std::string> tags = [] {
		std::vector<std::string> out;
		for (const ObjectEffect &effect : kObjectEffects) out.push_back(effect.tag);
		return out;
	}();
	return tags;
}

std::string blank_shader_text(const std::string &tag, const std::string &file) {
	if (strutil::iequals(basename_of(file), renderer::kFixedFunctionShaderFile))
		return fill(kFfp, {{"SHARED", kShared}, {"FILTERS", kFilters}, {"CLIP_SAMPLER", kClipSampler}});
	const ObjectEffect *effect = object_effect(tag);
	return effect ? object_text(*effect, basename_of(file)) : std::string();
}

bool make_blank_ffp_shader(const BlankRequest &, std::vector<uint8_t> &out, Diagnostic &) {
	return shader_bytes(blank_shader_text(std::string(), renderer::kFixedFunctionShaderFile), out);
}

bool make_blank_shader(const BlankRequest &request, std::vector<uint8_t> &out, Diagnostic &error) {
	const std::string &tag = request.value("tag");
	const std::string name = basename_of(request.logical_name);
	if (strutil::iequals(name, renderer::kFixedFunctionShaderFile)) return make_blank_ffp_shader(request, out, error);
	// The archive walk skips a name starting with '_' [orig: HLSLEffect_LoadAllFromPFFArchive @ 0x5AFF6E].
	if (!name.empty() && name[0] == '_') {
		error = make_finding(CoreFinding::BlankShader, DiagnosticSeverity::Error,
				"The game loads no effect whose file name starts with '_' (but _ffp.fx, by its name): name it "
				"otherwise.", request.logical_name);
		return false;
	}
	const ObjectEffect *effect = object_effect(tag);
	if (!effect) {
		std::string known;
		for (const ObjectEffect &e : kObjectEffects) known += (known.empty() ? "" : ", ") + std::string(e.tag);
		error = make_finding(CoreFinding::BlankShader, DiagnosticSeverity::Error,
				"The editor makes a shader for " + known + (tag.empty() ? std::string(": choose one.") :
				                                                           "; not for '" + tag + "'."),
				request.logical_name);
		return false;
	}
	return shader_bytes(object_text(*effect, name), out);
}

} // namespace opennova::editor
