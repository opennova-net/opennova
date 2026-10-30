#pragma once

// THE WORLD-ANCHORED LABELS the HUD pass draws over the scene: the seat and armory labels around
// the local player and the overhead friendly tags. Each gathered row's world point goes through
// the HUD's projector (hud_viewport.h) into the frame state the compiler draws; the gathers are
// the world's (world::LocalPlayer::collect_attach_labels; inmatch::collect_friendly_tags).

#include <vector>

#include <runtime/hud/game_text_lookup.h>
#include <runtime/hud/hud_frame.h>
#include <runtime/hud/hud_viewport.h>
#include <runtime/world/friendly_tags.h>
#include <runtime/world/vehicle_attach.h>

namespace opennova::world {
class LocalPlayer;
class VehicleOccupancySource;
} // namespace opennova::world

namespace opennova::hud {

// The labels of the gathered seats and armory points: each point through the viewport, a
// clipped one (any flag, a side plane as the near plane) skipped, each kept one with its text
// from the gametext's Overlays section (attach_label_text) and its nearest highlight.
// [orig: HUD_DrawVehicleSeatAndArmoryLabels @0x5A3290 - Math_FixedPointTransformPoint22
//  @0x5A3628 and HUD_ClipPointToFrustumAndProject @0x5A3655, then `test eax,eax; jnz` past
//  the label: the seat loop @0x5A3655..0x5A365F, the armory loop @0x5A3879..0x5A3883]
void project_attach_labels(const std::vector<world::AttachLabel> &labels,
		const HudViewport &viewport, const GameTextLookup &gametext,
		std::vector<HudAttachLabel> &out);

// The local player's labels this frame: the seat scan (`occupancy`, a joiner's decoded rows;
// null on every other role) projected as above. Out is cleared first.
void fill_attach_labels(world::LocalPlayer &local, const world::VehicleOccupancySource *occupancy,
		const HudViewport &viewport, const GameTextLookup &gametext,
		std::vector<HudAttachLabel> &out);

// The friendly tags of the gathered rows: each anchor (friendly_tag_anchor) through the
// viewport, kept whatever its clip (a clipped tag bails in the compiler unless the death
// screen pins it), its distance the anchor's from the view block's eye held at 2147418112.0
// and truncated, and the row's facts.
// [orig: HUD_DrawEntityLabel @0x5A39B0 - the anchor @0x5A3A84..0x5A3A98, the distance
//  @0x5A3A9C..0x5A3AFB (flt_7C19E0's cap @0x5A3ADE..0x5A3AF7), the projection
//  Math_FixedPointTransformPoint22 @0x5A3B47 + HUD_ClipPointToFrustumAndProject @0x5A3B51]
void project_friendly_tags(const std::vector<world::FriendlyTagSource> &tags,
		const HudViewport &viewport, std::vector<HudFriendlyTag> &out);

} // namespace opennova::hud
