#pragma once

#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/sub_viewport.hpp>

#include <cstdint>
#include <map>
#include <string>

#include <editor/preview/model_preview_json.h>
#include <editor/preview/model_preview_state.h>
#include <editor/session/session_view.h>
#include <editor/ui/model_preview_pane.h>

#include "object/object_data.h"
#include "object/object_model.h"

namespace godot {

// The editor's model preview (ADR 0046 S10p3): the device half of the Preview window's
// model pane seam. The portable model (editor/preview) says which model the view
// previews, the model as it would save, when to build again, the camera and the level;
// this renders it with the runtime's own ObjectModel (its meshes, materials, CTRL
// registers, part animations) under the retail noon light and one display decode
// (avatar_preview.gd's recipe), its textures read through the project's files (the open
// documents standing in for theirs), into an offscreen SubViewport the pane draws.
// Created in every editor run, headless included: the MCP and the tests read its JSON.
// The shell refreshes and ticks it every frame, whether the pane shows or not.
class ModelPreview : public opennova::editor::ModelPreviewViewport {
public:
	explicit ModelPreview(Node &owner);
	~ModelPreview() override;

	// Once per pump: follow the session's preview target.
	void refresh(const opennova::editor::SessionView &view);
	// What it shows now, for the JSON.
	opennova::editor::ModelPreviewSnapshot snapshot(const opennova::editor::SessionView &view) const;

	opennova::editor::ModelPreviewModel &model() override { return model_; }
	const opennova::editor::ModelPreviewModel &model() const { return model_; }
	void draw(int device_width, int device_height) override;
	// Place the camera and apply the level the portable half picks (the camera moved).
	void place_camera();
	// Once per frame: the portable clock runs `delta` seconds (while it plays) and the
	// model's part animations read it, so the parts and the overlays pose alike; a clip
	// plays at the clip clock's tick (the portable playhead is its only clock).
	void tick(double delta);

	// The device's nodes, for the tests: its camera and its model.
	Camera3D *camera() const { return camera_; }
	ObjectModel *object_model() const { return object_; }

private:
	void apply_options_();
	// The rig the portable half loaded, bound to the model (again when it is rebuilt).
	void bind_rig_();
	void play_clip_();

	Node &owner_;
	SubViewport *viewport_ = nullptr;
	Camera3D *camera_ = nullptr;
	ObjectModel *object_ = nullptr;
	Ref<ObjectData> data_;
	Ref<PanmClock> clock_;
	int64_t frame_ = 0;
	opennova::editor::ModelPreviewModel model_;
	std::map<std::string, int64_t> applied_ctrl_; // the registers the model holds now
	int applied_lod_ = -1;
	uint64_t applied_skeleton_ = UINT64_MAX; // the rig serial the model holds
};

} // namespace godot
