#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <base/vfs/file_source.h>
#include <editor/model/edit.h>
#include <editor/preview/model_handle_edit.h>
#include <editor/preview/model_overlay.h>
#include <editor/preview/model_preview_camera.h>
#include <editor/preview/model_preview_rig.h>
#include <runtime/assets/asset_store.h>

namespace opennova::editor {

class Document;
class ModelDocument;
struct SessionView;

// What the model preview can show, and why not (ADR 0046 S10p2, S10p6).
enum class ModelPreviewStatus : uint8_t {
	NoProject,      // no project is open
	NoModel,        // no model, clip or table is open to preview
	NoDevice,       // no renderer is attached (an engine-only run)
	Unserializable, // the model, clip or table cannot be written, so the game could not read it
	Unreadable,     // what it writes does not read back (or the rig's model does not read)
	NoRig,          // an animation no model plays: no item pairs its table with a graphic
	Ready,
};
// "no_project", "ready", ...: the token the preview JSON carries.
const char *model_preview_status_token(ModelPreviewStatus status);
// The line the preview shows for a status (`detail`: the first serialize issue, the
// animation's file).
std::string model_preview_status_message(ModelPreviewStatus status, const std::string &detail);

// How the preview draws the model: the level (Auto: the one the game draws at the camera's
// distance), the CTRL registers held at a value (a register not held reads 0, as a model
// with no entity driving it does), whether its clock runs (part animations, flipbooks,
// generators, a clip), what the overlays mark, and the model an animation plays on when
// the author picks one (a model's file name; "" the one an item pairs with the table).
struct ModelPreviewOptions {
	int lod = -1; // -1 Auto
	std::map<std::string, int64_t> ctrl;
	bool playing = true;
	ModelOverlayOptions overlays;
	std::string rig_model;
	bool operator==(const ModelPreviewOptions &other) const {
		return lod == other.lod && ctrl == other.ctrl && playing == other.playing && overlays == other.overlays &&
		       rig_model == other.rig_model;
	}
	bool operator!=(const ModelPreviewOptions &other) const { return !(*this == other); }
};

// A file read, with its stamp when it was read.
struct FileStamp {
	std::string name;
	uint64_t stamp = 0;
};

// A file set that remembers every name read through it (each once, with the stamp it had
// then): what the device read while it built the model (its textures), what a rig read (its
// table and clips), so the preview builds again when one of them changes.
class StampedFiles : public FileSource {
public:
	explicit StampedFiles(std::shared_ptr<const FileSource> files) : files_(std::move(files)) {}
	bool read(const std::string &name, std::vector<uint8_t> &out) const override;
	uint64_t stamp(const std::string &name) const override;
	const std::vector<FileStamp> &read_names() const { return read_; }
	// True when a name read has another stamp in `files` now.
	bool moved(const FileSource &files) const;

private:
	void note_(const std::string &name, uint64_t stamp) const;

	std::shared_ptr<const FileSource> files_;
	mutable std::vector<FileStamp> read_;
};

// What the preview's device does after a follow().
enum class ModelPreviewAction : uint8_t {
	Keep,    // nothing changed that it draws
	Rebuild, // open model() again (a new scene: its meshes, materials and textures), then apply the options
	Update,  // the drawn model is the same: apply the options (the level, the registers, the rig);
	         // what changed only the overlays show (a user point)
	Clear,   // drop what it built (the status says why there is nothing)
};

// The model preview's portable half, the device's guide: which model the view previews
// (SessionView::model_preview: an open model document, or an open clip or table played on
// its rig's model), the model the game would read were it saved now (a model document's
// serialize(), read back; a rig's model as the project's files hold it), when to build it
// again (a revision that changes what is drawn, or a file the last build read moved its
// stamp), the camera, the level to draw, and for an animation the rig (the table over the
// model's bone table, through the game's loader) and the clip the selection plays on the
// preview's tick clock. A model that cannot be written or read keeps its status until it
// changes (no retry every frame).
class ModelPreviewModel {
public:
	ModelPreviewAction follow(const SessionView &view);
	// The device built model() reading its textures through `files` (it keeps reading
	// through them: a flipbook frame loads when first drawn).
	void built(std::shared_ptr<const StampedFiles> files) { built_files_ = std::move(files); }
	// Options apply on the next follow (an Update).
	void set_options(const ModelPreviewOptions &options);
	const ModelPreviewOptions &options() const { return options_; }
	// The device's size in pixels (what Auto and the overlays project across).
	void set_device_size(int width, int height);
	int device_width() const { return width_; }
	int device_height() const { return height_; }

