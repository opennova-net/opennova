#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <editor/model/edit.h>
#include <editor/preview/model_handle_edit.h>
#include <editor/preview/model_overlay.h>
#include <editor/preview/model_preview_camera.h>
#include <editor/preview/model_preview_rig.h>
#include <editor/preview/viewport_follow.h>
#include <editor/preview/viewport_model.h>
#include <runtime/assets/asset_store.h>

namespace opennova::editor {

class Document;
class ModelDocument;
struct ModelCanvasFrame;

// What a model viewport shows, and why not (ADR 0046 S10p2, S10p6, S13 V5): the kind's reason.
enum class ModelViewStatus : uint8_t {
	NoProject, // no project is open
	NoModel, // no model, clip or table is open to show
	Unserializable, // the model, clip or table cannot be written, so the game could not read it
	Unreadable, // what it writes does not read back (or the rig's model does not read)
	NoRig, // an animation no model plays: no item pairs its table with a graphic
	Ready,
};
// "no_project", "ready", ...: its token on the wire.
const char *model_view_status_token(ModelViewStatus status);
// The line a model viewport shows for a status (`detail`: the first serialize issue, the
// animation's file).
std::string model_view_status_message(ModelViewStatus status, const std::string &detail);

// How a model viewport draws the model (ADR 0046 S10p2, S13 V5): the level (Auto: the one the game
// draws at the camera's distance), the CTRL registers held at a value (a register not held reads 0,
// as a model with no entity driving it does), what the overlays mark, and the model an animation
// plays on when the author picks one (a model's file name; "" the one an item pairs with the
// table). Its camera is its own state too; whether its clock runs is the preview clock's.
struct ModelViewportOptions {
	int lod = -1; // -1 Auto
	std::map<std::string, int64_t> ctrl;
	ModelOverlayOptions overlays;
	std::string rig_model;
	bool operator==(const ModelViewportOptions &other) const {
		return lod == other.lod && ctrl == other.ctrl && overlays == other.overlays &&
				rig_model == other.rig_model;
	}
	bool operator!=(const ModelViewportOptions &other) const { return !(*this == other); }
};

// The change a SetViewport makes to set a model viewport's camera to `camera` (its target, yaw,
// pitch and distance): what an orbit, a pan, a dolly or a framing on its canvas sends.
std::string model_camera_change(const OrbitCamera &camera);
// The options on the wire (the envelope's `options`, a SetViewport's): {lod ("auto" or a level),
// ctrl {register: value}, overlays {user_points, lights, pivots}, rig_model}.
io::JsonValue model_options_to_json(const ModelViewportOptions &options);

// A model's viewport (ADR 0046 S10p2, S10p6, S13 V5; ViewportKind::Model): the model document at its
// path as it would save (its serialize(), read back), or a clip or an animation table played on its
// rig's model (the one an item pairs with the table, else the one the author picks; the table over
// the model's bone table through the game's loader), the clip the selection plays on the preview
// clock's ticks. Its own camera orbits the model (framed when a model first shows), and the level it
// draws is the options', else the one the game picks at the camera's distance (Auto). It builds
// again when its document changes what is drawn (a user point's edit is the overlays' alone: an
// Update), when a file the device read moves its stamp (a texture), and its rig again when the rig
// or a file it read moves; a document the game could not read, and a rig model that does not read,
// keep their reason until they change (no retry every frame).
class ModelViewport final : public ViewportModel {
public:
	explicit ModelViewport(std::string path);
	static std::unique_ptr<ViewportModel> make(const std::string &path);

	ModelViewStatus view_status() const { return reason_; }
	const ModelViewportOptions &options() const { return options_; }
	const OrbitCamera &camera() const { return camera_; }
	// The model the device shows (null unless ready).
	const assets::Model &model() const { return model_; }
	// How many times it read its document's bytes (wrote and read back the model, wrote the clip or
	// the table): a follow over a document that did not change reads none.
	uint64_t reads() const { return reads_; }
	// How many times a clip's or a table's viewport read its rig's model from the project's files:
	// once per model chosen or file changed, a model that does not read included (the failure latch:
	// not again each pump until its file changes).
	uint64_t rig_model_reads() const { return rig_model_reads_; }
	// The level to draw: the options', clamped to the model's levels, else Auto's at the device's
	// width.
	int lod() const;
	int auto_lod(int32_t *projected_q16 = nullptr) const;
	// What the overlays mark on the model as drawn at `clock` (the level, the held registers);
	// empty unless ready.
	std::vector<ModelOverlay> overlays(const PreviewClock &clock) const;
	// How long a marker's axis is drawn (its tip is the Axis handle): a share of the camera's
	// distance, so it keeps its size on the picture.
	float axis_length() const { return camera_.distance * 0.08f; }
	PreviewVec3 axis_tip(const ModelOverlay &overlay) const;
	// A drag of `overlay`'s handle to picture pixel (x, y) of a picture `width` x `height`: the point
	// under it on the plane through the handle that faces the eye, planned into the record's edits
	// (model_handle_edits) at the clock, the level and the held registers drawn now. A place's drag
	// moves `others` (other markers, as they were when the drag began) as far, in the same batch
	// (S13 D7: a drag over a selection of several records is one batch); a marker of them with no
	// record that moves is passed over.
	bool handle_edits(const ModelDocument &document, const ModelOverlay &overlay, ModelHandle handle,
			float x, float y, int width, int height, float snap, uint64_t gesture,
			const PreviewClock &clock, std::vector<Edit> &out,
			const std::vector<ModelOverlay> *others = nullptr) const;
	// The camera looking at the whole model on a picture `width` x `height` (from its angles now).
	OrbitCamera framed(int width, int height) const;
	// The camera looking at the marker `overlay` (a light's reach around it, else a share of the
	// model).
	OrbitCamera framed_on(const ModelOverlay &overlay, int width, int height) const;

