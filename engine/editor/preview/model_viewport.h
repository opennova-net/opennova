#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <editor/model/edit.h>
#include <editor/preview/model_collision.h>
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
	// An animation no model plays yet while the project's references are still being read (its first
	// validation runs): the pairing item may not be read yet (S17).
	Reading,
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
	// A clip's preview (ADR 0046 S17): a one-shot played again from its start after it ends and
	// holds its last frame a moment (the game plays it once; a loop loops either way), and the rig's
	// bones drawn over the picture, each named.
	bool repeat = true;
	bool bones = true;
	// The grid a canvas's drag of a marker's place snaps to on each of the file's axes (metres; 0 free: the
	// toolbar's Snap, kModelHandleSnaps; the MCP gaps lane), the canvas's alone: no picture changes.
	float snap = 1.0f / 16.0f;
	bool operator==(const ModelViewportOptions &other) const {
		return lod == other.lod && ctrl == other.ctrl && overlays == other.overlays &&
				rig_model == other.rig_model && repeat == other.repeat && bones == other.bones && snap == other.snap;
	}
	bool operator!=(const ModelViewportOptions &other) const { return !(*this == other); }
};

// The change a SetViewport makes to set a model viewport's camera to `camera` (its target, yaw,
// pitch and distance): what an orbit, a pan, a dolly or a framing on its canvas sends.
std::string model_camera_change(const OrbitCamera &camera);
// The options on the wire (the envelope's `options`, a SetViewport's): {lod ("auto" or a level),
// ctrl {register: value}, overlays {user_points, lights, pivots}, rig_model, repeat, bones, snap}.
io::JsonValue model_options_to_json(const ModelViewportOptions &options);

// How long a repeated one-shot holds its last frame before it plays again, in game ticks (half a
// second): the editor's aid, not the game's (the game plays a one-shot once).
inline constexpr int32_t kClipRepeatHoldTicks = 31;

// A model's viewport (ADR 0046 S10p2, S10p6, S13 V5; ViewportKind::Model): the model document at its
// path as it would save (its serialize(), read back), or a clip or an animation table played on its
// rig's model (the one an item pairs with the table, else the one the author picks; the table over
// the model's bone table through the game's loader), the clip the selection plays on the preview
// clock's ticks. Its own camera orbits the model (framed when a model first shows), and the level it
// draws is the options', else the one the game picks at the camera's distance (Auto). It builds
// again when its document changes what is drawn, when a file the device read moves its stamp (a
// texture), and its rig again when the rig or a file it read moves; a document the game could not
// read, and a rig model that does not read, keep their reason until they change (no retry every
// frame). What changed is the document's change set (S13 V8): one naming the model row alone, its
// versions alike but for their user points (alike_but_user_points), is the overlays' alone, the
// held model patched with the row's user points (no read) and the scene standing (an Update); any
// other change set the model read again and the scene built again; what the document cannot say
// (read again, a state its history no longer holds) read again and built again only when the drawn
// model moved, the user points aside (the written model's hash).
class ModelViewport final : public ViewportModel {
public:
	explicit ModelViewport(std::string path);
	static std::unique_ptr<ViewportModel> make(const std::string &path);

