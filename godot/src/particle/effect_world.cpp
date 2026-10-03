#include "particle/effect_world.h"

#include "particle/particle_convert.h"
#include "particle/particle_effect.h"
#include "particle/particle_renderer.h"
#include "resource_index/resource_root.h"
#include "simulation/effect_section_source.h"
#include "simulation/simulation.h"
#include "util/axes.h"
#include "util/string_convert.h"

#include <base/io/fixed.h>

#include <array>
#include <vector>

#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/core/math.hpp>
#include <godot_cpp/core/object.hpp>
#include <godot_cpp/templates/hash_set.hpp>
#include <godot_cpp/variant/basis.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <runtime/particle/effect_scene.h>
#include <runtime/renderer/particle_frame.h>

using namespace godot;
using opennova::to_gd;

EffectWorld::EffectWorld() {
	scene_.instantiate();
	load_report_ = scene_->open(TypedArray<ParticleFile>());
    scene_->shared_native_scene()->set_spawn_enabled(!particles_disabled_);
}

void EffectWorld::_notification(int p_what) {
	if (p_what == NOTIFICATION_READY) {
		_ensure_renderer();
		set_process(false);
	}
}

ParticleRenderer *EffectWorld::_renderer() const {
	return Object::cast_to<ParticleRenderer>(ObjectDB::get_instance(renderer_id_));
}

Node *EffectWorld::_environment_source() const {
	return Object::cast_to<Node>(ObjectDB::get_instance(environment_source_id_));
}

Camera3D *EffectWorld::_reflection_camera() const {
	return Object::cast_to<Camera3D>(ObjectDB::get_instance(reflection_camera_id_));
}

ParticleRenderer *EffectWorld::_ensure_renderer() {
	ParticleRenderer *renderer = _renderer();
	if (renderer != nullptr) {
		return renderer;
	}
	renderer = memnew(ParticleRenderer);
	renderer->set_name("ParticleRenderer");
	add_child(renderer);
	renderer_id_ = renderer->get_instance_id();
	renderer->set_scene(scene_);
	renderer->set_texture_provider(texture_provider_);
	renderer->set_texture_dir(texture_dir_);
	renderer->set_environment_source(_environment_source());
	renderer->set_water_plane(water_height_, _reflection_camera());
	renderer->set_second_scene_camera(get_second_scene_camera());
	renderer->set_hidden(particles_disabled_);
	return renderer;
}

void EffectWorld::release_runtime_renderer_resources() {
	ParticleRenderer *renderer = _renderer();
	if (renderer != nullptr) {
		renderer->shutdown();
	}
}

void EffectWorld::set_environment_source(Node *p_source) {
	environment_source_id_ = p_source != nullptr ? ObjectID(p_source->get_instance_id()) : ObjectID();
	_ensure_renderer()->set_environment_source(p_source);
}

int EffectWorld::file_count() const {
	return static_cast<int>(files_.size());
}

TypedArray<ParticleFile> EffectWorld::get_files() const {
	TypedArray<ParticleFile> out;
	for (int64_t i = 0; i < files_.size(); ++i) {
		out.push_back(files_[i]);
	}
	return out;
}

int EffectWorld::effect_count() const {
	return load_report_.is_valid() ? load_report_->get_effect_count() : 0;
}

int EffectWorld::live_group_count() const {
	return static_cast<int>(scene_->native_scene().live_counts().group_count);
}

int EffectWorld::active_entry_count() const {
	return particles_disabled_ ? 0 : live_group_count();
}

int EffectWorld::interned_count() const {
	return static_cast<int>(scene_->native_scene().inspect(false).interned_effect_count);
}

void EffectWorld::set_particles_hidden(bool p_hidden) {
	particles_disabled_ = p_hidden;
    scene_->shared_native_scene()->set_spawn_enabled(!p_hidden);
	set_visible(!p_hidden);
	_ensure_renderer()->set_hidden(p_hidden);
}

bool EffectWorld::are_particles_hidden() const {
	return particles_disabled_;
}

Callable EffectWorld::get_texture_provider() const {
	return texture_provider_;
}

void EffectWorld::set_owner_position_provider(const Callable &p_provider) {
	owner_position_provider_ = p_provider;
}

void EffectWorld::set_water_plane(float p_value, Camera3D *p_reflection_camera) {
	water_height_ = p_value;
	reflection_camera_id_ = p_reflection_camera != nullptr
			? ObjectID(p_reflection_camera->get_instance_id())
			: ObjectID();
	_ensure_renderer()->set_water_plane(p_value, p_reflection_camera);
}

void EffectWorld::set_second_scene_camera(Camera3D *p_camera) {
	const ObjectID next = p_camera != nullptr
			? ObjectID(p_camera->get_instance_id())
			: ObjectID();
	if (second_scene_camera_id_ == next) {
		return; // the per-frame hand-in of an unchanged view costs nothing
	}
	second_scene_camera_id_ = next;
	_ensure_renderer()->set_second_scene_camera(p_camera);
}

Camera3D *EffectWorld::get_second_scene_camera() const {
	return Object::cast_to<Camera3D>(ObjectDB::get_instance(second_scene_camera_id_));
}

