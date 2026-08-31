////////////////////////////////////////////////////////////////
//	_tdepth.fx — depth-mask technique for RIGID meshes.
//
//	Include file for the OpenNova authored set: writes the
//	depth-gradient ramp for the spotlight depth test.
//	Requires _baseinc.fx and _vsshared.fx (vscDepth).
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
		VertexShader = (vscDepth);
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
