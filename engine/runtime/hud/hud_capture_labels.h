#pragma once

// The capture-point labels: the walk over the map-overlay slots that picks
// every spawn-point zone carrying a zone number and a capture bit, its marker
// type, colour, letter and name, and its zone-timer bar facts. The device
// projects each admitted point; the frame compiler draws it through the
// marker drawer's types 8/9 and, in binoculars, the progress bar
// (HudFrameCompiler::element_capture_point_labels, hud_capture_labels.cpp).
// [orig: Render_CapturePointLabels @0x5a2840, called unconditionally from
//  HUD_RenderAllOverlays @0x5a852b after the scope-details fork and before
//  HUD_DrawZoneStatusPanel; witness record docs/interface/hud-re.md "The
//  capture-point labels"]

#include <cstdint>
#include <vector>

#include <runtime/hud/game_text_lookup.h>
#include <runtime/hud/hud_combat.h>
#include <runtime/hud/hud_minimap.h>

namespace opennova::hud {

// The label's anchor sits 2 units above the entity [orig: `add ecx, 20000h`
// @0x5a2a81].
inline constexpr int32_t kCaptureLabelLiftQ16 = 0x20000;
// HUD_DrawEntityMarker's types for the zone tiles: 8 binds lfp_alf.tga
// (another team's zone), 9 lfp_dlf.tga (the viewer's) [orig: push 8 / push 9
// @0x5a2aae / @0x5a2ab2; HUD_DrawEntityMarker @0x593702..0x593724].
inline constexpr uint8_t kMarkerTypeZoneOther = 8;
inline constexpr uint8_t kMarkerTypeZoneOwn = 9;
// The under-attack flash colour [orig: `mov edx, 0FFFFFF00h` @0x5a2a5d].
inline constexpr uint32_t kCaptureLabelFlashColor = 0xFFFFFF00u;

// The walk: the transient, persistent then special bank (retail's three
// contiguous banks, 1160 slots), skipping a free handle, a special (0x40)
// slot, an unresolved entity, a def without the SpawnPoint attrib (0x40000),
// a zero zone number and a slot whose source byte (the zone number at
// classification) carries neither capture bit (0xC0). The strings resolve
// through `gametext` (GameText_GetString: a miss is ""). `out` is cleared
// first.
void capture_point_label_walk(const std::vector<HudMinimapMarker> &markers, uint8_t local_team,
		const GameTextLookup &gametext, std::vector<HudCaptureLabel> &out);

} // namespace opennova::hud