int EffectWorld::load_from_resource_root(const Ref<ResourceRoot> &p_root) {
	clear_world();
	if (p_root.is_null()) {
		return 0;
	}
	root_ = p_root;
	// A particle graphic loads through the particle manager's reader: the loose
	// tga folder first, then the mounted name (renderer::TextureLoader::Particle).
	texture_provider_ = Callable(p_root.ptr(), "load_texture").bind(ResourceRoot::TEXTURE_LOADER_PARTICLE);
	_ensure_renderer()->set_texture_provider(texture_provider_);
	Array entries = p_root->list_file_entries(".ptl");
	entries.append_array(p_root->list_file_entries(p_root->particle_extension()));
	for (int64_t i = 0; i < entries.size(); ++i) {
		const Dictionary entry = entries[i];
		const String logical_name = entry.get("logical_name", "");
		if (logical_name.is_empty()) {
			continue;
		}
		const PackedByteArray bytes = p_root->read_file(logical_name);
		if (bytes.is_empty()) {
			continue;
		}
		Ref<ParticleFile> file;
		file.instantiate();
		if (file->load_from_buffer(bytes, logical_name) != OK) {
			UtilityFunctions::push_warning(vformat("effect world: failed to parse %s", logical_name));
			continue;
		}
		files_.push_back(file);
	}
	_open_files();
	return effect_count();
}

void EffectWorld::load_particle_file(const Ref<ParticleFile> &p_file) {
	if (p_file.is_null()) {
		return;
	}
	files_.push_back(p_file);
	if (root_.is_null() && texture_dir_.is_empty()) {
		texture_dir_ = p_file->get_source_path().get_base_dir();
	}
	_open_files();
}

void EffectWorld::_open_files() {
	scene_.instantiate();
	load_report_ = scene_->open(files_);
    scene_->shared_native_scene()->set_spawn_enabled(!particles_disabled_);
	ParticleRenderer *renderer = _ensure_renderer();
	renderer->set_scene(scene_);
	renderer->set_texture_provider(texture_provider_);
	renderer->set_texture_dir(texture_dir_);
}

void EffectWorld::clear_world() {
	files_.clear();
	slot_tokens_.clear();
	owner_tokens_.clear();
	owner_keys_by_token_.clear();
	owner_pose_cache_.clear();
	next_token_ = 1;
	root_.unref();
	texture_provider_ = Callable();
	texture_dir_ = String();
	owner_position_provider_ = Callable();
	water_height_ = 0.0f;
	reflection_camera_id_ = ObjectID();
	second_scene_camera_id_ = ObjectID();
	if (ParticleRenderer *renderer = _renderer()) {
		renderer->set_water_plane(0.0f, nullptr);
		renderer->set_second_scene_camera(nullptr);
	}
	scene_.instantiate();
	load_report_ = scene_->open(TypedArray<ParticleFile>());
    scene_->shared_native_scene()->set_spawn_enabled(!particles_disabled_);
	ParticleRenderer *renderer = _ensure_renderer();
	renderer->set_scene(scene_);
	renderer->set_texture_provider(texture_provider_);
	renderer->set_texture_dir(String());
}

void EffectWorld::reset_runtime_state() {
	slot_tokens_.clear();
	owner_tokens_.clear();
	owner_keys_by_token_.clear();
	owner_pose_cache_.clear();
	next_token_ = 1;
	if (ParticleRenderer *renderer = _renderer()) {
		renderer->clear_warm_pipelines();
	}
	scene_->reset_runtime_state();
}

int64_t EffectWorld::intern_effect(const String &p_name) {
	return scene_->intern(p_name);
}

String EffectWorld::effect_name_for_handle(int64_t p_handle) const {
	return scene_->effect_name(p_handle);
}

int64_t EffectWorld::_allocate_token() {
	const int64_t token = next_token_;
	++next_token_;
	return token;
}

int64_t EffectWorld::_slot_token_for(const Variant &p_key) {
	if (const int64_t *existing = slot_tokens_.getptr(p_key)) {
		return *existing;
	}
	const int64_t token = _allocate_token();
	slot_tokens_.insert(p_key, token);
	return token;
}

int64_t EffectWorld::_owner_token_for(const Variant &p_key) {
	if (const int64_t *existing = owner_tokens_.getptr(p_key)) {
		return *existing;
	}
	const int64_t token = _allocate_token();
	owner_tokens_.insert(p_key, token);
	owner_keys_by_token_.insert(token, p_key);
	return token;
}

// A descriptor spawn (transient or owned) with a zero orientation is retail's
// "no orientation" case: CEffectWorld_SpawnEmitterAtPosition @ 0x5f6e52..0x5f6e5c
// hands the group a zero vector, CEffectEmitter_SetOrientationFromDirection
// @ 0x5e5d51 leaves every EMITVECTOR member's emission axis zero, and the
// direction helper then emits around world +Y. A pose always carries a basis,
// so that case aims the forward at +Y, which the engine's cone helper resolves
// to the identical world-axis frame. Attached spawns keep their identity local
// frame: their orientation comes from the owner transform they compose with.
Transform3D EffectWorld::descriptor_pose(const Vector3 &p_position, const Vector3 &p_orientation) {
	if (p_orientation.length_squared() <= 0.000001f) {
		return forward_pose(p_position, Vector3(0.0f, 1.0f, 0.0f));
	}
	return forward_pose(p_position, p_orientation);
}

