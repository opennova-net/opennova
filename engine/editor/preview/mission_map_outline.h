#pragma once

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#include <editor/preview/mission_items.h>

namespace opennova::threedi {
struct Threedi3di3;
}

namespace opennova::editor {

struct SessionView;

// A model as the mission's 2D map draws it (ADR 0046 S23 C, the maintainer's ask): its geometry seen from straight
// above, from its LOD 0 mesh (the mesh the game draws nearest, the one the 3D view's device places) or a coarser LOD's
// (kMissionOutlineEdgesMax), in the model's own
// words (mission_model_words: forward, left, up; metres, before the item's SCALE). An editor's derivation, nothing of the
// game's: the edges a top-down view shows of the mesh, simplified so a building reads as its plan rather than as its
// triangulation:
//   - an edge of one triangle (an open sheet's border), one where a face looking up meets one that does not (a roof's
//     eaves, a wall's top, a floor's border: the contour seen from above), one where two faces looking up meet at a
//     ridge or a valley (more than 30 degrees), and one where two faces fold back on each other (a fin, a fence drawn
//     from both sides); not one inside a surface (a quad's diagonal) nor one between two walls;
//   - and of those, the ones with no length seen from above while the model stands level (an upright edge), and one
//     that lands on another seen so (a roof's eave over its floor's), drawn once;
// the triangles themselves (what a pick tests); every distinct point (what a box of a tilted entity tests); and the
// convex hull of the points seen from above while level, its footprint's outline.
// A model's plan is LOD 0's where it has kMissionOutlineEdgesMax edges or fewer; a model whose LOD 0 plan has more (a
// palm's fronds, a hut's thatch) is drawn by its first coarser LOD whose plan has no more, else by the LOD whose plan
// has the fewest; one whose every plan is empty (upright cards alone) by its footprint's outline. JO:CA's 00TRa
// (943 entities, 156 models) draws so in some 80 000 lines; LOD 0's every edge seen from above in some 800 000.
inline constexpr size_t kMissionOutlineEdgesMax = 192;

// The view an outline is seen in: from above (the map's plan: the up word looked along, forward and left seen), or
// from the side (an elevation: the left word looked along, forward and up seen; the palette's picture of an item).
enum class MissionOutlineView : uint8_t { Above, Side };

struct MissionModelOutline {
	int lod = 0; // the LOD the plan is of
	MissionOutlineView view = MissionOutlineView::Above;
	std::vector<float> edges; // six words an edge: its two ends
	std::vector<float> triangles; // nine words a triangle
	std::vector<float> points; // three words a distinct point
	std::vector<float> hull; // (forward, left) pairs, counter-clockwise
	float reach = 0.0f; // the farthest point from the origin seen from above, metres
};

// The outline of `model` (its LOD 0's, else a coarser LOD's: kMissionOutlineEdgesMax); false (out empty) where it has
// no mesh.
// `view` Side: its elevation by the same rules, the left word in the up word's place (the faces looking at the viewer,
// the edges with no length seen from the side dropped); its hull in (forward, up).
bool mission_model_outline(const threedi::Threedi3di3 &model, MissionModelOutline &out,
		MissionOutlineView view = MissionOutlineView::Above);

// A point of the model (render frame: threedi's vertex position, x as the file holds it) as the placement matrix takes
// it: the words (forward, left, up) the device's model holds the vertex at, (p2, -p0, p1); a user point's are
// mission_model_words of threedi_user_point_position.
void mission_vertex_words(const float position[3], double out[3]);

// An entity's footprint on the map: its model's outline placed as the entity stands (its position, its angles, its
// item's SCALE; mission_anchor_offset's placement matrix), in mission metres seen from above. A tilted entity's hull is
// the hull of its every point placed; a level one's its model's hull placed.
struct MissionMapFootprint {
	std::shared_ptr<const MissionModelOutline> outline;
	double origin[2] = { 0.0, 0.0 }; // the entity's position
	double basis[2][3] = {}; // mission (x, y) = origin + basis x words
	std::vector<double> hull; // (x, y) pairs, counter-clockwise
	double centre[2] = { 0.0, 0.0 }; // the hull's cull circle
	double reach = 0.0;
	double area = 0.0; // the hull's
	// A point of the model's words placed.
	void place(const float words[3], double &x, double &y) const;
	// Whether (x, y) falls on the model's geometry seen from above (inside one of its triangles placed).
	bool covers(double x, double y) const;
	// The distance from (x, y) to the hull (0 inside it).
	double distance(double x, double y) const;
	// Whether the hull meets the axis box [x0, x1] x [y0, y1].
	bool meets(double x0, double y0, double x1, double y1) const;
};

// `outline` placed at (x, y) at the record's angles (degrees) and the item's SCALE (16.16; 0 unscaled).
MissionMapFootprint mission_map_footprint(std::shared_ptr<const MissionModelOutline> outline, double x, double y,
		double pitch, double yaw, double roll, int32_t scale_q16);

// The outlines of the items a map's entities name (ADR 0046 S23 C): an item's model (its graphic, through the project's
// graph) and its SCALE as the item cache reads them (MissionItemCache::facts, which reads each model once while its
// file's stamp stands and keeps its outline), asked a few at a time within a budget of time a step, so a mission of a
// thousand entities draws its pins at once and its models' outlines over the frames that follow. Everything is asked
// again where the graph or the files' generation moved (the cache reads again only what moved).
class MissionOutlineCache {
public:
	struct Item {
		std::shared_ptr<const MissionModelOutline> outline; // null: no model, or one with no mesh
		int32_t scale_q16 = 0;
		uint64_t epoch = 0; // the generations it was asked under
	};
	// Asks the items of `items` not asked under the graph and files that stand, until `budget_us` microseconds have
	// gone (one at least); true when any item's outline or SCALE moved.
	bool step(const SessionView &view, const std::vector<int64_t> &items, int64_t budget_us);
	// Items of the last step's list not asked yet.
	size_t pending() const { return pending_; }
	const Item *item(int64_t id) const;
	// How many models it has read in all (a test's count).
	size_t files_read() const { return cache_.files_read(); }

private:
	MissionItemCache cache_{ true };
	std::unordered_map<int64_t, Item> items_;
	uint64_t graph_generation_ = 0;
	uint64_t files_generation_ = 0;
	bool generations_ = false;
	uint64_t epoch_ = 0;
	size_t pending_ = 0;
};

} // namespace opennova::editor
