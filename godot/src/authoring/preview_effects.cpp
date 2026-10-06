#include "authoring/preview_effects.h"

#include <godot_cpp/variant/callable.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>

#include <editor/assets/project_asset_source.h>

#include "particle/particle_renderer.h"
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
	stamped_ = std::make_shared<opennova::editor::StampedFiles>(files);
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

opennova::editor::FileStamps PreviewEffects::stamps() const {
	return stamped_ ? stamped_->stamps() : opennova::editor::FileStamps();
}

std::vector<std::string> PreviewEffects::missing() const {
	std::vector<std::string> out;
	if (ParticleRenderer *renderer = this->renderer()) {
		const PackedStringArray names = renderer->get_unresolved_texture_names();
		for (int64_t i = 0; i < names.size(); ++i) out.push_back(opennova::to_std(names[i]));
	}
	return out;
}

} // namespace godot