Transform3D EffectWorld::forward_pose(const Vector3 &p_position, const Vector3 &p_forward) {
	if (p_forward.length_squared() <= 0.000001f) {
		return Transform3D(Basis(), p_position);
	}
	const Vector3 forward = p_forward.normalized();
	Vector3 up_hint = Vector3(0, 1, 0);
	if (Math::abs(forward.dot(up_hint)) > 0.999f) {
		up_hint = Vector3(1, 0, 0);
	}
	const Vector3 right = up_hint.cross(forward).normalized();
	const Vector3 up = forward.cross(right).normalized();
	return Transform3D(Basis(right, up, forward), p_position);
}

void EffectWorld::_seed_owner_pose(int64_t p_owner_token, const Transform3D &p_transform) {
	owner_pose_cache_.insert(p_owner_token, p_transform);
	owner_pose_batch_.clear();
	owner_pose_batch_.add(p_owner_token, p_transform);
	scene_->apply_owner_poses(owner_pose_batch_);
}

Ref<EffectSpawnReceipt> EffectWorld::_disabled_receipt() const {
	Ref<EffectSpawnReceipt> receipt;
	receipt.instantiate();
	receipt->set_status_name("particles_disabled");
	return receipt;
}

Ref<EffectSpawnReceipt> EffectWorld::spawn_effect_request(const String &p_name,
		const Transform3D &p_transform, const Ref<EffectSpawnOptions> &p_options) {
	if (particles_disabled_) {
		return _disabled_receipt();
	}
	Ref<EffectSpawnOptions> options = p_options;
	if (options.is_null()) {
		options.instantiate();
	}
	const int64_t handle = intern_effect(p_name);
	const int admission = options->get_admission();
	int64_t slot_token = 0;
	int64_t owner_token = 0;
	if (options->get_slot_key().get_type() != Variant::NIL) {
		slot_token = _slot_token_for(options->get_slot_key());
	}
	if (options->get_owner_key().get_type() != Variant::NIL) {
		owner_token = _owner_token_for(options->get_owner_key());
	}
	Transform3D owner_transform;
	bool seed_owner_after_spawn = false;
	if (owner_token > 0 && options->get_has_owner_transform()) {
		owner_transform = options->get_owner_transform();
		// ReplaceOwned must detach its predecessor at that predecessor's
		// last pose. Seeding the new pose first would move both groups
		// before the portable scene gets a chance to detach the old one.
		seed_owner_after_spawn = admission == ADMISSION_REPLACE_OWNED;
		if (!seed_owner_after_spawn) {
			_seed_owner_pose(owner_token, owner_transform);
		}
	}
	Ref<EffectSpawnRequest> request = EffectSpawnRequest::make(handle, p_transform);
	request->set_admission(admission);
	request->set_binding(options->get_binding());
	request->set_render_domain(options->get_render_domain());
	request->set_slot_token(slot_token);
	request->set_owner_token(owner_token);
	request->set_owner_relative_transform(options->get_owner_relative_transform());
	request->set_initial_age_ticks(options->get_initial_age_ticks());
	request->set_force_zone(options->get_force_zone());
	request->set_source_tick(options->get_source_tick());
	request->set_source_order(options->get_source_order());
	request->set_kill_plane(KILL_PLANE_DISABLED);
	request->set_kill_plane_y(water_height_);
	if (options->get_section_tagged()) {
		_stamp_section_gate(request, p_transform.origin);
	}
	Ref<EffectSpawnReceipt> receipt = scene_->spawn(request);
	if (seed_owner_after_spawn && receipt.is_valid() && receipt->get_spawned()) {
		_seed_owner_pose(owner_token, owner_transform);
	}
	return receipt;
}

void EffectWorld::spawn_script_effect(const opennova::world::ScriptEffectEvent &event, uint32_t age_ticks) {
    if (particles_disabled_) return;
    const String key = vformat("script_entity:%d", int(event.owner.packed));
    const int64_t slot = event.store_slot ? _slot_token_for(key) : 0;
    const int64_t owner = event.owner.valid() ? _owner_token_for(key) : 0;
    // Every script handler tags its descriptor with its entity
    // (particle::spawn_script_effect); the blink query runs at the
    // descriptor's mission position.
    opennova::particle::EffectSectionGate gate;
    gate.tagged = true;
    if (Simulation *source = _section_source()) {
        const std::array<float, 3> mission_position = {
                opennova::io::fp16_16_to_float(event.position[0]),
                opennova::io::fp16_16_to_float(event.position[1]),
                opennova::io::fp16_16_to_float(event.position[2])};
        EffectSectionSource(source).blink_hits(mission_to_godot(mission_position),
                gate.blink_hits);
    }
    scene_->spawn_script_effect(event, slot, owner, age_ticks, water_height_, gate);
}

Simulation *EffectWorld::_section_source() const {
	return section_source_id_.is_valid()
			? Object::cast_to<Simulation>(ObjectDB::get_instance(section_source_id_))
			: nullptr;
}

