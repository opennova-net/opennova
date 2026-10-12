#pragma once

// The follow pass (ADR 0046 S25, the heights lane, A5): the missions on a sculpted terrain, their entities moved with
// the ground a bake landed.

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
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
// pane's): the view's own where it is a mission, else the open missions whose own family (terrain_family: their
// header's terrain and .env, as a stroke in their view routes) edits the heights `family` resolves for it.
std::vector<const MissionDocument *> missions_under_view(const SessionView &view, const DocumentBase &document,
		const TerrainFamily &family);

// What an open mission stood at when a heights document's ground began to change (its window: the first stroke after
// a landing left nothing due): an entity edited since was put against the ground the views showed then (the live
// heights, the stroke's input over the regions due), so a landing does not move it again.
struct FollowMark {
	uint64_t identity = 0, load = 0, revision = 0;
	std::vector<NodeId> followed; // the rows the window's earlier landings moved (sorted): no edit of the user's
};

// A heights bake landing on a .cpt, for the missions on it.
struct FollowGround {
	std::string cpt;  // project-relative: the .cpt the missions' .trn name
	std::string view; // the heights document (the followers join its scope: what its Save and its Undo reach)
	const std::vector<uint16_t> *before = nullptr, *after = nullptr; // the baked atlas, 1024 x 1024 depth words
	uint64_t link = 0;                                                 // the stroke's gesture token
	const std::map<std::string, FollowMark> *marks = nullptr;          // by mission path; null: every entity follows
};

// A mission's follow held back while a drag is open in it (its gesture: the follow's step would split the drag): each
// entity's change of z, applied to its z as it then stands once the drag ends.
struct FollowDeferred {
	std::string mission, cpt_name, view;
	uint64_t identity = 0, link = 0;
	std::vector<std::pair<NodeAddress, int32_t>> changes; // 16.16
};

struct FollowResult {
	size_t moved = 0;
	std::vector<std::string> missions; // those whose entities moved (a Save of the .cpt writes them too)
	std::map<std::string, std::vector<NodeId>> rows; // the rows each moved
	std::vector<FollowDeferred> deferred;
};

// A heights bake landed on the .cpt `ground.cpt`: its baked atlas went from `before` to `after`. Every mission of the
// project whose ground is that .cpt (its header's terrain and environment resolved as the game reads them, the .trn,
// overcast.def and its .env through one parser naming the .cpt as its polytrn_polydata: the routing a stroke in the
// mission's own view takes, session/terrain_family, D-TERRAIN-18) follows it: each entity that is not height-locked,
// and was not edited since the ground's window opened (`marks`), moves by the change of the ground under it, its z less
// the ground's height there kept, as the original editor's terrain change keeps an unlocked item's height relative to
// the ground and a locked one's absolute [orig: JOTACmed.exe sub_402770 @ 0x402c9f..0x402ccd], the ground sampled as
// that editor samples it (mission::mis_terrain_height, with its scale: mission_base_terrain.h) over the sector grid the
// game resolves for the mission. Each mission that moves takes one batch setting its entities' z, carrying the stroke's
// link so one Undo takes the stroke, its bake and the entities that followed it, its step named "Followed the ground of
// dvxi5.cpt (N entities)"; one with a drag open is deferred (FollowDeferred). A closed mission opens in the background
// only when one of its entities its .mis beside it does not height lock moves (the .mis read and matched as the
// mission's load reads it, mission_names_file.h); a blocked mission, and one whose file changed outside the editor
// under unsaved edits, is left as it is and named in Output.
FollowResult follow_the_ground(SessionCore &core, const FollowGround &ground);
// A deferred follow applied once its mission's drag has ended (true: applied, or its mission gone: done with it);
// false while the drag is open.
bool apply_deferred_follow(SessionCore &core, const FollowDeferred &deferred);

} // namespace opennova::editor
