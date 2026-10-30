#pragma once

// THE HUD'S TEXTURE LOAD PLAN: every texture the HUD's set-up loads, in its order, with the file
// it opens, the loader its retail caller reads it through and the material word its maker gives
// it; and what the HUD's draws read back of each loaded texture (its validity and its sides),
// stamped into the layout and the frame state. The embedder walks the plan and does the device
// loads only (witness record: docs/interface/hud-re.md "The HUD texture loader").

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include <runtime/hud/hud_frame.h>
#include <runtime/hud/hud_layout_from_hudpos.h> // HudLayoutAssets
#include <runtime/renderer/texture_load_rules.h> // TextureLoader

namespace opennova::hud {

// One planned load into a device slot.
struct HudTextureLoad {
	int32_t slot = kHudTexNone; // HudTexture
	std::string name;           // the file's flat name; "" leaves the slot empty
	renderer::TextureLoader loader = renderer::TextureLoader::HudColor;
	// The word the texture's maker gives it (hud_texture_materials.h); 0 for a HUD loader's
	// texture, whose word is the loader's own by the mode it resolved
	// (renderer::hud_loader_material_word), and for a texture its maker gives none.
	uint32_t material_word = 0;
	// The full box-filtered mip chain the texture is uploaded with (the map icon strip).
	bool mipmaps = false;
	// The stdbox border atlas: its brush cell (stdbox_brush_cell) lifted into this slot under
	// the same word and zeroed out of the atlas; kHudTexNone for every other load.
	int32_t brush_slot = kHudTexNone;
};

// The HUD's set-up loads for one build. Both arms load the box styles' atlases and the
// connection icons; the full arm (`full_arm`; a dedicated host's reduced arm loads none of the
// HUD's own art) also loads, ahead of them, the combat sprites and the static frame
// (`frame_present`: the layout's frame_pos), and after them the tip panel, the zone panel's
// icons and tiles, the HUDLS pair, the stance records, the crosshair of `crosshair_style` in the
// mode `crosshair_alpha` names, the map icon strip, the compass ring, the radar slices and the
// waypoint indicator.
// [orig: HUD_LoadAllTextures @0x59DDA0; Game_StartMission @0x524360 (the stdbox's border.tga
//  @0x525AAD and boxtile.tga @0x525AA8); CNetworkIcons_LoadTextures @0x4C2CF0; CTipSystem_Init
//  @0x5B6970; engine mission/mission_load_plan.h carries the two arms]
std::vector<HudTextureLoad> hud_texture_load_plan(const HudLayoutAssets &assets, bool full_arm,
		bool frame_present, int crosshair_style, bool crosshair_alpha);

// The crosshair's own load: the style's texture through the HUD loader, in colour mode at the
// HUD's set-up and in alpha mode once the in-game options' Accept reloaded it.
HudTextureLoad hud_crosshair_texture_load(int crosshair_style, bool crosshair_alpha);

// What a load gave back: the texture's sides, and whether it loaded.
struct HudTextureSize {
	int32_t width = 0;
	int32_t height = 0;
	bool valid = false;
};
using HudTextureSizes = std::array<HudTextureSize, kHudTexSightsBase>;

// Stamp what the draws read of each planned slot's loaded texture (`sizes`, by slot) into the
// layout (the texture-derived fields hud_layout_from_hudpos leaves alone) and the frame state
// (the minimap's icon strip sides and its radar-slice gate). Only the slots `loads` names are
// written.
void hud_take_texture_loads(const std::vector<HudTextureLoad> &loads, const HudTextureSizes &sizes,
		HudLayout &layout, HudFrameState &state);

} // namespace opennova::hud
