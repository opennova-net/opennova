#pragma once

#include <godot_cpp/classes/array_mesh.hpp>
#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/node.hpp>

#include <cstdint>
#include <functional>
#include <vector>

#include <editor/preview/definition_weapon.h>
#include <runtime/renderer/scar_draw_list.h>
#include <runtime/renderer/tracer_frame.h>

#include "render/tracer_ribbon_surfaces.h"
#include "resource_index/resource_root.h"

namespace godot {

class ScarPresenter;

// A weapon range drawn in a preview's picture (ADR 0046 DI-24, the model preview's clip fire; the shapes DI-22's
// definition device draws): the target's face as a plain quad (the editor's aid), the tracers as the game's ribbon
// pass builds them against the device's camera (render/tracer_ribbon_surfaces over renderer::compile_tracer_ribbons),
// the scars on the face by the game's ScarPresenter. A device helper with no state of the preview's own: what it draws
// is what the portable range hands it, each frame.
class PreviewRangeDraw {
public:
	// Its nodes added under `parent` (a node of the device's SubViewport).
	explicit PreviewRangeDraw(Node &parent);
	~PreviewRangeDraw();
	PreviewRangeDraw(const PreviewRangeDraw &) = delete;
	PreviewRangeDraw &operator=(const PreviewRangeDraw &) = delete;

	// The root the scars' textures and the tracers' smoke are read through (the effects' mount).
	void set_resource_root(const Ref<ResourceRoot> &root);
	// Nothing drawn.
	void clear();
	// The target's face at `corners` (null: none).
	void show_target(const opennova::editor::PreviewVec3 *corners);
	// The scars the range holds, uploaded again where its serial moved.
	void show_scars(uint64_t serial, int count, const std::function<opennova::renderer::ScarDrawList()> &compile);
	// The tracers against `camera` at the clock's `time_ms`.
	void show_tracers(const std::vector<opennova::editor::DefinitionTrail> &trails, Camera3D &camera,
			int64_t time_ms);

	// Its nodes, for the device tests.
	MeshInstance3D *target() const { return target_; }
	Ref<ArrayMesh> tracer_mesh() const { return tracer_mesh_; }
	ScarPresenter *scars() const { return scars_; }

private:
	MeshInstance3D *target_ = nullptr;
	Ref<ArrayMesh> target_mesh_;
	MeshInstance3D *tracers_ = nullptr;
	Ref<ArrayMesh> tracer_mesh_;
	TracerRibbonSurfaces ribbons_;
	opennova::renderer::TracerRibbonFrame tracer_frame_;
	std::vector<float> tracer_points_;
	ScarPresenter *scars_ = nullptr;
	uint64_t scars_shown_ = UINT64_MAX;
	bool rooted_ = false;
	bool drawn_ = false; // anything drawn since the last clear
};

} // namespace godot