	OrbitCamera &camera() { return camera_; }
	const OrbitCamera &camera() const { return camera_; }
	// The camera looks at the whole model again.
	void frame();

	ModelPreviewStatus status() const { return status_; }
	const std::string &detail() const { return detail_; }
	// The model the device shows (null unless ready).
	const assets::Model &model() const { return model_; }
	// The document the preview follows: its path and revision (a clip or a table: the
	// animation's, the model its rig's).
	const std::string &shown_path() const { return shown_path_; }
	uint64_t shown_revision() const { return shown_revision_; }
	// How many times the device was told to build (an edit that changes only what the
	// overlays show is not one).
	uint64_t builds() const { return builds_; }
	// The level to draw: the options', clamped to the model's levels, else Auto's.
	int lod() const;
	int auto_lod(int32_t *projected_q16 = nullptr) const;

	// The preview's one clock, the device's part animations and the overlays both read it:
	// it runs while the options play (advance), and a seek sets it. A clip plays on the same
	// clock in game ticks (io::kTickHz).
	void advance(double seconds);
	void seek(uint32_t time_ms) { clock_ms_ = time_ms; }
	uint32_t clock_ms() const { return clock_ms_; }
	void seek_ticks(int32_t ticks);
	int32_t clip_ticks() const { return ticks_; }
	// What the overlays mark on the model as drawn now (the level, the clock, the held
	// registers); empty unless ready.
	std::vector<ModelOverlay> overlays() const;
	// How long a marker's axis is drawn (its tip is the Axis handle): a share of the
	// camera's distance, so it keeps its size on the device.
	float axis_length() const { return camera_.distance * 0.08f; }
	// Where the tip of a marker's axis is.
	PreviewVec3 axis_tip(const ModelOverlay &overlay) const;
	// A drag of `overlay`'s handle to device pixel (x, y): the point under it on the plane
	// through the handle that faces the eye, planned into the record's edits
	// (model_handle_edits) at the clock, level and held registers drawn now.
	bool handle_edits(const ModelDocument &document, const ModelOverlay &overlay, ModelHandle handle, float x, float y,
	                  float snap, uint64_t gesture, std::vector<Edit> &out) const;

	// An animation's preview (S10p6): true while the followed document is a clip or a table.
	bool animating() const { return animating_; }
	const PreviewRig &rig() const { return rig_; }
	// The rig the device binds (null when it does not load; `detail` then says why), and a
	// serial that moves whenever it is rebuilt.
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
	// The clip's frame at the clip clock (loops wrap, a one-shot holds its end).
	double clip_frame() const;
	// The first tick the clip's clock runs on `frame` (-1 when it never does), where its
	// event fires in the game (anim::ClipTimeline::first_ticks).
	int32_t tick_of_frame(int frame) const;

private:
	ModelPreviewAction stop_(ModelPreviewStatus status, const std::string &detail);
	ModelPreviewAction follow_model_(const SessionView &view, const ModelDocument &document);
	ModelPreviewAction follow_animation_(const SessionView &view, const Document &document);
	void reset_animation_();

	ModelPreviewOptions options_;
	bool options_moved_ = false;
	ModelPreviewStatus status_ = ModelPreviewStatus::NoProject;
	std::string detail_;
	bool device_built_ = false; // the device holds a model
	bool failed_ = false;       // the shown revision could not be written or read
	uint64_t shown_identity_ = 0, shown_revision_ = 0;
	std::string shown_path_;
	std::string framed_path_; // the model the camera last framed
	uint64_t generation_ = 0;
	uint64_t drawn_key_ = 0; // what the device draws, the user points aside
	uint64_t builds_ = 0;
	assets::Model model_;
	std::shared_ptr<const StampedFiles> built_files_;
	OrbitCamera camera_;
	int width_ = 800, height_ = 600;
	uint32_t clock_ms_ = 0;
	double clock_carry_ = 0.0; // the fraction of a millisecond the clock has not taken yet
	// The animation's preview.
	bool animating_ = false;
	PreviewRig rig_;
	std::string model_file_;  // the rig's model the device holds
	uint64_t model_stamp_ = 0; // its stamp when read
	std::shared_ptr<const anim::SkeletalClips> skeleton_;
	std::shared_ptr<const StampedFiles> rig_read_; // what the rig read
	uint64_t skeleton_serial_ = 0;
	std::string clip_key_, clip_file_;
	int clip_variant_ = 0;
	std::vector<PreviewClipEvent> clip_events_;
	std::string unwritable_; // why the followed clip or table cannot be written ("" it can)
	int32_t ticks_ = 0;
	double tick_carry_ = 0.0;
	NodeId sought_event_ = 0; // the event record the clock last sought
};

} // namespace opennova::editor
