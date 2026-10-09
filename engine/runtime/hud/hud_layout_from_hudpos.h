#pragma once

// The hudpos.def parse -> the HUD layout globals (ADR 0040 ladder E3b): every
// position, rect, colour, fade, stance offset and frame pick that
// HUD_ParseHudposToken writes into the dword_27237xx block, applied to a
// HudLayout in one place. The device resolves the texture NAMES this hands
// back (the static frame, the six stance frames, the parachute and armor
// icons, the HUD font) and stamps the texture-derived fields (valid flags,
// pixel sizes) itself.
// [orig: HUD_ParseHudposToken @0x59f370 hudpos.def parser, registered by
//  HUD_InitOverlaySystem @0x5a4620, see docs/interface/hud-re.md]

#include <runtime/hud/hud_frame.h>
#include <runtime/renderer/texture_roles.h>

#include <array>
#include <cstdint>
#include <string>

namespace opennova::def {
struct DefHudPosFile;  // formats/def/def.h
struct DefHudColor;
}

namespace opennova::hud {

// The names the layout fill leaves for the device to resolve to textures.
struct HudLayoutAssets {
	// The two HUD font names hudpos authors ("" when a key is absent): FONTHUD1_LO
	// fills byte_2723C54, FONTHUD1_HI unk_2723C64 [orig: HUD_ParseHudposToken
	// @0x5A1593 / @0x5A15C7]. hudpos_font_for_width picks one per surface width.
	std::string font_lo;
	std::string font_hi;
	// The static frame retail draws (hud_static_frame_index: the LAST authored
	// line); "" when no StaticFrame line is authored, and then
	// HudLayout::frame_pos stays absent.
	std::string static_frame;
	// The six HUDSTANCE frames by their explicit id.
	std::array<std::string, 6> stance_textures{};
	std::string parachute_icon;
	std::string armor_icon;
	// The HUDLS bar's two hudpos-named textures, loaded alpha mode 0 and only
	// when named [orig: HUD_LoadAllTextures @0x59DEEE..0x59DF3E].
	std::string hudls_bracket;
	std::string hudls_moreav;

	// The mode the HUD's loader loads each name above in, as its texture role
	// (sub_591750's alpha mode: HudColour mode 0, HudAlphaOnly mode 1; docs/
	// interface/hud-re.md "The HUD texture loader"): the static frame and the
	// HUDLS bar's two in colour [orig: HUD_LoadAllTextures @0x59DF69, @0x59DF0F /
	// @0x59DF39], each HUDSTANCE frame and the parachute and armor icons alpha
	// only [orig: the HUDSTANCE loop @0x59DED8, @0x59DF99, @0x59DFC9].
	static constexpr renderer::TextureRoleId kStaticFrameRole = renderer::TextureRoleId::HudColour;
	static constexpr renderer::TextureRoleId kStanceRole = renderer::TextureRoleId::HudAlphaOnly;
	static constexpr renderer::TextureRoleId kParachuteIconRole = renderer::TextureRoleId::HudAlphaOnly;
	static constexpr renderer::TextureRoleId kArmorIconRole = renderer::TextureRoleId::HudAlphaOnly;
	static constexpr renderer::TextureRoleId kHudlsRole = renderer::TextureRoleId::HudColour;
};

// The mode a VEHICLE_HUD block's interface art loads in: an item def's HUD
// image, alpha only [orig: HUD_LoadAllTextures @0x59E26A, the name at the item
// record's +0xA74 whose texture the mounted panel's gate reads at
// itemDef+0x960; docs/interface/hud-re.md "The HUD texture loader"].
inline constexpr renderer::TextureRoleId kVehicleHudInterfaceRole = renderer::TextureRoleId::HudAlphaOnly;

// A parsed hudpos colour as the packed 0xAARRGGBB the layout carries (each
// channel clamped to a byte).
uint32_t hud_color_argb(const def::DefHudColor &c);

// Fill `out` from the parsed file. Only the fields the parse authors are
// written; the texture-derived ones (`*_texture_valid`, `*_tex_w/h`,
// `stance_frame0_*`, `box_*`, `lfp_*_texture_valid`, the crosshair pair) and
// `sights` are the device's and are left as they are.
void hud_layout_from_hudpos(const def::DefHudPosFile &file, HudLayout &out,
		HudLayoutAssets &assets);

// The hudpos font name the HUD slot loads at this surface width: the HI name
// above 640 pixels, the LO name at 640 and below, with no fallback from one to
// the other. An empty result (or a failed load) leaves the slot to the bold
// label font, which HudFrameCompiler resolves.
// [orig: HUD_SelectHudposFont @0x591890, called from HUD_InitAllFonts @0x51EFAB]
const std::string &hudpos_font_for_width(const HudLayoutAssets &assets, int surface_w);

} // namespace opennova::hud