void EffectWorld::set_section_source(Simulation *p_source) {
	section_source_id_ = p_source != nullptr ? p_source->get_instance_id() : ObjectID();
}

void EffectWorld::_stamp_section_gate(const Ref<EffectSpawnRequest> &p_request,
		const Vector3 &p_position) const {
	p_request->set_section_tagged(true);
	Simulation *source = _section_source();
	if (source == nullptr) {
		return;
	}
	std::array<uint32_t, 4> hits{};
	EffectSectionSource(source).blink_hits(p_position, hits);
	PackedInt64Array packed;
	for (const uint32_t hit : hits) {
		packed.push_back(static_cast<int64_t>(hit));
	}
	p_request->set_blink_hits(packed);
}

int EffectWorld::warm_all_effects(const Vector3 &p_position) {
	// Deterministic half first: one quad per FirstPerson blend shader plus a
	// compositor request for all eight World RD pipelines, independent of
	// emitter timing (delayed emitters emit nothing during the warm frames).
	ParticleRenderer *renderer = _ensure_renderer();
	renderer->warm_pipelines(p_position);
	HashSet<String> seen;
	int spawned = 0;
	for (int64_t i = 0; i < files_.size(); ++i) {
		const Ref<ParticleFile> file = files_[i];
		if (file.is_null()) {
			continue;
		}
		for (const auto &effect : file->native_file().effects) {
			const String effect_id(effect.id.c_str());
			if (effect_id.is_empty() || seen.has(effect_id)) {
				continue;
			}
			seen.insert(effect_id);
			// Catalog values warm through the uncapped World draw list. The
			// deterministic helpers above already compile all eight
			// FirstPerson shaders; cloning a large catalog into that
			// ArrayMesh domain can exceed Godot's per-mesh surface limit.
			if (spawn_effect_transient(effect_id, p_position) != 0) {
				++spawned;
			}
		}
	}
	// GameWorld skips its draw/reset leg when there was nothing to warm. Do
	// not strand the deterministic shader helper quads in that
	// empty-catalog path.
	if (spawned == 0) {
		renderer->clear_warm_pipelines();
	}
	return spawned;
}

int64_t EffectWorld::render_now(int64_t p_time_ms) {
	ParticleRenderer *renderer = _ensure_renderer();
	renderer->render_now(p_time_ms);
	return renderer->get_draw_command_count();
}

Dictionary EffectWorld::get_debug_draw_list_report() {
	return _ensure_renderer()->get_debug_draw_list_report();
}

int64_t EffectWorld::spawn_effect_transient(const String &p_name, const Vector3 &p_position,
		const Vector3 &p_orientation, int p_initial_age_ticks, int p_render_domain,
		int64_t p_source_tick, int64_t p_source_order, bool p_section_tagged) {
	Ref<EffectSpawnOptions> options;
	options.instantiate();
	options->set_section_tagged(p_section_tagged);
	options->set_initial_age_ticks(p_initial_age_ticks);
	options->set_render_domain(p_render_domain);
	options->set_source_tick(p_source_tick);
	options->set_source_order(p_source_order);
	const Ref<EffectSpawnReceipt> receipt =
			spawn_effect_request(p_name, descriptor_pose(p_position, p_orientation), options);
	return receipt->get_effect_handle();
}

int64_t EffectWorld::spawn_effect(const String &p_name, const Vector3 &p_position,
		const Vector3 &p_orientation, bool p_section_tagged) {
	return spawn_effect_transient(p_name, p_position, p_orientation, 0, RENDER_DOMAIN_WORLD, 0,
			0, p_section_tagged);
}

Ref<EffectSpawnReceipt> EffectWorld::spawn_effect_owned_request(const Variant &p_owner_key,
		const String &p_name, const Vector3 &p_position, const Vector3 &p_orientation,
		bool p_section_tagged) {
	if (particles_disabled_) {
		return _disabled_receipt();
	}
	const Transform3D initial_transform = descriptor_pose(p_position, p_orientation);
	Ref<EffectSpawnOptions> options;
	options.instantiate();
	options->set_admission(ADMISSION_REPLACE_OWNED);
	options->set_binding(BINDING_FOLLOW_OWNER);
	options->set_slot_key(p_owner_key);
	options->set_owner_key(p_owner_key);
	options->set_owner_transform(initial_transform);
	options->set_has_owner_transform(true);
	options->set_section_tagged(p_section_tagged);
	return spawn_effect_request(p_name, initial_transform, options);
}

int64_t EffectWorld::spawn_effect_owned(const Variant &p_owner_key, const String &p_name,
		const Vector3 &p_position, const Vector3 &p_orientation, bool p_section_tagged) {
	const Ref<EffectSpawnReceipt> receipt = spawn_effect_owned_request(
			p_owner_key, p_name, p_position, p_orientation, p_section_tagged);
	return receipt->get_effect_handle();
}

