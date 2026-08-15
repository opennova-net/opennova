// ObjectModel core: lifecycle, data wiring, CTRL registers, the
// event-driven runtime frame, and the class registration surface.
// Ported verbatim from nova_object_model.gd (2026-08-09 de-scripting).

#include "object/nova_object_model.h"

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/geometry_instance3d.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/core/math.hpp>
#include <godot_cpp/templates/local_vector.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include "object/nova_object_shader_cache.h"

namespace godot {

namespace {

bool is_part_local_ctrl(const String &p_register) {
	// Retail reuses one global CTRL bus, but rewrites 93..95 separately just
	// before each head/body/arms submit. A retained composed model therefore
	// keeps these three values local to each submitted part. See the submit-site
	// evidence in docs/playerinfo/avatars-re.md.
	return p_register == "TEX_CAMO1" || p_register == "TEX_CAMO2" ||
			p_register == "TEX_CAMO3";
}

} // namespace

void EnvLightValues::_bind_methods() {
	ClassDB::bind_method(D_METHOD("equals", "other"), &EnvLightValues::equals);
	ClassDB::bind_static_method("EnvLightValues", D_METHOD("retail_noon_defaults"),
			&EnvLightValues::retail_noon_defaults);
#define NOVA_ENV_PROP(m_type, m_name)                                             \
	ClassDB::bind_method(D_METHOD("set_" #m_name, "value"),                       \
			&EnvLightValues::set_##m_name);                                   \
	ClassDB::bind_method(D_METHOD("get_" #m_name), &EnvLightValues::get_##m_name); \
	ADD_PROPERTY(PropertyInfo(m_type, #m_name), "set_" #m_name, "get_" #m_name)
	NOVA_ENV_PROP(Variant::VECTOR3, hemi_sky);
	NOVA_ENV_PROP(Variant::VECTOR3, dir);
	NOVA_ENV_PROP(Variant::VECTOR3, dir_color);
	NOVA_ENV_PROP(Variant::VECTOR3, hemi_ground);
	NOVA_ENV_PROP(Variant::VECTOR3, ceiling);
	NOVA_ENV_PROP(Variant::VECTOR3, floor_color);
	NOVA_ENV_PROP(Variant::VECTOR3, gain);
	NOVA_ENV_PROP(Variant::BOOL, fog_enabled);
	NOVA_ENV_PROP(Variant::VECTOR3, fog_color);
	NOVA_ENV_PROP(Variant::FLOAT, fog_start);
	NOVA_ENV_PROP(Variant::FLOAT, fog_end);
	NOVA_ENV_PROP(Variant::INT, fog_type);
#undef NOVA_ENV_PROP
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
	ClassDB::bind_method(D_METHOD("publish", "values"), &EnvLightState::publish);
	ClassDB::bind_method(D_METHOD("get_values"), &EnvLightState::get_values);
	ClassDB::bind_method(D_METHOD("get_generation"), &EnvLightState::get_generation);
	ADD_SIGNAL(MethodInfo("changed"));
}

void EnvLightState::publish(const Ref<EnvLightValues> &p_values) {
	values_ = p_values;
	++generation_;
	emit_signal("changed");
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

ObjectModel::ObjectModel() {
	env_stagger_slot_ = static_cast<int>(
			(static_cast<uint64_t>(get_instance_id()) >> 3) % kEnvRestampSpreadFrames);
}

ObjectModel::~ObjectModel() {
	// A model can be freed without a PREDELETE notification in some teardown
	// paths; never leave a dangling pointer in the shared awake set.
	if (awake_) {
		awake_ = false;
		awake_models_.erase(this);
	}
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
	for (const ObjectID id : presentation_links_) {
		ObjectModel *model = id.is_valid()
				? Object::cast_to<ObjectModel>(ObjectDB::get_instance(id))
				: nullptr;
		if (model != nullptr) {
			out.push_back(model);
		}
	}
	return out;
}

void ObjectModel::add_presentation_link(ObjectModel *p_model) {
	if (p_model == nullptr || p_model == this) {
		return;
	}
	const ObjectID id(p_model->get_instance_id());
	for (const ObjectID existing : presentation_links_) {
		if (existing == id) {
			return;
		}
	}
	presentation_links_.push_back(id);
}

int ObjectModel::get_presentation_link_count() const {
	return live_presentation_links().size();
}

void ObjectModel::set_model_light_preview_enabled(bool p_enabled) {
	if (model_light_preview_enabled_ == p_enabled) {
		return;
	}
	model_light_preview_enabled_ = p_enabled;
	last_light_push_valid_ = false;
	wake_runtime_frame();
	if (p_enabled) {
		apply_lights();
		return;
	}
	for (const Ref<ShaderMaterial> &material : surface_materials_) {
		if (material.is_valid()) {
			material->set_shader_parameter("u_local_light_count", 0);
		}
	}
}

void ObjectModel::set_shadow_caster_enabled(bool p_enabled) {
	set_shadow_caster_layer_enabled(LAYER_DYNAMIC_SHADOW_CASTER, p_enabled);
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
	apply_shadow_casting_below(this);
}

void ObjectModel::apply_shadow_casting_below(Node *p_root) {
	const GeometryInstance3D::ShadowCastingSetting setting =
			shadow_caster_layers_ != 0 ? GeometryInstance3D::SHADOW_CASTING_SETTING_ON
									   : GeometryInstance3D::SHADOW_CASTING_SETTING_OFF;
	for (int i = 0; i < p_root->get_child_count(); ++i) {
		Node *child = p_root->get_child(i);
		GeometryInstance3D *geometry = Object::cast_to<GeometryInstance3D>(child);
		if (geometry != nullptr) {
			geometry->set_cast_shadows_setting(setting);
			geometry->set_layer_mask((geometry->get_layer_mask() &
											 ~uint32_t(LAYER_SHADOW_CASTER_MASK)) |
					shadow_caster_layers_);
		}
		apply_shadow_casting_below(child);
	}
}

void ObjectModel::set_environment_state(const Ref<EnvLightState> &p_state) {
	// The typed env channel: the environment PUBLISHES into this shared state
	// (values + generation + a changed signal); the model never holds the
	// environment object itself.
	const Callable changed = callable_mp(this, &ObjectModel::on_env_generation_changed);
	if (env_state_.is_valid() && env_state_->is_connected("changed", changed)) {
		env_state_->disconnect("changed", changed);
	}
	env_state_ = p_state;
	last_env_gen_ = -1;
	last_env_values_.unref();
	last_section_env_values_.unref();
	if (env_state_.is_valid()) {
		env_state_->connect("changed", changed);
	}
	wake_runtime_frame();
	apply_environment_to_materials();
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
	last_env_gen_ = -1;
	last_env_values_.unref();
	last_section_env_values_.unref();
	apply_environment_to_materials();
}

void ObjectModel::set_interior_section_light_transfer(float p_daylight) {
	const float next_daylight = CLAMP(p_daylight, 0.0f, 1.0f);
	if (interior_section_lighting_ &&
			Math::is_equal_approx(interior_section_daylight_, next_daylight)) {
		return;
	}
	interior_section_lighting_ = true;
	interior_section_daylight_ = next_daylight;
	last_env_gen_ = -1;
	last_env_values_.unref();
	last_section_env_values_.unref();
	// The exterior/interior split is part of the material-cache key. Owners
	// normally configure it before set_object_data(), but preserve correctness
	// for a live reconfiguration too.
	if (object_data_.is_valid() && object_data_->has_document()) {
		rebuild();
	} else {
		apply_environment_to_materials();
	}
}

int ObjectModel::lighting_context_for_robj(int p_robj_index) const {
	if (interior_section_lighting_ && p_robj_index != 0) {
		return LIGHTING_CONTEXT_INTERIOR_SECTION;
	}
	return LIGHTING_CONTEXT_ENTITY;
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
	for (const KeyValue<int, Node3D *> &kv : robj_nodes_) {
		if (kv.value != nullptr) {
			kv.value->set_visible(p_mask == -1 || ((p_mask >> kv.key) & 1) == 1);
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

Dictionary ObjectModel::get_material_defs() const {
	Dictionary result;
	for (const KeyValue<int64_t, Dictionary> &kv : material_defs_) {
		result[kv.key] = kv.value;
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

void ObjectModel::reset_animation_time() {
	wake_runtime_frame();
	anim_time_ms_ = 0;
	anim_time_ = 0.0;
	anim_external_phase_ = false;
	body_phase_stamp_valid_ = false;
	reset_remote_body_state();
	apply_runtime_state(0.0);
}

void ObjectModel::set_active_lod(int p_lod_index) {
	const int next_lod = clamp_lod_index(p_lod_index);
	if (active_lod_ == next_lod) {
		return;
	}
	active_lod_ = next_lod;
	rebuild();
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

void ObjectModel::finish_ctrl_change(bool p_apply_now) {
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
	if (!is_part_local_ctrl(reg)) {
		for (ObjectModel *linked : live_presentation_links()) {
			linked->set_ctrl_value(reg, p_value);
		}
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
	if (!is_part_local_ctrl(reg)) {
		for (ObjectModel *linked : live_presentation_links()) {
			linked->clear_ctrl_value(reg);
		}
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
	if (!is_part_local_ctrl(reg)) {
		for (ObjectModel *linked : live_presentation_links()) {
			linked->set_ctrl_override(p_owner, reg, p_value);
		}
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
	if (!is_part_local_ctrl(reg)) {
		for (ObjectModel *linked : live_presentation_links()) {
			linked->clear_ctrl_override(p_owner, reg);
		}
	}
	const String *current_owner = ctrl_value_owners_.getptr(reg);
	if (current_owner == nullptr || *current_owner != p_owner) {
		return;
	}
	ctrl_value_owners_.erase(reg);
	ctrl_values_.erase(reg);
	finish_ctrl_change(true);
}

void ObjectModel::clear_ctrl_values() {
	if (ctrl_values_.is_empty() && ctrl_value_owners_.is_empty()) {
		return;
	}
	ctrl_values_.clear();
	ctrl_value_owners_.clear();
	finish_ctrl_change(true);
}

Dictionary ObjectModel::get_ctrl_values() const {
	return ctrl_values_.duplicate(true);
}

void ObjectModel::rebuild() {
	wake_runtime_frame();
	rebuild_scene();
}

// Blended materials take their water-side transparency rung from the witnessed
// frame ladder (REN-3): below-water alpha draws before the water surface,
// above-water after [orig: the Q1/Q2 split @ 0x5d932e..0x5d9354 + the flush
// bracket @ 0x5c9596 / @ 0x5c967a]. Retail bins per STRIP; we bin per MODEL
// from its placed height (D-RORD-3).
void ObjectModel::refresh_render_order() {
	if (alpha_materials_.is_empty() || !is_inside_tree()) {
		return;
	}
	ObjectShaderCache *shader_cache = ObjectShaderCache::get_singleton();
	const int32_t rung = shader_cache->alpha_rung_for_height(
			static_cast<float>(get_global_position().y));
	for (const Ref<ShaderMaterial> &material : alpha_materials_) {
		if (material.is_valid()) {
			material->set_render_priority(rung);
		}
	}
}

void ObjectModel::on_object_changed() {
	wake_runtime_frame();
	const int64_t update_mask = last_object_update_mask();
	const int64_t panm_lght = ObjectData::UPDATE_PANM | ObjectData::UPDATE_LGHT;
	if (update_mask == ObjectData::UPDATE_PANM ||
			update_mask == ObjectData::UPDATE_LGHT ||
			update_mask == panm_lght) {
		// A coalesced deferred-flush batch could read LGHT/PANM even when a
		// generator style also changed; reclassify (cheap, idempotent) so the
		// dynamic-material set can never go stale relative to the current data.
		classify_materials();
		refresh_live_panm_classification();
		bounds_dirty_ = true;
		apply_runtime_state(0.0);
		return;
	}
	rebuild();
}

int64_t ObjectModel::last_object_update_mask() const {
	if (object_data_.is_valid()) {
		return int64_t(object_data_->get_last_oed_update_mask()) & ObjectData::UPDATE_ALL;
	}
	return ObjectData::UPDATE_ALL;
}

// The shared awake set: every model with live per-frame work. One driver
// (ObjectModel::advance_awake_frame) walks it per render frame — the game from
// GameFramePipeline's render_material_frame leg, the menu shell and ONED from
// their one process loop. There is no per-node _process, so nothing self-clocks
// off Godot's frame outside that one driver.
HashSet<ObjectModel *> ObjectModel::awake_models_;

void ObjectModel::advance_awake_frame(double p_delta) {
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
			model->advance_runtime_frame(p_delta);
		}
	}
}

int64_t ObjectModel::awake_model_count() {
	return static_cast<int64_t>(awake_models_.size());
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
	// The private preview clock accumulates wall time per frame while playing
	// (OED preview owners; mission/wire models ride the shared PANM clock).
	if (panm_clock_.is_null() && is_playing_) {
		return;
	}
	if (awake_) {
		awake_ = false;
		awake_models_.erase(this);
	}
}

void ObjectModel::on_env_generation_changed() {
	// One restamp per stagger window per parked model; global lighting moves
	// well under 1/255 per frame at mission TOD rates, so the stagger is
	// invisible (see the GDScript origin's derivation).
	if ((Engine::get_singleton()->get_process_frames() + env_stagger_slot_) %
					kEnvRestampSpreadFrames !=
			0) {
		return;
	}
	wake_runtime_frame();
}

void ObjectModel::_notification(int p_what) {
	if (p_what == NOTIFICATION_READY) {
		wake_runtime_frame();
		if (object_data_.is_valid()) {
			rebuild();
		}
	} else if (p_what == NOTIFICATION_VISIBILITY_CHANGED) {
		// Becoming visible must re-check the env generation missed while hidden.
		wake_runtime_frame();
	} else if (p_what == NOTIFICATION_PREDELETE) {
		// A freed model must not leave a stale off-screen claim in the shared
		// submission registry (the walk would keep gating a reused id).
		if (submission_registry_bound_) {
			submission_registry_.erase(get_instance_id());
		}
		if (awake_) {
			awake_ = false;
			awake_models_.erase(this);
		}
	}
}

// Advance the model's render-time state once. Public for deterministic owners
// and tests: clocks continue while hidden; render-derived work waits until the
// model can be submitted again.
void ObjectModel::advance_runtime_frame(double p_delta) {
	if (object_data_.is_null() || !object_data_->has_document()) {
		if (awake_) {
			awake_ = false;
			awake_models_.erase(this);
		}
		return;
	}
	const bool renderable = is_visible_in_tree() && on_screen_;
	if (!needs_runtime_frame_work()) {
		// Keep the private preview clock continuous even while the model has no
		// time-driven consumer.
		if (panm_clock_.is_null() && is_playing_) {
			anim_time_ms_ = (anim_time_ms_ + static_cast<int64_t>(p_delta * 1000.0)) &
					0xffffffff;
		}
		if (renderable) {
			apply_environment_to_materials();
		}
		sleep_runtime_frame_if_idle();
		return;
	}
	apply_runtime_state(p_delta, renderable);
	sleep_runtime_frame_if_idle();
}

bool ObjectModel::needs_runtime_frame_work() const {
	if (bounds_dirty_ || has_live_panm_ || !dynamic_material_slots_.is_empty() ||
			(model_light_preview_enabled_ && has_lights_) || !part_anims_.is_empty()) {
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

void ObjectModel::apply_runtime_state(double p_delta, bool p_renderable) {
	if (object_data_.is_null() || !object_data_->has_document()) {
		return;
	}
	if (panm_clock_.is_valid()) {
		anim_time_ms_ = panm_clock_->get_time_ms();
	} else if (is_playing_) {
		anim_time_ms_ = (anim_time_ms_ + static_cast<int64_t>(p_delta * 1000.0)) &
				0xffffffff;
	}
	const bool part_changed = advance_part_anims(p_delta);
	advance_body_animation(p_delta, p_renderable);
	if (!p_renderable) {
		// Everything below derives from the absolute clock + the register/pose
		// state advanced above; it re-derives on the next visible frame.
		return;
	}
	// Retail poses PANM during entity submission before the later render-batch
	// flush evaluates material generators — noise waveforms share one random
	// stream. [orig: Render_SubmitEntity @0x5DAD80 -> Model_TransformBoneMatrices
	//  @0x58E390; CRenderBatchQueue_SortAndFlush @0x5DAE40 ->
	//  apply_shader_parameters @0x58DB80]
	bool robj_changed = false;
	if (has_live_panm_ || bounds_dirty_) {
		robj_changed = apply_robj_transforms();
	}
	// Only dynamic materials need a per-frame push; static slots keep the
	// identity values written at material creation.
	for (int64_t s = 0; s < dynamic_material_slots_.size(); ++s) {
		const int i = dynamic_material_slots_[s];
		const Ref<ShaderMaterial> material = surface_materials_[i];
		if (material.is_null()) {
			continue;
		}
		const int material_index = surface_material_indices_[i];
		if (material_needs_eval_[i]) {
			const Dictionary runtime = object_data_->eval_material_runtime(
					material_index, anim_time_ms_, ctrl_values_);
			if (!runtime.is_empty()) {
				material->set_shader_parameter("u_uv_transform_u",
						runtime.get("uv_transform_u", Vector3(1.0f, 0.0f, 0.0f)));
				material->set_shader_parameter("u_uv_transform_v",
						runtime.get("uv_transform_v", Vector3(0.0f, 1.0f, 0.0f)));
				material->set_shader_parameter("u_rgb_mod",
						runtime.get("rgb_mod", Vector3(1.0f, 1.0f, 1.0f)));
				material->set_shader_parameter("u_alpha_mod",
						runtime.get("alpha_mod", 1.0f));
			}
		}
		const Array *frames = anim_frames_by_mat_.getptr(material_index);
		if (frames != nullptr && frames->size() > 1) {
			const int frame_index = object_data_->compute_anim_frame(
					material_index, anim_time_ms_, ctrl_values_);
			if (frame_index >= 0 && frame_index < frames->size()) {
				const Ref<Texture2D> frame = (*frames)[frame_index];
				if (frame.is_valid()) {
					material->set_shader_parameter("u_diffuse", frame);
				}
			}
		}
	}
	apply_lights();
	apply_environment_to_materials();
	if (bounds_dirty_ || part_changed || robj_changed) {
		set_model_bounds(compute_transformed_mesh_bounds());
		bounds_dirty_ = false;
	}
}

bool ObjectModel::apply_robj_transforms() {
	if (object_data_.is_null() || robj_nodes_.is_empty()) {
		return false;
	}
	// One native call evaluates PANM at most once per graphic per frame (the
	// placer shares one ObjectData across every instance of a graphic) and
	// writes only the parts whose transforms changed since this model applied.
	const int64_t revision = object_data_->apply_panm_to_nodes(
			active_lod_, anim_time_ms_, ctrl_values_, robj_dense_,
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
	publish_submission_state();
	if (p_value) {
		wake_runtime_frame();
	}
}

// Bind the present walk's shared camera-submission registry (owned by
// PresentApplier, shared BY REFERENCE): this model's instance id is
// present exactly while its bounds notifier reports off-screen.
// [orig: Terrain_RenderSectorModels @ 0x5c5d30]
void ObjectModel::set_submission_registry(const Dictionary &p_registry) {
	if (submission_registry_bound_ && p_registry == submission_registry_) {
		return;
	}
	if (submission_registry_bound_) {
		submission_registry_.erase(get_instance_id());
	}
	submission_registry_ = p_registry;
	submission_registry_bound_ = true;
	publish_submission_state();
}

void ObjectModel::publish_submission_state() {
	if (!submission_registry_bound_) {
		return;
	}
	if (on_screen_) {
		submission_registry_.erase(get_instance_id());
	} else {
		submission_registry_[get_instance_id()] = true;
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

void ObjectModel::_bind_methods() {
	ClassDB::bind_static_method("ObjectModel",
			D_METHOD("advance_awake_frame", "delta"),
			&ObjectModel::advance_awake_frame);
	ClassDB::bind_static_method("ObjectModel",
			D_METHOD("awake_model_count"), &ObjectModel::awake_model_count);
	ClassDB::bind_method(D_METHOD("is_runtime_frame_awake"),
			&ObjectModel::is_runtime_frame_awake);
	ClassDB::bind_method(D_METHOD("wake_runtime_frame"),
			&ObjectModel::wake_runtime_frame);
	ClassDB::bind_method(D_METHOD("set_object_data", "data"), &ObjectModel::set_object_data);
	ClassDB::bind_method(D_METHOD("get_object_data"), &ObjectModel::get_object_data);
	ClassDB::bind_method(D_METHOD("add_presentation_link", "model"),
			&ObjectModel::add_presentation_link);
	ClassDB::bind_method(D_METHOD("get_presentation_link_count"),
			&ObjectModel::get_presentation_link_count);
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "object_data",
						 PROPERTY_HINT_RESOURCE_TYPE, "ObjectData"),
			"set_object_data", "get_object_data");
	ClassDB::bind_method(D_METHOD("set_mirror_reflected", "reflected"),
			&ObjectModel::set_mirror_reflected);
	ClassDB::bind_method(D_METHOD("get_mirror_reflected"),
			&ObjectModel::get_mirror_reflected);
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "mirror_reflected"),
			"set_mirror_reflected", "get_mirror_reflected");
	ClassDB::bind_method(D_METHOD("set_native_frame", "native"),
			&ObjectModel::set_native_frame);
	ClassDB::bind_method(D_METHOD("get_native_frame"),
			&ObjectModel::get_native_frame);
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "native_frame"),
			"set_native_frame", "get_native_frame");
	ClassDB::bind_method(D_METHOD("set_model_light_preview_enabled", "enabled"),
			&ObjectModel::set_model_light_preview_enabled);
	ClassDB::bind_method(D_METHOD("set_shadow_caster_enabled", "enabled"),
			&ObjectModel::set_shadow_caster_enabled);
	ClassDB::bind_method(D_METHOD("is_shadow_caster_enabled"),
			&ObjectModel::is_shadow_caster_enabled);
	ClassDB::bind_method(D_METHOD("set_static_shadow_caster_enabled", "enabled"),
			&ObjectModel::set_static_shadow_caster_enabled);
	ClassDB::bind_method(D_METHOD("is_static_shadow_caster_enabled"),
			&ObjectModel::is_static_shadow_caster_enabled);
	ClassDB::bind_method(D_METHOD("set_environment_state", "state"),
			&ObjectModel::set_environment_state);
	ClassDB::bind_method(D_METHOD("get_environment_state"),
			&ObjectModel::get_environment_state);
	ClassDB::bind_method(
			D_METHOD("set_entity_lighting_context", "effect_scale", "interior_lerp",
					"interior_daylight"),
			&ObjectModel::set_entity_lighting_context);
	ClassDB::bind_method(D_METHOD("set_interior_section_light_transfer", "daylight"),
			&ObjectModel::set_interior_section_light_transfer);
	ClassDB::bind_method(D_METHOD("get_model_bounds"), &ObjectModel::get_model_bounds);
	ClassDB::bind_method(D_METHOD("get_render_part_nodes"),
			&ObjectModel::get_render_part_nodes);
	ClassDB::bind_method(D_METHOD("set_section_visibility_mask", "mask"),
			&ObjectModel::set_section_visibility_mask);
	ClassDB::bind_method(D_METHOD("get_surface_material_indices"),
			&ObjectModel::get_surface_material_indices);
	ClassDB::bind_method(D_METHOD("get_surface_materials"),
			&ObjectModel::get_surface_materials);
	ClassDB::bind_method(D_METHOD("get_material_defs"), &ObjectModel::get_material_defs);
	ClassDB::bind_method(D_METHOD("is_playing"), &ObjectModel::is_playing);
	ClassDB::bind_method(D_METHOD("set_playing", "value"), &ObjectModel::set_playing);
	ClassDB::bind_method(D_METHOD("set_panm_clock", "clock"), &ObjectModel::set_panm_clock);
	ClassDB::bind_method(D_METHOD("get_panm_clock"), &ObjectModel::get_panm_clock);
	ClassDB::bind_method(D_METHOD("reset_animation_time"),
			&ObjectModel::reset_animation_time);
	ClassDB::bind_method(D_METHOD("set_active_lod", "lod_index"),
			&ObjectModel::set_active_lod);
	ClassDB::bind_method(D_METHOD("get_active_lod"), &ObjectModel::get_active_lod);
	ClassDB::bind_method(D_METHOD("rebuild"), &ObjectModel::rebuild);
	ClassDB::bind_method(D_METHOD("refresh_render_order"),
			&ObjectModel::refresh_render_order);
	ClassDB::bind_method(D_METHOD("advance_runtime_frame", "delta"),
			&ObjectModel::advance_runtime_frame);
	ClassDB::bind_method(D_METHOD("set_on_screen", "value"), &ObjectModel::set_on_screen);
	ClassDB::bind_method(D_METHOD("set_submission_registry", "registry"),
			&ObjectModel::set_submission_registry);

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
	ClassDB::bind_method(D_METHOD("clear_ctrl_values"), &ObjectModel::clear_ctrl_values);
	ClassDB::bind_method(D_METHOD("get_ctrl_values"), &ObjectModel::get_ctrl_values);

	ClassDB::bind_method(D_METHOD("set_skeletal_anim", "skeletal"),
			&ObjectModel::set_skeletal_anim);
	ClassDB::bind_method(D_METHOD("get_skeletal_anim"), &ObjectModel::get_skeletal_anim);
	ClassDB::bind_method(D_METHOD("get_skeleton"), &ObjectModel::get_skeleton);
	ClassDB::bind_method(D_METHOD("has_skeleton"), &ObjectModel::has_skeleton);
	ClassDB::bind_method(D_METHOD("has_muzzle"), &ObjectModel::has_muzzle);
	ClassDB::bind_method(D_METHOD("set_muzzle_point_name", "name"),
			&ObjectModel::set_muzzle_point_name);
	ClassDB::bind_method(D_METHOD("get_muzzle_point_name"),
			&ObjectModel::get_muzzle_point_name);
	ClassDB::bind_method(D_METHOD("get_muzzle_world_position"),
			&ObjectModel::get_muzzle_world_position);
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
	ClassDB::bind_method(D_METHOD("set_weapon_channel", "key", "phase_ticks"),
			&ObjectModel::set_weapon_channel);
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

	ClassDB::bind_static_method("ObjectModel",
			D_METHOD("entity_lighting_values", "world_values", "effect_scale",
					"interior_lerp", "interior_daylight"),
			&ObjectModel::entity_lighting_values);
	ClassDB::bind_static_method("ObjectModel",
			D_METHOD("apply_environment_values", "material", "values"),
			&ObjectModel::apply_environment_values);
	ClassDB::bind_static_method("ObjectModel",
			D_METHOD("material_supports_projected_shadow_receiver", "blend_mode",
					"material_flags"),
			&ObjectModel::material_supports_projected_shadow_receiver);

	ADD_SIGNAL(MethodInfo("bounds_changed", PropertyInfo(Variant::AABB, "bounds")));

	BIND_ENUM_CONSTANT(LIGHTING_CONTEXT_ENTITY);
	BIND_ENUM_CONSTANT(LIGHTING_CONTEXT_INTERIOR_SECTION);
}

} // namespace godot
