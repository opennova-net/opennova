#pragma once

#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/sub_viewport.hpp>

#include <cstdint>
#include <map>
#include <memory>
#include <string>

#include <editor/preview/viewport_follow.h>

#include "authoring/viewport_applier.h"
#include "object/object_data.h"
#include "object/object_model.h"

namespace godot {

// A model viewport's device work (ADR 0046 S10p3, S13 V5): the runtime's own ObjectModel (its
// meshes, materials, CTRL registers, part animations) under the retail noon light and one display
// decode (avatar_preview.gd's recipe), built from the model the viewport shows (as it would save, or
// an animation's rig model), its textures read through the project's files (the open documents
// standing in for theirs, each read noted with its stamp, a flipbook's frame as it is first drawn
// too); the camera placed where the viewport's orbit camera is, the level drawn the one it picks;
// an animation's rig bound and its clip posed at the preview clock's tick; the part animations and
// material generators run at the clock's milliseconds on the device's own PanmClock.
class ModelViewportApplier final : public ViewportApplier {
public:
	explicit ModelViewportApplier(SubViewport &viewport);

	void rebuild(const opennova::editor::ViewportModel &model, const opennova::editor::SessionView &view) override;
	void update(const opennova::editor::ViewportModel &model) override;
	void clear() override;
	void step(const opennova::editor::ViewportModel &model, const opennova::editor::PreviewClock &clock,
			opennova::editor::ViewportDeviceReport &report) override;
	void tick(const opennova::editor::ViewportModel &model, const opennova::editor::PreviewClock &clock) override;
	void resize(int, int) override {}

	// Its nodes, for the parity tests: the camera and the model.
	Camera3D *camera() const { return camera_; }
	ObjectModel *object_model() const { return object_; }

private:
	// The CTRL registers the options hold (one let go reads 0 again).
	void apply_registers_(const opennova::editor::ViewportModel &model);
	// The camera where the viewport's is, and the level it picks.
	void place_camera_(const opennova::editor::ViewportModel &model);
	// The rig the viewport loaded, bound to the model (again when it is loaded again).
	void bind_rig_(const opennova::editor::ViewportModel &model);
	void play_clip_(const opennova::editor::ViewportModel &model, const opennova::editor::PreviewClock &clock);

	Camera3D *camera_ = nullptr;
	ObjectModel *object_ = nullptr;
	Ref<ObjectData> data_;
	Ref<PanmClock> clock_;
	int64_t frame_ = 0;
	// The files the built scene read (its textures), noted as they are read.
	std::shared_ptr<const opennova::editor::StampedFiles> files_;
	std::map<std::string, int64_t> applied_ctrl_; // the registers the model holds now
	int applied_lod_ = -1;
	uint64_t applied_skeleton_ = UINT64_MAX; // the rig serial the model holds
};

} // namespace godot
