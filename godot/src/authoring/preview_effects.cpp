#include "authoring/preview_effects.h"

#include <godot_cpp/variant/callable.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>

#include <editor/assets/project_asset_source.h>

#include <godot_cpp/variant/packed_color_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_int64_array.hpp>
#include <godot_cpp/variant/packed_vector2_array.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>

#include <runtime/world/impact_scar.h>

#include "particle/particle_renderer.h"
#include "util/color_convert.h"
#include "util/string_convert.h"

namespace godot {

PreviewEffects::PreviewEffects(Node &parent) {
	ParticleRenderer *renderer = memnew(ParticleRenderer);
	renderer->set_name("ParticleRenderer");
	scene_.instantiate();
	scene_->share_native_scene(nullptr);
	renderer->set_scene(scene_);
	parent.add_child(renderer);
	renderer_id_ = renderer->get_instance_id();
	root_.instantiate();
}

PreviewEffects::~PreviewEffects() = default;

ParticleRenderer *PreviewEffects::renderer() const {
	return Object::cast_to<ParticleRenderer>(ObjectDB::get_instance(renderer_id_));
}

void PreviewEffects::mount(const std::shared_ptr<const opennova::editor::ProjectAssetSource> &files) {
	// The same files, none of what was read moved: the mount and what it cached stand.
	if (files == mounted_ && stamped_ && (!files || !stamped_->stamps().moved(*files))) return;
	mounted_ = files;
	if (!files) {
		stamped_.reset();
		root_->clear();
		if (ParticleRenderer *renderer = this->renderer()) renderer->set_texture_provider(Callable());
		return;
	}
	// A fresh record of what is read, the root mounted over it (its caches dropped).
	stamped_ = std::make_shared<opennova::StampedFiles>(files);
	root_->mount_files(stamped_);
	if (ParticleRenderer *renderer = this->renderer())
		renderer->set_texture_provider(
				Callable(root_.ptr(), "load_texture").bind(ResourceRoot::TEXTURE_LOADER_PARTICLE));
}

void PreviewEffects::show(const std::shared_ptr<opennova::particle::EffectScene> &scene) {
	// The renderer builds its catalog (the graphics, the atlas) from the scene's definitions as it first
	// draws them: a scene opened anew is a catalog anew, its graphics read through the mount (cached by
	// it until a file read moves, mount()).
	shown_ = scene;
	scene_->share_native_scene(scene);
}

void PreviewEffects::set_environment_source(Node *source) {
	if (ParticleRenderer *renderer = this->renderer()) renderer->set_environment_source(source);
}

void PreviewEffects::render(int64_t time_ms) {
	ParticleRenderer *renderer = this->renderer();
	if (!renderer || !renderer->is_inside_tree()) return;
	scene_->invalidate_snapshot();
	renderer->render_now(time_ms);
}

void PreviewEffects::clear() {
	shown_.reset();
	scene_->share_native_scene(nullptr);
	if (ParticleRenderer *renderer = this->renderer()) {
		renderer->set_texture_provider(Callable());
		if (renderer->is_inside_tree()) renderer->render_now(0);
	}
	mounted_.reset();
	stamped_.reset();
	root_->clear();
}

opennova::FileStamps PreviewEffects::stamps() const {
	return stamped_ ? stamped_->stamps() : opennova::FileStamps();
}

std::vector<std::string> PreviewEffects::missing() const {
	std::vector<std::string> out;
	if (ParticleRenderer *renderer = this->renderer()) {
		const PackedStringArray names = renderer->get_unresolved_texture_names();
		for (int64_t i = 0; i < names.size(); ++i) out.push_back(opennova::to_std(names[i]));
	}
	return out;
}

Ref<ScarDrawList> preview_scar_record(const opennova::renderer::ScarDrawList &list) {
	Ref<ScarDrawList> out;
	out.instantiate();
	PackedVector3Array vertices;
	PackedVector2Array uvs;
	PackedColorArray colors;
	vertices.resize(int64_t(list.vertices.size()));
	uvs.resize(int64_t(list.vertices.size()));
	colors.resize(int64_t(list.vertices.size()));
	for (size_t i = 0; i < list.vertices.size(); ++i) {
		const opennova::renderer::ScarVertex &v = list.vertices[i];
		vertices[int64_t(i)] = Vector3(v.x, v.y, v.z);
		uvs[int64_t(i)] = Vector2(v.u, v.v);
		colors[int64_t(i)] = opennova::color_from_argb(v.argb);
	}
	PackedInt32Array owner, texture, section, flags, first, count, bms, world_first;
	PackedInt64Array spawn_origin;
	for (const opennova::renderer::ScarDrawBatch &batch : list.batches) {
		if (batch.entity_local) continue; // the shared ring's, or rings the caller made world-space
		owner.push_back(batch.owner_packed);
		texture.push_back(batch.texture);
		section.push_back(batch.section);
		flags.push_back(batch.building ? ScarDrawList::FLAG_BUILDING : 0);
		first.push_back(int32_t(batch.first_vertex));
		count.push_back(int32_t(batch.vertex_count));
		bms.push_back(0);
		spawn_origin.push_back(0);
		world_first.push_back(-1);
	}
	PackedStringArray strip_names;
	PackedInt32Array strip_mode_words;
	strip_names.resize(opennova::world::kScarTextureStripCount);
	strip_mode_words.resize(opennova::world::kScarTextureStripCount);
	for (int strip = 0; strip < opennova::world::kScarTextureStripCount; ++strip) {
		strip_names[strip] = String(opennova::world::scar_texture_strip_name(strip));
		strip_mode_words[strip] = int32_t(opennova::world::scar_texture_strip_mode_word(strip));
	}
	out->set_vertices(vertices);
	out->set_uvs(uvs);
	out->set_colors(colors);
	out->set_batch_owner(owner);
	out->set_batch_texture(texture);
	out->set_batch_section(section);
	out->set_batch_flags(flags);
	out->set_batch_first(first);
	out->set_batch_count(count);
	out->set_batch_bms_id(bms);
	out->set_batch_spawn_origin(spawn_origin);
	out->set_batch_world_first(world_first);
	out->set_strip_names(strip_names);
	out->set_strip_mode_words(strip_mode_words);
	out->set_slots_live(int(list.slots_live));
	out->set_slots_culled(int(list.slots_culled));
	return out;
}

} // namespace godot
