// ObjectModel core: lifecycle, data wiring, CTRL registers, the
// event-driven runtime frame, and the class registration surface.
// Ported verbatim from object_model.gd (2026-08-09 de-scripting).

#include "object/object_model.h"

#include <cmath>

#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/occluder_instance3d.hpp>

#include <algorithm>
#include <limits>

#include "env/slot_shadow.h"
#include "terrain/terrain.h"

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/geometry_instance3d.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/core/math.hpp>
#include <godot_cpp/templates/local_vector.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include "object/object_shader_cache.h"
#include "render/frame_fx.h"
#include "render/object_lod_frame.h"
#include <runtime/renderer/object_lod.h>
#include <runtime/renderer/render_order.h>

namespace godot {

void EnvLightValues::_bind_methods() {
	ClassDB::bind_method(D_METHOD("equals", "other"), &EnvLightValues::equals);
	ClassDB::bind_static_method("EnvLightValues", D_METHOD("retail_noon_defaults"),
			&EnvLightValues::retail_noon_defaults);
#define ENV_PROP(m_type, m_name)                                             \
	ClassDB::bind_method(D_METHOD("set_" #m_name, "value"),                       \
			&EnvLightValues::set_##m_name);                                   \
	ClassDB::bind_method(D_METHOD("get_" #m_name), &EnvLightValues::get_##m_name); \
	ADD_PROPERTY(PropertyInfo(m_type, #m_name), "set_" #m_name, "get_" #m_name)
	ENV_PROP(Variant::VECTOR3, hemi_sky);
	ENV_PROP(Variant::VECTOR3, dir);
	ENV_PROP(Variant::VECTOR3, dir_color);
	ENV_PROP(Variant::VECTOR3, hemi_ground);
	ENV_PROP(Variant::VECTOR3, ceiling);
	ENV_PROP(Variant::VECTOR3, floor_color);
	ENV_PROP(Variant::VECTOR3, gain);
	ENV_PROP(Variant::BOOL, fog_enabled);
	ENV_PROP(Variant::VECTOR3, fog_color);
	ENV_PROP(Variant::FLOAT, fog_start);
	ENV_PROP(Variant::FLOAT, fog_end);
	ENV_PROP(Variant::INT, fog_type);
#undef ENV_PROP
}

bool EnvLightValues::equals(const Ref<EnvLightValues> &p_other) const {
	if (p_other.is_null()) {
		return false;
	}
	// Colours compare with is_equal_approx (the weather smoother quantises to
	// 8-bit, so real changes are >= 1/255, far above epsilon).
	const EnvLightValues &o = **p_other;
	return hemi_sky.is_equal_approx(o.hemi_sky) && dir.is_equal_approx(o.dir) &&
			dir_color.is_equal_approx(o.dir_color) &&
			hemi_ground.is_equal_approx(o.hemi_ground) &&
			ceiling.is_equal_approx(o.ceiling) &&
			floor_color.is_equal_approx(o.floor_color) &&
			gain.is_equal_approx(o.gain) && fog_enabled == o.fog_enabled &&
			fog_color.is_equal_approx(o.fog_color) &&
			Math::is_equal_approx(fog_start, o.fog_start) &&
			Math::is_equal_approx(fog_end, o.fog_end) && fog_type == o.fog_type;
}

Ref<EnvLightValues> EnvLightValues::retail_noon_defaults() {
	Ref<EnvLightValues> v;
	v.instantiate();
	v->hemi_sky = ObjectModel::default_hemi_sky_color();
	v->dir = ObjectModel::default_dir_light_dir();
	v->dir_color = ObjectModel::default_dir_light_color();
	v->hemi_ground = ObjectModel::default_hemi_ground_color();
	v->ceiling = ObjectModel::default_hemi_sky_color();
	v->floor_color = ObjectModel::default_hemi_ground_color();
	v->gain = Vector3(1, 1, 1);
	v->fog_enabled = false;
	v->fog_color = Vector3(0.5f, 0.6f, 0.8f);
	v->fog_start = 0.0f;
	v->fog_end = 1024.0f;
	v->fog_type = 0;
	return v;
}

void EnvLightState::_bind_methods() {
	ClassDB::bind_method(D_METHOD("publish", "values", "pass_changed"),
			&EnvLightState::publish, DEFVAL(false));
	ClassDB::bind_method(D_METHOD("get_values"), &EnvLightState::get_values);
	ClassDB::bind_method(D_METHOD("get_generation"), &EnvLightState::get_generation);
	ADD_SIGNAL(MethodInfo("changed"));
	ADD_SIGNAL(MethodInfo("pass_changed"));
}

void EnvLightState::publish(const Ref<EnvLightValues> &p_values,
		bool p_pass_changed) {
	values_ = p_values;
	++generation_;
	emit_signal("changed");
	if (p_pass_changed) {
		emit_signal("pass_changed");
	}
}

void PanmClock::_bind_methods() {
	ClassDB::bind_method(D_METHOD("sample_frame"), &PanmClock::sample_frame);
	ClassDB::bind_method(D_METHOD("sample", "value_ms", "frame"), &PanmClock::sample);
	ClassDB::bind_method(D_METHOD("get_time_ms"), &PanmClock::get_time_ms);
	ClassDB::bind_method(D_METHOD("set_time_ms_for_test", "value_ms"),
			&PanmClock::set_time_ms_for_test);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "time_ms", PROPERTY_HINT_NONE, "",
						 PROPERTY_USAGE_NONE),
			"", "get_time_ms");
}

bool PanmClock::sample_frame() {
	return sample(static_cast<int64_t>(Time::get_singleton()->get_ticks_msec()),
			static_cast<int64_t>(Engine::get_singleton()->get_process_frames()));
}

bool PanmClock::sample(int64_t p_value_ms, int64_t p_frame) {
	if (p_frame == sampled_frame_) {
		return false;
	}
	sampled_frame_ = p_frame;
	time_ms_ = p_value_ms & 0xffffffff;
	return true;
}

void PanmClock::set_time_ms_for_test(int64_t p_value_ms) {
	time_ms_ = p_value_ms & 0xffffffff;
	sampled_frame_ = -1;
}

ObjectModel::ObjectModel() {}

ObjectModel::~ObjectModel() {
	// A model can be freed without a PREDELETE notification in some teardown
	// paths; never leave a dangling pointer in the shared awake set, and keep
	// the row-plan stamp monotonic for a planned model (a second bump after
	// PREDELETE is harmless).
	if (present_planned_) {
		++lifetime_generation_;
	}
	if (awake_) {
		awake_ = false;
		awake_models_.erase(this);
	}
	match_terrain_models_.erase(this);
	authored_lod_models_.erase(this);
	retire_geometry_instances();
}

void ObjectModel::set_object_data(const Ref<ObjectData> &p_data) {
	const Callable changed = callable_mp(this, &ObjectModel::on_object_changed);
	if (object_data_.is_valid() && object_data_->is_connected("object_changed", changed)) {
		object_data_->disconnect("object_changed", changed);
	}
	object_data_ = p_data;
	reset_remote_body_state();
	part_anims_.clear();
	part_anim_tick_accum_s_ = 0.0;
	active_lod_ = clamp_lod_index(active_lod_);
	if (object_data_.is_valid() && !object_data_->is_connected("object_changed", changed)) {
		object_data_->connect("object_changed", changed, CONNECT_DEFERRED);
	}
	rebuild();
}

Vector<ObjectModel *> ObjectModel::live_presentation_links() const {
	Vector<ObjectModel *> out;
	for (const PresentationLink &link : presentation_links_) {
		ObjectModel *model = link.id.is_valid()
				? Object::cast_to<ObjectModel>(ObjectDB::get_instance(link.id))
				: nullptr;
		if (model != nullptr) {
			out.push_back(model);
		}
	}
	return out;
}

// The linked parts that share `p_register` with this model — every link except
// those whose composer declared the register part-local.
Vector<ObjectModel *> ObjectModel::live_presentation_links_sharing(
		const String &p_register) const {
	Vector<ObjectModel *> out;
	for (const PresentationLink &link : presentation_links_) {
		if (link.part_local_registers.has(p_register)) {
			continue;
		}
		ObjectModel *model = link.id.is_valid()
				? Object::cast_to<ObjectModel>(ObjectDB::get_instance(link.id))
				: nullptr;
		if (model != nullptr) {
			out.push_back(model);
		}
	}
	return out;
}

void ObjectModel::add_presentation_link(ObjectModel *p_model,
		const PackedStringArray &p_part_local_registers) {
	if (p_model == nullptr || p_model == this) {
		return;
	}
	const ObjectID id(p_model->get_instance_id());
	for (const PresentationLink &existing : presentation_links_) {
		if (existing.id == id) {
			return;
		}
	}
	PresentationLink link;
	link.id = id;
	for (const String &name : p_part_local_registers) {
		const String reg = ObjectData::canonical_control_register_name(name);
		if (!reg.is_empty()) {
			link.part_local_registers.insert(reg);
		}
	}
	presentation_links_.push_back(link);
	p_model->set_match_terrain_enabled(match_terrain_enabled_);
}


void ObjectModel::set_shadow_caster_enabled(bool p_enabled) {
	set_shadow_caster_layer_enabled(LAYER_DYNAMIC_SHADOW_CASTER, p_enabled);
	// Dynamic casters join the render-slot ground-shadow group the SlotShadow
	// device plans over [orig: slot registration at entity init —
	// Entity_InitFromModel, see docs/render/render-lighting-re.md].
	update_slot_shadow_group();
}

void ObjectModel::update_slot_shadow_group() {
	if (!is_inside_tree()) {
		return;
	}
	const StringName &group = SlotShadow::caster_group();
	if (is_shadow_caster_enabled()) {
		if (!is_in_group(group)) {
			add_to_group(group);
			SlotShadow::bump_caster_group_revision();
		}
	} else if (is_in_group(group)) {
		remove_from_group(group);
		SlotShadow::bump_caster_group_revision();
	}
}

void ObjectModel::set_slot_shadow_person(bool p_person) {
	if (slot_shadow_person_ != p_person) {
		SlotShadow::bump_caster_group_revision();
	}
	slot_shadow_person_ = p_person;
}

bool ObjectModel::is_slot_shadow_person() const {
	return slot_shadow_person_;
}

void ObjectModel::set_entity_uniform_scale_q16(int64_t p_scale_q16) {
	entity_uniform_scale_q16_ = static_cast<int32_t>(
			static_cast<uint32_t>(p_scale_q16));
}

int64_t ObjectModel::get_entity_uniform_scale_q16() const {
	return entity_uniform_scale_q16_;
}

Transform3D ObjectModel::compose_entity_transform(const Basis &p_basis,
		const Vector3 &p_origin) const {
	if (entity_uniform_scale_q16_ == 0) {
		return Transform3D(p_basis, p_origin);
	}
	const float scale = static_cast<float>(entity_uniform_scale_q16_) / 65536.0f;
	return Transform3D(p_basis.scaled(Vector3(scale, scale, scale)), p_origin);
}

void ObjectModel::set_shadow_bound_radii(float p_model_sphere, float p_entity_bound) {
	const float sphere = p_model_sphere > 0.0f ? p_model_sphere : 0.0f;
	const float bound = p_entity_bound > 0.0f ? p_entity_bound : 0.0f;
	if (model_sphere_radius_ != sphere || entity_bound_radius_ != bound) {
		SlotShadow::bump_caster_group_revision();
	}
	model_sphere_radius_ = sphere;
	entity_bound_radius_ = bound;
}

float ObjectModel::get_model_sphere_radius() const {
	return model_sphere_radius_;
}

float ObjectModel::get_entity_bound_radius() const {
	return entity_bound_radius_;
}

void ObjectModel::set_slot_shadow_capture_with(ObjectModel *p_owner) {
	const ObjectID next = p_owner != nullptr
			? ObjectID(p_owner->get_instance_id())
			: ObjectID();
	if (slot_shadow_capture_with_ != next) {
		SlotShadow::bump_caster_group_revision();
	}
	slot_shadow_capture_with_ = next;
}

ObjectModel *ObjectModel::get_slot_shadow_capture_with() const {
	return Object::cast_to<ObjectModel>(
			ObjectDB::get_instance(slot_shadow_capture_with_));
}

void ObjectModel::set_slot_shadow_decal(const String &p_texture,
		const Vector4 &p_dims) {
	if (slot_shadow_decal_texture_ != p_texture) {
		SlotShadow::bump_caster_group_revision();
	}
	slot_shadow_decal_texture_ = p_texture;
	slot_shadow_decal_dims_ = p_dims;
}

String ObjectModel::get_slot_shadow_decal_texture() const {
	return slot_shadow_decal_texture_;
}

Vector4 ObjectModel::get_slot_shadow_decal_dims() const {
	return slot_shadow_decal_dims_;
}

bool ObjectModel::is_shadow_caster_enabled() const {
	return (shadow_caster_layers_ & LAYER_DYNAMIC_SHADOW_CASTER) != 0;
}

void ObjectModel::set_static_shadow_caster_enabled(bool p_enabled) {
	set_shadow_caster_layer_enabled(LAYER_STATIC_SHADOW_CASTER, p_enabled);
}

bool ObjectModel::is_static_shadow_caster_enabled() const {
	return (shadow_caster_layers_ & LAYER_STATIC_SHADOW_CASTER) != 0;
}

void ObjectModel::set_shadow_caster_layer_enabled(uint32_t p_layer, bool p_enabled) {
	const uint32_t next_layers = p_enabled ? (shadow_caster_layers_ | p_layer)
										   : (shadow_caster_layers_ & ~p_layer);
	if (next_layers == shadow_caster_layers_) {
		return;
	}
	shadow_caster_layers_ = next_layers;
	apply_presentation_layer_below(this);
}

void ObjectModel::set_presentation_layer(PresentationLayer p_layer) {
	if (presentation_layer_ == p_layer) {
		return;
	}
	presentation_layer_ = p_layer;
	apply_presentation_layer_below(this);
}

uint32_t ObjectModel::presentation_layer_mask(bool p_auxiliary) const {
	uint32_t base = 0;
	bool markers = true;
	switch (presentation_layer_) {
		case PRESENTATION_LAYER_WORLD:
			// Mission placement has already resolved the engine's two-part
			// building/vehicle reflection policy; this device leg only maps
			// that typed decision to Godot visibility layers.
			base = mirror_reflected_ ? LAYER_WORLD : LAYER_WORLD_NO_MIRROR;
			markers = !p_auxiliary;
			break;
		case PRESENTATION_LAYER_LOCAL_BODY:
			base = LAYER_WORLD;
			break;
		case PRESENTATION_LAYER_LOCAL_BODY_HIDDEN:
			base = LAYER_FP_BODY_SHADOW_ONLY;
			break;
		case PRESENTATION_LAYER_VIEWMODEL:
			base = LAYER_VIEWMODEL;
			markers = false;
			break;
	}
	return markers ? (base | shadow_caster_layers_) : base;
}

GeometryInstance3D::ShadowCastingSetting ObjectModel::presentation_cast_setting(
		bool p_auxiliary) const {
	switch (presentation_layer_) {
		case PRESENTATION_LAYER_WORLD:
			return !p_auxiliary && shadow_caster_layers_ != 0
					? GeometryInstance3D::SHADOW_CASTING_SETTING_ON
					: GeometryInstance3D::SHADOW_CASTING_SETTING_OFF;
		case PRESENTATION_LAYER_LOCAL_BODY:
		case PRESENTATION_LAYER_LOCAL_BODY_HIDDEN:
			// Camera-renderable and hidden by LAYER alone, so the render-slot
			// capture cameras can photograph the silhouette (SHADOWS_ONLY
			// geometry is invisible to every camera, capture viewports
			// included).
			return GeometryInstance3D::SHADOW_CASTING_SETTING_ON;
		case PRESENTATION_LAYER_VIEWMODEL:
			return GeometryInstance3D::SHADOW_CASTING_SETTING_OFF;
	}
	return GeometryInstance3D::SHADOW_CASTING_SETTING_OFF;
}

void ObjectModel::apply_presentation_layer_below(Node *p_root) {
	// The render-slot captures walk this subtree's geometry directly
	// (SlotShadow's RenderingDevice pass); no capture channel rides the
	// layer mask, so a policy write is the whole mask.
	for (int i = 0; i < p_root->get_child_count(); ++i) {
		Node *child = p_root->get_child(i);
		VisualInstance3D *visual = Object::cast_to<VisualInstance3D>(child);
		if (visual != nullptr) {
			const bool auxiliary =
					bool(visual->get_meta("_opennova_auxiliary_draw", false));
			visual->set_layer_mask(presentation_layer_mask(auxiliary));
			GeometryInstance3D *geometry =
					Object::cast_to<GeometryInstance3D>(child);
			if (geometry != nullptr) {
				geometry->set_cast_shadows_setting(
						presentation_cast_setting(auxiliary));
			}
		}
		apply_presentation_layer_below(child);
	}
}

void ObjectModel::set_entity_lighting_context(float p_effect_scale,
		bool p_interior_lerp, float p_interior_daylight) {
	const float next_effect = CLAMP(p_effect_scale, 0.0f, 1.0f);
	const float next_daylight = CLAMP(p_interior_daylight, 0.0f, 1.0f);
	if (Math::is_equal_approx(lighting_effect_scale_, next_effect) &&
			interior_lerp_ == p_interior_lerp &&
			Math::is_equal_approx(interior_daylight_, next_daylight)) {
		return;
	}
	lighting_effect_scale_ = next_effect;
	interior_lerp_ = p_interior_lerp;
	interior_daylight_ = next_daylight;
	stamp_entity_lighting_instances();
}

void ObjectModel::set_interior_section_light_transfer(float p_daylight) {
	const float next_daylight = CLAMP(p_daylight, 0.0f, 1.0f);
	if (interior_section_lighting_ &&
			Math::is_equal_approx(interior_section_daylight_, next_daylight)) {
		return;
	}
	interior_section_lighting_ = true;
	interior_section_daylight_ = next_daylight;
	stamp_entity_lighting_instances();
}

// The per-entry lighting factors as instance state on every surface instance
// (the auxiliary postmultiply draws under the same part included): retail
// pushes them per batch entry at flush from the entity's proximity slice
// (effectScale) and its interior parent (flag bit 1 + the parent's daylight
// openness), while the world block itself stays a per-pass global. A portal
// building is not an ordinary entity submission: its exterior shell (ROBJ 0)
// always keeps effectScale 1 with no interior lerp, and only ROBJ 1+ takes
// its own ItemDef light transfer. Re-stamped by rebuild_scene (fresh
// instances) and on every context edge; never per frame [orig:
// setup_entity_lighting_and_shader_constants @0x5d98a0 and the model+536
// daylight push per visible building in Terrain_RenderSectorModels
// @0x5c5d30, see docs/render/render-lighting-re.md; the math itself is
// opennova::renderer::compute_entity_lighting].
void ObjectModel::stamp_entity_lighting_instances() {
	const StringName name("u_entity_light");
	const Vector4 entity = interior_section_lighting_
			? Vector4(1.0f, 0.0f, 1.0f, 0.0f)
			: Vector4(lighting_effect_scale_, interior_lerp_ ? 1.0f : 0.0f,
					interior_daylight_, 0.0f);
	const Vector4 section(1.0f, 1.0f, interior_section_daylight_, 0.0f);
	const auto apply_to = [&](Node *p_parent, const Vector4 &p_value) {
		if (p_parent == nullptr) {
			return;
		}
		const int children = p_parent->get_child_count();
		for (int child = 0; child < children; ++child) {
			GeometryInstance3D *instance = Object::cast_to<GeometryInstance3D>(
					p_parent->get_child(child));
			if (instance == nullptr) {
				continue;
			}
			instance->set_instance_shader_parameter(name, p_value);
		}
	};
	for (const KeyValue<int, Node3D *> &kv : robj_nodes_) {
		apply_to(kv.value,
				interior_section_lighting_ && kv.key != 0 ? section : entity);
	}
	apply_to(skeleton_, entity);
}

Dictionary ObjectModel::get_render_part_nodes() const {
	Dictionary result;
	for (const KeyValue<int, Node3D *> &kv : robj_nodes_) {
		result[kv.key] = kv.value;
	}
	return result;
}

void ObjectModel::set_section_visibility_mask(int64_t p_mask) {
	// [orig: g_HiddenSectionMask @ 0xB7965C consumption in
	//  Terrain_RenderSectorModels @ 0x5c5d30 — per-draw hidden mask is ~mask;
	//  the renderer-specific two-pass legs are D-OCC-13]
	if (section_visibility_mask_ == p_mask) {
		return;
	}
	section_visibility_mask_ = p_mask;
	point_light_draw_parts_dirty_ = true;
	for (const KeyValue<int, Node3D *> &kv : robj_nodes_) {
		if (kv.value != nullptr) {
			kv.value->set_visible(p_mask == -1 || ((p_mask >> kv.key) & 1) == 1);
		}
	}
	for (const KeyValue<int, OccluderInstance3D *> &kv : authored_occluders_) {
		if (kv.value != nullptr) {
			kv.value->set_visible(
					p_mask == -1 ||
					(kv.key < 63 &&
							((static_cast<uint64_t>(p_mask) >> kv.key) & 1u) != 0));
		}
	}
}

Array ObjectModel::get_surface_materials() const {
	Array result;
	result.resize(surface_materials_.size());
	for (int i = 0; i < surface_materials_.size(); ++i) {
		result[i] = surface_materials_[i];
	}
	return result;
}

void ObjectModel::set_playing(bool p_value) {
	is_playing_ = p_value;
	wake_runtime_frame();
}

void ObjectModel::set_panm_clock(const Ref<PanmClock> &p_clock) {
	panm_clock_ = p_clock;
	wake_runtime_frame();
	if (panm_clock_.is_valid()) {
		anim_time_ms_ = panm_clock_->get_time_ms();
	}
	apply_runtime_state(0.0);
}

void ObjectModel::set_active_lod(int p_lod_index) {
	const int next_lod = clamp_lod_index(p_lod_index);
	if (active_lod_ == next_lod) {
		return;
	}
	active_lod_ = next_lod;
	if (!authored_lod_enabled_) {
		// Editor/preview models keep the historical one-LOD footprint. Mission
		// models opt into retained authored LODs before their first data build.
		rebuild_scene();
		return;
	}
	// The retained slots take the level's rows in place: no node is created
	// or freed, the build serial does not move.
	apply_level_surfaces();
	refresh_active_lod_rest_transforms();
	panm_applied_revision_ = 0;
	refresh_live_panm_classification();
	point_light_draw_parts_dirty_ = true;
	render_order_dirty_ = true;
	bounds_dirty_ = true;
	wake_runtime_frame();
	apply_runtime_state(0.0);
}

void ObjectModel::set_authored_lod_owner(ObjectModel *p_owner) {
	authored_lod_owner_ = p_owner != nullptr && p_owner != this
			? p_owner->get_instance_id()
			: ObjectID();
}

ObjectModel *ObjectModel::get_authored_lod_owner() const {
	if (authored_lod_owner_.is_null()) {
		return nullptr;
	}
	return Object::cast_to<ObjectModel>(
			ObjectDB::get_instance(authored_lod_owner_));
}

void ObjectModel::set_authored_lod_enabled(bool p_enabled) {
	if (authored_lod_enabled_ == p_enabled) {
		return;
	}
	authored_lod_enabled_ = p_enabled;
	active_lod_ = 0;
	if (object_data_.is_valid() && object_data_->has_document()) {
		rebuild_scene();
		return;
	}
	authored_lod_models_.erase(this);
}

void ObjectModel::set_authored_occluders_enabled(bool p_enabled) {
	if (authored_occluders_enabled_ == p_enabled) {
		return;
	}
	authored_occluders_enabled_ = p_enabled;
	if (object_data_.is_valid() && object_data_->has_document()) {
		rebuild_scene();
	}
}

int64_t ObjectModel::ctrl_dword(int64_t p_value) {
	// Retail's global CTRL bus stores signed dwords. Keep exact 0x10000
	// endpoints and negative angular controls instead of narrowing to uint16.
	int64_t next_value = p_value & 0xFFFFFFFF;
	if (next_value >= 0x80000000LL) {
		next_value -= 0x100000000LL;
	}
	return next_value;
}

opennova::renderer::ControlRegisterValues ObjectModel::runtime_ctrl_values() {
	if (!ctrl_native_cache_valid_) {
		ctrl_native_cache_ = ObjectData::runtime_control_values_dict_only(
				ctrl_values_, ctrl_native_has_flicker_, ctrl_native_has_swing_);
		ctrl_native_cache_valid_ = true;
	}
	opennova::renderer::ControlRegisterValues values = ctrl_native_cache_;
	ObjectData::stamp_weather_ctrl_registers(values, ctrl_native_has_flicker_,
			ctrl_native_has_swing_);
	return values;
}

void ObjectModel::finish_ctrl_change(bool p_apply_now) {
	ctrl_native_cache_valid_ = false;
	wake_runtime_frame();
	bounds_dirty_ = true;
	if (p_apply_now) {
		if (ctrl_batch_depth_ > 0) {
			ctrl_batch_dirty_ = true;
		} else {
			apply_runtime_state(0.0);
		}
	}
}

// Batch the ordered register stores that precede one retained-model sample.
// [see the GDScript origin's rationale — one shared waveform/random advance]
void ObjectModel::begin_ctrl_update() {
	for (ObjectModel *linked : live_presentation_links()) {
		linked->begin_ctrl_update();
	}
	++ctrl_batch_depth_;
}

void ObjectModel::end_ctrl_update() {
	if (ctrl_batch_depth_ <= 0) {
		return;
	}
	--ctrl_batch_depth_;
	if (ctrl_batch_depth_ == 0 && ctrl_batch_dirty_) {
		ctrl_batch_dirty_ = false;
		apply_runtime_state(0.0);
	}
	for (ObjectModel *linked : live_presentation_links()) {
		linked->end_ctrl_update();
	}
}

void ObjectModel::set_ctrl_value(const String &p_name, int64_t p_value) {
	const String reg = ObjectData::canonical_control_register_name(p_name);
	if (reg.is_empty()) {
		return;
	}
	for (ObjectModel *linked : live_presentation_links_sharing(reg)) {
		linked->set_ctrl_value(reg, p_value);
	}
	const int64_t next_value = ctrl_dword(p_value);
	if (ctrl_values_.has(reg) && int64_t(ctrl_values_[reg]) == next_value &&
			!ctrl_value_owners_.has(reg)) {
		return;
	}
	ctrl_values_[reg] = next_value;
	ctrl_value_owners_.erase(reg);
	finish_ctrl_change(true);
}

void ObjectModel::clear_ctrl_value(const String &p_name) {
	const String reg = ObjectData::canonical_control_register_name(p_name);
	for (ObjectModel *linked : live_presentation_links_sharing(reg)) {
		linked->clear_ctrl_value(reg);
	}
	if (reg.is_empty() || !ctrl_values_.has(reg)) {
		return;
	}
	ctrl_values_.erase(reg);
	ctrl_value_owners_.erase(reg);
	finish_ctrl_change(true);
}

// Publish one dedicated retail writer into the register's single current
// value. The owner tag is lifecycle bookkeeping only: a stale teardown cannot
// clear a later writer's store, and overwritten values are never stacked or
// restored. [orig: global CTRL value slots @0x83FCE8, stride 8]
void ObjectModel::set_ctrl_override(const String &p_owner, const String &p_name,
		int64_t p_value) {
	const String reg = ObjectData::canonical_control_register_name(p_name);
	if (p_owner.is_empty() || reg.is_empty()) {
		return;
	}
	for (ObjectModel *linked : live_presentation_links_sharing(reg)) {
		linked->set_ctrl_override(p_owner, reg, p_value);
	}
	const int64_t next_value = ctrl_dword(p_value);
	const String *current_owner = ctrl_value_owners_.getptr(reg);
	if (ctrl_values_.has(reg) && int64_t(ctrl_values_[reg]) == next_value &&
			current_owner != nullptr && *current_owner == p_owner) {
		return;
	}
	ctrl_values_[reg] = next_value;
	ctrl_value_owners_[reg] = p_owner;
	finish_ctrl_change(true);
}

void ObjectModel::clear_ctrl_override(const String &p_owner, const String &p_name) {
	const String reg = ObjectData::canonical_control_register_name(p_name);
	if (p_owner.is_empty() || reg.is_empty()) {
		return;
	}
	for (ObjectModel *linked : live_presentation_links_sharing(reg)) {
		linked->clear_ctrl_override(p_owner, reg);
	}
	const String *current_owner = ctrl_value_owners_.getptr(reg);
	if (current_owner == nullptr || *current_owner != p_owner) {
		return;
	}
	ctrl_value_owners_.erase(reg);
	ctrl_values_.erase(reg);
	finish_ctrl_change(true);
}

Dictionary ObjectModel::get_ctrl_values() const {
	return ctrl_values_.duplicate(true);
}

void ObjectModel::rebuild() {
	wake_runtime_frame();
	rebuild_scene();
}

// Blended strips take their water-side transparency rung from the witnessed
// frame ladder (REN-3): the side away from the camera draws before the water
// surface and the camera-side strip after it. The portable contract and exact
// witness addresses live in renderer/render_order and render-order-re.md. The
// renderer retains one MeshInstance3D per source strip; transform its source
// min/max center through the live ROBJ transform and classify it whenever
// that transform or the water plane changed (identical to retail's per-frame
// recompute, without a per-frame server round trip per strip).
void ObjectModel::refresh_render_order() {
	if (alpha_strip_draws_.is_empty() || !is_inside_tree()) {
		return;
	}
	ObjectShaderCache *shader_cache = ObjectShaderCache::get_singleton();
	const uint64_t generation = shader_cache->get_water_plane_generation();
	if (!render_order_dirty_ && generation == render_order_generation_) {
		return;
	}
	render_order_dirty_ = false;
	render_order_generation_ = generation;
	for (AlphaStripDraw &draw : alpha_strip_draws_) {
		if (draw.instance != nullptr && draw.material.is_valid() &&
				draw.instance->is_inside_tree()) {
			// The rigid collector classifies every strip from its transformed
			// authored center. The bone collector does not recompute deformed strip
			// centers: its caller selects Q1/Q2 for the whole entity with submit bit
			// 0x20 (renderer/render_order owns the cited submit-bit contract).
			const float world_height = draw.bone_path
					? static_cast<float>(get_global_position().y)
					: static_cast<float>(draw.instance->get_global_transform()
							.xform(draw.local_center).y);
			// The viewmodel flushes whole before the sky pass; its depth band
			// keeps later world alpha off it (renderer/render_order).
			const int32_t rung = viewmodel_pass_
					? opennova::renderer::kRungViewmodel
					: shader_cache->alpha_rung_for_height(world_height);
			if (rung != draw.rung) {
				draw.rung = rung;
				draw.material->set_render_priority(rung);
			}
		}
	}
}

void ObjectModel::mark_render_order_dirty_all() {
	for (ObjectModel *model : alpha_strip_models_) {
		model->render_order_dirty_ = true;
		model->wake_runtime_frame();
	}
}

void ObjectModel::on_object_changed() {
	// ObjectData changes only when its immutable .3di content is replaced, so
	// every observer takes the same full rebuild path.
	rebuild();
}

// The shared awake set: every model with live per-frame work. One driver
// (ObjectModel::advance_awake_frame) walks it per render frame — the game from
// GameFramePipeline's render_material_frame leg, the menu shell and ONED from
// their one process loop. There is no per-node _process, so nothing self-clocks
// off Godot's frame outside that one driver.
HashSet<ObjectModel *> ObjectModel::awake_models_;
HashSet<ObjectModel *> ObjectModel::alpha_strip_models_;
HashSet<ObjectModel *> ObjectModel::match_terrain_models_;
HashSet<ObjectModel *> ObjectModel::authored_lod_models_;
uint64_t ObjectModel::lifetime_generation_ = 0;
int64_t ObjectModel::live_geometry_instance_count_ = 0;

void ObjectModel::retire_geometry_instances() {
	live_geometry_instance_count_ -= geometry_instance_count_;
	geometry_instance_count_ = 0;
}

int ObjectModel::update_authored_lods(const Transform3D &p_camera_transform,
		float p_vertical_fov_degrees,
		float p_viewport_width,
		float p_viewport_height) {
	if (authored_lod_models_.is_empty()) {
		return 0;
	}
	// The frame scale, the projected radius and the selector are engine facts
	// (runtime/renderer/object_lod.h); the frame struct converts the camera.
	const ObjectLodFrame frame = ObjectLodFrame::make(p_camera_transform,
			p_vertical_fov_degrees, p_viewport_width, p_viewport_height);
	if (!frame.valid) {
		return 0;
	}
	// The cheap math runs over the registered set in place; a level change is
	// applied after the walk so set_active_lod's runtime-state refresh never
	// runs against the set being iterated. Nothing allocates while no model
	// crosses a threshold.
	struct LodSwitch {
		ObjectModel *model = nullptr;
		int lod_index = 0;
	};
	// Frame scratch that keeps its capacity across calls (deliberately never
	// freed: a static with a Godot allocator destructor would run after the
	// extension's allocator hooks are gone), so a frame with attachments or
	// crossings allocates nothing once warm.
	static LocalVector<LodSwitch> &switches = *memnew(LocalVector<LodSwitch>);
	switches.clear();
	// Attachments take their owner's level after the owners' own selections
	// have been applied (renderer::attachment_lod_index).
	static LocalVector<ObjectModel *> &attachments =
			*memnew(LocalVector<ObjectModel *>);
	attachments.clear();
	for (ObjectModel *model : authored_lod_models_) {
		if (!model->is_inside_tree()) {
			continue;
		}
		if (!model->authored_lod_owner_.is_null()) {
			attachments.push_back(model);
			continue;
		}
		const Transform3D world = model->get_global_transform();
		const float source_radius =
				model->model_sphere_radius_ > 0.0f
				? model->model_sphere_radius_
				: model->model_bounds_.get_longest_axis_size() * 0.5f;
		const float radius =
				source_radius * ObjectLodFrame::uniform_scale(world.basis);
		int32_t projected_q16 = 0;
		// A model outside the frustum keeps its level: retail never reaches the
		// selector for an entity its collector rejected.
		if (!frame.project(world.origin, radius, projected_q16)) {
			continue;
		}
		const opennova::renderer::ObjectLodSelection selection =
				opennova::renderer::select_object_lod(
						model->authored_lod_thresholds_q16_, projected_q16,
						frame.projection_scale, model->authored_lod_available_);
		if (selection.lod_index < 0 || selection.lod_index == model->active_lod_) {
			continue;
		}
		// The tree-visibility walk only for the models that actually cross: a
		// hidden model re-selects on the frame it becomes visible.
		if (!model->is_visible_in_tree()) {
			continue;
		}
		switches.push_back(LodSwitch{ model, selection.lod_index });
	}
	int applied = 0;
	for (const LodSwitch &change : switches) {
		// A switch applied earlier in this loop can unregister or free another
		// queued model (set_active_lod's runtime-state refresh reaches child
		// nodes); only a still-registered model is dereferenced.
		if (!authored_lod_models_.has(change.model)) {
			continue;
		}
		change.model->set_active_lod(change.lod_index);
		++applied;
	}
	for (ObjectModel *attachment : attachments) {
		if (!authored_lod_models_.has(attachment)) {
			continue;
		}
		const ObjectModel *owner = attachment->get_authored_lod_owner();
		const int level = opennova::renderer::attachment_lod_index(
				owner != nullptr ? owner->active_lod_ : 0,
				static_cast<int>(attachment->authored_lod_thresholds_q16_.size()));
		if (level < 0 || level == attachment->active_lod_) {
			continue;
		}
		attachment->set_active_lod(level);
		++applied;
	}
	return applied;
}

void ObjectModel::advance_awake_frame(double p_delta) {
	advance_awake_frame_impl(p_delta, nullptr);
}

PackedInt64Array ObjectModel::profile_awake_frame(double p_delta) {
	AwakeFrameProfile profile;
	advance_awake_frame_impl(p_delta, &profile);
	PackedInt64Array result;
	result.resize(AWAKE_PROFILE_SLOT_COUNT);
	result.set(AWAKE_PROFILE_CLOCK_ANIMATION_US, profile.clock_animation_us);
	result.set(AWAKE_PROFILE_PANM_US, profile.panm_us);
	result.set(AWAKE_PROFILE_MATERIAL_US, profile.material_us);
	result.set(AWAKE_PROFILE_ORDER_BOUNDS_US, profile.order_bounds_us);
	result.set(AWAKE_PROFILE_AWAKE_MODELS, profile.awake_models);
	result.set(AWAKE_PROFILE_RENDERABLE_MODELS, profile.renderable_models);
	return result;
}

void ObjectModel::advance_awake_frame_impl(double p_delta,
		AwakeFrameProfile *p_profile) {
	// The set is process-global while the drivers are per-context; a per-frame
	// guard keeps exactly one advance per Godot frame no matter how many
	// contexts call — the first caller wins, so a game frame and an idle menu
	// process cannot double-advance anim time.
	static uint64_t last_frame = UINT64_MAX;
	const uint64_t frame = Engine::get_singleton()->get_process_frames();
	if (frame == last_frame) {
		return;
	}
	last_frame = frame;
	if (awake_models_.is_empty()) {
		return;
	}
	// Copy first: advance_runtime_frame parks models (erasing them), and a model
	// could theoretically wake another mid-walk.
	LocalVector<ObjectModel *> batch;
	batch.reserve(awake_models_.size());
	for (ObjectModel *model : awake_models_) {
		batch.push_back(model);
	}
	for (ObjectModel *model : batch) {
		if (awake_models_.has(model)) {
			if (p_profile != nullptr) {
				++p_profile->awake_models;
			}
			model->advance_runtime_frame_profiled(p_delta, p_profile);
		}
	}
}

// MATCHTERRAIN-class instances read the terrain page projection (the c7/c8 fold at
// Foliage_RenderFarPatches @0x60a220..0x60a34f; class selection CRenderBatchQueue_FlushBatches
// @0x5d9ff3 - docs/render/render-material-re.md).
void ObjectModel::stamp_match_terrain_instances(bool p_page_ready,
		float p_layer, const Vector4 &p_projection) {
	match_terrain_page_ready_ = p_page_ready;
	match_terrain_page_layer_ = p_layer;
	match_terrain_page_projection_ = p_projection;
	const StringName enabled_name("u_match_terrain_enabled");
	const StringName ready_name("u_match_terrain_page_ready");
	const StringName layer_name("u_match_terrain_page_layer");
	const StringName projection_name("u_match_terrain_page_projection");
	const auto apply_to = [&](Node *p_parent) {
		if (p_parent == nullptr) {
			return;
		}
		const int children = p_parent->get_child_count();
		for (int child = 0; child < children; ++child) {
			GeometryInstance3D *instance = Object::cast_to<GeometryInstance3D>(
					p_parent->get_child(child));
			if (instance == nullptr) {
				continue;
			}
			instance->set_instance_shader_parameter(
					enabled_name, match_terrain_enabled_);
			instance->set_instance_shader_parameter(
					ready_name, match_terrain_enabled_ && p_page_ready);
			instance->set_instance_shader_parameter(layer_name, p_layer);
			instance->set_instance_shader_parameter(projection_name, p_projection);
		}
	};
	for (int64_t entry = 0; entry < robj_dense_.size(); ++entry) {
		apply_to(Object::cast_to<Node>(
				static_cast<Object *>(robj_dense_[entry])));
	}
	apply_to(skeleton_);
}

void ObjectModel::stamp_instance_uniforms(GeometryInstance3D *p_instance) const {
	if (p_instance == nullptr) {
		return;
	}
	p_instance->set_instance_shader_parameter(
			StringName("u_match_terrain_enabled"), match_terrain_enabled_);
	p_instance->set_instance_shader_parameter(
			StringName("u_match_terrain_page_ready"),
			match_terrain_enabled_ && match_terrain_page_ready_);
	p_instance->set_instance_shader_parameter(
			StringName("u_match_terrain_page_layer"), match_terrain_page_layer_);
	p_instance->set_instance_shader_parameter(
			StringName("u_match_terrain_page_projection"),
			match_terrain_page_projection_);
	// The same flag and margin set_viewmodel_pass stamps on the built set.
	p_instance->set_instance_shader_parameter(
			StringName("u_viewmodel_pass"), viewmodel_pass_);
	p_instance->set_extra_cull_margin(viewmodel_pass_ ? 8.0f : 0.0f);
}

void ObjectModel::set_viewmodel_pass(bool p_enabled) {
	if (viewmodel_pass_ == p_enabled &&
			(!p_enabled || viewmodel_pass_stamped_serial_ == scene_build_serial_)) {
		return;
	}
	viewmodel_pass_ = p_enabled;
	viewmodel_pass_stamped_serial_ = scene_build_serial_;
	const StringName pass_name("u_viewmodel_pass");
	// Retail's gun sits within ~2 u of the eye; 8 u puts the eye inside every
	// part's cull box under any beauty fov.
	const float margin = p_enabled ? 8.0f : 0.0f;
	const auto apply_to = [&](Node *p_parent) {
		if (p_parent == nullptr) {
			return;
		}
		const int children = p_parent->get_child_count();
		for (int child = 0; child < children; ++child) {
			GeometryInstance3D *instance = Object::cast_to<GeometryInstance3D>(
					p_parent->get_child(child));
			if (instance == nullptr) {
				continue;
			}
			instance->set_instance_shader_parameter(pass_name, p_enabled);
			instance->set_extra_cull_margin(margin);
		}
	};
	for (int64_t entry = 0; entry < robj_dense_.size(); ++entry) {
		apply_to(Object::cast_to<Node>(
				static_cast<Object *>(robj_dense_[entry])));
	}
	apply_to(skeleton_);
	render_order_dirty_ = true;
	refresh_render_order();
}

void ObjectModel::set_match_terrain_enabled(bool p_enabled) {
	if (match_terrain_enabled_ == p_enabled) {
		return;
	}
	match_terrain_enabled_ = p_enabled;
	if (p_enabled) {
		match_terrain_models_.insert(this);
	} else {
		match_terrain_models_.erase(this);
	}
	stamp_match_terrain_instances(false, 0.0f, Vector4());
	for (ObjectModel *linked : live_presentation_links()) {
		linked->set_match_terrain_enabled(p_enabled);
	}
}

void ObjectModel::refresh_match_terrain_frame(Terrain *p_terrain) {
	if (match_terrain_models_.is_empty()) {
		return;
	}
	LocalVector<ObjectModel *> batch;
	batch.reserve(match_terrain_models_.size());
	for (ObjectModel *model : match_terrain_models_) {
		batch.push_back(model);
	}
	const Ref<Texture2DArray> cache = p_terrain != nullptr
			? p_terrain->get_tile_cache_texture()
			: Ref<Texture2DArray>();
	for (ObjectModel *model : batch) {
		if (!match_terrain_models_.has(model)) {
			continue;
		}
		for (const Ref<ShaderMaterial> &material : model->surface_materials_) {
			if (material.is_valid()) {
				material->set_shader_parameter("u_match_terrain_cache", cache);
				material->set_shader_parameter(
						"u_has_match_terrain_cache", cache.is_valid());
			}
		}
		bool ready = false;
		float layer = 0.0f;
		Vector4 projection_uniform;
		if (p_terrain != nullptr && cache.is_valid() && model->is_inside_tree()) {
			const Vector3 position = model->get_global_position();
			const std::optional<opennova::TerrainTilePageBinding> page =
					p_terrain->get_tile_cache_binding_for_world_point_native(
							position.x, position.z);
			if (page.has_value() && page->ready) {
				const std::optional<opennova::TerrainTilePageProjection> projection =
						opennova::TerrainTileCompositionCache::page_projection(
								page->page);
				ready = projection.has_value();
				layer = static_cast<float>(page->layer);
				if (projection.has_value()) {
					projection_uniform = Vector4(projection->world_origin_x,
							projection->world_origin_z,
							projection->inverse_world_span,
							projection->world_span);
				}
			}
		}
		model->stamp_match_terrain_instances(ready, layer, projection_uniform);
	}
}

// Event-driven scheduling for the per-frame runtime advance. Models self-park:
// every mutation that can create per-frame work wakes the model (adds it to the
// shared set), and advance_runtime_frame parks it again the first frame nothing
// is live.
// [orig: Terrain_RenderSectorModels @0x5c5d30 computes runtime constants only
//  for models the batch draws]
void ObjectModel::wake_runtime_frame() {
	if (!awake_) {
		awake_ = true;
		awake_models_.insert(this);
	}
}

void ObjectModel::sleep_runtime_frame_if_idle() {
	if (needs_runtime_frame_work()) {
		return;
	}
	// A model without a shared PANM clock owns a local runtime clock and must
	// remain awake while playing; mission/wire models ride the shared clock.
	if (panm_clock_.is_null() && is_playing_) {
		return;
	}
	if (awake_) {
		awake_ = false;
		awake_models_.erase(this);
	}
}

void ObjectModel::_notification(int p_what) {
	if (p_what == NOTIFICATION_READY) {
		wake_runtime_frame();
		if (object_data_.is_valid()) {
			rebuild();
		}
		update_slot_shadow_group();
	} else if (p_what == NOTIFICATION_PARENTED || p_what == NOTIFICATION_UNPARENTED) {
		// Reparenting can change the caster registry's ancestor-derived
		// seat_parented fact (a caster moved under, or out from under, another
		// caster) without touching group membership; both edges bump.
		if (is_in_group(SlotShadow::caster_group())) {
			SlotShadow::bump_caster_group_revision();
		}
	} else if (p_what == NOTIFICATION_VISIBILITY_CHANGED) {
		// Becoming visible re-derives the render-side state (PANM pose, light
		// draw parts, order) that stayed stale while hidden.
		point_light_draw_parts_dirty_ = true;
		wake_runtime_frame();
	} else if (p_what == NOTIFICATION_TRANSFORM_CHANGED) {
		// Only models with blended strips enable this notification: a moved
		// model re-classifies its strips against the water plane in place,
		// without waking the full runtime walk.
		render_order_dirty_ = true;
		refresh_render_order();
	} else if (p_what == NOTIFICATION_PREDELETE) {
		// Only a model a PresentApplier row plan retains by pointer moves the
		// stamp: a throwable, viewmodel, wire-body, or preview model freeing
		// must not force every mission row back through a cold plan rebuild.
		if (present_planned_) {
			++lifetime_generation_;
		}
		if (awake_) {
			awake_ = false;
			awake_models_.erase(this);
		}
		alpha_strip_models_.erase(this);
		retire_geometry_instances();
	}
}

// Advance the model's render-time state once. Public for deterministic owners
// and tests: clocks continue while hidden; render-derived work waits until the
// model can be submitted again.
void ObjectModel::advance_runtime_frame(double p_delta) {
	advance_runtime_frame_profiled(p_delta, nullptr);
}

void ObjectModel::advance_runtime_frame_profiled(double p_delta,
		AwakeFrameProfile *p_profile) {
	if (object_data_.is_null() || !object_data_->has_document()) {
		if (awake_) {
			awake_ = false;
			awake_models_.erase(this);
		}
		return;
	}
	const bool renderable = is_visible_in_tree() && on_screen_;
	if (p_profile != nullptr && renderable) {
		++p_profile->renderable_models;
	}
	if (!needs_runtime_frame_work()) {
		// Keep the private preview clock continuous even while the model has no
		// time-driven consumer.
		const uint64_t clock_start = p_profile != nullptr
				? Time::get_singleton()->get_ticks_usec()
				: 0;
		if (panm_clock_.is_null() && is_playing_) {
			anim_time_ms_ = (anim_time_ms_ + static_cast<int64_t>(p_delta * 1000.0)) &
					0xffffffff;
		}
		if (p_profile != nullptr) {
			p_profile->clock_animation_us +=
					Time::get_singleton()->get_ticks_usec() - clock_start;
		}
		sleep_runtime_frame_if_idle();
		return;
	}
	apply_runtime_state(p_delta, renderable, p_profile);
	sleep_runtime_frame_if_idle();
}

bool ObjectModel::needs_runtime_frame_work() const {
	if (bounds_dirty_ || has_live_panm_ || !dynamic_material_slots_.is_empty() ||
			(render_order_dirty_ && !alpha_strip_draws_.is_empty()) ||
			!part_anims_.is_empty()) {
		return true;
	}
	if (skeleton_ == nullptr || skeletal_.is_null() || anim_key_.is_empty()) {
		return false;
	}
	return body_pose_dirty_ ||
			(is_playing_ && anim_playing_ && !anim_external_phase_) ||
			remote_pending_state_ >= 0;
}

void ObjectModel::refresh_live_panm_classification() {
	if (object_data_.is_null()) {
		has_live_panm_ = false;
	} else {
		has_live_panm_ = object_data_->has_live_panm_for_lod(active_lod_);
	}
}

int ObjectModel::clamp_lod_index(int p_lod_index) const {
	if (object_data_.is_null() || !object_data_->has_document()) {
		return 0;
	}
	const Dictionary summary = object_data_->get_summary();
	const int lod_count = int(summary.get("lod_count", 1));
	return CLAMP(p_lod_index, 0, MAX(lod_count - 1, 0));
}

void ObjectModel::refresh_active_lod_rest_transforms() {
	robj_rest_transforms_.clear();
	if (object_data_.is_null() || !object_data_->has_document()) {
		return;
	}
	const Threedi3di3 &model = object_data_->native_model();
	if (active_lod_ < 0 ||
			static_cast<std::size_t>(active_lod_) >= model.lod_count ||
			model.lods == nullptr) {
		return;
	}
	const ThreediLod &lod = model.lods[active_lod_];
	for (std::size_t i = 0; i < lod.render_object_count; ++i) {
		const ThreediRenderObject &part = lod.render_objects[i];
		robj_rest_transforms_[static_cast<int>(i)] =
				Transform3D(Basis(), Vector3(-part.abs[0], part.abs[1], part.abs[2]));
	}
}

Node3D *ObjectModel::get_or_create_robj_node(int p_robj_index) {
	Node3D **existing = robj_nodes_.getptr(p_robj_index);
	if (existing != nullptr) {
		return *existing;
	}
	Node3D *node = memnew(Node3D);
	node->set_name(String("Robj_") + String::num_int64(p_robj_index));
	// Rebuilds honor the applied section mask.
	if (section_visibility_mask_ != -1) {
		node->set_visible(((section_visibility_mask_ >> p_robj_index) & 1) == 1);
	}
	add_child(node);
	robj_nodes_[p_robj_index] = node;
	// The dense part-index -> node array apply_panm_to_nodes writes through
	// (nulls for parts without a node).
	while (robj_dense_.size() <= p_robj_index) {
		robj_dense_.append(Variant());
	}
	robj_dense_[p_robj_index] = node;
	return node;
}

void ObjectModel::apply_runtime_state(double p_delta, bool p_renderable,
		AwakeFrameProfile *p_profile) {
	if (object_data_.is_null() || !object_data_->has_document()) {
		return;
	}
	const uint64_t clock_start = p_profile != nullptr
			? Time::get_singleton()->get_ticks_usec()
			: 0;
	if (panm_clock_.is_valid()) {
		anim_time_ms_ = panm_clock_->get_time_ms();
	} else if (is_playing_) {
		anim_time_ms_ = (anim_time_ms_ + static_cast<int64_t>(p_delta * 1000.0)) &
				0xffffffff;
	}
	const bool part_changed = advance_part_anims(p_delta);
	advance_body_animation(p_delta, p_renderable);
	if (p_profile != nullptr) {
		p_profile->clock_animation_us +=
				Time::get_singleton()->get_ticks_usec() - clock_start;
	}
	if (!p_renderable) {
		// Everything below derives from the absolute clock + the register/pose
		// state advanced above; it re-derives on the next visible frame.
		return;
	}
	// The weather's FLICKER / SWING registers hashed on THIS model's position:
	// retail's gnrc/Sway world bone callbacks run HUD_CacheEntityDisplayInfo
	// on their own entity right before the batch collector snapshots the
	// registers (BoneCallback_gnrc_World @ 0x4e286c, BoneCallback_Sway_World
	// @ 0x4e2b22, the collect @ 0x5d968d); only a model whose 3DI declares
	// either register can read them, so only those hash. Written straight into
	// the dictionary the evaluations below read (no batch replay: the value
	// moves every tick anyway).
	if (object_data_.is_valid() && object_data_->uses_weather_ctrl_registers()) {
		const Vector3 p = get_global_position(); // Godot (x, up, -y)
		int32_t flicker = 0;
		int32_t swing = 0;
		if (ObjectData::weather_ctrl_registers_at(
					static_cast<int32_t>(std::lround(static_cast<double>(p.x) * 65536.0)),
					static_cast<int32_t>(std::lround(static_cast<double>(-p.z) * 65536.0)),
					static_cast<int32_t>(std::lround(static_cast<double>(p.y) * 65536.0)),
					flicker, swing)) {
			static const String flicker_register =
					ObjectData::canonical_control_register_name("FLICKER");
			static const String swing_register =
					ObjectData::canonical_control_register_name("SWING");
			ctrl_values_[flicker_register] = static_cast<int64_t>(flicker);
			ctrl_values_[swing_register] = static_cast<int64_t>(swing);
			ctrl_native_cache_valid_ = false;
		}
	}
	// Retail poses PANM during entity submission before the later render-batch
	// flush evaluates material generators — noise waveforms share one random
	// stream. [orig: Render_SubmitEntity @0x5DAD80 -> Model_TransformBoneMatrices
	//  @0x58E390; CRenderBatchQueue_SortAndFlush @0x5DAE40 ->
	//  apply_shader_parameters @0x58DB80]
	const uint64_t panm_start = p_profile != nullptr
			? Time::get_singleton()->get_ticks_usec()
			: 0;
	bool robj_changed = false;
	if (has_live_panm_ || bounds_dirty_) {
		robj_changed = apply_robj_transforms();
	}
	if (p_profile != nullptr) {
		p_profile->panm_us +=
				Time::get_singleton()->get_ticks_usec() - panm_start;
	}
	// Only dynamic materials need a per-frame push; static slots keep the
	// identity values written at material creation.
	const uint64_t material_start = p_profile != nullptr
			? Time::get_singleton()->get_ticks_usec()
			: 0;
	const opennova::renderer::ControlRegisterValues material_ctrl_values =
			dynamic_material_slots_.is_empty()
			? opennova::renderer::ControlRegisterValues{}
			: runtime_ctrl_values();
	for (int64_t s = 0; s < dynamic_material_slots_.size(); ++s) {
		const int i = dynamic_material_slots_[s];
		const Ref<ShaderMaterial> material = surface_materials_[i];
		if (material.is_null()) {
			continue;
		}
		const int material_index = surface_material_indices_[i];
		MaterialRuntimeStamp &stamp = material_runtime_stamps_[
				static_cast<size_t>(i)];
		// The focused Q3 compile caches this material's block; every write
		// below names the material so its surfaces re-read it once.
		bool q3_parameters_changed = false;
		if (material_needs_eval_[i]) {
			opennova::renderer::MaterialRuntime runtime;
			if (object_data_->eval_material_runtime_native(material_index,
						anim_time_ms_, material_ctrl_values, runtime)) {
				const opennova::renderer::MaterialRuntime &previous = stamp.runtime;
				if (!stamp.runtime_valid || runtime.uv.m00 != previous.uv.m00 ||
						runtime.uv.m10 != previous.uv.m10 ||
						runtime.uv.m20 != previous.uv.m20) {
					material->set_shader_parameter("u_uv_transform_u",
							Vector3(runtime.uv.m00, runtime.uv.m10, runtime.uv.m20));
					q3_parameters_changed = true;
				}
				if (!stamp.runtime_valid || runtime.uv.m01 != previous.uv.m01 ||
						runtime.uv.m11 != previous.uv.m11 ||
						runtime.uv.m21 != previous.uv.m21) {
					material->set_shader_parameter("u_uv_transform_v",
							Vector3(runtime.uv.m01, runtime.uv.m11, runtime.uv.m21));
					q3_parameters_changed = true;
				}
				if (!stamp.runtime_valid || runtime.rgb_r != previous.rgb_r ||
						runtime.rgb_g != previous.rgb_g ||
						runtime.rgb_b != previous.rgb_b) {
					material->set_shader_parameter("u_rgb_mod",
							Vector3(runtime.rgb_r, runtime.rgb_g, runtime.rgb_b));
					q3_parameters_changed = true;
				}
				if (!stamp.runtime_valid || runtime.alpha != previous.alpha) {
					material->set_shader_parameter("u_alpha_mod", runtime.alpha);
					q3_parameters_changed = true;
				}
				stamp.runtime = runtime;
				stamp.runtime_valid = true;
			}
		}
		const Array *frames = anim_frames_by_mat_.getptr(material_index);
		if (frames != nullptr && frames->size() > 1) {
			const int frame_index = object_data_->compute_anim_frame_native(
					material_index, anim_time_ms_, material_ctrl_values);
			if (frame_index != stamp.anim_frame && frame_index >= 0 &&
					frame_index < frames->size()) {
				const Ref<Texture2D> frame = (*frames)[frame_index];
				if (frame.is_valid()) {
					set_material_and_auxiliary_parameter(material, "u_diffuse", frame);
					stamp.anim_frame = frame_index;
					q3_parameters_changed = true;
				}
			}
		}
		if (q3_parameters_changed) {
			FrameFx::invalidate_q3_object_material(material);
		}
	}
	if (p_profile != nullptr) {
		p_profile->material_us +=
				Time::get_singleton()->get_ticks_usec() - material_start;
	}
	const uint64_t order_start = p_profile != nullptr
			? Time::get_singleton()->get_ticks_usec()
			: 0;
	if (part_changed || robj_changed) {
		render_order_dirty_ = true;
		point_light_draw_parts_dirty_ = true;
	}
	refresh_render_order();
	if (bounds_dirty_ || part_changed || robj_changed) {
		set_model_bounds(compute_transformed_mesh_bounds());
		bounds_dirty_ = false;
	}
	if (p_profile != nullptr) {
		p_profile->order_bounds_us +=
				Time::get_singleton()->get_ticks_usec() - order_start;
	}
}

bool ObjectModel::apply_robj_transforms() {
	if (object_data_.is_null() || robj_nodes_.is_empty()) {
		return false;
	}
	// One native call evaluates PANM at most once per graphic per frame (the
	// placer shares one ObjectData across every instance of a graphic) and
	// writes only the parts whose transforms changed since this model applied.
	const int64_t revision = object_data_->apply_panm_to_nodes_table(
			active_lod_, anim_time_ms_, runtime_ctrl_values(), robj_dense_,
			panm_applied_revision_);
	const bool changed = revision != panm_applied_revision_;
	panm_applied_revision_ = revision;
	return changed;
}

// The camera-submission input for the render-derived gate; public so the
// bounds notifier's signals and deterministic owners/tests share one seam.
void ObjectModel::set_on_screen(bool p_value) {
	if (on_screen_ == p_value) {
		return;
	}
	on_screen_ = p_value;
	if (p_value) {
		wake_runtime_frame();
	}
}

void ObjectModel::set_model_bounds(const AABB &p_bounds) {
	sync_screen_notifier(p_bounds);
	if (aabb_equal_approx(model_bounds_, p_bounds)) {
		return;
	}
	model_bounds_ = p_bounds;
	emit_signal("bounds_changed", model_bounds_);
}

bool ObjectModel::aabb_equal_approx(const AABB &p_a, const AABB &p_b) {
	return p_a.position.is_equal_approx(p_b.position) &&
			p_a.size.is_equal_approx(p_b.size);
}

AABB ObjectModel::get_world_bounds() const {
	if (!is_inside_tree()) {
		return AABB(get_position(), Vector3());
	}
	return get_global_transform().xform(model_bounds_);
}

void ObjectModel::collect_point_light_draw_parts(
		std::vector<PointLightDrawPart> &r_parts) const {
	const Transform3D current = is_inside_tree() ? get_global_transform()
												: Transform3D();
	if (!point_light_draw_parts_dirty_ &&
			current == point_light_draw_parts_transform_) {
		r_parts = point_light_draw_parts_cache_;
		return;
	}
	point_light_draw_parts_dirty_ = false;
	point_light_draw_parts_transform_ = current;
	r_parts.clear();
	r_parts.reserve(robj_nodes_.size());
	for (const KeyValue<int, Node3D *> &kv : robj_nodes_) {
		Node3D *part = kv.value;
		if (part == nullptr || !part->is_visible_in_tree()) {
			continue;
		}
		bool has_bounds = false;
		AABB world_bounds;
		for (int child = 0; child < part->get_child_count(); ++child) {
			MeshInstance3D *instance = Object::cast_to<MeshInstance3D>(
					part->get_child(child));
			if (instance == nullptr || !instance->is_visible_in_tree()) {
				continue;
			}
			const AABB child_bounds = instance->get_global_transform().xform(
					instance->get_aabb());
			world_bounds = has_bounds ? world_bounds.merge(child_bounds)
					: child_bounds;
			has_bounds = true;
		}
		if (has_bounds) {
			r_parts.push_back(PointLightDrawPart{kv.key, world_bounds});
		}
	}
	std::stable_sort(r_parts.begin(), r_parts.end(),
			[](const PointLightDrawPart &a, const PointLightDrawPart &b) {
				return a.robj_index < b.robj_index;
			});
	point_light_draw_parts_cache_ = r_parts;
}

Vector3 ObjectModel::get_model_light_world_position(int p_index) const {
	if (object_data_.is_null() || p_index < 0 ||
			p_index >= object_data_->get_light_count()) {
		return get_global_position();
	}
	const Dictionary info = object_data_->get_light_info(p_index);
	const Vector3 model_position = info.get("position", Vector3());
	Vector3 position = get_global_transform().xform(model_position);
	const int subobject = int(info.get("subobject", 0));
	// Zero is the witnessed unattached sentinel. A nonzero subobject follows
	// the rest-to-live transform, matching the user-point attachment basis.
	if (subobject > 0 && skeleton_ != nullptr &&
			subobject < skeleton_->get_bone_count()) {
		position = (skeleton_->get_global_transform() *
					   skeleton_->get_bone_global_pose(subobject) *
					   skeleton_->get_bone_global_rest(subobject).affine_inverse())
					   .xform(model_position);
	} else if (subobject > 0) {
		Node3D *const *node = robj_nodes_.getptr(subobject);
		const Transform3D *rest = robj_rest_transforms_.getptr(subobject);
		if (node != nullptr && *node != nullptr && rest != nullptr) {
			position = (*node)->get_global_transform().xform(
					rest->affine_inverse().xform(model_position));
		}
	}
	return position;
}

void ObjectModel::apply_point_light_selection_to_robj(int p_robj_index,
		int p_count, const Vector4 *p_posr, const Vector4 *p_color) {
	const int count = CLAMP(p_count, 0, 4);
	uint64_t hash = 0xcbf29ce484222325ull;
	const auto mix = [&hash](const void *data, size_t size) {
		const uint8_t *bytes = static_cast<const uint8_t *>(data);
		for (size_t i = 0; i < size; ++i) {
			hash = (hash ^ bytes[i]) * 0x100000001b3ull;
		}
	};
	mix(&count, sizeof(count));
	for (int i = 0; i < count; ++i) {
		mix(&p_posr[i], sizeof(Vector4));
		mix(&p_color[i], sizeof(Vector4));
	}
	const uint64_t *last = point_light_selection_hashes_.getptr(p_robj_index);
	if (last != nullptr && hash == *last) {
		return;
	}
	point_light_selection_hashes_[p_robj_index] = hash;
	// Applies are hash-gated and infrequent; per-call StringName construction
	// avoids a DLL-teardown-ordered static against Godot's name table.
	const StringName count_name("u_point_light_count");
	const StringName posr_names[4] = {
		StringName("u_point_light_posr_0"), StringName("u_point_light_posr_1"),
		StringName("u_point_light_posr_2"), StringName("u_point_light_posr_3")
	};
	const StringName color_names[4] = {
		StringName("u_point_light_color_0"),
		StringName("u_point_light_color_1"),
		StringName("u_point_light_color_2"),
		StringName("u_point_light_color_3")
	};
	const auto apply_to = [&](Node *p_parent) {
		if (p_parent == nullptr) {
			return;
		}
		const int children = p_parent->get_child_count();
		for (int child = 0; child < children; ++child) {
			GeometryInstance3D *instance = Object::cast_to<GeometryInstance3D>(
					p_parent->get_child(child));
			if (instance == nullptr) {
				continue;
			}
			instance->set_instance_shader_parameter(count_name,
					static_cast<float>(count));
			for (int i = 0; i < 4; ++i) {
				const Vector4 posr = i < count ? p_posr[i] : Vector4();
				const Vector4 color = i < count ? p_color[i] : Vector4();
				instance->set_instance_shader_parameter(posr_names[i], posr);
				instance->set_instance_shader_parameter(color_names[i], color);
			}
		}
	};
	Node *target = p_robj_index < 0 ? static_cast<Node *>(skeleton_) : nullptr;
	if (p_robj_index >= 0) {
		Node3D *const *part = robj_nodes_.getptr(p_robj_index);
		target = part != nullptr ? static_cast<Node *>(*part) : nullptr;
	}
	apply_to(target);
}

void ObjectModel::apply_point_light_selection(int p_count,
		const Vector4 *p_posr, const Vector4 *p_color) {
	// Surface instances are direct children of their Robj part node or the
	// shared skeleton (object_model_scene.cpp attach split).
	for (const KeyValue<int, Node3D *> &kv : robj_nodes_) {
		apply_point_light_selection_to_robj(
				kv.key, p_count, p_posr, p_color);
	}
	if (skeleton_ != nullptr) {
		apply_point_light_selection_to_robj(-1, p_count, p_posr, p_color);
	}
}

void ObjectModel::_bind_methods() {
	ClassDB::bind_static_method("ObjectModel",
			D_METHOD("advance_awake_frame", "delta"),
			&ObjectModel::advance_awake_frame);
	ClassDB::bind_static_method("ObjectModel",
			D_METHOD("profile_awake_frame", "delta"),
			&ObjectModel::profile_awake_frame);
	ClassDB::bind_static_method("ObjectModel",
			D_METHOD("refresh_match_terrain_frame", "terrain"),
			&ObjectModel::refresh_match_terrain_frame);
	ClassDB::bind_static_method("ObjectModel",
			D_METHOD("update_authored_lods",
					"camera_transform", "vertical_fov",
					"viewport_width", "viewport_height"),
			&ObjectModel::update_authored_lods);
	ClassDB::bind_static_method("ObjectModel",
			D_METHOD("get_live_geometry_instance_count"),
			&ObjectModel::get_live_geometry_instance_count);
	ClassDB::bind_method(D_METHOD("is_runtime_frame_awake"),
			&ObjectModel::is_runtime_frame_awake);
	ClassDB::bind_method(D_METHOD("get_scene_build_serial"),
			&ObjectModel::get_scene_build_serial);
	ClassDB::bind_method(D_METHOD("wake_runtime_frame"),
			&ObjectModel::wake_runtime_frame);
	ClassDB::bind_method(D_METHOD("set_object_data", "data"), &ObjectModel::set_object_data);
	ClassDB::bind_method(D_METHOD("get_object_data"), &ObjectModel::get_object_data);
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "object_data",
						 PROPERTY_HINT_RESOURCE_TYPE, "ObjectData"),
			"set_object_data", "get_object_data");
	ClassDB::bind_method(D_METHOD("set_mirror_reflected", "reflected"),
			&ObjectModel::set_mirror_reflected);
	ClassDB::bind_method(D_METHOD("get_mirror_reflected"),
			&ObjectModel::get_mirror_reflected);
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "mirror_reflected"),
			"set_mirror_reflected", "get_mirror_reflected");
	ClassDB::bind_method(D_METHOD("set_match_terrain_enabled", "enabled"),
			&ObjectModel::set_match_terrain_enabled);
	ClassDB::bind_method(D_METHOD("set_viewmodel_pass", "enabled"),
			&ObjectModel::set_viewmodel_pass);
	ClassDB::bind_method(D_METHOD("set_presentation_layer", "layer"),
			&ObjectModel::set_presentation_layer);
	ClassDB::bind_method(D_METHOD("set_shadow_caster_enabled", "enabled"),
			&ObjectModel::set_shadow_caster_enabled);
	ClassDB::bind_method(D_METHOD("is_shadow_caster_enabled"),
			&ObjectModel::is_shadow_caster_enabled);
	ClassDB::bind_method(D_METHOD("set_static_shadow_caster_enabled", "enabled"),
			&ObjectModel::set_static_shadow_caster_enabled);
	ClassDB::bind_method(D_METHOD("is_static_shadow_caster_enabled"),
			&ObjectModel::is_static_shadow_caster_enabled);
	ClassDB::bind_method(D_METHOD("get_entity_uniform_scale_q16"),
			&ObjectModel::get_entity_uniform_scale_q16);
	ClassDB::bind_method(D_METHOD("compose_entity_transform", "basis", "origin"),
			&ObjectModel::compose_entity_transform);
	ClassDB::bind_method(D_METHOD("set_shadow_bound_radii", "model_sphere", "entity_bound"),
			&ObjectModel::set_shadow_bound_radii);
	ClassDB::bind_method(D_METHOD("set_slot_shadow_capture_with", "owner"),
			&ObjectModel::set_slot_shadow_capture_with);
	ClassDB::bind_method(
			D_METHOD("set_entity_lighting_context", "effect_scale", "interior_lerp",
					"interior_daylight"),
			&ObjectModel::set_entity_lighting_context);
	ClassDB::bind_method(D_METHOD("set_interior_section_light_transfer", "daylight"),
			&ObjectModel::set_interior_section_light_transfer);
	ClassDB::bind_method(D_METHOD("get_model_bounds"), &ObjectModel::get_model_bounds);
	ClassDB::bind_method(D_METHOD("get_model_light_world_position", "index"),
			&ObjectModel::get_model_light_world_position);
	ClassDB::bind_method(D_METHOD("get_render_part_nodes"),
			&ObjectModel::get_render_part_nodes);
	ClassDB::bind_method(D_METHOD("set_section_visibility_mask", "mask"),
			&ObjectModel::set_section_visibility_mask);
	ClassDB::bind_method(D_METHOD("get_surface_material_indices"),
			&ObjectModel::get_surface_material_indices);
	ClassDB::bind_method(D_METHOD("get_surface_materials"),
			&ObjectModel::get_surface_materials);
	ClassDB::bind_method(D_METHOD("is_playing"), &ObjectModel::is_playing);
	ClassDB::bind_method(D_METHOD("set_playing", "value"), &ObjectModel::set_playing);
	ClassDB::bind_method(D_METHOD("set_panm_clock", "clock"), &ObjectModel::set_panm_clock);
	ClassDB::bind_method(D_METHOD("set_active_lod", "lod_index"),
			&ObjectModel::set_active_lod);
	ClassDB::bind_method(D_METHOD("get_active_lod"),
			&ObjectModel::get_active_lod);
	ClassDB::bind_method(D_METHOD("set_authored_lod_enabled", "enabled"),
			&ObjectModel::set_authored_lod_enabled);
	ClassDB::bind_method(D_METHOD("set_authored_lod_owner", "owner"),
			&ObjectModel::set_authored_lod_owner);
	ClassDB::bind_method(D_METHOD("get_authored_lod_owner"),
			&ObjectModel::get_authored_lod_owner);
	ClassDB::bind_method(D_METHOD("get_surface_slot_count"),
			&ObjectModel::get_surface_slot_count);
	ClassDB::bind_method(D_METHOD("get_level_surface_count", "lod_index"),
			&ObjectModel::get_level_surface_count);
	ClassDB::bind_method(D_METHOD("get_retained_surface_instance_count"),
			&ObjectModel::get_retained_surface_instance_count);
	ClassDB::bind_method(D_METHOD("add_level_bound_visual", "lod_index", "visual"),
			&ObjectModel::add_level_bound_visual);
	ClassDB::bind_method(D_METHOD("set_authored_occluders_enabled", "enabled"),
			&ObjectModel::set_authored_occluders_enabled);
	ClassDB::bind_method(D_METHOD("get_authored_occluder_count"),
			&ObjectModel::get_authored_occluder_count);
	ClassDB::bind_method(D_METHOD("rebuild"), &ObjectModel::rebuild);
	ClassDB::bind_method(D_METHOD("advance_runtime_frame", "delta"),
			&ObjectModel::advance_runtime_frame);
	ClassDB::bind_method(D_METHOD("set_on_screen", "value"), &ObjectModel::set_on_screen);
	ClassDB::bind_method(D_METHOD("is_on_screen"), &ObjectModel::is_on_screen);

	ClassDB::bind_method(D_METHOD("begin_ctrl_update"), &ObjectModel::begin_ctrl_update);
	ClassDB::bind_method(D_METHOD("end_ctrl_update"), &ObjectModel::end_ctrl_update);
	ClassDB::bind_method(D_METHOD("set_ctrl_value", "name", "value"),
			&ObjectModel::set_ctrl_value);
	ClassDB::bind_method(D_METHOD("clear_ctrl_value", "name"),
			&ObjectModel::clear_ctrl_value);
	ClassDB::bind_method(D_METHOD("set_ctrl_override", "owner", "name", "value"),
			&ObjectModel::set_ctrl_override);
	ClassDB::bind_method(D_METHOD("clear_ctrl_override", "owner", "name"),
			&ObjectModel::clear_ctrl_override);
	ClassDB::bind_method(D_METHOD("get_ctrl_values"), &ObjectModel::get_ctrl_values);

	ClassDB::bind_method(D_METHOD("set_skeletal_anim", "skeletal"),
			&ObjectModel::set_skeletal_anim);
	ClassDB::bind_method(D_METHOD("get_skeletal_anim"), &ObjectModel::get_skeletal_anim);
	ClassDB::bind_method(D_METHOD("get_skeleton"), &ObjectModel::get_skeleton);
	ClassDB::bind_method(D_METHOD("has_skeleton"), &ObjectModel::has_skeleton);
	ClassDB::bind_method(D_METHOD("has_muzzle"), &ObjectModel::has_muzzle);
	ClassDB::bind_method(D_METHOD("set_muzzle_point_name", "name"),
			&ObjectModel::set_muzzle_point_name);
	ClassDB::bind_method(D_METHOD("play_body_clip", "key"), &ObjectModel::play_body_clip);
	ClassDB::bind_method(D_METHOD("play_body_clip_variant", "key", "variant"),
			&ObjectModel::play_body_clip_variant);
	ClassDB::bind_method(
			D_METHOD("play_body_clip_variant_at_time", "key", "variant", "seconds"),
			&ObjectModel::play_body_clip_variant_at_time);
	ClassDB::bind_method(D_METHOD("play_body_clip_at", "key", "phase_ticks"),
			&ObjectModel::play_body_clip_at);
	ClassDB::bind_method(
			D_METHOD("play_body_blend_at", "source_key", "source_phase_ticks",
					"target_key", "target_phase_ticks", "weight"),
			&ObjectModel::play_body_blend_at);
	ClassDB::bind_method(D_METHOD("play_body_clip_seeded", "key", "phase_ticks"),
			&ObjectModel::play_body_clip_seeded);
	ClassDB::bind_method(
			D_METHOD("apply_remote_body_state", "state_id", "key", "flags", "phase_ticks"),
			&ObjectModel::apply_remote_body_state, DEFVAL(-1));
	ClassDB::bind_method(D_METHOD("reset_remote_body_state"),
			&ObjectModel::reset_remote_body_state);
	ClassDB::bind_method(D_METHOD("advance_remote_body_blend_tick", "state_id"),
			&ObjectModel::advance_remote_body_blend_tick);
	ClassDB::bind_method(D_METHOD("remote_body_needs_fixed_tick"),
			&ObjectModel::remote_body_needs_fixed_tick);
	ClassDB::bind_method(D_METHOD("stop_body_clip"), &ObjectModel::stop_body_clip);
	ClassDB::bind_method(D_METHOD("get_active_body_clip"),
			&ObjectModel::get_active_body_clip);
	ClassDB::bind_method(D_METHOD("play_body_anim", "slot"), &ObjectModel::play_body_anim);
	ClassDB::bind_method(D_METHOD("play_body_anim_at", "slot", "phase_ticks"),
			&ObjectModel::play_body_anim_at);
	ClassDB::bind_method(D_METHOD("get_animation_time_ms"),
			&ObjectModel::get_animation_time_ms);
	ClassDB::bind_method(D_METHOD("set_animation_time", "seconds"),
			&ObjectModel::set_animation_time);
	ClassDB::bind_method(D_METHOD("get_animation_time"), &ObjectModel::get_animation_time);
	ClassDB::bind_method(D_METHOD("play_part_anim", "channel", "play_type", "time_s"),
			&ObjectModel::play_part_anim);
	ClassDB::bind_method(D_METHOD("restart_part_anim", "channel", "play_type", "time_s"),
			&ObjectModel::restart_part_anim);
	ClassDB::bind_method(D_METHOD("set_part_phase", "channel", "phase"),
			&ObjectModel::set_part_phase);
	ClassDB::bind_method(D_METHOD("clear_part_phase", "channel"),
			&ObjectModel::clear_part_phase);
	ClassDB::bind_method(D_METHOD("clear_part_anims"), &ObjectModel::clear_part_anims);
	ClassDB::bind_method(D_METHOD("get_active_part_anims"),
			&ObjectModel::get_active_part_anims);
	ClassDB::bind_method(D_METHOD("set_weapon_channel", "key", "phase_ticks",
								 "prev_key", "prev_phase_ticks", "blend_weight",
								 "variant", "prev_variant"),
			&ObjectModel::set_weapon_channel, DEFVAL(String()), DEFVAL(0),
			DEFVAL(1.0f), DEFVAL(0), DEFVAL(0));
	ClassDB::bind_method(D_METHOD("get_weapon_channel"),
			&ObjectModel::get_weapon_channel);
	ClassDB::bind_method(D_METHOD("set_aim_overlay", "deltas"),
			&ObjectModel::set_aim_overlay);
	ClassDB::bind_method(D_METHOD("get_aim_overlay"),
			&ObjectModel::get_aim_overlay);
	ClassDB::bind_method(D_METHOD("set_right_hand_collapsed", "collapsed"),
			&ObjectModel::set_right_hand_collapsed);
	ClassDB::bind_method(D_METHOD("is_right_hand_collapsed"),
			&ObjectModel::is_right_hand_collapsed);
	ClassDB::bind_method(D_METHOD("get_body_blend"),
			&ObjectModel::get_body_blend);
	ClassDB::bind_method(D_METHOD("advance_body_animation", "delta", "write_pose"),
			&ObjectModel::advance_body_animation, DEFVAL(true));
	ClassDB::bind_method(D_METHOD("is_body_pose_dirty"),
			&ObjectModel::is_body_pose_dirty);

	ADD_SIGNAL(MethodInfo("bounds_changed", PropertyInfo(Variant::AABB, "bounds")));

	BIND_ENUM_CONSTANT(AWAKE_PROFILE_CLOCK_ANIMATION_US);
	BIND_ENUM_CONSTANT(AWAKE_PROFILE_PANM_US);
	BIND_ENUM_CONSTANT(AWAKE_PROFILE_MATERIAL_US);
	BIND_ENUM_CONSTANT(AWAKE_PROFILE_ORDER_BOUNDS_US);
	BIND_ENUM_CONSTANT(AWAKE_PROFILE_AWAKE_MODELS);
	BIND_ENUM_CONSTANT(AWAKE_PROFILE_RENDERABLE_MODELS);
	BIND_ENUM_CONSTANT(AWAKE_PROFILE_SLOT_COUNT);
	BIND_ENUM_CONSTANT(PRESENTATION_LAYER_LOCAL_BODY);
	BIND_ENUM_CONSTANT(PRESENTATION_LAYER_LOCAL_BODY_HIDDEN);
	BIND_ENUM_CONSTANT(PRESENTATION_LAYER_VIEWMODEL);
}

} // namespace godot
