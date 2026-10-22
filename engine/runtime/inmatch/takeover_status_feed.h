#pragma once

// The takeover dialog's feed: the zone-timer entry nearest the local player
// that is running, holds the player in its zone's cylinder and whose zone the
// player may see on the map, joined with its zone entity's facts
// (hud/hud_takeover_status.h composes and draws the dialog).
// [orig: HUD_DrawTakeoverStatus @0x59B630 -> Entity_FindNearestInProximityList
//  @0x537F80 (g_ZoneTimerList, &g_LocalPlayerEntity->Position, 7)]

#include <runtime/hud/hud_takeover_status.h>
#include <runtime/inmatch/role_feeds.h>
#include <runtime/world/spawn_select.h>

namespace opennova::inmatch {

// The nearest search with filter 7, over every entry of the role's zone-timer
// image whose zone entity this client holds:
//   bit 0, a running entry: the window ([7]) or the value ([12]) active, and a
//     value not yet at its limit ([9] != [10]);
//   bit 1, the player in the zone's cylinder: the 2-D length (the min against
//     2147418112.0, then ftol) within the zone radius << 16 and the height
//     within half of it;
//   bit 2, a regular overlay slot for the zone, whose source byte carries a
//     capture bit (0xC0) when the zone is a numbered SpawnPoint;
// the nearest by the strict 2-D length from 0x40000000. False (`out` cleared)
// without a kernel, a replica runtime, a local player or a qualifying entry.
// [orig: Entity_FindNearestInProximityList @0x537F80 -- the length
//  @0x537FC0..0x538001, the height @0x538003..0x53800E, bit 0 @0x538010..0x53802F,
//  bit 1 @0x538031..0x53804D, bit 2 @0x538050..0x53807E (MapOverlay_FindRegularSlotByEntity
//  @0x538057), the nearest @0x538080..0x53808A]
bool collect_takeover_status(const RoleView &view, const world::SpawnZoneRegistry &zones,
		hud::HudTakeoverFacts &out);

} // namespace opennova::inmatch
