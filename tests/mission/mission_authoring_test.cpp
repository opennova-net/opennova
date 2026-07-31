// Unit test for the mission authoring facade (mission/authoring.h): the
// editing policies the editor shell used to hand-roll — the item-type -> entity
// list table, the path-consistent marker item-id policy, and the author-time
// Ground-userpoint bake [orig: sub_401A90, dfx2med.exe] — now portable engine
// capabilities over MissionDocument.

#include <cmath>
#include <cstddef>

#include "common/test_expect.h"
#include "def/def.h"
#include "mission/authoring.h"
#include "mission/mission.h"

using opennova::mission::EntityKind;
using opennova::mission::EntityRecord;
using opennova::mission::EntityTransform;
using opennova::mission::MissionDocument;
using opennova::mission::WaypointPath;
namespace authoring = opennova::mission::authoring;

namespace {

bool near(float a, double b, double eps = 1e-3) {
	return std::fabs(static_cast<double>(a) - b) <= eps;
}

} // namespace

int main() {
	// --- entity_kind_for_item_type: the full items.def type range ---------
	// Types are the witnessed engine values at ItemDef+0x5C [orig:
	// ItemDef_ParseProperty @ 0x49eb00; docs/world/itemdef-re.md D-ITEMDEF-1]:
	// vehicle=1, decoration/foliage=2, person=3, marker=4, building=5,
	// powerup/object=6, effect=8; 0=unset, 7 unused. Decoration and Foliage
	// (value 2) land in Building alongside Building (5); every other type is Item.
	{
		TEST_EXPECT(authoring::entity_kind_for_item_type(DEF_ITEM_TYPE_PERSON) == EntityKind::Organic);      // 3
		TEST_EXPECT(authoring::entity_kind_for_item_type(DEF_ITEM_TYPE_BUILDING) == EntityKind::Building);   // 5
		TEST_EXPECT(authoring::entity_kind_for_item_type(DEF_ITEM_TYPE_DECORATION) == EntityKind::Building); // 2
		TEST_EXPECT(authoring::entity_kind_for_item_type(DEF_ITEM_TYPE_FOLIAGE) == EntityKind::Building);    // 2 (= decoration)
		TEST_EXPECT(authoring::entity_kind_for_item_type(DEF_ITEM_TYPE_MARKER) == EntityKind::Marker);       // 4
		TEST_EXPECT(authoring::entity_kind_for_item_type(DEF_ITEM_TYPE_UNSET) == EntityKind::Item);          // 0
		TEST_EXPECT(authoring::entity_kind_for_item_type(DEF_ITEM_TYPE_VEHICLE) == EntityKind::Item);        // 1
		TEST_EXPECT(authoring::entity_kind_for_item_type(DEF_ITEM_TYPE_POWERUP) == EntityKind::Item);        // 6
		TEST_EXPECT(authoring::entity_kind_for_item_type(DEF_ITEM_TYPE_OBJECT) == EntityKind::Item);         // 6 (= powerup)
		TEST_EXPECT(authoring::entity_kind_for_item_type(DEF_ITEM_TYPE_EFFECT) == EntityKind::Item);         // 8
		TEST_EXPECT(authoring::entity_kind_for_item_type(7) == EntityKind::Item);                            // unused engine value
		TEST_EXPECT(authoring::entity_kind_for_item_type(-1) == EntityKind::Item);                           // no db loaded
	}

	// --- bake_ground_transform: zero rotation = plain anchor subtraction
	//     through the model-forward correction pair -----------------------
	{
		// At zero rotation the matrix is Rz(90) * Rz(90) = Rz(180): the anchor
		// (ax, ay, az) maps to (-ax, -ay, az). Hand-computed.
		EntityTransform hit;
		hit.x = 100.0f; hit.y = 50.0f; hit.z = 10.0f;
		const float anchor[3] = {1.0f, 2.0f, 3.0f};
		const EntityTransform baked = authoring::bake_ground_transform(hit, anchor);
		TEST_EXPECT(near(baked.x, 100.0 - (-1.0)));
		TEST_EXPECT(near(baked.y, 50.0 - (-2.0)));
		TEST_EXPECT(near(baked.z, 10.0 - 3.0));
		TEST_EXPECT(baked.pitch == 0 && baked.yaw == 0 && baked.roll == 0);
	}

	// --- bake_ground_transform: yaw-only, hand-computed -------------------
	{
		// yaw=90: R = Rz(90-90) * Ry(0) * Rx(0) * Rz(90) = Rz(90).
		// Rz(90): (x, y, z) -> (-y, x, z). Anchor (1, 2, 3) -> (-2, 1, 3).
		EntityTransform hit;
		hit.x = 0.0f; hit.y = 0.0f; hit.z = 0.0f;
		hit.yaw = 90;
		const float anchor[3] = {1.0f, 2.0f, 3.0f};
		const EntityTransform baked = authoring::bake_ground_transform(hit, anchor);
		TEST_EXPECT(near(baked.x, 2.0));
		TEST_EXPECT(near(baked.y, -1.0));
		TEST_EXPECT(near(baked.z, -3.0));
		TEST_EXPECT(baked.yaw == 90);
	}

	// --- bake_ground_transform: pitch-only, hand-computed -----------------
	{
		// pitch=90: R = Rz(90) * Ry(-90) * Rz(90).
		// Step by step on anchor (1, 2, 3):
		//   Rz(90):  (1, 2, 3)  -> (-2, 1, 3)
		//   Ry(-90): (-2, 1, 3) -> (-3, 1, -2)
		//   Rz(90):  (-3, 1, -2) -> (-1, -3, -2)
		EntityTransform hit;
		hit.pitch = 90;
		const float anchor[3] = {1.0f, 2.0f, 3.0f};
		const EntityTransform baked = authoring::bake_ground_transform(hit, anchor);
		TEST_EXPECT(near(baked.x, 1.0));
		TEST_EXPECT(near(baked.y, 3.0));
		TEST_EXPECT(near(baked.z, 2.0));
	}

	// --- place_entity_grounded: kind derivation + zero-rotation bake ------
	{
		MissionDocument doc;
		doc.create_default();
		const float hit[3] = {64.0f, -32.0f, 8.0f};
		const float anchor[3] = {0.5f, 0.25f, 1.5f};

		EntityRecord rec;
		// id 102001 is retail cblock02 "Pile of cinder blocks #2", type decoration (2);
		// decoration shares the Building list with building (the non-obvious case).
		TEST_EXPECT(authoring::place_entity_grounded(doc, 102001, DEF_ITEM_TYPE_DECORATION, hit, anchor, &rec)); // 2 = Decoration -> Building list
		TEST_EXPECT(rec.kind == EntityKind::Building);
		TEST_EXPECT(rec.item_id == 102001);
		// Place-time bake is the UNROTATED subtraction (new records are
		// rotation-zero; mirrors the editor's place path).
		TEST_EXPECT(near(rec.transform.x, 64.0 - 0.5));
		TEST_EXPECT(near(rec.transform.y, -32.0 - 0.25));
		TEST_EXPECT(near(rec.transform.z, 8.0 - 1.5));
		TEST_EXPECT(rec.transform.pitch == 0 && rec.transform.yaw == 0 && rec.transform.roll == 0);

		// Markers ignore the anchor entirely.
		EntityRecord marker;
		TEST_EXPECT(authoring::place_entity_grounded(doc, 106005, DEF_ITEM_TYPE_MARKER, hit, anchor, &marker)); // 4 = Marker
		TEST_EXPECT(marker.kind == EntityKind::Marker);
		TEST_EXPECT(near(marker.transform.x, 64.0));
		TEST_EXPECT(near(marker.transform.y, -32.0));
		TEST_EXPECT(near(marker.transform.z, 8.0));
	}

	// --- move_entity_grounded: rotation preserved + rotated bake ----------
	{
		MissionDocument doc;
		doc.create_default();
		const float hit[3] = {0.0f, 0.0f, 0.0f};
		const float anchor[3] = {1.0f, 2.0f, 3.0f};
		EntityRecord rec;
		// id 102001 = retail cblock02, type decoration (2) -> Building list.
		TEST_EXPECT(authoring::place_entity_grounded(doc, 102001, DEF_ITEM_TYPE_DECORATION, hit, anchor, &rec)); // 2 = Decoration -> Building list
		// Give it a yaw, then re-ground: the bake must rotate the anchor.
		EntityTransform with_yaw = rec.transform;
		with_yaw.yaw = 90;
		TEST_EXPECT(doc.set_entity_transform(EntityKind::Building, rec.index, with_yaw));

		const float new_hit[3] = {10.0f, 20.0f, 30.0f};
		TEST_EXPECT(authoring::move_entity_grounded(doc, EntityKind::Building, rec.index, new_hit, anchor));
		EntityRecord moved;
		TEST_EXPECT(doc.get_entity(EntityKind::Building, rec.index, moved));
		TEST_EXPECT(moved.transform.yaw == 90); // rotation kept
		// yaw=90 bake (see hand computation above): anchor -> (-2, 1, 3).
		TEST_EXPECT(near(moved.transform.x, 10.0 + 2.0));
		TEST_EXPECT(near(moved.transform.y, 20.0 - 1.0));
		TEST_EXPECT(near(moved.transform.z, 30.0 - 3.0));

		// Missing entity rejects without touching the document.
		TEST_EXPECT(!authoring::move_entity_grounded(doc, EntityKind::Organic, 99, new_hit, anchor));
	}

	// --- marker_item_id_for_path: path-local reuse, canonical fallback ----
	{
		MissionDocument doc;
		doc.create_default();
		// Empty path -> the canonical waypoint id.
		TEST_EXPECT(authoring::marker_item_id_for_path(doc, 0) == authoring::kWaypointMarkerItemId);

		// Seed path 0 with a variant waypoint id; new markers on path 0 reuse it.
		const float hit[3] = {1.0f, 2.0f, 0.0f};
		EntityRecord first;
		WaypointPath path;
		TEST_EXPECT(doc.add_waypoint_marker(0, 106026, EntityTransform{1, 2, 0, 0, 0, 0}, -1, &first, &path));
		TEST_EXPECT(authoring::marker_item_id_for_path(doc, 0) == 106026);

		EntityRecord second;
		TEST_EXPECT(authoring::add_path_marker_grounded(doc, 0, hit, -1, &second, &path));
		TEST_EXPECT(second.item_id == 106026);
		TEST_EXPECT(path.marker_indices.size() == 2);

		// The old "copy markers[0] from the whole scene" bug: a different id
		// on ANOTHER path must never bleed into this one.
		EntityRecord other;
		TEST_EXPECT(doc.add_waypoint_marker(1, 106031, EntityTransform{5, 5, 0, 0, 0, 0}, -1, &other, nullptr));
		TEST_EXPECT(authoring::marker_item_id_for_path(doc, 0) == 106026);
		// And a fresh empty path still falls back to the canonical id.
		TEST_EXPECT(authoring::marker_item_id_for_path(doc, 2) == authoring::kWaypointMarkerItemId);

		// Markers store the hit directly (no anchor math).
		TEST_EXPECT(near(second.transform.x, 1.0));
		TEST_EXPECT(near(second.transform.y, 2.0));
	}

	// --- reground_entities: dry-run count, rotated bake, epsilon skip ------
	{
		MissionDocument doc;
		doc.create_default();
		const float anchor[3] = {1.0f, 2.0f, 3.0f};
		const float hit_old[3] = {100.0f, 50.0f, 10.0f};
		// Two Building-list decorations on the old ground (one yawed after placement) + a marker.
		// id 102001 = retail cblock02, type decoration (2) -> Building list.
		EntityRecord a, b, m;
		TEST_EXPECT(authoring::place_entity_grounded(doc, 102001, DEF_ITEM_TYPE_DECORATION, hit_old, anchor, &a)); // 2 = Decoration -> Building list
		TEST_EXPECT(authoring::place_entity_grounded(doc, 102001, DEF_ITEM_TYPE_DECORATION, hit_old, anchor, &b)); // 2 = Decoration -> Building list
		EntityTransform with_yaw = b.transform;
		with_yaw.yaw = 90;
		TEST_EXPECT(doc.set_entity_transform(EntityKind::Building, b.index, with_yaw));
		TEST_EXPECT(authoring::place_entity_grounded(doc, 106005, DEF_ITEM_TYPE_MARKER, hit_old, anchor, &m)); // 4 = Marker

		// The terrain rose under everything: ground z 10 -> 14.
		authoring::RegroundRequest reqs[3];
		reqs[0].kind = EntityKind::Building;
		reqs[0].index = a.index;
		reqs[1].kind = EntityKind::Building;
		reqs[1].index = b.index;
		reqs[2].kind = EntityKind::Marker;
		reqs[2].index = m.index;
		for (authoring::RegroundRequest &request : reqs) {
			request.ground_hit_bms[0] = 100.0f;
			request.ground_hit_bms[1] = 50.0f;
			request.ground_hit_bms[2] = 14.0f;
			for (int i = 0; i < 3; ++i) {
				request.ground_anchor_bms[i] = anchor[i];
			}
		}

		// Dry run: counts every drifted entity without writing a thing, and
		// reports the would-move rows (indices into reqs, request order).
		std::vector<size_t> would_move;
		TEST_EXPECT(authoring::reground_entities(doc, reqs, 3, 0.01f, false, &would_move) == 3);
		TEST_EXPECT(would_move == (std::vector<size_t>{0, 1, 2}));
		EntityRecord untouched;
		TEST_EXPECT(doc.get_entity(EntityKind::Building, a.index, untouched));
		TEST_EXPECT(near(untouched.transform.z, 10.0 - 3.0));

		// Apply: zero-rot bake (anchor -> (-1,-2,3)), yaw-90 bake (-> (-2,1,3)),
		// marker stores the hit directly; all rotations kept. The moved-row
		// report matches the count.
		std::vector<size_t> moved_rows;
		TEST_EXPECT(authoring::reground_entities(doc, reqs, 3, 0.01f, true, &moved_rows) == 3);
		TEST_EXPECT(moved_rows == (std::vector<size_t>{0, 1, 2}));
		EntityRecord moved_a, moved_b, moved_m;
		TEST_EXPECT(doc.get_entity(EntityKind::Building, a.index, moved_a));
		TEST_EXPECT(near(moved_a.transform.x, 101.0));
		TEST_EXPECT(near(moved_a.transform.y, 52.0));
		TEST_EXPECT(near(moved_a.transform.z, 11.0));
		TEST_EXPECT(doc.get_entity(EntityKind::Building, b.index, moved_b));
		TEST_EXPECT(moved_b.transform.yaw == 90);
		TEST_EXPECT(near(moved_b.transform.x, 102.0));
		TEST_EXPECT(near(moved_b.transform.y, 49.0));
		TEST_EXPECT(near(moved_b.transform.z, 11.0));
		TEST_EXPECT(doc.get_entity(EntityKind::Marker, m.index, moved_m));
		TEST_EXPECT(near(moved_m.transform.x, 100.0));
		TEST_EXPECT(near(moved_m.transform.y, 50.0));
		TEST_EXPECT(near(moved_m.transform.z, 14.0));

		// Everything now sits on the new ground: both count and apply are no-ops,
		// and the moved-row report is empty (within-epsilon rows never appear).
		moved_rows.clear();
		TEST_EXPECT(authoring::reground_entities(doc, reqs, 3, 0.01f, false, &moved_rows) == 0);
		TEST_EXPECT(moved_rows.empty());
		TEST_EXPECT(authoring::reground_entities(doc, reqs, 3) == 0);

		// Unknown entities are skipped rows, never errors — and never reported.
		authoring::RegroundRequest missing;
		missing.kind = EntityKind::Organic;
		missing.index = 99;
		TEST_EXPECT(authoring::reground_entities(doc, &missing, 1, 0.01f, true, &moved_rows) == 0);
		TEST_EXPECT(moved_rows.empty());
		// And a null request list is a zero, not a crash.
		TEST_EXPECT(authoring::reground_entities(doc, nullptr, 5) == 0);
	}

	return 0;
}
