////////////////////////////////////////////////////////////////
//	_ffp.fx — the fixed-function pipeline effect.
//
//	OpenNova authored set. The boot registry compiles this file
//	24 times — {TEX_SINGLE,TEX_MULTIPLE} x {,SELFLUM} x
//	{BLEND_NONE,BLEND_ALPHA,BLEND_ADD} x {,TEX_UVXFORM} — and
//	registers the variants as the FF_* tags every material
//	without a live shader tag falls back to
//	[orig: HLSLEffect_InitFixedFunctionShaders @ 0x5af790].
//	Loaded by literal name, so this file must keep it.
////////////////////////////////////////////////////////////////

string EffectInfo <
	string EffectTag = "FFP_BORING";
	string EffectName = "Fixed function";
	string EffectDesc1 = "Standard FFP lighting and rasterizing.";
	string EffectDesc2 = "";
	bool   EffectAlt_UV = true;
>;

#include "_baseinc.fx"
#include "_vsshared.fx"

////////////////////////////////////////
////	Blend-variant selection		////
////////////////////////////////////////
#ifdef BLEND_NONE
#define	LOCAL_FOGMODE		FOGMODE_NORMAL
#define	LOCAL_AMODE			AMODE_NORMAL
#define	LOCAL_ZMODE			ZMODE_NORMAL
#define	LOCAL_ABLEND		FALSE
#define	LOCAL_ABLENDSRC		ONE
#define	LOCAL_ABLENDDEST	ZERO
#define	ALLOW_SPOTLIGHTS	1
#endif
#ifdef BLEND_ALPHA
#define	LOCAL_FOGMODE		FOGMODE_NORMAL
#define	LOCAL_AMODE			AMODE_NORMAL
#define	LOCAL_ZMODE			ZMODE_NORMAL
#define	LOCAL_ABLEND		TRUE
#define	LOCAL_ABLENDSRC		SRCALPHA
#define	LOCAL_ABLENDDEST	INVSRCALPHA
#define	ALLOW_SPOTLIGHTS	0
#endif
#ifdef BLEND_ADD
#define	LOCAL_FOGMODE		FOGMODE_NORMALADD
#define	LOCAL_AMODE			AMODE_NORMAL
#define	LOCAL_ZMODE			ZMODE_NORMAL
#define	LOCAL_ABLEND		TRUE
#define	LOCAL_ABLENDSRC		ONE
#define	LOCAL_ABLENDDEST	ONE
#define	ALLOW_SPOTLIGHTS	0
#endif
#ifdef BLEND_MULT
#define	LOCAL_FOGMODE		FOGMODE_NORMALSET
#define	LOCAL_AMODE			AMODE_NORMAL
#define	LOCAL_ZMODE			ZMODE_NORMAL
#define	LOCAL_ABLEND		TRUE
#define	LOCAL_ABLENDSRC		DESTCOLOR
#define	LOCAL_ABLENDDEST	SRCCOLOR
#define	ALLOW_SPOTLIGHTS	0
#endif

