////////////////////////////////////////////////////////////////
//	_tskin.fx — depth-mask + terrain-match techniques for
//	SKINNED meshes.
//
//	Include file for the OpenNova authored set. Requires
//	_baseinc.fx, _vsskshared.fx (vscSkDepth, vscSkMatchTerrain)
//	and _psshared.fx (ps11TrnMatch).
////////////////////////////////////////////////////////////////

technique Simple_DepthMask
<
bool usevs=true;
bool useps=false;
int ttype=TECHNIQUE_DEPTHMASK;
>
{
	pass P0
	<
		int fogmode=FOGMODE_SHADERSET;
		int zmode=ZMODE_NORMAL;
		int amode=AMODE_NORMAL;
	>
	{
		VertexShader = (vscSkDepth);
		PixelShader = NULL;

		SetTextureSh(0, TexDepthGradWrite,	sampLinearClamp2D)

		RSAlphaMode(FALSE, ONE, ZERO)
		SpecularEnable	= FALSE;

		TSSColor(0, SelectArg1,	Texture, Diffuse)
		TSSAlpha(0, SelectArg1,	Texture, Diffuse)
		TSSEnd(1)

		FogEnable	= FALSE;
	}
}

// Pixel-shader terrain match: page texture modulated by ambient + sun.
technique Simple_MatchTerrain_PS
<
bool usevs=true;
bool useps=true;
int ttype=TECHNIQUE_MATCHTERRAIN;
>
{
	pass P0
	<
		int fogmode=FOGMODE_SHADER;
		int zmode=ZMODE_NORMAL;
		int amode=AMODE_NORMAL;
	>
	{
		VertexShader = (vscSkMatchTerrain);
		PixelShader = (ps11TrnMatch);

		PixelShaderConstantF[0]	= (HemiSkyColor);
		PixelShaderConstantF[1]	= (DirLightColor);

		SetTextureSh(0, TexCurProj,	sampLinearClamp2D)

		RSAlphaMode(FALSE, ONE, ZERO)
		SpecularEnable	= FALSE;

		FogEnable	= TRUE;
	}
}

// Fixed-function fallback: the same page through a TFactor fold.
technique Simple_MatchTerrain
<
bool usevs=true;
bool useps=false;
int ttype=TECHNIQUE_MATCHTERRAIN;
>
{
	pass P0
	<
		int fogmode=FOGMODE_SHADER;
		int zmode=ZMODE_NORMAL;
		int amode=AMODE_NORMAL;
	>
	{
		VertexShader = (vscSkMatchTerrain);
		PixelShader = NULL;

		SetTextureSh(0, TexCurProj,	sampLinearClamp2D)

		RSAlphaMode(FALSE, ONE, ZERO)
		SpecularEnable	= FALSE;

		TextureFactor = (float4(HemiSkyColor + 0.707f * DirLightColor,1));

		TSSColor(0, Modulate2x,	Texture, TFactor)
		TSSAlpha(0, SelectArg1,	Texture, Diffuse)
		TSSEnd(1)

		FogEnable	= TRUE;
	}
}
