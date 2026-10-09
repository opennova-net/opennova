#pragma once

#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/core/object_id.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <editor/preview/viewport_follow.h>
#include <runtime/particle/effect_scene.h>

#include "particle/effect_scene.h"
#include "resource_index/resource_root.h"

namespace opennova::editor {
class ProjectAssetSource;
} // namespace opennova::editor

namespace godot {

class ParticleRenderer;

// The effects a preview draws (ADR 0046 DI-14): a portable effect scene the editor core opens and steps
// (editor/preview/effect_playback, the engine's own particle::EffectScene), drawn by the game's own
// particle renderer (ParticleRenderer: its draw-list compiler and the compositor passes on the camera of
// the device's SubViewport) with the particle manager's graphics, read through its loader
// (ResourceRoot::TEXTURE_LOADER_PARTICLE) from the project's files as the game would read them were they
// saved now (the open documents standing in), each read noted with its stamp so a graphic changed builds
// the picture again. A device helper with no state of the preview's own: the effect viewport's
// (authoring/effect_viewport_applier), and a later definition's picture or a model's Shoot. Nothing here
// simulates: the scene is stepped by its owner, and drawn as it stands.
class PreviewEffects {
public:
	// Its renderer added under `parent` (a node of the device's SubViewport, whose camera it draws for).
	explicit PreviewEffects(Node &parent);
	~PreviewEffects();
	PreviewEffects(const PreviewEffects &) = delete;
	PreviewEffects &operator=(const PreviewEffects &) = delete;

	// The project's files the graphics are read from: mounted again where the source is another or a file
	// read through it moved (what the mount cached dropped, a fresh record of what is read); the renderer's
	// catalog then loads its graphics again.
	void mount(const std::shared_ptr<const opennova::editor::ProjectAssetSource> &files);
	// The scene drawn from now on (null: none): its catalog's graphics are loaded at its first render.
	void show(const std::shared_ptr<opennova::particle::EffectScene> &scene);
	// The renderer's environment source (a MissionEnvironment: the fog and the particle tints it reads).
	void set_environment_source(Node *source);
	// The scene's snapshot as its owner left it, drawn: `time_ms` the clock the renderer's passes read.
	void render(int64_t time_ms);
	// Nothing drawn, nothing mounted.
	void clear();

	// The graphics read for the picture, each with its stamp, and the names that loaded nothing.
	opennova::FileStamps stamps() const;
	std::vector<std::string> missing() const;
	// The root the graphics are read through (null before a mount): a device's other readers of the project's
	// files share it (DI-22: the scars' textures, the tracers' smoke).
	const Ref<ResourceRoot> &root() const { return root_; }
	// Its nodes and values, for the device tests: the renderer, and the scene it draws.
	ParticleRenderer *renderer() const;
	const std::shared_ptr<opennova::particle::EffectScene> &scene() const { return shown_; }

private:
	ObjectID renderer_id_;
	Ref<EffectScene> scene_; // the renderer's wrapper over the shown scene
	Ref<ResourceRoot> root_;
	std::shared_ptr<const opennova::editor::ProjectAssetSource> mounted_;
	std::shared_ptr<opennova::StampedFiles> stamped_;
	std::shared_ptr<opennova::particle::EffectScene> shown_;
};

} // namespace godot