////////////////////////////////////////
////	Standard					////
////////////////////////////////////////
technique TBoringFFP
<
#if ALLOW_SPOTLIGHTS
bool usevs=true;
#else
bool usevs=false;
#endif
bool useps=false;
bool useffplights=true;
int ttype=TECHNIQUE_NORMAL;
>
{
	// One pass does everything when no spotlight is live: FFP lighting,
	// Modulate2x texturing, material emissive carrying ambient/selflum.
	pass P0
	<
		int fogmode=LOCAL_FOGMODE;
		int amode=LOCAL_AMODE;
		int zmode=LOCAL_ZMODE;

#if ALLOW_SPOTLIGHTS
		int passrules = PASSRULE_IF_NO_SPOTLIGHTS;
#else
		int passrules = 0;
#endif
	>
	{
		VertexShader = NULL;
		PixelShader = NULL;

#ifdef FFPTRANSPOSE
		WorldTransform[0] = (transpose(MatWorld));
#else
		WorldTransform[0] = (MatWorld);
#endif

		SetTextureSh(0, TexDiffuse1,	sampLinearWrap2D)
#ifdef TEX_MULTIPLE
		SetTextureSh(1, TexDiffuse2,	sampLinearWrap2D)
#endif

#ifdef TEX_UVXFORM
		TextureTransformFlags[0]= COUNT2;
#ifdef FFPTRANSPOSE
		TextureTransform[0]		= (transpose(MatTexCoord1));
#else
		TextureTransform[0]		= (MatTexCoord1);
#endif
#endif
#ifdef TEX_MULTIPLE
#ifdef TEX_UVXFORM
		TextureTransformFlags[1]= COUNT2;
#ifdef FFPTRANSPOSE
		TextureTransform[1]		= (transpose(MatTexCoord1));
#else
		TextureTransform[1]		= (MatTexCoord1);
#endif
#endif
#endif

		RSAlphaMode(LOCAL_ABLEND, LOCAL_ABLENDSRC, LOCAL_ABLENDDEST)

		SpecularEnable		= FALSE;

#ifdef TEX_SINGLE
		TSSColor(0, Modulate2x,	Texture, Diffuse)
		TSSAlpha(0, Modulate,	Texture, Diffuse)
		TSSEnd(1)
#else
		TSSColor(0, Modulate2x,	Texture, Diffuse)
		TSSAlpha(0, Modulate,	Texture, Diffuse)
		TSSColor(1, Modulate2x,	Texture, Current)
		TSSAlpha(1, Modulate,	Texture, Current)
		TSSEnd(2)
#endif

#ifdef SELFLUM
		MaterialAmbient  = (float4(ColorSrcZero,0));
		MaterialDiffuse  = (float4(ColorSrcZero,0));
		MaterialEmissive = (float4(SelfLumColor*ColorSrcGlobalGain,0));
#else
		MaterialAmbient  = (float4(ColorSrcZero,0));
		MaterialDiffuse  = (float4(1,1,1, AlphaGenValue));
		MaterialEmissive = (float4(AmbientColor,0));
#endif
		Lighting		= TRUE;
		FogEnable		= TRUE;
	}

#if ALLOW_SPOTLIGHTS
	// Spotlights force multi-pass: solid lighting first (texture alpha
	// only), then one beam pass per spotlight, then the texture fold.
	pass P1a
	<
		int fogmode=LOCAL_FOGMODE;
		int amode=LOCAL_AMODE;
		int zmode=LOCAL_ZMODE;
		int passrules = PASSRULE_IF_SPOTLIGHTS + PASSRULE_POINTLIGHT_VARIATIONS;
	>
	{
		VertexShader = (vscFlatBaseHemiArray[CurNumPointLights]);
		PixelShader = NULL;

		SetTextureSh(0, TexDiffuse1,	sampLinearWrap2D)
#ifdef TEX_MULTIPLE
		SetTextureSh(1, TexDiffuse2,	sampLinearWrap2D)
#endif

		RSAlphaMode(LOCAL_ABLEND, LOCAL_ABLENDSRC, LOCAL_ABLENDDEST)

		SpecularEnable		= FALSE;

#ifdef TEX_SINGLE
		TSSColor(0, SelectArg2,	Texture, Diffuse)
		TSSAlpha(0, Modulate,	Texture, Diffuse)
		TSSEnd(1)
#else
		TSSColor(0, SelectArg2,	Texture, Diffuse)
		TSSAlpha(0, Modulate,	Texture, Diffuse)
		TSSColor(1, SelectArg2,	Texture, Current)
		TSSAlpha(1, Modulate,	Texture, Current)
		TSSEnd(2)
#endif

		FogEnable		= TRUE;
	}

	// Additive beam: depth-gradient test against the projected spot.
	pass P2a
	<
		int fogmode=FOGMODE_SHADERADD;
		int zmode=ZMODE_NORMAL;
		int amode=AMODE_DEPTHTEST;
		int passrules = PASSRULE_ONCE_PER_SPOTLIGHT;
	>
	{
		VertexShader = (vscFlatSpotDepth);
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

	// Fold the diffuse texture(s) over the accumulated lighting.
	pass P3a
	<
		int fogmode=FOGMODE_SHADER;
		int zmode=ZMODE_NORMAL;
		int amode=AMODE_NORMAL;
		int passrules = PASSRULE_IF_SPOTLIGHTS;
	>
	{
#ifdef TEX_SINGLE
		VertexShader = (vscPostMultiplyT1);
#else
		VertexShader = (vscPostMultiplyT2);
#endif
		PixelShader = NULL;

		RSAlphaMode(TRUE, DESTCOLOR, SRCCOLOR)
		SpecularEnable	= FALSE;

#ifdef TEX_SINGLE
		SetTextureSh(0, TexDiffuse1,	sampLinearWrap2D)
		TSSColor(0, Modulate2x,	Texture, Diffuse)
		TSSAlpha(0, Modulate,	Texture, Diffuse)
		TSSEnd(1)
#else
		SetTextureSh(0, TexDiffuse1,	sampLinearWrap2D)
		SetTextureSh(1, TexDiffuse2,	sampLinearWrap2D)
		TSSColor(0, Modulate2x,	Texture, Diffuse)
		TSSAlpha(0, Modulate,	Texture, Diffuse)
		TSSColor(1, Modulate2x,	Texture, Current)
		TSSAlpha(1, Modulate,	Texture, Current)
		TSSEnd(2)
#endif

		FogEnable	= TRUE;
	}

#endif

}

////////////////////////////////////////
////	Clipped						////
////////////////////////////////////////
// The camera-space clip ramp rides stage 1 (TexClip1D through
// MatTexClipPlane) for the reflection/underwater clip renders.
technique TBoringFFPClip
<
bool usevs=false;
bool useps=false;
int ttype=TECHNIQUE_CLIP;
>
{
	pass P0
	<
#ifdef BLEND_NONE
		int fogmode=FOGMODE_NORMAL;
#endif
#ifdef BLEND_ALPHA
		int fogmode=FOGMODE_NORMALSET;
#endif
#ifdef BLEND_ADD
		int fogmode=FOGMODE_NORMALADD;
#endif
#ifdef BLEND_MULT
		int fogmode=FOGMODE_NORMALSET;
#endif
		int amode=AMODE_CLIP;
	>
	{
		VertexShader = NULL;
		PixelShader = NULL;

#ifdef FFPTRANSPOSE
		WorldTransform[0] = (transpose(MatWorld));
#else
		WorldTransform[0] = (MatWorld);
#endif

		SetTextureSh(0, TexDiffuse1,	sampLinearWrap2D)
		Sampler[1]				= <sampLinearClamp2D>;
		Texture[1]				= (TexClip1D);
		TexCoordIndex[1]		= CAMERASPACEPOSITION;
		TextureTransformFlags[1]= COUNT2;
#ifdef FFPTRANSPOSE
		TextureTransform[1]		= (transpose(MatTexClipPlane));
#else
		TextureTransform[1]		= (MatTexClipPlane);
#endif

#ifdef TEX_UVXFORM
		TextureTransformFlags[0]= COUNT2;
#ifdef FFPTRANSPOSE
		TextureTransform[0]		= (transpose(MatTexCoord1));
#else
		TextureTransform[0]		= (MatTexCoord1);
#endif
#endif

#ifdef BLEND_NONE
		RSAlphaMode(FALSE, ONE, ZERO)
#endif
#ifdef BLEND_ALPHA
		RSAlphaMode(TRUE, SRCALPHA, INVSRCALPHA)
#endif
#ifdef BLEND_ADD
		RSAlphaMode(TRUE, ONE, ONE)
#endif
#ifdef BLEND_MULT
		RSAlphaMode(TRUE, DESTCOLOR, SRCCOLOR)
#endif

		SpecularEnable		= FALSE;

		TSSColor(0, Modulate2x,	Texture, Diffuse)
		TSSAlpha(0, Modulate,	Texture, Diffuse)
		TSSColor(1, SelectArg2,	Texture, Current)
		TSSAlpha(1, Modulate,	Texture, Current)
		TSSEnd(2)

		MaterialAmbient  = (float4(ColorSrcZero,0));
		MaterialDiffuse  = (float4(1,1,1, AlphaGenValue));
		MaterialEmissive = (float4(AmbientColor,0));

		Lighting		= TRUE;
		FogEnable		= TRUE;
	}
}

////////////////////////////////////////
////	Projected shadow			////
////////////////////////////////////////
// Black-lit silhouette for the drape render.
technique TBoringFFPProjShad
<
bool usevs=false;
bool useps=false;
int ttype=TECHNIQUE_PROJSHAD;
>
{
	pass P0
	<
#ifdef BLEND_NONE
		int fogmode=FOGMODE_NORMAL;
#endif
#ifdef BLEND_ALPHA
		int fogmode=FOGMODE_NORMALSET;
#endif
#ifdef BLEND_ADD
		int fogmode=FOGMODE_NORMALADD;
#endif
#ifdef BLEND_MULT
		int fogmode=FOGMODE_NORMALSET;
#endif
		int amode=AMODE_NORMAL;
	>
	{
		VertexShader = NULL;
		PixelShader = NULL;

#ifdef FFPTRANSPOSE
		WorldTransform[0] = (transpose(MatWorld));
#else
		WorldTransform[0] = (MatWorld);
#endif

		SetTextureSh(0, TexDiffuse1,	sampLinearWrap2D)
#ifdef TEX_MULTIPLE
		SetTextureSh(1, TexDiffuse2,	sampLinearWrap2D)
#endif

#ifdef BLEND_NONE
		RSAlphaMode(FALSE, ONE, ZERO)
#endif
#ifdef BLEND_ALPHA
		RSAlphaMode(TRUE, SRCALPHA, INVSRCALPHA)
#endif
#ifdef BLEND_ADD
		RSAlphaMode(TRUE, ONE, ONE)
#endif
#ifdef BLEND_MULT
		RSAlphaMode(TRUE, DESTCOLOR, SRCCOLOR)
#endif

		SpecularEnable		= FALSE;

#ifdef TEX_SINGLE
		TSSColor(0, SelectArg2,	Texture, Diffuse)
		TSSAlpha(0, Modulate,	Texture, Diffuse)
		TSSEnd(1)
#else
		TSSColor(0, SelectArg2,	Texture, Diffuse)
		TSSAlpha(0, Modulate,	Texture, Diffuse)
		TSSColor(1, SelectArg2,	Texture, Current)
		TSSAlpha(1, Modulate,	Texture, Current)
		TSSEnd(2)
#endif

		MaterialAmbient  = (float4(ColorSrcZero,0));
		MaterialDiffuse  = (float4(ColorSrcZero,AlphaGenValue));
		MaterialEmissive = (float4(ColorSrcZero,0));
		Lighting		= TRUE;
		FogEnable		= TRUE;
	}
}

////////////////////////////////////////
////	Depth mask					////
////////////////////////////////////////
#ifdef BLEND_NONE
#include "_tdepth.fx"
#endif