	// An animation's (S10p6): true while its document is a clip or a table.
	bool animating() const { return animating_; }
	const PreviewRig &rig() const { return rig_; }
	// The rig the device binds (null when it does not load: `detail` says why), and a serial that
	// moves whenever it is loaded again.
	const std::shared_ptr<const anim::SkeletalClips> &skeleton() const { return skeleton_; }
	uint64_t skeleton_serial() const { return skeleton_serial_; }
	// The clip the selection plays (key "" none), its file, and its events on the timeline.
	const std::string &clip_key() const { return clip_key_; }
	int clip_variant() const { return clip_variant_; }
	const std::string &clip_file() const { return clip_file_; }
	const std::vector<PreviewClipEvent> &clip_events() const { return clip_events_; }
	// The playing clip's length in ticks (-1 unknown) and whether it loops.
	int32_t clip_length_ticks() const;
	bool clip_loops() const;
	// The clip's frame at the clock's ticks (loops wrap, a one-shot holds its end).
	double clip_frame(const PreviewClock &clock) const;
	// The first tick the clip's clock runs on `frame` (-1 when it never does), where its event fires
	// in the game (anim::ClipTimeline::first_ticks).
	int32_t tick_of_frame(int frame) const;

	// What a canvas maps of it in a frame (model_canvas.h): its markers at the clock and, while the
	// model is the active document, the selected records' markers.
	ModelCanvasFrame canvas_frame(const ViewportContext &context) const;
	// The record a marker is while the picture shows the document as it is now (none otherwise).
	NodeAddress record_of(const ViewportInput &input, const ModelOverlay &overlay) const;

	ViewportStatus status() const override;
	const char *reason() const override { return model_view_status_token(reason_); }
	std::string message() const override { return model_view_status_message(reason_, detail_); }
	const std::string &detail() const override { return detail_; }
	std::string caption() const override;
	const char *units() const override { return "pixels"; }
	ViewportLayout layout() const override { return ViewportLayout(); }
	std::unique_ptr<CanvasHalf> make_canvas() const override;
	ViewportHit hit(const ViewportContext &context, float x, float y) const override;
	bool drag(const ViewportContext &context, const ViewportDrag &drag, CanvasRequests &out,
			std::string &error) const override;
	bool command(const ViewportContext &context, const std::string &name,
			const std::vector<NodeId> &ids, CanvasRequests &out, std::string &error) const override;
	io::JsonValue options_json() const override;
	io::JsonValue camera_json() const override;
	io::JsonValue body_json(const ViewportInput &input) const override;
	io::JsonValue items_json(const ViewportInput &input) const override;

protected:
	ViewportAction follow_(const ViewportInput &input, PreviewClock &clock) override;
	bool takes_(const std::string &member) const override;
	bool check_(const io::JsonValue &json, std::string &error) const override;
	void apply_(const io::JsonValue &json, PreviewClock &clock) override;
	void report_(const ViewportDeviceReport &report) override;

private:
	ViewportAction stop_(ModelViewStatus reason, const std::string &detail, bool failed);
	ViewportAction follow_model_(const ViewportInput &input, const ModelDocument &document);
	ViewportAction follow_animation_(const ViewportInput &input, const Document &document,
			PreviewClock &clock);
	void reset_animation_();
	// The camera looks at the whole model.
	void frame_();

	ModelViewportOptions options_;
	bool options_moved_ = false;
	OrbitCamera camera_;
	ModelViewStatus reason_ = ModelViewStatus::NoProject;
	std::string detail_;
	PreviewFollow picture_;
	uint64_t reads_ = 0;
	uint64_t drawn_key_ = 0; // what the device draws, the user points aside (0: no scene of model_)
	assets::Model model_;
	std::string framed_; // the model the camera last framed
	// The animation's.
	bool animating_ = false;
	PreviewRig rig_;
	std::string model_file_; // the rig's model the scene is built of
	FileStamps model_read_; // its stamp as read
	bool model_failed_ = false; // the rig's model did not read
	uint64_t rig_model_reads_ = 0;
	std::string unwritable_; // why the clip or table cannot be written ("" it can)
	std::shared_ptr<const anim::SkeletalClips> skeleton_;
	FileStamps rig_read_; // what the rig read
	uint64_t generation_ = 0; // the files' generation the rig was checked at
	uint64_t skeleton_serial_ = 0;
	std::string clip_key_;
	std::string clip_file_;
	int clip_variant_ = 0;
	std::vector<PreviewClipEvent> clip_events_;
	NodeId sought_event_ = 0; // the event record the clock last sought
};

} // namespace opennova::editor
