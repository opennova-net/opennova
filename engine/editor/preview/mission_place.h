#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <editor/documents/mission_table.h>
#include <editor/model/edit.h>
#include <editor/model/node.h>

namespace opennova::editor {

class Document;
class MissionDocument;
class MissionScene;
class ViewportDevice;

// What the mission viewport's placing and tweaking verbs write (ADR 0046 S15, Placing and tweaking),
// each one batch of the document's own edits, so one undo step, planned headless from the scene and
// the document (the canvas raises them; the wire's `command` and `drop` plan the same):
//
// - a duplicate: each entity or area copied (EditOperation::Duplicate: an entity takes a fresh SSN, an
//   area a fresh zone id, documents/mission_document) and the copy moved `east` and `north` metres
//   (Ctrl+D's one step, an Alt-drag's way), its height over the ground kept with `stick` over a
//   device that answers it, the primary's copy first so the batch selects it as the primary;
// - an area: an area trigger added with the box's four bounds (its z bounds and flags the new
//   record's, documents/mission_document's make_node);
// - a stop: a marker of `item` added at the point, facing `yaw`, and a stop naming it appended to the
//   path of number `path` (a stop names a marker by its index among the file's markers [orig:
//   Pool_GetEntryUnchecked @0x441FC0]: the new marker's, the last of its band);
// - the same item: every entity whose item is one of the given records' items.
//
// Positions are mission units, which the game reads as metres (a trigger's whole-metre distance is
// shifted into the 16.16 world units positions are in: world/world-wac-ai-re.md, the SSN distance
// triggers [orig: Entity_CompareDistancesToTarget @0x4F12E0]); a yaw whole degrees clockwise from
// north, as the file stores it (the engine's heading is 90 minus it [orig: Entity_SpawnFromBMSRecord
// @0x40EB42..0x40EB66]).

// The copies of `records` (entities and areas; the primary's first where it is among them), each
// moved by (`east`, `north`). False with `error` for a record that is neither, or none.
bool mission_duplicate_edits(const Document &document, const MissionScene &scene, const std::vector<NodeAddress> &records,
		const NodeAddress &primary, double east, double north, bool stick, const ViewportDevice *device,
		std::vector<Edit> &out, std::string &error);

// An area trigger over the box from `a` to `b` (mission x and y), each bound snapped to `snap`
// metres. False with `error` for a box with no extent on an axis.
bool mission_area_edits(const double a[2], const double b[2], float snap, std::vector<Edit> &out, std::string &error);

// A stop of the path of number `path`: a marker of `item` added at mission point `at`, facing `yaw`
// degrees, and a stop naming it after the path's last. False with `error` for a path the file has
// not, a command (0 and 123..127 name no route), a path holding its 32 stops, an item placed in
// another pool than the markers.
bool mission_stop_edits(const MissionDocument &document, int path, int64_t item, MissionKind pool, const double at[3],
		int yaw, std::vector<Edit> &out, std::string &error);

// The marker item a new stop of path `path` takes: the item of the marker its last stop names, else
// of the last stop of any path of the mission, else 0 (none: the planner asks for one).
int64_t mission_stop_item(const MissionDocument &document, const MissionScene &scene, int path);

// Every entity of the scene whose item is the item of one of `records`, in the scene's order.
std::vector<NodeAddress> mission_same_item(const MissionScene &scene, const std::vector<NodeAddress> &records);

// Degrees in 0..359, as the file stores a yaw.
int mission_wrapped_yaw(double degrees);

} // namespace opennova::editor
