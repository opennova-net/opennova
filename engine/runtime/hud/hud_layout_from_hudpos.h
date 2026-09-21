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
	// The HUD font named by hudpos: FONTHUD1_HI first, FONTHUD1_LO as the
	// fallback ("" when neither is authored).
	std::string font;
	// The static frame retail draws (hud_static_frame_index: the LAST authored
	// line); "" when no StaticFrame line is authored, and then
	// HudLayout::frame_pos stays absent.
	std::string static_frame;
	// The six HUDSTANCE frames by their explicit id.
	std::array<std::string, 6> stance_textures{};
	std::string parachute_icon;
	std::string armor_icon;
};

// A parsed hudpos colour as the packed 0xAARRGGBB the layout carries (each
// channel clamped to a byte).
uint32_t hud_color_argb(const def::DefHudColor &c);

// Fill `out` from the parsed file. Only the fields the parse authors are
// written; the texture-derived ones (`*_texture_valid`, `*_tex_w/h`,
// `stance_frame0_*`, `box_*`, `lfp_*_texture_valid`, the crosshair pair) and
// `sights` are the device's and are left as they are.
void hud_layout_from_hudpos(const def::DefHudPosFile &file, HudLayout &out,
		HudLayoutAssets &assets);

} // namespace opennova::hud
