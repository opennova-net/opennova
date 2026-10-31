#include <runtime/hud/hud_texture_load_plan.h>

#include <runtime/hud/hud_texture_materials.h>
#include <runtime/hud/hud_texture_names.h>
#include <runtime/renderer/texture_roles.h> // texture_role: a role's loader

namespace opennova::hud {

namespace {

using renderer::TextureLoader;

HudTextureLoad fixed(int32_t slot, TextureLoader loader, uint32_t word = 0) {
	HudTextureLoad load;
	load.slot = slot;
	load.name = hud_fixed_texture_name(slot);
	load.loader = loader;
	load.material_word = word;
	return load;
}

HudTextureLoad named(int32_t slot, const std::string &name, renderer::TextureRoleId role) {
	HudTextureLoad load;
	load.slot = slot;
	load.name = name;
	load.loader = renderer::texture_role(role).loader;
	return load;
}

HudSprite sprite_of(const HudTextureSize &size) {
	return { size.valid ? size.width : 0, size.valid ? size.height : 0, size.valid };
}

// The combat cues' sprite of a slot, or null for a slot that is none of theirs.
HudSprite *combat_sprite(HudCombatLayout &l, int32_t slot) {
	switch (slot) {
		case kHudTexVehicleFixed: return &l.vehicle_fixed;
		case kHudTexVehicleLag: return &l.vehicle_lag;
		case kHudTexDriverCrosshair: return &l.driver_crosshair;
		case kHudTexTarget: return &l.target;
		case kHudTexTargetFriendly: return &l.target_friendly;
		case kHudTexParachute: return &l.parachute;
		case kHudTexArmor: return &l.armor;
		case kHudTexLogoHelo: return &l.logo_helo;
		case kHudTexLogoHumm: return &l.logo_humm;
		case kHudTexLogoBoat: return &l.logo_boat;
		case kHudTexScopeCrosshair: return &l.scope_crosshair;
		default: return nullptr;
	}
}

} // namespace

HudTextureLoad hud_crosshair_texture_load(int crosshair_style, bool crosshair_alpha) {
	HudTextureLoad load;
	load.slot = kHudTexCrosshair;
	load.name = hud_crosshair_texture_name(crosshair_style);
	load.loader = crosshair_alpha ? TextureLoader::HudAlpha : TextureLoader::HudColor;
	return load;
}

std::vector<HudTextureLoad> hud_texture_load_plan(const HudLayoutAssets &assets, bool full_arm,
		bool frame_present, int crosshair_style, bool crosshair_alpha) {
	std::vector<HudTextureLoad> plan;
	constexpr TextureLoader colour = TextureLoader::HudColor;
	if (full_arm) {
		// The combat sprites: the fixed art in colour mode, the hudpos parachute and armor
		// icons in the modes HudLayoutAssets names.
		plan.push_back(fixed(kHudTexVehicleFixed, colour));
		plan.push_back(fixed(kHudTexVehicleLag, colour));
		plan.push_back(fixed(kHudTexDriverCrosshair, colour));
		plan.push_back(fixed(kHudTexTarget, colour));
		plan.push_back(fixed(kHudTexTargetFriendly, colour));
		plan.push_back(named(kHudTexParachute, assets.parachute_icon,
				HudLayoutAssets::kParachuteIconRole));
		plan.push_back(named(kHudTexArmor, assets.armor_icon, HudLayoutAssets::kArmorIconRole));
		plan.push_back(fixed(kHudTexLogoHelo, colour));
		plan.push_back(fixed(kHudTexLogoHumm, colour));
		plan.push_back(fixed(kHudTexLogoBoat, colour));
		// The scope crosshair in its alpha-mode role (renderer fixed_texture_names), the quad a
		// title's Inset reticle draws (HudCombatState::inset_reticle_quad).
		plan.push_back(named(kHudTexScopeCrosshair, kHudScopeCrosshairTexture,
				renderer::TextureRoleId::HudAlphaOnly));
		// The static HUD frame the layout picked (the last authored StaticFrame line,
		// hud_static_frame_index); absent, nothing loads.
		if (frame_present)
			plan.push_back(
					named(kHudTexFrame, assets.static_frame, HudLayoutAssets::kStaticFrameRole));
	}
	// The Tab board's stdbox atlases and the connection-icon strips through the TGA reader
	// alone. The style lifts the border atlas's interior brush cell (3,0) into its own texture
	// under the same box material and zeroes it out of the atlas; the fill's one wrap-addressed
	// quad samples it (hud_frame.h kHudTexBoxFill carries the witness); boxtile.tga is the
	// pieces' second texture stage (HudQuad::texture2) and takes no material of its own.
	HudTextureLoad border = fixed(kHudTexBoxBorder, TextureLoader::Tga, kBoxMaterialWord);
	border.brush_slot = kHudTexBoxFill;
	plan.push_back(border);
	plan.push_back(fixed(kHudTexBoxTile, TextureLoader::Tga));
	plan.push_back(fixed(kHudTexNetIcon, TextureLoader::Tga, kNetIconMaterialWord));
	plan.push_back(fixed(kHudTexNetLinkIcon, TextureLoader::Tga, kNetIconMaterialWord));
	plan.push_back(fixed(kHudTexNetNovaWorldIcon, TextureLoader::Tga, kNetIconMaterialWord));
	if (!full_arm) return plan;
	// The tip panel's own box atlas (no second stage) and its two icons through the stage
	// loader.
	plan.push_back(fixed(kHudTexTipBox, TextureLoader::Tga, kBoxMaterialWord));
	plan.push_back(fixed(kHudTexTipKeyboard, TextureLoader::Stage, kTipIconMaterialWord));
	plan.push_back(fixed(kHudTexTipGameplay, TextureLoader::Stage, kTipIconMaterialWord));
	// The AAS zone status panel's three team-icon atlases through the file loader and the
	// two tiles in colour mode [orig: HUD_LoadAllTextures @0x59dda0 — JO_LFP.tga team 1,
	// R_LFP.tga team 2, N_LFP.tga neutral; lfp_alf.tga the tile for everyone else's zones ->
	// 0x27239C0 @0x59e104/@0x59e10e, lfp_dlf.tga the tile for the viewer's OWN zones ->
	// 0x27239D0 (textureId +4 = 0x27239D4) @0x59e11a/@0x59e11f, see docs/interface/hud-re.md].
	plan.push_back(fixed(kHudTexLfpTeam1, TextureLoader::File, kLfpIconMaterialWord));
	plan.push_back(fixed(kHudTexLfpTeam2, TextureLoader::File, kLfpIconMaterialWord));
	plan.push_back(fixed(kHudTexLfpNeutral, TextureLoader::File, kLfpIconMaterialWord));
	plan.push_back(fixed(kHudTexLfpTileOwn, colour));
	plan.push_back(fixed(kHudTexLfpTileOther, colour));
	// The HUDLS bar's bracket and more-available marker, named by hudpos.
	plan.push_back(named(kHudTexSlotBarBracket, assets.hudls_bracket, HudLayoutAssets::kHudlsRole));
	plan.push_back(named(kHudTexSlotBarMoreAv, assets.hudls_moreav, HudLayoutAssets::kHudlsRole));
	// The eight HUDSTANCE records' textures by slot (the six stance frames, then the team
	// tiles), each with its image's own size.
	for (int i = 0; i < kHudStanceRecords; ++i)
		plan.push_back(named(kHudTexStance0 + i, assets.stance_textures[static_cast<size_t>(i)],
				HudLayoutAssets::kStanceRole));
	plan.push_back(hud_crosshair_texture_load(crosshair_style, crosshair_alpha));
	// Retail uploads the map icon strip at its authored resolution with its full box-filtered
	// mip chain; the default spinmap badges then sample near the 16px level, and a mipless
	// upload aliases the source into a visibly broken armory "A".
	HudTextureLoad icons = fixed(kHudTexMapIcons, TextureLoader::File, kTsdIconMaterialWord);
	icons.mipmaps = true;
	plan.push_back(icons);
	// The compass ring and the radar sector-slice marks are colour-mode HUD textures: like
	// every textured map sprite their material is colour family 0x600, whose MODULATE2X stage
	// runs on the device.
	plan.push_back(fixed(kHudTexMapCompass, colour));
	plan.push_back(fixed(kHudTexMapRadar, colour));
	plan.push_back(fixed(kHudTexMapRadarNarrow, colour));
	plan.push_back(fixed(kHudTexMapWpIndicator, TextureLoader::File, kWpIndicatorMaterialWord));
	return plan;
}

void hud_take_texture_loads(const std::vector<HudTextureLoad> &loads, const HudTextureSizes &sizes,
		HudLayout &layout, HudFrameState &state) {
	const auto size_of = [&sizes](int32_t slot) -> const HudTextureSize & {
		static const HudTextureSize kNone;
		return slot >= 0 && slot < static_cast<int32_t>(sizes.size())
				? sizes[static_cast<size_t>(slot)]
				: kNone;
	};
	for (const HudTextureLoad &load : loads) {
		const HudTextureSize &s = size_of(load.slot);
		const int32_t w = s.valid ? s.width : 0;
		const int32_t h = s.valid ? s.height : 0;
		if (HudSprite *sprite = combat_sprite(layout.combat, load.slot)) {
			*sprite = sprite_of(s);
			continue;
		}
		if (load.slot >= kHudTexStance0 && load.slot < kHudTexStance0 + kHudStanceRecords) {
			const size_t r = static_cast<size_t>(load.slot - kHudTexStance0);
			layout.stance_texture_valid[r] = s.valid;
			layout.stance_frame_w[r] = w;
			layout.stance_frame_h[r] = h;
			continue;
		}
		switch (load.slot) {
			case kHudTexFrame:
				layout.frame_texture_valid = s.valid;
				layout.frame_tex_w = w;
				layout.frame_tex_h = h;
				break;
			case kHudTexBoxBorder:
				// The style draws NOTHING without its secondary texture: the combined material
				// rec+0x30 gates the whole drawer [orig: the combine @0x56af3c, the null gate
				// @0x56b71f, see docs/interface/hud-re.md]. The piece size comes off the border
				// atlas's own width, not a constant [orig: the 4x4 cell grid, see
				// docs/interface/hud-re.md].
				layout.box_texture_valid = s.valid && size_of(kHudTexBoxTile).valid;
				layout.box_tex_w = w;
				layout.box_tex_h = h;
				break;
			case kHudTexBoxTile:
				// The camo's own dims divide the pieces' screen-anchored second UV
				// (HudLayout::box_tile_w/h carries the witness).
				layout.box_tile_w = w;
				layout.box_tile_h = h;
				break;
			case kHudTexNetIcon: layout.net_icon_texture_valid = s.valid; break;
			case kHudTexNetLinkIcon: layout.net_link_icon_texture_valid = s.valid; break;
			case kHudTexNetNovaWorldIcon: layout.net_novaworld_icon_texture_valid = s.valid; break;
			case kHudTexTipBox:
				layout.tip_box_texture_valid = s.valid;
				layout.tip_box_tex_w = w;
				layout.tip_box_tex_h = h;
				break;
			case kHudTexTipKeyboard: layout.tip_keyboard_texture_valid = s.valid; break;
			case kHudTexTipGameplay: layout.tip_gameplay_texture_valid = s.valid; break;
			case kHudTexLfpTeam1: layout.lfp_icon_texture_valid[0] = s.valid; break;
			case kHudTexLfpTeam2: layout.lfp_icon_texture_valid[1] = s.valid; break;
			case kHudTexLfpNeutral: layout.lfp_icon_texture_valid[2] = s.valid; break;
			case kHudTexLfpTileOwn: layout.lfp_tile_own_texture_valid = s.valid; break;
			case kHudTexLfpTileOther: layout.lfp_tile_other_texture_valid = s.valid; break;
			case kHudTexSlotBarBracket:
				layout.hudls.bracket_texture_valid = s.valid;
				layout.hudls.bracket_tex_w = w;
				layout.hudls.bracket_tex_h = h;
				break;
			case kHudTexSlotBarMoreAv:
				layout.hudls.moreav_texture_valid = s.valid;
				layout.hudls.moreav_tex_w = w;
				layout.hudls.moreav_tex_h = h;
				break;
			case kHudTexCrosshair:
				layout.crosshair_texture_valid = s.valid;
				layout.crosshair_tex_w = w;
				layout.crosshair_tex_h = h;
				break;
			case kHudTexMapIcons:
				// The compiler's half-texel cell insets ride the PHYSICAL dimensions of whatever
				// strip this install mounts (stock 16x480, RevX02 64x1920).
				state.minimap.icon_strip_w_px =
						s.valid ? static_cast<float>(s.width) : HudMinimapInput{}.icon_strip_w_px;
				state.minimap.icon_strip_h_px =
						s.valid ? static_cast<float>(s.height) : HudMinimapInput{}.icon_strip_h_px;
				break;
			case kHudTexMapRadar:
			case kHudTexMapRadarNarrow:
				// The marks draw only while both loaded (HudMinimapInput::radar_slices_loaded
				// carries the witness).
				state.minimap.radar_slices_loaded =
						size_of(kHudTexMapRadar).valid && size_of(kHudTexMapRadarNarrow).valid;
				break;
			default: break;
		}
	}
}

} // namespace opennova::hud
