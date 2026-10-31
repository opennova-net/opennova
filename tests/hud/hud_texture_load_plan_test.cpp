// The HUD's texture load plan (hud/hud_texture_load_plan.h): which file each set-up load opens,
// through which loader, under which material word, on which arm and in what order; and what the
// draws read back of the loaded textures (the box gate, the radar-slice gate, the icon strip's
// sides, the combat sprites, the stance records).
// [orig: HUD_LoadAllTextures @0x59DDA0; BoxTexture_LoadAndSetupUVRegions @0x56ACD0 (the null
//  gate @0x56B71F); CNetworkIcons_LoadTextures @0x4C2CF0; CTipSystem_Init @0x5B6970]
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <runtime/hud/hud_texture_load_plan.h>
#include <runtime/hud/hud_texture_materials.h>

using namespace opennova;
using namespace opennova::hud;
using renderer::TextureLoader;

static int failures = 0;
#define CHECK(x)                                                                                   \
	do {                                                                                           \
		if (!(x)) {                                                                                \
			std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x);                              \
			++failures;                                                                            \
		}                                                                                          \
	} while (0)

namespace {

const HudTextureLoad *find(const std::vector<HudTextureLoad> &plan, int32_t slot) {
	for (const HudTextureLoad &load : plan)
		if (load.slot == slot) return &load;
	return nullptr;
}

HudLayoutAssets assets() {
	HudLayoutAssets a;
	a.static_frame = "frame.tga";
	a.parachute_icon = "chute.tga";
	a.armor_icon = "armor.tga";
	a.hudls_bracket = "bracket.tga";
	a.hudls_moreav = "moreav.tga";
	for (size_t i = 0; i < a.stance_textures.size(); ++i)
		a.stance_textures[i] = "stance" + std::to_string(i) + ".tga";
	return a;
}

// The full arm: every load's file, loader and word, in the set-up's order.
void full_arm() {
	const std::vector<HudTextureLoad> plan = hud_texture_load_plan(assets(), true, true, 0, false);
	CHECK(!plan.empty() && plan.front().slot == kHudTexVehicleFixed);
	const HudTextureLoad *rock = find(plan, kHudTexVehicleFixed);
	CHECK(rock && rock->name == "rockpip.tga" && rock->loader == TextureLoader::HudColor &&
			rock->material_word == 0);
	const HudTextureLoad *chute = find(plan, kHudTexParachute);
	CHECK(chute && chute->name == "chute.tga" && chute->loader == TextureLoader::HudAlpha);
	const HudTextureLoad *frame = find(plan, kHudTexFrame);
	CHECK(frame && frame->name == "frame.tga" && frame->loader == TextureLoader::HudColor);
	const HudTextureLoad *border = find(plan, kHudTexBoxBorder);
	CHECK(border && border->name == "border.tga" && border->loader == TextureLoader::Tga &&
			border->material_word == kBoxMaterialWord && border->brush_slot == kHudTexBoxFill);
	const HudTextureLoad *tile = find(plan, kHudTexBoxTile);
	CHECK(tile && tile->name == "boxtile.tga" && tile->material_word == 0 &&
			tile->brush_slot == kHudTexNone);
	const HudTextureLoad *net = find(plan, kHudTexNetLinkIcon);
	CHECK(net && net->name == "neticon1.tga" && net->material_word == kNetIconMaterialWord);
	const HudTextureLoad *tip = find(plan, kHudTexTipKeyboard);
	CHECK(tip && tip->name == "k_tip.tga" && tip->loader == TextureLoader::Stage &&
			tip->material_word == kTipIconMaterialWord);
	const HudTextureLoad *lfp = find(plan, kHudTexLfpTeam2);
	CHECK(lfp && lfp->name == "R_LFP.tga" && lfp->loader == TextureLoader::File &&
			lfp->material_word == kLfpIconMaterialWord);
	const HudTextureLoad *own = find(plan, kHudTexLfpTileOwn);
	CHECK(own && own->name == "lfp_dlf.tga" && own->loader == TextureLoader::HudColor);
	const HudTextureLoad *bracket = find(plan, kHudTexSlotBarBracket);
	CHECK(bracket && bracket->name == "bracket.tga" && bracket->loader == TextureLoader::HudColor);
	const HudTextureLoad *stance = find(plan, kHudTexStance0 + 7);
	CHECK(stance && stance->name == "stance7.tga" && stance->loader == TextureLoader::HudAlpha);
	const HudTextureLoad *cross = find(plan, kHudTexCrosshair);
	CHECK(cross && cross->name == "cross01.tga" && cross->loader == TextureLoader::HudColor);
	const HudTextureLoad *icons = find(plan, kHudTexMapIcons);
	CHECK(icons && icons->name == "TSDicon.tga" && icons->loader == TextureLoader::File &&
			icons->mipmaps && icons->material_word == kTsdIconMaterialWord);
	const HudTextureLoad *wp = find(plan, kHudTexMapWpIndicator);
	CHECK(wp && wp->name == "WPIndctr.tga" && wp->material_word == kWpIndicatorMaterialWord);
	CHECK(plan.back().slot == kHudTexMapWpIndicator);
	// No StaticFrame line, no frame load; the options' alpha crosshair.
	const std::vector<HudTextureLoad> bare = hud_texture_load_plan(assets(), true, false, 4, true);
	CHECK(find(bare, kHudTexFrame) == nullptr);
	const HudTextureLoad *alpha = find(bare, kHudTexCrosshair);
	CHECK(alpha && alpha->name == "cross05.tga" && alpha->loader == TextureLoader::HudAlpha);
}

// The reduced arm: the box styles and the connection icons alone.
void reduced_arm() {
	const std::vector<HudTextureLoad> plan = hud_texture_load_plan(assets(), false, true, 0, false);
	CHECK(plan.size() == 5);
	if (plan.size() == 5) {
		CHECK(plan[0].slot == kHudTexBoxBorder && plan[1].slot == kHudTexBoxTile);
		CHECK(plan[2].slot == kHudTexNetIcon && plan[3].slot == kHudTexNetLinkIcon &&
				plan[4].slot == kHudTexNetNovaWorldIcon);
	}
}

// What the draws read back: the box draws only with both its atlas and its secondary, the radar
// marks only with both slices, the strip's sides or the defaults, each stamped slot's own size.
void stamps() {
	const std::vector<HudTextureLoad> plan = hud_texture_load_plan(assets(), true, true, 0, false);
	HudTextureSizes sizes{};
	sizes[kHudTexBoxBorder] = { 128, 128, true };
	sizes[kHudTexTarget] = { 32, 16, true };
	sizes[kHudTexStance0 + 2] = { 20, 30, true };
	sizes[kHudTexMapRadar] = { 64, 64, true };
	sizes[kHudTexFrame] = { 800, 100, true };
	HudLayout layout;
	HudFrameState state;
	hud_take_texture_loads(plan, sizes, layout, state);
	CHECK(!layout.box_texture_valid && layout.box_tex_w == 128 && layout.box_tile_w == 0);
	CHECK(layout.combat.target.valid && layout.combat.target.width == 32 &&
			layout.combat.target.height == 16);
	CHECK(!layout.combat.vehicle_fixed.valid);
	CHECK(layout.stance_texture_valid[2] && layout.stance_frame_w[2] == 20 &&
			layout.stance_frame_h[2] == 30);
	CHECK(layout.frame_texture_valid && layout.frame_tex_w == 800);
	CHECK(!state.minimap.radar_slices_loaded);
	CHECK(state.minimap.icon_strip_w_px == HudMinimapInput{}.icon_strip_w_px);
	sizes[kHudTexBoxTile] = { 64, 64, true };
	sizes[kHudTexMapRadarNarrow] = { 64, 64, true };
	sizes[kHudTexMapIcons] = { 16, 480, true };
	hud_take_texture_loads(plan, sizes, layout, state);
	CHECK(layout.box_texture_valid && layout.box_tile_w == 64 && layout.box_tile_h == 64);
	CHECK(state.minimap.radar_slices_loaded);
	CHECK(state.minimap.icon_strip_w_px == 16.0f && state.minimap.icon_strip_h_px == 480.0f);
	// Only the planned slots are written: the reduced arm leaves the stance records alone.
	HudLayout reduced;
	reduced.stance_texture_valid[2] = true;
	hud_take_texture_loads(hud_texture_load_plan(assets(), false, false, 0, false), sizes, reduced,
			state);
	CHECK(reduced.stance_texture_valid[2] && reduced.box_texture_valid);
}

} // namespace

int main() {
	full_arm();
	reduced_arm();
	stamps();
	if (failures != 0) {
		std::fprintf(stderr, "%d failure(s)\n", failures);
		return EXIT_FAILURE;
	}
	std::puts("hud_texture_load_plan_test: ok");
	return EXIT_SUCCESS;
}
