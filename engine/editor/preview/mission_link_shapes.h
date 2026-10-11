#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include <editor/documents/mission_links.h>
#include <editor/preview/canvas_gesture.h>
#include <editor/preview/canvas_half.h>
#include <editor/preview/viewport_overlay.h>

namespace opennova::editor {

class Document;
class MissionDocument;
class MissionScene;
struct MissionUses;

// A mission's logic drawn where the things it names stand (ADR 0046 S24, In the views), in the 3D view
// and on the map alike: for the selected event, trigger or action (documents/mission_links), each record
// it names rung where it stands (what its triggers watch in one colour, what its actions act on in
// another; a group's members each and a spoke from their middle, a path's line through its stops), a
// line from each thing watched to each thing acted on, and each distance trigger's reach about its
// subject as the game measures it: a sphere (the 3D distance), drawn as its circle at the subject's
// height (from straight above, on the map, that circle is the whole of it) and, in 3D, its outline; a
// sees trigger's 30 degrees either side of the subject's heading as a wedge of it. The selected trigger
// or action's own are drawn bolder than the rest of its event's. And on every mark some event names, a
// badge with how many events name it. The colours are the editor's.

inline constexpr uint32_t kMissionWatchedRgb = 0xFF5FD2; // what a trigger watches
inline constexpr uint32_t kMissionActedRgb = 0xB48CFF; // what an action acts on
inline constexpr uint32_t kMissionBadgeRgb = 0xFFB4E8;

// A record a logic record names, found in the scene: its anchor (an entity's position, an area's middle at
// its z_min, a group's members' middle, a path's first stop, mission metres), whether a trigger names it
// (else an action), whether the selected trigger or action does, and what it reaches: a group's members,
// a path's stops in order (`path`), a marker number's markers.
struct MissionLinkSpot {
	NodeAddress record;
	std::array<double, 3> at = { 0.0, 0.0, 0.0 };
	bool watched = false;
	bool selected = false;
	bool area = false; // an area: its anchor's height is the view's (the ground where a device answers)
	int area_index = -1; // into scene.areas()
	bool path = false;
	std::vector<std::array<double, 3>> members;
};
// A distance trigger's reach: about its subject's position, `metres` across its radius; a sees trigger's
// wedge about the subject's heading (a compass yaw, degrees).
struct MissionLinkRing {
	std::array<double, 3> at = { 0.0, 0.0, 0.0 };
	double metres = 0.0;
	bool facing = false;
	double yaw = 0.0;
	bool selected = false;
};
struct MissionLinkScene {
	std::vector<MissionLinkSpot> spots;
	std::vector<std::pair<size_t, size_t>> lines; // a watched spot to an acted-on one
	std::vector<MissionLinkRing> rings;
	bool any() const { return !spots.empty() || !rings.empty(); }
};
// The links of `selected` (an event, a trigger or an action of `document`) placed over `scene`: what the
// scene does not hold (an event, a value nothing holds) has no spot. Empty for any other record.
MissionLinkScene mission_link_scene(const MissionDocument &document, const MissionScene &scene,
		const NodeAddress &selected);

// How a view draws a mission point: its pixel (false off the picture: behind the eye), a segment's ends
// clipped to what it sees (false: none of it), and, in 3D, the pixels a sphere of `metres` about a point
// spans across (0: seen from above, its circle at its height is its outline).
struct MissionLinkView {
	std::function<bool(const std::array<double, 3> &at, CanvasPoint &out)> point;
	std::function<bool(const std::array<double, 3> &a, const std::array<double, 3> &b, CanvasPoint &from,
			CanvasPoint &to)>
			segment;
	std::function<float(const std::array<double, 3> &at, double metres)> outline;
	// An area spot's anchor height in this view (null: its z_min).
	std::function<double(const MissionLinkSpot &spot)> area_height;
};
void mission_link_shapes(const MissionLinkScene &links, const MissionLinkView &view, OverlayList &out);

// The badge beside a mark `count` events name ("2"), its hover saying so: nothing for 0.
void mission_link_badge(CanvasPoint at, size_t count, OverlayList &out);
// The SelectRecord of every member of the groups `records` carry or are (documents/mission_links'
// mission_group_members), the primary kept where it is a member (else the first): Select group members, the views'
// `select_group` command (S24, In the views). False, with why, where they carry none.
bool mission_select_group(const MissionDocument &document, const std::vector<NodeAddress> &records,
		const NodeAddress &primary, CanvasRequests &out, std::string &error);

// S24 pick mode (the workspace's document `pick`, which a board chip's Pick in view asks): what a click on a mark
// gives the picked slot, by mark (`values` its value, `fills` whether it fills the slot), made once a frame over
// the views' marks (an entity's or an area's record each); the hint the views show while the pick waits ("Click an
// area for Zone (Esc cancels)"); the requests a click raises (the record's parameter set, then the pick ended) and
// those Esc or a click on nothing raises (the pick ended).
struct MissionPickFrame {
	NodeAddress record;
	int slot = -1;
	MissionPickSlot takes;
	std::vector<char> fills;
	std::vector<int64_t> values;
	bool active() const { return record.row != 0; }
	bool fills_mark(int mark) const { return mark >= 0 && size_t(mark) < fills.size() && fills[size_t(mark)]; }
};
MissionPickFrame mission_pick_frame(const MissionDocument &document, const NodeAddress &record, int slot,
		const std::vector<NodeAddress> &marks);
std::string mission_pick_hint(const MissionPickFrame &pick);
void mission_pick_requests(const std::string &path, const MissionPickFrame &pick, int mark, CanvasRequests &out);
void mission_pick_end(const std::string &path, CanvasRequests &out);
// The views' `pick` command (the wire's click of a mark while a pick waits): the record `ids` names given to the
// pick the workspace holds for `document`, as a click on its mark gives it (the parameter set, the pick ended).
// False, with why, where no pick waits, the context takes no edits, or the record fills no slot of it.
bool mission_pick_command(const ViewportContext &context, const Document &document, const std::vector<NodeId> &ids,
		CanvasRequests &out, std::string &error);
// The ring a candidate of the pick is drawn with (each mark that fills the slot).
void mission_pick_candidate(CanvasPoint at, OverlayList &out);

// The hover's words for a record events name (its uses, documents/mission_uses): how many events, then
// each one's sentence, at most `most` ("" for a record no event names).
std::string mission_link_hover(const MissionUses &uses, size_t most = 8);

} // namespace opennova::editor
