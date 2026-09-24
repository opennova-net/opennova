#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/object_id.hpp>
#include <godot_cpp/variant/basis.hpp>
#include <godot_cpp/variant/variant.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <runtime/world/person_overlays.h>

#include <cstdint>

namespace godot {

class MissionObjectPlacer;
class Node3D;
class ObjectModel;

// One body's item overlays beside its held weapon: the parachute canopy, the
// night-vision goggles, the binoculars and the carried object. The engine
// decides which are drawn and their angles (world/person_overlays.h, which
// also carries the placement calibration and the witness); this device leg
// builds each item's model beside the body, poses it RIGID from the body's
// skeleton, and gives it the body's draw context (the render-slot parent, the
// light owner, the attachment RLOD owner).
//
// A composed player avatar draws in two submits, head then body, and the
// person callback runs for each: the canopy, the goggles and the binoculars
// ride BOTH submits (each at that part's RLOD level), while the carried
// object — like the held weapon — rides only the first. The second pass
// exists only for a composed avatar.
// [retail BoneCallback_org0_World @ 0x4e3940 — the body-submit skip
//  `and edi, 10000000h` @ 0x4e3c87 gates only draws 5 and 6; the two submits
//  Terrain_RenderSectorEntitiesBySide @ 0x5c7ffc / @ 0x5c8039]
class PersonOverlayModels : public RefCounted {
	GDCLASS(PersonOverlayModels, RefCounted)

public:
	enum Kind {
		KIND_CANOPY = 0,
		KIND_NVG,
		KIND_BINOCULARS,
		KIND_CARRIED,
		KIND_COUNT,
	};
	enum Pass {
		PASS_FIRST = 0,
		PASS_SECOND,
		PASS_COUNT,
	};
	// How the overlays present: a remote body's world layer, or the local
	// player's own avatar in third or first person. In first person the
	// goggles, binoculars and carried object hide from the camera like the
	// body (the tracked-entity gate returns after the canopy), still casting
	// into the render slot; the canopy draws BEFORE that gate, so the local
	// player sees his own canopy in first person.
	// [retail @ 0x4e3ab3..0x4e3aca, after the canopy submit @ 0x4e3aa5]
	enum Presentation {
		PRESENT_WORLD = 0,
		PRESENT_LOCAL_THIRD_PERSON,
		PRESENT_LOCAL_FIRST_PERSON,
	};

	// One frame: build what the state wants (rebuilding when a slot's item
	// type changes), pose it from `p_body`'s skeleton, and show it iff the
	// body is drawn. A body swap (a rebuilt node) rebuilds every overlay.
	void present(const opennova::world::PersonOverlays &p_overlays, ObjectModel *p_body,
			bool p_body_drawn, MissionObjectPlacer *p_placer, Node3D *p_parent,
			Presentation p_presentation);
	// Free every built model (the body left the walk).
	void release();
	bool is_empty() const;
	ObjectModel *node(int p_kind, int p_pass) const;

	// The rigid placement per overlay, from the body root or its skeleton;
	// null when the skeleton lacks the bone. The ONE home each walk and the
	// tests share.
	static Variant canopy_transform(Object *p_body, float p_canopy_yaw_deg);
	static Variant nvg_transform(Object *p_body);
	static Variant binoculars_transform(Object *p_body);
	static Variant carried_transform(Object *p_body, const Vector3 &p_carrier_euler_bms);
	// The calibrated bases alone (the tests re-derive them from the original
	// element placement).
	static Basis binoculars_frame_basis(const Basis &p_bone_model_to_world);
	static Basis carried_frame_basis(const Vector3 &p_carrier_euler_bms);
	// The engine calibration re-exported for the tests.
	static Vector3 nvg_nudge();
	static Vector3 binoculars_nudge();
	static double binoculars_frame_z_rad();
	static double binoculars_frame_x_rad();
	static double binoculars_frame_y_rad();
	static double carried_frame_z_rad();

	Object *get_node(int p_kind, int p_pass) const;

protected:
	static void _bind_methods();

private:
	struct Slot {
		ObjectID node;
		int32_t type_id = 0;
		int32_t ctrl_first = INT32_MIN;
		int32_t ctrl_second = INT32_MIN;
	};
	Slot slots_[KIND_COUNT][PASS_COUNT];
	ObjectID body_id_;

	void present_slot(Slot &p_slot, int p_kind, int32_t p_type_id, bool p_draw,
			const Variant &p_transform, ObjectModel *p_body, ObjectModel *p_lod_owner,
			MissionObjectPlacer *p_placer, Node3D *p_parent, int p_layer,
			const opennova::world::PersonOverlays &p_overlays);
	static void free_slot(Slot &p_slot);
};

} // namespace godot

VARIANT_ENUM_CAST(godot::PersonOverlayModels::Kind);
VARIANT_ENUM_CAST(godot::PersonOverlayModels::Pass);
VARIANT_ENUM_CAST(godot::PersonOverlayModels::Presentation);
