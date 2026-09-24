#include "simulation/person_overlay_models.h"

#include "mission/mission_object_placer.h"
#include "object/item_database.h"
#include "object/object_model.h"
#include "simulation/entity_presenter.h"

#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/skeleton3d.hpp>
#include <godot_cpp/core/class_db.hpp>

#include <runtime/world/person_overlays.h>

#include <cmath>

namespace godot {

namespace {

namespace ow = opennova::world;

const Vector3 kNightVisionNudge(ow::kNightVisionNudgeX, ow::kNightVisionNudgeY,
		ow::kNightVisionNudgeZ);
const Vector3 kBinocularsNudge(ow::kBinocularsNudgeX, ow::kBinocularsNudgeY,
		ow::kBinocularsNudgeZ);

// Bone `p_bone`'s posed joint and its MODEL->WORLD matrix (pose relative to
// rest), the pair every rigid overlay is built from: `M · (pivot + nudge)` is
// the joint plus the nudge carried by the model->world rotation, and the
// matrix's own translation is where the model origin lands.
bool bone_frames(Object *p_body, int p_bone, Transform3D &r_joint_world,
		Transform3D &r_model_to_world) {
	Skeleton3D *skel = Object::cast_to<Skeleton3D>(EntityPresenter::find_skeleton(p_body));
	if (skel == nullptr || skel->get_bone_count() <= p_bone) {
		return false;
	}
	r_joint_world = skel->get_global_transform() * skel->get_bone_global_pose(p_bone);
	r_model_to_world = r_joint_world * skel->get_bone_global_rest(p_bone).affine_inverse();
	return true;
}

int layer_for(PersonOverlayModels::Presentation p_presentation, int p_kind) {
	switch (p_presentation) {
		case PersonOverlayModels::PRESENT_WORLD:
			return ObjectModel::PRESENTATION_LAYER_WORLD;
		case PersonOverlayModels::PRESENT_LOCAL_THIRD_PERSON:
			return ObjectModel::PRESENTATION_LAYER_LOCAL_BODY;
		case PersonOverlayModels::PRESENT_LOCAL_FIRST_PERSON:
			return p_kind == PersonOverlayModels::KIND_CANOPY
					? ObjectModel::PRESENTATION_LAYER_LOCAL_BODY
					: ObjectModel::PRESENTATION_LAYER_LOCAL_BODY_HIDDEN;
	}
	return ObjectModel::PRESENTATION_LAYER_WORLD;
}

} // namespace

Variant PersonOverlayModels::canopy_transform(Object *p_body, float p_canopy_yaw_deg) {
	Transform3D joint;
	Transform3D model_to_world;
	if (!bone_frames(p_body, ow::kCanopyBoneIndex, joint, model_to_world)) {
		return Variant();
	}
	// The up-axis turn alone, at bone 0's translation (the model origin under
	// that bone), not the hip joint. [retail BoneCallback_org0_World
	// @ 0x4e39d6..0x4e3a48]
	return Transform3D(MissionObjectPlacer::bms_to_godot_basis(
							   Vector3(0.0f, p_canopy_yaw_deg, 0.0f)),
			model_to_world.origin);
}

Variant PersonOverlayModels::nvg_transform(Object *p_body) {
	Transform3D joint;
	Transform3D model_to_world;
	if (!bone_frames(p_body, ow::kNightVisionBoneIndex, joint, model_to_world)) {
		return Variant();
	}
	// The head bone's own rotation; its pivot nudged through the bone.
	// [retail Entity_BuildBoneTransformMatrices @ 0x4b24e3..0x4b258e]
	return Transform3D(model_to_world.basis,
			joint.origin + model_to_world.basis.xform(kNightVisionNudge));
}

Basis PersonOverlayModels::binoculars_frame_basis(const Basis &p_bone_model_to_world) {
	// Row-major `Ry · Rx · Rz · M15`: the calibration on the RIGHT in column
	// form, the X block's sign flipped (world/person_overlays.h carries why).
	return p_bone_model_to_world * Basis(Vector3(0, 0, 1), ow::kBinocularsFrameZRad) *
			Basis(Vector3(1, 0, 0), -ow::kBinocularsFrameXRad) *
			Basis(Vector3(0, 1, 0), ow::kBinocularsFrameYRad);
}

Variant PersonOverlayModels::binoculars_transform(Object *p_body) {
	Transform3D joint;
	Transform3D model_to_world;
	if (!bone_frames(p_body, ow::kBinocularsBoneIndex, joint, model_to_world)) {
		return Variant();
	}
	// [retail Entity_BuildBoneTransformMatrices @ 0x4b2308..0x4b24d2]
	return Transform3D(binoculars_frame_basis(model_to_world.basis),
			joint.origin + model_to_world.basis.xform(kBinocularsNudge));
}

Basis PersonOverlayModels::carried_frame_basis(const Vector3 &p_carrier_euler_bms) {
	// Row-major `E · Rz(k)`: the fixed turn about render Z (Godot X) applies
	// AFTER the entity matrix, so in column form it multiplies on the left.
	// The builder stores +sin (a plain float matrix), so render Z by +k is
	// Godot X by -k. [retail Math_BuildRotationMatrixZ_Float @ 0x613010;
	//  BoneCallback_org0_World @ 0x4e3dcb..0x4e3df1]
	// The angle is reduced in double first: a float trig call on 205887 rad
	// would lose the small remainder the x87 reduction keeps.
	const double turn = std::remainder(static_cast<double>(ow::kCarriedFrameZRad), Math_TAU);
	return Basis(Vector3(1, 0, 0), static_cast<real_t>(-turn)) *
			MissionObjectPlacer::bms_to_godot_basis(p_carrier_euler_bms);
}

Variant PersonOverlayModels::carried_transform(Object *p_body,
		const Vector3 &p_carrier_euler_bms) {
	const Variant hand = binoculars_transform(p_body);
	if (hand.get_type() != Variant::TRANSFORM3D) {
		return Variant();
	}
	// The binocular frame's point, the carrier's own orientation.
	// [retail BoneCallback_org0_World @ 0x4e3df6..0x4e3e26]
	return Transform3D(carried_frame_basis(p_carrier_euler_bms),
			static_cast<Transform3D>(hand).origin);
}

Vector3 PersonOverlayModels::nvg_nudge() {
	return kNightVisionNudge;
}

Vector3 PersonOverlayModels::binoculars_nudge() {
	return kBinocularsNudge;
}

double PersonOverlayModels::binoculars_frame_z_rad() {
	return ow::kBinocularsFrameZRad;
}

double PersonOverlayModels::binoculars_frame_x_rad() {
	return ow::kBinocularsFrameXRad;
}

double PersonOverlayModels::binoculars_frame_y_rad() {
	return ow::kBinocularsFrameYRad;
}

double PersonOverlayModels::carried_frame_z_rad() {
	return ow::kCarriedFrameZRad;
}

ObjectModel *PersonOverlayModels::node(int p_kind, int p_pass) const {
	if (p_kind < 0 || p_kind >= KIND_COUNT || p_pass < 0 || p_pass >= PASS_COUNT) {
		return nullptr;
	}
	return Object::cast_to<ObjectModel>(ObjectDB::get_instance(slots_[p_kind][p_pass].node));
}

Object *PersonOverlayModels::get_node(int p_kind, int p_pass) const {
	return node(p_kind, p_pass);
}

bool PersonOverlayModels::is_empty() const {
	for (const auto &kind : slots_) {
		for (const Slot &slot : kind) {
			if (slot.node.is_valid()) return false;
		}
	}
	return true;
}

void PersonOverlayModels::free_slot(Slot &p_slot) {
	if (ObjectModel *model = Object::cast_to<ObjectModel>(ObjectDB::get_instance(p_slot.node))) {
		model->queue_free();
	}
	p_slot = Slot();
}

void PersonOverlayModels::release() {
	for (auto &kind : slots_) {
		for (Slot &slot : kind) {
			free_slot(slot);
		}
	}
	body_id_ = ObjectID();
}

void PersonOverlayModels::present_slot(Slot &p_slot, int p_kind, int32_t p_type_id,
		bool p_draw, const Variant &p_transform, ObjectModel *p_body, ObjectModel *p_lod_owner,
		MissionObjectPlacer *p_placer, Node3D *p_parent, int p_layer,
		const opennova::world::PersonOverlays &p_overlays) {
	if (p_type_id != 0 && p_type_id != p_slot.type_id) {
		free_slot(p_slot);
		p_slot.type_id = p_type_id;
		if (p_placer != nullptr && p_parent != nullptr) {
			const Ref<ItemDatabase> item_db = p_placer->get_item_db();
			const String graphic = item_db.is_valid()
					? item_db->get_graphic(p_placer->resolve_player_visual_item_id(p_type_id))
					: String();
			ObjectModel *built = graphic.is_empty()
					? nullptr
					: p_placer->build_model_from_graphic(graphic, String(), p_parent,
							  String(), String(), true);
			if (built != nullptr) {
				built->set_name(vformat("PersonOverlay_%s", graphic));
				built->set_shadow_caster_enabled(true);
				// Drawn inside the body's own submit: its render slot, its
				// light group, its RLOD level clamped to the item's count.
				built->set_slot_shadow_capture_with(p_body);
				built->set_entity_light_owner(p_body);
				built->set_authored_lod_owner(p_lod_owner);
				p_slot.node = built->get_instance_id();
			}
		}
	}
	ObjectModel *model = Object::cast_to<ObjectModel>(ObjectDB::get_instance(p_slot.node));
	if (model == nullptr) {
		return;
	}
	// The draw decision is the model's presentation intent; the node's
	// visibility stays the product with its owner's occlusion / sub-pixel
	// verdicts, which the attachment walk hands it.
	if (!p_draw || p_transform.get_type() != Variant::TRANSFORM3D) {
		model->set_present_visible(false);
		return;
	}
	const Transform3D xf = p_transform;
	if (model->get_global_transform() != xf) {
		model->set_global_transform(xf);
	}
	model->set_presentation_layer(static_cast<ObjectModel::PresentationLayer>(p_layer));
	model->set_thermal_entity_wave(p_body->get_thermal_entity_wave());
	model->set_entity_lighting_context(p_body->get_lighting_effect_scale(),
			p_body->is_interior_lerp(), p_body->get_interior_daylight());
	// The overlay's CTRL registers, written before its submit: the canopy's
	// PARA / PARA_O, the goggles' NVG_FLIP.
	// [retail BoneCallback_org0_World @ 0x4e3a16..0x4e3a56, @ 0x4e3b9d]
	if (p_kind == KIND_CANOPY) {
		if (p_slot.ctrl_first != p_overlays.canopy_para) {
			model->set_ctrl_value("PARA", p_overlays.canopy_para);
			p_slot.ctrl_first = p_overlays.canopy_para;
		}
		if (p_slot.ctrl_second != p_overlays.canopy_para_o) {
			model->set_ctrl_value("PARA_O", p_overlays.canopy_para_o);
			p_slot.ctrl_second = p_overlays.canopy_para_o;
		}
	} else if (p_kind == KIND_NVG && p_slot.ctrl_first != p_overlays.nvg_flip) {
		model->set_ctrl_value("NVG_FLIP", p_overlays.nvg_flip);
		p_slot.ctrl_first = p_overlays.nvg_flip;
	}
	model->set_present_visible(true);
}

void PersonOverlayModels::present(const opennova::world::PersonOverlays &p_overlays,
		ObjectModel *p_body, bool p_body_drawn, MissionObjectPlacer *p_placer, Node3D *p_parent,
		Presentation p_presentation) {
	const ObjectID body_id = p_body != nullptr ? ObjectID(p_body->get_instance_id()) : ObjectID();
	if (body_id != body_id_) {
		release();
		body_id_ = body_id;
	}
	if (p_body == nullptr) {
		return;
	}
	ObjectModel *head = MissionObjectPlacer::avatar_head_part(p_body);
	const bool drawn = p_body_drawn;
	const int32_t types[KIND_COUNT] = {
		p_overlays.canopy() ? ow::kParachuteItemTypeId : 0,
		p_overlays.nvg ? ow::kNightVisionGogglesItemTypeId : 0,
		p_overlays.binoculars ? ow::kBinocularsItemTypeId : 0,
		p_overlays.carried_type_id,
	};
	for (int kind = 0; kind < KIND_COUNT; ++kind) {
		const bool want = types[kind] != 0;
		Variant xf;
		if (want && drawn) {
			switch (kind) {
				case KIND_CANOPY:
					xf = canopy_transform(p_body, p_overlays.canopy_yaw_deg);
					break;
				case KIND_NVG:
					xf = nvg_transform(p_body);
					break;
				case KIND_BINOCULARS:
					xf = binoculars_transform(p_body);
					break;
				default:
					xf = carried_transform(p_body,
							Vector3(p_overlays.carried_pitch_deg, p_overlays.carried_yaw_deg,
									p_overlays.carried_roll_deg));
					break;
			}
		}
		const int layer = layer_for(p_presentation, kind);
		// The first pass is the head submit of a composed avatar (or the one
		// submit of any other body); the second, the body submit, carries
		// every overlay but the carried object.
		present_slot(slots_[kind][PASS_FIRST], kind, types[kind], want && drawn, xf, p_body,
				head != nullptr ? head : p_body, p_placer, p_parent, layer, p_overlays);
		const bool second = head != nullptr && kind != KIND_CARRIED;
		present_slot(slots_[kind][PASS_SECOND], kind, second ? types[kind] : 0,
				second && want && drawn, xf, p_body, p_body, p_placer, p_parent, layer,
				p_overlays);
	}
}

void PersonOverlayModels::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_node", "kind", "pass"), &PersonOverlayModels::get_node);
	ClassDB::bind_method(D_METHOD("is_empty"), &PersonOverlayModels::is_empty);
	ClassDB::bind_static_method("PersonOverlayModels",
			D_METHOD("canopy_transform", "body", "canopy_yaw_deg"),
			&PersonOverlayModels::canopy_transform);
	ClassDB::bind_static_method("PersonOverlayModels", D_METHOD("nvg_transform", "body"),
			&PersonOverlayModels::nvg_transform);
	ClassDB::bind_static_method("PersonOverlayModels", D_METHOD("binoculars_transform", "body"),
			&PersonOverlayModels::binoculars_transform);
	ClassDB::bind_static_method("PersonOverlayModels",
			D_METHOD("carried_transform", "body", "carrier_euler_bms"),
			&PersonOverlayModels::carried_transform);
	ClassDB::bind_static_method("PersonOverlayModels",
			D_METHOD("binoculars_frame_basis", "bone_model_to_world"),
			&PersonOverlayModels::binoculars_frame_basis);
	ClassDB::bind_static_method("PersonOverlayModels",
			D_METHOD("carried_frame_basis", "carrier_euler_bms"),
			&PersonOverlayModels::carried_frame_basis);
	ClassDB::bind_static_method("PersonOverlayModels", D_METHOD("nvg_nudge"),
			&PersonOverlayModels::nvg_nudge);
	ClassDB::bind_static_method("PersonOverlayModels", D_METHOD("binoculars_nudge"),
			&PersonOverlayModels::binoculars_nudge);
	ClassDB::bind_static_method("PersonOverlayModels", D_METHOD("binoculars_frame_z_rad"),
			&PersonOverlayModels::binoculars_frame_z_rad);
	ClassDB::bind_static_method("PersonOverlayModels", D_METHOD("binoculars_frame_x_rad"),
			&PersonOverlayModels::binoculars_frame_x_rad);
	ClassDB::bind_static_method("PersonOverlayModels", D_METHOD("binoculars_frame_y_rad"),
			&PersonOverlayModels::binoculars_frame_y_rad);
	ClassDB::bind_static_method("PersonOverlayModels", D_METHOD("carried_frame_z_rad"),
			&PersonOverlayModels::carried_frame_z_rad);
	BIND_ENUM_CONSTANT(KIND_CANOPY);
	BIND_ENUM_CONSTANT(KIND_NVG);
	BIND_ENUM_CONSTANT(KIND_BINOCULARS);
	BIND_ENUM_CONSTANT(KIND_CARRIED);
	BIND_ENUM_CONSTANT(KIND_COUNT);
	BIND_ENUM_CONSTANT(PASS_FIRST);
	BIND_ENUM_CONSTANT(PASS_SECOND);
	BIND_ENUM_CONSTANT(PASS_COUNT);
	BIND_ENUM_CONSTANT(PRESENT_WORLD);
	BIND_ENUM_CONSTANT(PRESENT_LOCAL_THIRD_PERSON);
	BIND_ENUM_CONSTANT(PRESENT_LOCAL_FIRST_PERSON);
}

} // namespace godot
