#include "simulation/person_overlay_models.h"

#include "mission/mission_object_placer.h"
#include "object/item_database.h"
#include "object/object_model.h"
#include "simulation/entity_presenter.h"

#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/core/class_db.hpp>

#include <runtime/world/entity_pose.h> // the attach frames
#include <runtime/world/person_overlays.h>

#include <cmath>

namespace godot {

namespace {

namespace ow = opennova::world;

const Vector3 kNightVisionNudge(ow::kNightVisionNudgeX, ow::kNightVisionNudgeY,
		ow::kNightVisionNudgeZ);
const Vector3 kBinocularsNudge(ow::kBinocularsNudgeX, ow::kBinocularsNudgeY,
		ow::kBinocularsNudgeZ);

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

Variant PersonOverlayModels::canopy_transform(ObjectModel *p_body, const Transform3D &p_body_world,
		float p_canopy_yaw_deg) {
	ow::PosedBoneFrames root;
	if (p_body == nullptr || !p_body->get_posed_bone_frames(ow::kCanopyBoneIndex, p_body_world, root)) {
		return Variant();
	}
	return ObjectModel::from_pose_transform(ow::canopy_attach(root, p_canopy_yaw_deg));
}

Variant PersonOverlayModels::nvg_transform(ObjectModel *p_body, const Transform3D &p_body_world) {
	ow::PosedBoneFrames head;
	if (p_body == nullptr ||
			!p_body->get_posed_bone_frames(ow::kNightVisionBoneIndex, p_body_world, head)) {
		return Variant();
	}
	return ObjectModel::from_pose_transform(ow::night_vision_attach(head));
}

Variant PersonOverlayModels::binoculars_transform(ObjectModel *p_body, const Transform3D &p_body_world) {
	ow::PosedBoneFrames hand;
	if (p_body == nullptr ||
			!p_body->get_posed_bone_frames(ow::kBinocularsBoneIndex, p_body_world, hand)) {
		return Variant();
	}
	return ObjectModel::from_pose_transform(ow::binoculars_attach(hand));
}

Variant PersonOverlayModels::carried_transform(ObjectModel *p_body, const Transform3D &p_body_world,
		const Vector3 &p_carrier_euler_bms) {
	ow::PosedBoneFrames hand;
	if (p_body == nullptr ||
			!p_body->get_posed_bone_frames(ow::kBinocularsBoneIndex, p_body_world, hand)) {
		return Variant();
	}
	const float carrier[3] = {static_cast<float>(p_carrier_euler_bms.x),
			static_cast<float>(p_carrier_euler_bms.y), static_cast<float>(p_carrier_euler_bms.z)};
	return ObjectModel::from_pose_transform(ow::carried_attach(hand, carrier));
}

Basis PersonOverlayModels::binoculars_frame_basis(const Basis &p_bone_model_to_world) {
	ow::PosedBoneFrames hand;
	hand.model_to_world = ObjectModel::to_pose_transform(Transform3D(p_bone_model_to_world, Vector3()));
	return ObjectModel::from_pose_transform(ow::binoculars_attach(hand)).basis;
}

Basis PersonOverlayModels::carried_frame_basis(const Vector3 &p_carrier_euler_bms) {
	ow::PosedBoneFrames hand;
	const float carrier[3] = {static_cast<float>(p_carrier_euler_bms.x),
			static_cast<float>(p_carrier_euler_bms.y), static_cast<float>(p_carrier_euler_bms.z)};
	return ObjectModel::from_pose_transform(ow::carried_attach(hand, carrier)).basis;
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
	// (engine: runtime/world/person_overlays.h)
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
		ObjectModel *p_body, const Transform3D &p_body_world, bool p_body_drawn,
		MissionObjectPlacer *p_placer, Node3D *p_parent, Presentation p_presentation) {
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
					xf = canopy_transform(p_body, p_body_world, p_overlays.canopy_yaw_deg);
					break;
				case KIND_NVG:
					xf = nvg_transform(p_body, p_body_world);
					break;
				case KIND_BINOCULARS:
					xf = binoculars_transform(p_body, p_body_world);
					break;
				default:
					xf = carried_transform(p_body, p_body_world,
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
			D_METHOD("canopy_transform", "body", "body_world", "canopy_yaw_deg"),
			&PersonOverlayModels::canopy_transform);
	ClassDB::bind_static_method("PersonOverlayModels", D_METHOD("nvg_transform", "body", "body_world"),
			&PersonOverlayModels::nvg_transform);
	ClassDB::bind_static_method("PersonOverlayModels",
			D_METHOD("binoculars_transform", "body", "body_world"),
			&PersonOverlayModels::binoculars_transform);
	ClassDB::bind_static_method("PersonOverlayModels",
			D_METHOD("carried_transform", "body", "body_world", "carrier_euler_bms"),
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
