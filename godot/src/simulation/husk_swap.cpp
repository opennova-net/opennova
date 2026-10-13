#include "simulation/husk_swap.h"

#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/visual_instance3d.hpp>
#include <godot_cpp/core/object.hpp>
#include <godot_cpp/variant/variant.hpp>

#include <runtime/world/model_geometry.h>

#include "mission/mission_object_placer.h"
#include "object/entity_ref.h"
#include "object/object_data.h"
#include "object/object_model.h"
#include "render/visual_layers.h"

namespace godot {

namespace {

Node3D *live_node3d(const ObjectID &p_id) {
	return p_id.is_valid() ? Object::cast_to<Node3D>(ObjectDB::get_instance(p_id)) : nullptr;
}

// A husk that replaces a static-shadow caster casts it in its place.
void set_husk_static_shadow(ObjectModel *p_model, bool p_enabled) {
	if (p_enabled && p_model != nullptr) {
		p_model->set_static_shadow_caster_enabled(true);
	}
}

} // namespace

bool node_has_static_shadow_caster(Node *p_root) {
	if (VisualInstance3D *visual = Object::cast_to<VisualInstance3D>(p_root)) {
		if ((visual->get_layer_mask() & visual_layers::STATIC_SHADOW_CASTER) != 0) {
			return true;
		}
	}
	for (int i = 0; i < p_root->get_child_count(); ++i) {
		if (node_has_static_shadow_caster(p_root->get_child(i))) {
			return true;
		}
	}
	return false;
}

HuskGraftResult husk_graft_individual(MissionObjectPlacer &p_placer, Node3D &p_node,
		const String &p_husk_graphic, int p_bms_id, HuskGraft &r_graft) {
	// A qualifying intact model transfers its static-caster role to the husk.
	const bool casts_static_shadow = node_has_static_shadow_caster(&p_node);
	ObjectModel *intact_model = Object::cast_to<ObjectModel>(&p_node);
	const bool mirror_reflected = intact_model != nullptr && intact_model->get_mirror_reflected();
	ObjectModel *model = p_placer.build_model_from_graphic(
			p_husk_graphic, String(), &p_node, String(), String(), true);
	if (model == nullptr) {
		return HuskGraftResult::NoModel;
	}
	model->set_name("HuskModel");
	if (intact_model != nullptr) {
		model->set_authored_lod_projection_owner(intact_model);
		model->set_occlusion_section_mask(intact_model->get_occlusion_section_mask(),
				intact_model->get_forced_section_mask());
	}
	set_husk_static_shadow(model, casts_static_shadow);
	// The reflect flag belongs to the entity, not its current graphic. The
	// individual branch must preserve it just like the batched carve branch
	// below; build_model_from_graphic has already built the replacement, so
	// apply the layer choice through one rebuild.
	if (mirror_reflected) {
		model->set_mirror_reflected(true);
		model->rebuild();
	}
	r_graft = HuskGraft();
	r_graft.kind = HuskGraft::INDIVIDUAL;
	r_graft.bms_id = p_bms_id;
	r_graft.husk_graphic = p_husk_graphic;
	r_graft.casts_static_shadow = casts_static_shadow;
	r_graft.husk = model->get_instance_id();
	for (int i = 0; i < p_node.get_child_count(); ++i) {
		Node3D *child = Object::cast_to<Node3D>(p_node.get_child(i));
		if (child != nullptr && child != model) {
			r_graft.children.emplace_back(child->get_instance_id(), child->is_visible());
			child->set_visible(false);
		}
	}
	if (p_bms_id != 0) {
		p_placer.set_static_terrain_shadow_replacement(
				p_bms_id, p_husk_graphic, p_node.get_transform(), casts_static_shadow);
	}
	return HuskGraftResult::Grafted;
}

HuskGraftResult husk_graft_static(MissionObjectPlacer &p_placer, Node3D &p_container, int p_bms_id,
		const String &p_husk_graphic, HuskGraft &r_graft) {
	// Batched replacements inherit the carved instance's authored eligibility.
	const bool casts_static_shadow = p_placer.static_instance_casts_terrain_shadow(p_bms_id);
	const bool mirror_reflected = p_placer.static_instance_is_mirror_reflected(p_bms_id);
	ObjectModel *graft = p_placer.build_model_from_graphic(
			p_husk_graphic, String(), &p_container, String(), String(), true);
	if (graft == nullptr) {
		return HuskGraftResult::NoModel;
	}
	// The placer owns its static lookup by raw BMS id; the caller's identity key
	// must not change the key used to carve and later restore this batch slot.
	const Variant placed = p_placer.hide_static_instance(p_bms_id);
	if (placed.get_type() != Variant::TRANSFORM3D) {
		graft->set_visible(false);
		graft->queue_free();
		return HuskGraftResult::NotCarved;
	}
	const Transform3D placed_transform = placed;
	r_graft = HuskGraft();
	r_graft.kind = HuskGraft::STATIC;
	r_graft.bms_id = p_bms_id;
	r_graft.placed_transform = placed_transform;
	r_graft.husk_graphic = p_husk_graphic;
	r_graft.casts_static_shadow = casts_static_shadow;
	r_graft.husk = graft->get_instance_id();
	graft->set_name(vformat("HuskModel_%d", p_bms_id));
	graft->set_transform(placed_transform);
	p_placer.inherit_static_entity_projection(p_bms_id, graft);
	// Changing the rendered graphic does not reinitialize entity+0. Carry the
	// static entity query and identity into its new live-model representation.
	// The native EntityLightQuery contract retains the original query on swaps.
	const auto light_sources = p_placer.static_item_effect_sources();
	for (const auto &source : light_sources) {
		if (source.bms_id != p_bms_id) {
			continue;
		}
		graft->set_bound_radii_q16(opennova::world::model_bound_radius_q16_from_3di(
										   graft->get_object_data()->native_model()),
				source.entity_bound_radius_q16);
		graft->set_entity_ref(
				EntityRef::make(source.kind, source.entity_index, p_bms_id, source.item_id, -1));
		break;
	}
	set_husk_static_shadow(graft, casts_static_shadow);
	// Retail's husk swap keeps the entity's reflect flag: the mirror
	// collectors keep filtering on entity+36 & 0x400, which destruction never
	// clears [orig: Entity_SpawnFromBMSRecord @ 0x40ed1d..0x40ed2b writer;
	// husk swap flips only Flags & 4].
	if (mirror_reflected) {
		graft->set_mirror_reflected(true);
		graft->rebuild();
	}
	p_placer.set_static_terrain_shadow_replacement(
			p_bms_id, p_husk_graphic, graft->get_transform(), casts_static_shadow);
	return HuskGraftResult::Grafted;
}

void husk_graft_restore(MissionObjectPlacer *p_placer, const HuskGraft &p_graft) {
	if (p_placer != nullptr && p_graft.bms_id != 0) {
		p_placer->clear_static_terrain_shadow_replacement(p_graft.bms_id);
	}
	if (p_graft.kind == HuskGraft::STATIC) {
		if (p_placer != nullptr) {
			p_placer->show_static_instance(p_graft.bms_id);
		}
		return;
	}
	for (const std::pair<ObjectID, bool> &saved : p_graft.children) {
		if (Node3D *child = live_node3d(saved.first)) {
			child->set_visible(saved.second);
		}
	}
}

} // namespace godot