Ref<EffectSpawnReceipt> EffectWorld::spawn_effect_attached_request(const Variant &p_owner_key,
		const String &p_name, const Transform3D &p_initial_transform,
		const Vector3 &p_local_pos, const Vector3 &p_local_dir, bool p_section_tagged) {
	if (particles_disabled_) {
		return _disabled_receipt();
	}
	const Transform3D local_transform = forward_pose(p_local_pos, p_local_dir);
	Ref<EffectSpawnOptions> options;
	options.instantiate();
	options->set_binding(BINDING_FOLLOW_OWNER);
	options->set_owner_key(p_owner_key);
	options->set_owner_transform(p_initial_transform);
	options->set_has_owner_transform(true);
	options->set_owner_relative_transform(local_transform);
	options->set_section_tagged(p_section_tagged);
	return spawn_effect_request(p_name, p_initial_transform * local_transform, options);
}

int64_t EffectWorld::spawn_effect_attached(const Variant &p_owner_key, const String &p_name,
		const Transform3D &p_initial_transform, const Vector3 &p_local_pos,
		const Vector3 &p_local_dir, bool p_section_tagged) {
	const Ref<EffectSpawnReceipt> receipt = spawn_effect_attached_request(p_owner_key, p_name,
			p_initial_transform, p_local_pos, p_local_dir, p_section_tagged);
	if (!receipt->get_spawned()) {
		return 0;
	}
	return receipt->get_effect_handle();
}

int64_t EffectWorld::spawn_effect_unless_alive(const Variant &p_owner_key, const String &p_name,
		const Vector3 &p_position, const Vector3 &p_orientation) {
	if (particles_disabled_) {
		return 0;
	}
	Ref<EffectSpawnOptions> options;
	options.instantiate();
	options->set_admission(ADMISSION_SUPPRESS_WHILE_OWNED);
	options->set_slot_key(p_owner_key);
	const Ref<EffectSpawnReceipt> receipt =
			spawn_effect_request(p_name, descriptor_pose(p_position, p_orientation), options);
	return receipt->get_effect_handle();
}

bool EffectWorld::spawn_effect_by_handle(int64_t p_handle, const Vector3 &p_position,
		const Vector3 &p_orientation) {
	if (particles_disabled_ || effect_name_for_handle(p_handle).is_empty()) {
		return false;
	}
	Ref<EffectSpawnRequest> request =
			EffectSpawnRequest::make(p_handle, descriptor_pose(p_position, p_orientation));
	request->set_kill_plane_y(water_height_);
	return scene_->spawn(request)->get_spawned();
}

std::shared_ptr<opennova::particle::EffectScene> EffectWorld::shared_native_scene() const {
    return scene_->shared_native_scene();
}

void EffectWorld::stop_group(int64_t p_group_id) {
	scene_->detach(p_group_id);
}

bool EffectWorld::set_group_parameters(int64_t p_group_id, float p_rate_control,
		float p_offset_control) {
	return scene_.is_valid() && p_group_id != 0 &&
			scene_->set_group_parameters(p_group_id, p_rate_control, p_offset_control);
}

void EffectWorld::release_effect_binding(const Variant &p_owner_key) {
	if (const int64_t *slot_token = slot_tokens_.getptr(p_owner_key)) {
		scene_->detach_slot(*slot_token);
		slot_tokens_.erase(p_owner_key);
	}
	const int64_t *owner_token_ptr = owner_tokens_.getptr(p_owner_key);
	if (owner_token_ptr == nullptr) {
		return;
	}
	const int64_t owner_token = *owner_token_ptr;
	// A group may already be detached by stop_group(), but explicitly
	// retiring the native pose keeps this safe for rejected spawns and
	// callers that only know the owner identity.
	owner_pose_batch_.clear();
	owner_pose_batch_.add_absent(owner_token);
	scene_->apply_owner_poses(owner_pose_batch_);
	owner_pose_cache_.erase(owner_token);
	owner_keys_by_token_.erase(owner_token);
	owner_tokens_.erase(p_owner_key);
}

bool EffectWorld::has_owner_binding(const Variant &p_owner_key) const {
	const int64_t *owner_token = owner_tokens_.getptr(p_owner_key);
	return slot_tokens_.has(p_owner_key) && owner_token != nullptr &&
			owner_keys_by_token_.has(*owner_token);
}

bool EffectWorld::has_cached_owner_pose(const Variant &p_owner_key) const {
	const int64_t *owner_token = owner_tokens_.getptr(p_owner_key);
	if (owner_token == nullptr) {
		return false;
	}
	return owner_pose_cache_.has(*owner_token);
}

bool EffectWorld::has_no_owner_bindings() const {
	return slot_tokens_.is_empty() && owner_tokens_.is_empty() &&
			owner_keys_by_token_.is_empty() && owner_pose_cache_.is_empty();
}

