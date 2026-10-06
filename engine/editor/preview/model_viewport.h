#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <editor/model/edit.h>
#include <editor/preview/model_collision.h>
#include <editor/preview/model_damage.h>
#include <editor/preview/model_handle_edit.h>
#include <editor/preview/model_overlay.h>
#include <editor/preview/model_preview_camera.h>
#include <editor/preview/model_preview_rig.h>
#include <editor/preview/preview_clip_sounds.h>
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
	// How a clip's events are heard (DI-04, preview/preview_clip_sounds): no picture changes with it.
	ClipSoundOptions sound;
	// The damage state a model is drawn in (DI-10, preview/model_damage): intact, or destroyed as an item
	// naming it is, the death at the preview clock's tick 0.
	DamageOptions damage;
	bool operator==(const ModelViewportOptions &other) const {
		return lod == other.lod && ctrl == other.ctrl && overlays == other.overlays &&
				rig_model == other.rig_model && repeat == other.repeat && bones == other.bones && snap == other.snap &&
				sound == other.sound && damage == other.damage;
	}
	bool operator!=(const ModelViewportOptions &other) const { return !(*this == other); }
};

// The change a SetViewport makes to set a model viewport's camera to `camera` (its target, yaw,
// pitch and distance): what an orbit, a pan, a dolly or a framing on its canvas sends.
std::string model_camera_change(const OrbitCamera &camera);
// The options on the wire (the envelope's `options`, a SetViewport's): {lod ("auto" or a level),
// ctrl {register: value}, overlays {user_points, lights, pivots}, rig_model, repeat, bones, snap,
// sound {mute, surface, body, female, profile}, damage {state, item}}.
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
	// The camera looking at the whole model on a picture `width` x `height` (from its angles now): the model
	// as the clip playing poses it over its frames where one plays (posed_sphere), else as it stands.
	OrbitCamera framed(int width, int height) const;
	// The sphere the rig's model takes over every frame of the clip playing (a first-person clip poses
	// its rig away from the model's own sphere: the 357's, 0.9 m off): the model's sphere carried by each
	// bone's deform at each frame, since each part rides one bone rigidly. False when no clip plays.
	bool posed_sphere(PreviewVec3 &center, float &radius) const;
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

	// A clip's sounds (DI-04, preview/preview_clip_sounds): the project's files they play from, the profile
	// and the body they play through, the clip's event words and capsule bottoms as its channel reads
	// them, and the sounds its events fired as the clock ran it, the last kSoundsFiredKept, oldest first.
	const ClipSoundSources &sound_sources() const { return sound_sources_; }
	const ClipSoundBinding &sound_binding() const { return sound_binding_; }
	const ClipSoundTrack &clip_track() const { return clip_track_; }
	static constexpr size_t kSoundsFiredKept = 16;
	const std::vector<ClipSoundFired> &sounds_fired() const { return fired_; }
	// What an event of the clip plays under the sound options, a line a sound, nothing picked (the
	// timeline's hover).
	std::vector<std::string> event_sound_words(uint32_t trigger) const;
	// The sounds the clip's events fired over the ticks the clock ran through since the last call
	// (clip_events_due: never over a seek, a clip newly chosen, a pause; from the clip's tick of each, a
	// repeated one-shot's taken again from 0), each planned through `selector` (plan_clip_event) and
	// numbered from `next_seq` on, kept with the last ones fired, and returned. Nothing while no clip
	// plays. Each voice's wave is the project's file of its name (`scan`, find_clip_sound_waves).
	std::vector<ClipSoundFired> fire_sounds(const PreviewClock &clock, const AssetScan *scan,
			audio::SoundSelector &selector, uint64_t &next_seq);
	// The sounds the clip's event at `frame` plays, once, as a press of its mark on the timeline asks
	// (play_sound {frame}); false, with why, for no clip playing, a frame the game never reads (the end
	// pose, a frame the clock steps over) and a frame that fires no sound.
	bool press_event(int frame, const AssetScan *scan, audio::SoundSelector &selector,
			std::vector<ClipSoundFired> &out, std::string &error) const;

	// The model's damage states (DI-10, preview/model_damage), for a model document: the items naming it as
	// a graphic or a husk, the one the options choose (the first when they name none or one not among
	// them) and its role, the item as its catalog reads, its death as the game runs it, the state the game
	// draws at the clock (the death at tick 0), and the husk file drawn in the model's place ("" the model
	// itself) with why where the destroyed state draws otherwise ("": nothing to say).
	const std::vector<DamageUse> &damage_uses() const { return damage_uses_; }
	const DamageUse *damage_use() const;
	const DamageItem &damage_item() const { return damage_item_; }
	const DamagePlan &damage_plan() const { return damage_plan_; }
	DamageFrame damage_frame_at(const PreviewClock &clock) const;
	const std::string &damage_drawn() const { return damage_drawn_; }
	const std::string &damage_note() const { return damage_note_; }
	// Whether the destroyed state plays a death (an item chosen whose class dies: its legs fire on the clock,
	// its sounds play), and whether it drives what the picture shows (the death swaps in a husk the project
	// holds, and the picture is that husk: the graphic swapped, or the husk itself).
	bool damage_playing() const;
	bool damage_driven() const;
	// The CTRL registers the picture reads at `clock` (what the device holds, the overlays and the collision
	// pose by): the options' held registers, the six destroy-fade registers the game's in their place while
	// the destroyed state drives the picture (zero, unheld, before the fade). The sections of the picture
	// left as death pieces (hidden), likewise.
	std::map<std::string, int64_t> ctrl_at(const PreviewClock &clock) const;
	uint32_t hidden_sections_at(const PreviewClock &clock) const;
	// The death's sounds over the ticks the clock ran through since the last call, as fire_sounds fires a
	// clip's (never over a seek; a leg on the tick a seek lands on fires as the clock runs from it, so Play
	// destroy, a seek to 0, hears the death), each set played at the item as the camera hears it, kept with
	// the clip's (sounds_fired) and returned. Nothing unless the destroyed state drives the picture.
	std::vector<ClipSoundFired> fire_damage_sounds(const PreviewClock &clock, const AssetScan *scan,
			audio::SoundSelector &selector, uint64_t &next_seq);

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
	const FileStamps *picture_reads() const override { return &picture_.files(); }
	const char *units() const override { return "pixels"; }
	ViewportLayout layout() const override { return ViewportLayout(); }
	std::unique_ptr<CanvasHalf> make_canvas() const override;
	ViewportHit hit(const ViewportContext &context, float x, float y) const override;
	// A click (ViewportModel::click, its canvas driven: the marker there, else a joint of the clip's rig, its
	// bone selected in the clip, else the collision shape): a click that replaces alone (Shift on the canvas
	// pans, and it has no Ctrl click), at the picture's size.
	bool click_frame(const ViewportContext &context, SelectMode mode, int &width, int &height,
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
	bool report_(const ViewportDeviceReport &report) override;

private:
	// What a drag of the record `id` by its handle `token` holds: the handle and the record's marker
	// as the picture shows it now. False, with why: a handle no marker has, a picture that is not the
	// model as it is now, a record that is no marker of it.
	bool dragged_marker_(const ViewportContext &context, NodeId id, const std::string &token, ModelHandle &handle,
			ModelOverlay &marker, std::string &error) const;
	ViewportAction stop_(ModelViewStatus reason, const std::string &detail, bool failed);
	ViewportAction follow_model_(const ViewportInput &input, const ModelDocument &document, const PreviewClock &clock);
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
	// The damage state's follow (DI-10): the uses again when the graph moves, the item when its catalog's
	// stamp moves, the husk models when theirs do, then the plan; the husk file the picture draws at the
	// clock ("" the document). Returns that file.
	std::string follow_damage_(const ViewportInput &input, const PreviewClock &clock);
	void reset_damage_();
	// The envelope's `damage` (DI-10): the uses, the chosen item and its role, its death as the game runs
	// it, and the state the game draws at the clock.
	io::JsonValue damage_json_(const ViewportInput &input) const;
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
	std::string framed_clip_;   // the rig's model, clip and variant the animation's camera last followed
	OrbitCamera framed_camera_; // where the last framing put the camera
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
	// The clip's sounds (DI-04): the event words it reads, its sources and binding, what fired, and the
	// clock's tick the sounds were last fired to (-1: none since the clip was chosen) and its seeks then.
	ClipSoundTrack clip_track_;
	ClipSoundSources sound_sources_;
	ClipSoundBinding sound_binding_;
	std::vector<ClipSoundFired> fired_;
	int32_t sound_cursor_ = -1;
	uint64_t sound_seeks_ = 0;
	// The damage state's (DI-10): the uses and the graph they were read from, the chosen use, the item and
	// the catalog stamp it was read at, the husk models read (each by its file and stamp, a model that does
	// not read latched until its stamp moves), the plan, the husk file drawn and why, and the death sounds'
	// cursor (as the clip sounds').
	std::vector<DamageUse> damage_uses_;
	const void *damage_graph_ = nullptr;
	uint64_t damage_generation_ = 0;
	size_t damage_use_ = SIZE_MAX;
	std::string damage_catalog_;
	std::string damage_record_;
	uint64_t damage_catalog_stamp_ = 0;
	DamageItem damage_item_;
	struct DamageModel {
		std::string file; // the scan's file name
		uint64_t stamp = 0;
		bool read = false;
		assets::Model model;
	};
	DamageModel husk_model_;
	DamageModel piece_model_;
	DamageModels damage_models_;
	DamagePlan damage_plan_;
	bool damage_drives_ = false;
	std::string damage_drawn_;
	std::string damage_note_;
	int32_t damage_cursor_ = -1;
	uint64_t damage_seeks_ = 0;
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