	ModelViewStatus view_status() const { return reason_; }
	const ModelViewportOptions &options() const { return options_; }
	const OrbitCamera &camera() const { return camera_; }
	// The model the device shows and the overlays mark (null unless ready): as read, or patched with
	// the document's user points since (patches).
	const assets::Model &model() const { return model_; }
	// How many times it read its document's bytes (wrote and read back the model, wrote the clip or
	// the table): a follow over a document that did not change reads none, nor does one whose change
	// only the overlays show (a patch).
	uint64_t reads() const { return reads_; }
	// How many times it patched the held model with its document's user points (S13 V8).
	uint64_t patches() const { return patches_; }
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
	// The collision the options show at `clock` (S17, preview/model_collision), and the record `also`
	// names whatever its layer (the selection's); none while an animation plays (its rig's sections ride
	// the skeleton, which the shapes do not pose). Made once per change of what they depend on (the model,
	// the level, the options, `also`, and the clock while a part they ride animates) and kept: a frame, a
	// hit and the items read the same shapes.
	ModelCollisionShapesPtr collision(const PreviewClock &clock, ModelCollisionPick also = ModelCollisionPick()) const;
	// The collision record the viewport's document has selected (none unless the picture is the document's
	// as it is now and the document is the active one): the shape drawn whatever its layer.
	ModelCollisionPick selected_collision(const ViewportInput &input) const;
	// What the toolbar's Frame names: the selected record where it has a marker or a collision shape (a
	// section that stores no sphere has none), else nothing (the whole model, as F frames it).
	std::vector<NodeId> frame_ids(const ViewportContext &context) const;
	// How many times the shapes were made (a test's measure of the cache).
	uint64_t collision_builds() const { return collision_cache_.builds; }
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
	// Why the clip playing is not the one selected, in words (a clip the project lacks, a row the
	// game skips, the reset clip played in an unauthored slot's place: preview_clip_choice); "".
	const std::string &clip_note() const { return clip_note_; }
	// The playing clip's length in ticks (-1 unknown) and whether it loops.
	int32_t clip_length_ticks() const;
	bool clip_loops() const;
	// Its frames (the header's count: the last frame is that count) and its rate (0: none plays).
	uint32_t clip_frame_count() const;
	uint32_t clip_fps() const;
	// The tick of the clip's clock the clock's ticks play (S17): the clock's ticks, a repeated
	// one-shot's taken again from 0 every length and hold (options' repeat, kClipRepeatHoldTicks); what
	// the device poses the skeleton at and the timeline shows.
	int32_t clip_ticks(const PreviewClock &clock) const;
	// The clip's frame at the clock's ticks (loops wrap, a one-shot holds its end).
	double clip_frame(const PreviewClock &clock) const;
	// The first tick the clip's clock runs on `frame` (-1 when it never does), where its event fires
	// in the game (anim::ClipTimeline::first_ticks).
	int32_t tick_of_frame(int frame) const;
	// The tick a step of `by` frames from the frame shown at `ticks` lands on: the next (or the
	// previous) frame the clock runs on, kept within the clip (S17's frame step).
	int32_t tick_of_step(int32_t ticks, int by) const;
	// The tick that shows `frame`: the first the clip's clock runs on it, else on the next frame it
	// runs on (a fast clip's steps pass over some), else the clip's end (-1: no clip plays). What a
	// SetViewport's `frame` (S17: `{"frame": 14}`) holds the clock on.
	int32_t tick_of_frame_shown(int frame) const;
	// The rig's bones as the playing clip poses them at the clock (empty when none plays).
	std::vector<PreviewJoint> joints(const PreviewClock &clock) const;

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
	// A click: the marker there, else the collision shape the pixel is on, its record selected as the
	// canvas's click selects it (joined as `mode` says); on nothing, nothing changes.
	bool click(const ViewportContext &context, float x, float y, SelectMode mode, CanvasRequests &out,
			std::string &error) const override;
	bool handle_point(const ViewportContext &context, NodeId id, const std::string &handle, float &x, float &y,
			std::string &error) const override;
	bool drag(const ViewportContext &context, const ViewportDrag &drag, CanvasRequests &out,
			std::string &error) const override;
	// The frame command: the camera on the marker or the collision shape of the first record named (one
	// that is neither refused), else on the whole model.
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
	// What a drag of the record `id` by its handle `token` holds: the handle and the record's marker
	// as the picture shows it now. False, with why: a handle no marker has, a picture that is not the
	// model as it is now, a record that is no marker of it.
	bool dragged_marker_(const ViewportContext &context, NodeId id, const std::string &token, ModelHandle &handle,
			ModelOverlay &marker, std::string &error) const;
	ViewportAction stop_(ModelViewStatus reason, const std::string &detail, bool failed);
	ViewportAction follow_model_(const ViewportInput &input, const ModelDocument &document);
	// Whether what changed since the last follow is the overlays' alone (S13 V8): a change set
	// naming the model row alone, whose version now is alike but for its user points to the one the
	// held model holds, or naming nothing.
	bool overlays_alone_(const ViewportInput &input, const ModelDocument &document) const;
	// The held model with the document's user points: what was read, everything but its user
	// points, which are the model row's as it is now. False, nothing patched, when the held model
	// holds that version of the row already (a change set that names nothing).
	bool patch_user_points_(const ModelDocument &document);
	// The hash of the held model as drawn (written without its user points), taken once and kept with
	// it: what a model read for a change the document cannot say (ChangeClass::Unknown) is compared
	// with, written once itself. False when the held model does not write.
	bool held_drawn_hash_();
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
	uint64_t patches_ = 0;
	bool scene_ = false; // the device is told to build a scene of model_ (or of the rig's model)
	assets::Model model_; // what the device draws and the overlays mark: read_, or read_ patched
	assets::Model read_; // the model as last read (a model document's written and read back)
	std::shared_ptr<const Node> read_row_; // the model row's version model_ holds
	// The hash of read_ written without its user points (drawn_hashed_: taken), held_drawn_hash_'s.
	uint64_t drawn_hash_ = 0;
	bool drawn_hashed_ = false;
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
	std::string clip_note_;
	NodeId sought_event_ = 0; // the event record the clock last sought
	// The collision shapes last made and what they were made for.
	struct CollisionCache {
		const void *model = nullptr;
		int lod = -2;
		ModelOverlayOptions overlays;
		std::map<std::string, int64_t> ctrl;
		ModelCollisionPick also;
		uint32_t time_ms = 0;
		ModelCollisionShapesPtr shapes;
		uint64_t builds = 0;
	};
	mutable CollisionCache collision_cache_;
};

} // namespace opennova::editor