void EffectWorld::_sync_owner_poses(bool p_refresh_frame) {
	if (!owner_position_provider_.is_valid()) {
		return;
	}
	EffectOwnerPoseBatch &batch = owner_pose_batch_;
	batch.clear();
	const std::vector<opennova::particle::EffectOwnerToken> tokens =
			scene_->native_scene().active_owner_tokens();
	for (const opennova::particle::EffectOwnerToken &token : tokens) {
		const int64_t owner_token = token_to_godot(token.value);
		const Variant *owner_key = owner_keys_by_token_.getptr(owner_token);
		if (owner_key == nullptr) {
			continue;
		}
		const Variant state = owner_position_provider_.call(*owner_key);
		if (state.get_type() == Variant::TRANSFORM3D) {
			const Transform3D owner_transform = state;
			if (const Transform3D *cached = owner_pose_cache_.getptr(owner_token)) {
				if (cached->is_equal_approx(owner_transform)) {
					continue;
				}
			}
			owner_pose_cache_.insert(owner_token, owner_transform);
			batch.add(owner_token, owner_transform);
		} else if (state.get_type() == Variant::VECTOR3) {
			const Transform3D *cached_ptr = owner_pose_cache_.getptr(owner_token);
			const bool had_cached = cached_ptr != nullptr;
			const Transform3D cached = had_cached ? *cached_ptr : Transform3D();
			const Transform3D translated(cached.basis, Vector3(state));
			if (had_cached && cached.is_equal_approx(translated)) {
				continue;
			}
			owner_pose_cache_.insert(owner_token, translated);
			batch.add(owner_token, translated);
		} else {
			owner_pose_cache_.erase(owner_token);
			batch.add_absent(owner_token);
		}
	}
	if (batch.get_count() > 0) {
		if (p_refresh_frame) {
			scene_->apply_owner_poses(batch);
		} else {
			scene_->apply_owner_poses_in_place(batch);
		}
	}
}

void EffectWorld::set_mission_wind(int p_wind_speed, int p_wind_direction_degrees) {
	const opennova::particle::Vec3 wind =
			opennova::particle::mission_wind_vector(p_wind_speed, p_wind_direction_degrees);
	scene_->set_global_wind(Vector3(wind.x, wind.y, wind.z));
}

void EffectWorld::advance_fixed_tick(double p_delta) {
	advance_simulation_tick(p_delta, nullptr);
}

void EffectWorld::advance_simulation_tick(double p_delta,
		const opennova::particle::ParticleForceField *p_forces) {
	_sync_owner_poses(false);
	// The NOVISNOUPDATE gate reads the camera that rendered the previous
	// frame, like retail's manager clip state set at BeginFrame; without a
	// current camera every emitter advances.
	Camera3D *camera = nullptr;
	if (Viewport *viewport = get_viewport(); viewport != nullptr) {
		camera = viewport->get_camera_3d();
	}
	if (camera != nullptr) {
		const Transform3D camera_transform = camera->get_camera_transform();
		const Vector3 probe = camera_transform.origin -
				camera_transform.basis.get_column(2) * (camera->get_near() + 1.0f);
		scene_->set_view_frustum(camera->get_frustum(), probe);
	} else {
		scene_->clear_view_frustum();
	}
	// The group section gate reads this frame's building masks
	// (particle::EffectSectionGate); without a simulation every group stays visible.
	const EffectSectionSource masks(_section_source());
	scene_->set_section_masks(masks.valid() ? &masks : nullptr);
	scene_->advance_with_forces(p_delta > 0.0 ? p_delta : 0.0, p_forces);
	scene_->set_section_masks(nullptr);
}

bool EffectWorld::trigger_group_children(int64_t p_group_id, const Vector3 &p_position,
		const Vector3 &p_forward, int p_force_zone) {
	return scene_.is_valid() && p_group_id != 0 &&
			scene_->trigger_group_children(p_group_id, p_position, p_forward, p_force_zone);
}

std::shared_ptr<EffectDistortionDrawer> EffectWorld::distortion_drawer() {
	return _ensure_renderer()->distortion_drawer();
}

void EffectWorld::attach_distortion_row(Node *p_frame_fx) {
	_ensure_renderer()->attach_distortion_row(p_frame_fx);
}

void EffectWorld::render_frame(int64_t p_time_ms) {
	_sync_owner_poses(true);
	_ensure_renderer()->render_now(p_time_ms);
}

void EffectWorld::publish_scene_overlay(
		const std::shared_ptr<const SceneOverlaySubmission> &p_submission) {
	_ensure_renderer()->publish_scene_overlay(p_submission);
}

