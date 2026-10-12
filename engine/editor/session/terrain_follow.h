#pragma once

// The follow pass (ADR 0046 S25, the heights lane, A5): the missions on a sculpted terrain, their entities moved with
// the ground a bake landed.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <editor/model/value.h>

namespace opennova::editor {

class DocumentBase;
class MissionDocument;
class SessionCore;
struct SessionView;
struct TerrainFamily;

// Whether the entity at `address` of `mission` is height-locked (its height_lock field set; a mission whose entities
// carry none: each follows the ground).
bool height_locked(const MissionDocument &mission, const NodeAddress &address);

// The entities of `mission` within `radius` of (x, y) (mission units, on the plane): those that follow the ground when
// it is sculpted and those height-locked (the Terrain pane's lock line, the terrain query's lock op).
struct GroundFollowers {
	size_t follow = 0, locked = 0;
};
GroundFollowers ground_followers(const MissionDocument &mission, double x, double y, double radius);
// The open missions a heights stroke in the view of `document` moves the entities of (the lock op's and the Terrain
// pane's): the view's own where it is a mission, else the open missions on the terrain `family` resolves for it (the
// graph's terrain references to its .trn).
std::vector<const MissionDocument *> missions_under_view(const SessionView &view, const DocumentBase &document,
		const TerrainFamily &family);

// A heights bake landed on the .cpt at `cpt` (project-relative): its baked atlas went from `before` to `after` (1024 x
// 1024 depth words each, the game's height atlas). Every mission of the project whose terrain (a .trn naming the .cpt
// as its polytrn_polydata) is the ground they read follows it: each entity that is not height-locked moves by the
// change of the ground under it, its z less the ground's height there kept, as the original editor's terrain change
// keeps an unlocked item's height relative to the ground and a locked one's absolute [orig: JOTACmed.exe sub_402770 @
// 0x402c9f..0x402ccd], the ground sampled as that editor samples it, over each mission's .trn
// (mission_base_terrain.h, mission::mis_terrain_height). A height-locked entity (its `height_lock` field set) keeps
// its z. Each mission that moves takes one batch setting its entities' z, carrying `link` (the stroke's gesture token)
// so one Undo takes the stroke, its bake and the entities that followed it, its step named "Followed the ground of
// dvxi5.cpt (N entities)". A closed mission opens in the background only when one of its entities moves; a blocked
// mission, and one whose file changed outside the editor under unsaved edits, is left as it is and named in Output.
// The entities moved.
size_t follow_the_ground(SessionCore &core, const std::string &cpt, const std::vector<uint16_t> &before,
		const std::vector<uint16_t> &after, uint64_t link);

} // namespace opennova::editor