TypedArray<EffectGroupReport> EffectWorld::get_debug_group_report(bool p_include_hidden) {
	TypedArray<EffectGroupReport> out;
	if (particles_disabled_ && !p_include_hidden) {
		return out;
	}
	ParticleRenderer *renderer = _ensure_renderer();
	std::vector<opennova::renderer::ParticleEmitterDrawBounds> bounds_rows;
	renderer->collect_debug_emitter_bounds(bounds_rows);
	HashMap<int64_t, const opennova::renderer::ParticleEmitterDrawBounds *> rendered_by_id;
	for (const opennova::renderer::ParticleEmitterDrawBounds &row : bounds_rows) {
		rendered_by_id.insert(token_to_godot(row.emitter_id), &row);
	}
	// F3 and the optional effect-box view are recurring UI reads. Ask for
	// the compact topology/pose view and use renderer-owned bounds;
	// serializing the immutable catalog and every live particle here creates
	// a low-FPS feedback loop precisely while the counters are being
	// observed.
	const opennova::particle::EffectDebugSnapshot debug = scene_->native_scene().inspect(false);
	for (const opennova::particle::EffectGroupDebugSnapshot &group : debug.groups) {
		Ref<EffectGroupReport> row;
		row.instantiate();
		bool forever = false;
		for (const opennova::particle::EffectEmitterDebugSnapshot &emitter : group.emitters) {
			const int64_t emitter_id = token_to_godot(emitter.id);
			const opennova::renderer::ParticleEmitterDrawBounds *const *rendered =
					rendered_by_id.getptr(emitter_id);
			forever = forever || (emitter.definition_flags & PARTICLE_FLAG_FOREVER_EMIT) != 0;
			const bool bounds_valid = rendered != nullptr && (*rendered)->bounds.valid;
			Ref<EffectEmitterReport> emitter_row;
			emitter_row.instantiate();
			emitter_row->set_name(to_gd(emitter.definition_name));
			emitter_row->set_alive(static_cast<int>(emitter.alive_particle_count));
			emitter_row->set_emitting(emitter.emitting);
			emitter_row->set_rendered(
					rendered != nullptr ? static_cast<int>((*rendered)->quad_count) : 0);
			emitter_row->set_bounds(bounds_valid ? godot_aabb((*rendered)->bounds) : AABB());
			emitter_row->set_bounds_valid(bounds_valid);
			emitter_row->set_emitter_id(emitter_id);
			emitter_row->set_position(godot_vector(emitter.position));
			emitter_row->set_forward(godot_vector(emitter.forward));
			emitter_row->set_age(emitter.age);
			emitter_row->set_emit_rate(emitter.emit_rate);
			emitter_row->set_spawn_y_offset(emitter.spawn_y_offset);
			emitter_row->set_camera_pull(emitter.camera_pull);
			emitter_row->set_kill_plane(static_cast<int>(emitter.kill_plane));
			emitter_row->set_kill_plane_y(emitter.kill_plane_y);
			row->add_emitter(emitter_row);
		}
		row->set_id(token_to_godot(group.id.value));
		row->set_name(to_gd(group.effect_name));
		const Variant *owner_key = owner_keys_by_token_.getptr(token_to_godot(group.owner.value));
		row->set_owner_key(owner_key != nullptr ? *owner_key : Variant());
		row->set_source(to_gd(group.source).get_file());
		row->set_forever(forever && !group.detached);
		row->set_admission(static_cast<int>(group.admission));
		row->set_binding(static_cast<int>(group.binding));
		row->set_render_domain(static_cast<int>(group.render_domain));
		row->set_detached(group.detached);
		row->set_transform(godot_pose(group.pose));
		row->set_source_tick(token_to_godot(group.source_tick));
		row->set_source_order(token_to_godot(group.source_order));
		row->set_section_tagged(group.section_gate.tagged);
		out.push_back(row);
	}
	return out;
}

PackedStringArray EffectWorld::get_unresolved_texture_names() {
	return _ensure_renderer()->get_unresolved_texture_names();
}

void EffectWorld::_bind_methods() {
	ClassDB::bind_method(D_METHOD("release_runtime_renderer_resources"),
			&EffectWorld::release_runtime_renderer_resources);
	ClassDB::bind_method(D_METHOD("set_environment_source", "source"),
			&EffectWorld::set_environment_source);
	ClassDB::bind_method(D_METHOD("file_count"), &EffectWorld::file_count);
	ClassDB::bind_method(D_METHOD("get_files"), &EffectWorld::get_files);
	ClassDB::bind_method(D_METHOD("effect_count"), &EffectWorld::effect_count);
	ClassDB::bind_method(D_METHOD("live_group_count"), &EffectWorld::live_group_count);
	ClassDB::bind_method(D_METHOD("active_entry_count"), &EffectWorld::active_entry_count);
	ClassDB::bind_method(D_METHOD("interned_count"), &EffectWorld::interned_count);
	ClassDB::bind_method(D_METHOD("set_particles_hidden", "hidden"),
			&EffectWorld::set_particles_hidden);
	ClassDB::bind_method(D_METHOD("are_particles_hidden"), &EffectWorld::are_particles_hidden);
	ClassDB::bind_method(D_METHOD("get_texture_provider"), &EffectWorld::get_texture_provider);
	ClassDB::bind_method(D_METHOD("set_owner_position_provider", "provider"),
			&EffectWorld::set_owner_position_provider);
	ClassDB::bind_method(D_METHOD("set_water_plane", "value", "reflection_camera"),
			&EffectWorld::set_water_plane);
	ClassDB::bind_method(D_METHOD("set_second_scene_camera", "camera"),
			&EffectWorld::set_second_scene_camera);
	ClassDB::bind_method(D_METHOD("get_second_scene_camera"),
			&EffectWorld::get_second_scene_camera);
	ClassDB::bind_method(D_METHOD("load_from_resource_root", "root"),
			&EffectWorld::load_from_resource_root);
	ClassDB::bind_method(D_METHOD("load_particle_file", "file"), &EffectWorld::load_particle_file);
	ClassDB::bind_method(D_METHOD("clear_world"), &EffectWorld::clear_world);
	ClassDB::bind_method(D_METHOD("reset_runtime_state"), &EffectWorld::reset_runtime_state);
	ClassDB::bind_method(D_METHOD("intern_effect", "name"), &EffectWorld::intern_effect);
	ClassDB::bind_method(D_METHOD("effect_name_for_handle", "handle"),
			&EffectWorld::effect_name_for_handle);
	ClassDB::bind_static_method("EffectWorld", D_METHOD("descriptor_pose", "position", "orientation"),
			&EffectWorld::descriptor_pose);
	ClassDB::bind_static_method("EffectWorld", D_METHOD("forward_pose", "position", "forward"),
			&EffectWorld::forward_pose);
	ClassDB::bind_method(D_METHOD("spawn_effect_request", "name", "transform", "options"),
			&EffectWorld::spawn_effect_request, DEFVAL(Variant()));
	ClassDB::bind_method(D_METHOD("warm_all_effects", "position"), &EffectWorld::warm_all_effects);
	ClassDB::bind_method(D_METHOD("render_now", "time_ms"), &EffectWorld::render_now);
	ClassDB::bind_method(D_METHOD("attach_distortion_row", "frame_fx"),
			&EffectWorld::attach_distortion_row);
	ClassDB::bind_method(D_METHOD("get_debug_draw_list_report"),
			&EffectWorld::get_debug_draw_list_report);
	ClassDB::bind_method(D_METHOD("spawn_effect_transient", "name", "position", "orientation",
								 "initial_age_ticks", "render_domain", "source_tick", "source_order",
								 "section_tagged"),
			&EffectWorld::spawn_effect_transient, DEFVAL(Vector3()), DEFVAL(0),
			DEFVAL(static_cast<int>(RENDER_DOMAIN_WORLD)), DEFVAL(0), DEFVAL(0), DEFVAL(false));
	ClassDB::bind_method(D_METHOD("spawn_effect", "name", "position", "orientation",
								 "section_tagged"),
			&EffectWorld::spawn_effect, DEFVAL(Vector3()), DEFVAL(false));
	ClassDB::bind_method(D_METHOD("spawn_effect_owned_request", "owner_key", "name", "position",
								 "orientation", "section_tagged"),
			&EffectWorld::spawn_effect_owned_request, DEFVAL(Vector3()), DEFVAL(false));
	ClassDB::bind_method(D_METHOD("spawn_effect_owned", "owner_key", "name", "position",
								 "orientation", "section_tagged"),
			&EffectWorld::spawn_effect_owned, DEFVAL(Vector3()), DEFVAL(false));
	ClassDB::bind_method(D_METHOD("spawn_effect_attached", "owner_key", "name", "initial_transform",
								 "local_pos", "local_dir", "section_tagged"),
			&EffectWorld::spawn_effect_attached, DEFVAL(false));
	ClassDB::bind_method(
			D_METHOD("spawn_effect_unless_alive", "owner_key", "name", "position", "orientation"),
			&EffectWorld::spawn_effect_unless_alive, DEFVAL(Vector3()));
	ClassDB::bind_method(D_METHOD("spawn_effect_by_handle", "handle", "position", "orientation"),
			&EffectWorld::spawn_effect_by_handle, DEFVAL(Vector3()));
	ClassDB::bind_method(D_METHOD("stop_group", "group_id"), &EffectWorld::stop_group);
	ClassDB::bind_method(D_METHOD("set_group_parameters", "group_id", "rate_control",
					"offset_control"),
			&EffectWorld::set_group_parameters);
	ClassDB::bind_method(D_METHOD("release_effect_binding", "owner_key"),
			&EffectWorld::release_effect_binding);
	ClassDB::bind_method(D_METHOD("has_owner_binding", "owner_key"), &EffectWorld::has_owner_binding);
	ClassDB::bind_method(D_METHOD("has_cached_owner_pose", "owner_key"),
			&EffectWorld::has_cached_owner_pose);
	ClassDB::bind_method(D_METHOD("has_no_owner_bindings"), &EffectWorld::has_no_owner_bindings);
	ClassDB::bind_method(D_METHOD("advance_fixed_tick", "delta"), &EffectWorld::advance_fixed_tick);
	ClassDB::bind_method(D_METHOD("set_mission_wind", "wind_speed", "wind_direction_degrees"),
			&EffectWorld::set_mission_wind);
	ClassDB::bind_method(D_METHOD("render_frame", "time_ms"), &EffectWorld::render_frame);
	ClassDB::bind_method(D_METHOD("get_debug_group_report", "include_hidden"),
			&EffectWorld::get_debug_group_report, DEFVAL(false));
	ClassDB::bind_method(D_METHOD("get_unresolved_texture_names"),
			&EffectWorld::get_unresolved_texture_names);

	BIND_CONSTANT(PARTICLE_FLAG_FOREVER_EMIT);
	BIND_CONSTANT(ADMISSION_ALWAYS);
	BIND_CONSTANT(ADMISSION_REPLACE_OWNED);
	BIND_CONSTANT(ADMISSION_SUPPRESS_WHILE_OWNED);
    BIND_CONSTANT(ADMISSION_STORE_OWNED);
	BIND_CONSTANT(BINDING_WORLD);
	BIND_CONSTANT(BINDING_FOLLOW_OWNER);
	BIND_CONSTANT(RENDER_DOMAIN_WORLD);
	BIND_CONSTANT(RENDER_DOMAIN_FIRST_PERSON);
	BIND_CONSTANT(KILL_PLANE_DISABLED);
}
