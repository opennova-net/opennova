#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <base/io/json.h>
#include <editor/model/change_set.h>
#include <editor/model/node.h>
#include <editor/preview/canvas_gesture.h>
#include <editor/preview/preview_clock.h>
#include <editor/preview/viewport_build_report.h>
#include <editor/preview/viewport_follow.h>
#include <editor/preview/viewport_kinds.h>

namespace opennova::editor {

class CanvasHalf;
class DocumentBase;
class ViewportDevice;
struct JsonPage;
struct SessionView;
struct ViewEvent;
struct ViewportDeviceReport;

// What a viewport shows (ADR 0046 S13 V5): a picture of its document (Ready); nothing, there being
// nothing to show (Empty: no project, no document of its kind, no screen selected, an animation no
// model plays); or nothing, the game being unable to read what it should show (Failed: a document
// that cannot be written, a screen missing from what it writes, a model that does not read back).
// The kind's reason says which. Its picture is Loading too (S13 V6) while its device builds the
// picture over the Shell's frames, the last picture drawn meanwhile (picture_status, never a kind's
// status()), and Failed when its device could not build it.
enum class ViewportStatus : uint8_t { Empty, Failed, Ready, Loading };
// "empty", "failed", "ready", "loading".
const char *viewport_status_token(ViewportStatus status);

// The JSON text of a SetViewport that changes one member of a viewport of `kind`: {"kind": its
// token, member: value} (a canvas's camera, a toolbar's options, the clock, the device's size).
std::string viewport_change(ViewportKind kind, const char *member, io::JsonValue value);

// A SetViewport's clock member, {playing, rate, time_ms, ticks} each optional, set on `clock`: every
// member checked before any applies; false, nothing changed, with `error` naming the member and what
// it takes (a viewport's change applies it with its other members, ViewportModel::apply; a pathless
// change of the clock alone, Viewports::set_clock).
bool set_preview_clock(const io::JsonValue &json, PreviewClock &clock, std::string &error);

// What changed in a viewport's document since the viewport last followed it (Viewports::follow: the
// document's identity and load, and what it answers changed since the revision followed,
// DocumentBase::changes_since; ADR 0046 S13 V8). A kind classifies a change set by what its picture
// reads of the document (a menu's screen, a model's drawn rows); Unknown and Loaded are everything.
enum class ChangeClass : uint8_t {
	None, // the document is as it was
	Changed, // edited, undone or redone, and it says what changed (ViewportInput::changes)
	Unknown, // it changed and cannot say what (the "all" answer): another document at the path or
			 // the same one read again, a state its history no longer holds (given up to its
			 // budget, or a branch an edit after an undo discarded), a kind that does not say
	Loaded, // followed the first time, or no document is open at the path
};

// A viewport's own state beside its kind's (a menu's options, a model's options and camera): the
// size its device draws its picture at where no canvas sizes the picture (a headless Shell's, the
// menu's Device size), in pixels. Every change a person or a client makes to a viewport's state (a
// toolbar's options, a canvas's camera, the clock's play, pause and seek, the device's size) is a
// SetViewport request (apply). Three changes alone are derived instead, by the viewport's follow,
// from its document, the selection and a changed model or clip: a menu's held window following the
// selection (what its type cannot hold let go), a model's camera framed when another model first
// shows, and the preview clock sought (and held) when the clip the selection plays changes or a clip
// event is selected. Nothing else moves a viewport's state between two follows with no request, and
// each of these (a SetViewport, a derived change) moves the view's Viewports concern.
struct ViewportState {
	int width = 0;
	int height = 0;
};

// What a viewport reads of the session (Viewports::follow, a canvas's planning, the wire): the view,
// the preview clock (Viewports'), the document at its path (null: none open there), and what changed
// in it since the viewport last followed, with the change set when the class is Changed (null
// otherwise; a canvas's planning and the wire read None: they read the viewport as it followed).
struct ViewportInput {
	const SessionView &view;
	const PreviewClock &clock;
	const DocumentBase *document = nullptr;
	ChangeClass change = ChangeClass::None;
	const ChangeSet *changes = nullptr;
};

// What a viewport's planners read (a canvas's gestures, the MCP's drag and command): the input, the
// size the canvas draws the picture at (pixels; the state's where no canvas draws), the grid a drag
// snaps to (a menu's 8-unit grid where it is not 0, a model's grid in metres; 0 free), and the device
// drawing it (null: none; what a drop or a move lands on, ViewportDevice::surface_between and
// ground_at).
struct ViewportContext {
	ViewportInput input;
	int width = 0;
	int height = 0;
	float snap = 0.0f;
	const ViewportDevice *device = nullptr;
	// An edit is planned only where the session takes one now: no running operation refuses an edit
	// (S13 A3, SessionView::allows), and the document is open and not blocked.
	bool editable() const;
	// Why no edit is planned ("" when editable): the document blocked (its file holds what it cannot
	// carry), or an operation that holds the documents running.
	std::string not_editable() const;
};

class ViewportModel;
// What a planner reads of a viewport the session keeps, with no canvas drawing it (the viewport
// query's hit, an EditInViewport's drag and command, a test): the view, the preview clock, the
// document open at its path, as it followed (ChangeClass None), at the size its device draws at
// (ViewportModel::size), snapped by `snap`, over the device the Shell holds for it (Viewports::devices,
// read and not used; none in a session with no Shell).
ViewportContext viewport_context(const SessionView &view, const ViewportModel &model, float snap = 0.0f);

// How a canvas lays the picture out: a design picture `design_width` x `design_height` (a menu's
// 800 x 600, which the canvas fits, zooms and scrolls), or with none a picture filling the canvas
// (a model's, whose camera zooms and pans).
struct ViewportLayout {
	int design_width = 0;
	int design_height = 0;
};

// What lies under a point of the picture (the MCP's hit): the item's index (a menu's widget in the
// compiled screen's pre-order, a model's marker among its overlays; -1 none), its record while the
// picture is the document's own (0 otherwise), its name, its kind (a menu window's type, a model
// marker's kind token), and whether the picture is current.
struct ViewportHit {
	int index = -1;
	NodeId id = 0;
	std::string name;
	std::string kind;
	bool current = false;
};

// A viewport (ADR 0046 S13 V5; CONTEXT.md "Viewport"): one document's picture as the game would
// draw it, of one kind (viewport_kinds.h), kept by the session (Viewports) while its document is
// open and shared const on the view, so the windows read it and never change it. Its state (the
// device's size, the kind's options and camera) changes by a SetViewport request (apply) and by the
// three changes its follow derives (ViewportState's), nothing else; what follows from it (its
// status, what its picture is made from, the action its device takes next) is made by follow, which
// the session's devices drive
// (ViewportDeviceCache). A device attached to it takes its actions (take_action) and reports what it
// read (device_report); the device's picture is drawn only by a canvas, which owns input and plans
// every change as a request through the kind's half of it (make_canvas).
class ViewportModel {
public:
	virtual ~ViewportModel();
	ViewportModel(const ViewportModel &) = delete;
	ViewportModel &operator=(const ViewportModel &) = delete;

	ViewportKind kind() const { return kind_; }
	const ViewportKindRow &row() const { return viewport_kind_row(kind_); }
	// The document it shows, by its project-relative path.
	const std::string &path() const { return path_; }
	const ViewportState &state() const { return state_; }
	// The size its picture is drawn at now, in pixels: its device's, as the device last reported it (a
	// canvas's size where a canvas sizes the picture), else its state's (no device yet).
	ViewportState size() const;
	// A canvas draws its picture at a size of the canvas's own this frame (the device's report): its
	// device's size is then set by no SetViewport.
	bool canvas_sized() const { return canvas_sized_; }

	// --- what it shows ---------------------------------------------------------------------------

	virtual ViewportStatus status() const = 0;
	// The kind's reason (its token on the wire: "no_menu", "unserializable", "ready", ...), the
	// sentence it shows for it ("" when ready) and its detail (the first reason the document cannot
	// be written, the missing screen's name, the animation's file).
	virtual const char *reason() const = 0;
	virtual std::string message() const = 0;
	virtual const std::string &detail() const = 0;
	// A serial that moves with every change of its state, a SetViewport's or one its follow derived
	// (Viewports reads it to say the Viewports concern moved).
	uint64_t state_serial() const { return state_serial_; }
	// The document state its picture shows (its revision; 0 before any), and whether that is the
	// document at its path as it is now: the picture maps a point to a record only then.
	uint64_t shown_revision() const { return shown_revision_; }
	bool current(const ViewportInput &input) const;
	// What the line naming it says after the file's name (" - STARTUP", a menu's screen; " on
	// skinned.3di", the model an animation plays on; "" for nothing).
	virtual std::string caption() const { return std::string(); }
	// The units hit and drag take: "design" (a menu's 800 x 600) or "pixels" (the picture's).
	virtual const char *units() const = 0;
	virtual ViewportLayout layout() const = 0;

	// --- what the canvas and the MCP plan over it, every change a request --------------------------

	// The kind's half of a canvas that draws it: what one canvas keeps between frames (the gesture
	// machine, what a press or a nudge took), made once per canvas.
	virtual std::unique_ptr<CanvasHalf> make_canvas() const = 0;
	virtual ViewportHit hit(const ViewportContext &context, float x, float y) const = 0;
	// Where the picture shows the record `id`'s handle `handle` now, in its units: a menu window's
	// edge or corner (a move's its top left corner, an edge's middle), a model marker's place or the
	// tip of its axis, projected. What a drag's `by` goes from. False, with why, where drag refuses
	// the record or the handle.
	virtual bool handle_point(const ViewportContext &context, NodeId id, const std::string &handle, float &x,
			float &y, std::string &error) const = 0;
	// A drag of a record's handle (the editor MCP's, EditInViewport: session/editor_request.h's
	// ViewportDrag) planned into requests: one EditRecord, the batch of the edits over every selected
	// record the drag moves, each carrying `drag.gesture` (a new one when 0), so the drags of one
	// gesture fold into one undo step; then the gesture's EndEdit when `drag.end` (with or without a
	// batch: a last drag that moves nothing still ends the gesture its sample names). `by` (x, y) from
	// where the picture shows the handle now (handle_point; a `by` of nothing plans no batch), else to
	// the point (x, y). False, with why, for a picture that is not the document's own, a record it
	// does not show, a handle the record has not, a drag that writes nothing the session would take.
	virtual bool drag(const ViewportContext &context, const ViewportDrag &drag, CanvasRequests &out,
			std::string &error) const = 0;
	// A command by its name over the records `ids` (the menu's arrange of windows, "align_left" ...;
	// the model's "frame"), planned into requests: false, with why, for a command the kind has not or
	// one it cannot plan.
	virtual bool command(const ViewportContext &context, const std::string &name,
			const std::vector<NodeId> &ids, CanvasRequests &out, std::string &error) const = 0;
	// A command whole (session/editor_request.h's ViewportCommand: its name and records, and what it
	// takes beside them, ADR 0046 S15: `by` a way, `at` a point of the picture), planned into requests:
	// by default the command of its name over its records, refusing a `by` or an `at` the kind does not
	// read (a mission's duplicate reads `by`, its paste `at`); "click" (the MCP gaps lane) at its `at`,
	// joined as its `mode` says, is click's, whatever the kind.
	virtual bool command_of(const ViewportContext &context, const ViewportCommand &command, CanvasRequests &out,
			std::string &error) const;
	// A click of the picture at (x, y) in its units, planned as the kind's canvas plans its click (a
	// press and a release with no drag between): the selection it makes, joined to the selection as
	// `mode` says (a Shift or a Ctrl click's), a select_record; a click on nothing what the canvas makes
	// of it (a mission's empties the selection; a menu's and a model's change nothing). False, with why,
	// for a kind with no canvas (the default) or a picture that is not its document's own now.
	virtual bool click(const ViewportContext &context, float x, float y, SelectMode mode, CanvasRequests &out,
			std::string &error) const;
	// A drop on the picture (S14, EditInViewport: session/editor_request.h's ViewportDrop: a project
	// file or a reference kind's name let go at a point of it) planned into requests: what it makes
	// there, one batch. False, with why: by default a kind takes no drop (a menu's, a model's), and
	// one that does refuses a thing it cannot place, a picture that is not its document's own, a
	// drop the session would not take.
	virtual bool drop(const ViewportContext &context, const ViewportDrop &drop, CanvasRequests &out,
			std::string &error) const;
	// What a box of the picture selects (S14: a marquee's records, the viewport query's box): the
	// items the box from (x0, y0) to (x1, y1) takes, in its units, as a canvas's marquee over it
	// takes them (a menu's windows it touches, a mission's marks whose anchors lie in it), each as
	// hit names one; none for a kind with no marquee (the default), or a picture that is not current.
	virtual std::vector<ViewportHit> box(const ViewportContext &context, float x0, float y0, float x1,
			float y1) const;

	// --- the wire's (viewport_json.h) ----------------------------------------------------------------

	virtual io::JsonValue options_json() const = 0;
	// The kind's camera (null: it has none).
	virtual io::JsonValue camera_json() const { return io::JsonValue::make_null(); }
	// What the kind adds (a menu's screen and the files it lacks, a model's level, registers and
	// animation), its items (a menu's widgets, a model's markers) and its notes (a menu's compiler
	// notes), the items and notes only while the picture is current.
	virtual io::JsonValue body_json(const ViewportInput &input) const = 0;
	virtual io::JsonValue items_json(const ViewportInput &input) const = 0;
	virtual io::JsonValue notes_json(const ViewportInput &input) const;
	// One row of its document as the kind renders it alone, whatever the picture shows (the viewport
	// query's render, S13 V7): a menu's screen by its row, as the render check compiled it (the
	// menu_render query's answer, a page of its widgets and notes). Null, with why, for a row it does
	// not render (by default: a kind whose picture is its whole document, a model's).
	virtual io::JsonValue render_json(
			const ViewportInput &input, NodeId row, const JsonPage &page, std::string &error) const;
	// What the kind's place tool places (the viewport query's palette, ADR 0046 S15): a mission's
	// items by name in their groups, those whose name, id or model holds `text`, a page of them. Null,
	// with why, for a kind that places nothing (by default).
	virtual io::JsonValue palette_json(
			const SessionView &view, const std::string &text, const JsonPage &page, std::string &error) const;

	// --- the device's -------------------------------------------------------------------------------

	bool attached() const { return attached_; }
	// How many times its device was told to make its picture again (an edit that changes only what
	// the overlays show is not one): the build generation (S13 V6), which the device builds as it takes
	// the Rebuild, so a newer one cancels a build in flight by its generation.
	uint64_t builds() const { return builds_; }
	// A picture made again waits for the gesture open in its document to end (S13 V8, a kind whose
	// row holds for a gesture: the model's scene): the device keeps the picture it holds meanwhile,
	// while what the viewport shows (its overlays) follows the live rows.
	bool held() const { return held_; }
	// Its device's build as the device last said (S13 V6; none without a device): the generation it
	// builds or built, whether it builds over the frames (loading) or failed, its progress.
	const ViewportBuildReport &build() const { return build_; }
	// What its picture is now, its envelope's status (S13 V6): the kind's status, but Loading while its
	// device builds the picture (the last one drawn meanwhile) and Failed when its device's build
	// failed (build().message why), each only where the kind's status is Ready.
	ViewportStatus picture_status() const;
	// picture_status()'s reason token (the kind's, or "loading", or "build_failed") and its sentence.
	const char *picture_reason() const;
	std::string picture_message() const;

	// --- the session's (Viewports) ------------------------------------------------------------------

	// Follow `input`: what it shows now, and the action its device takes next, merged into the one
	// its device has not taken yet (Rebuild over Update, a Clear that drops a picture the device
	// holds; a Clear of nothing is nothing). Where its kind's row holds for a gesture
	// (ViewportKindRow::holds_for_gesture), a Rebuild due while a gesture is open in its document
	// (the view's documents.gestures) over a picture the device holds is held, and issued at the
	// first follow after that gesture ends (another gesture begun since included); an Update applies
	// meanwhile (the state applied again over the picture that stands: cheap), as does a Clear, which
	// drops what is held, and a device holding no picture makes it at once. The action pending.
	ViewportAction follow(const ViewportInput &input, PreviewClock &clock);
	// What changed in its document as its last follow read it.
	ChangeClass followed_change() const { return followed_change_; }
	// A SetViewport's change (`json` an object: kind, device {width, height}, clock {playing, rate,
	// time_ms, ticks}, and the kind's own members): every member checked before any applies; false,
	// nothing changed, with `error` naming the member and what it takes. The device's size is refused
	// while a canvas sizes the picture (canvas_sized): it applies where none does (a headless Shell,
	// a menu at its Device size).
	bool apply(const io::JsonValue &json, PreviewClock &clock, std::string &error);
	// What its device does now (Keep: nothing; Keep while no device is attached), then Keep until a
	// follow says otherwise. A Rebuild is the next build generation (builds() moves with it, once per
	// Rebuild taken: never a second Rebuild for one generation). An Update applies to a picture its
	// device built (S13 V6): while the device builds one, the Update is folded into that build, which
	// applies the state as it ends, and after its build failed there is no picture of the newest
	// generation to apply it to (Keep: what the next Rebuild builds applies it).
	ViewportAction take_action();
	// A device is attached: it holds nothing yet, so its first action makes the picture (Rebuild,
	// when there is one to show); its state is kept. Detached (given up for another, ADR 0046 S13 V5:
	// the device cache's least recently used): nothing pending, its state kept for the next.
	void attach();
	void detach();
	// What the device read as it made its picture (its textures), where it placed what it drew, the
	// size its picture is now and its build; true when the build moved as the envelope reads it
	// (ViewportBuildReport::reads_same: begun over the frames, a unit further, built, failed; S13 V6,
	// the view's Viewports concern moves with it).
	bool device_report(const ViewportDeviceReport &report);
	// The device's build after a frame's steps (S13 V6): true when it moved, as device_report says.
	bool device_build(const ViewportBuildReport &build);
	// A view event about its document, as the session posted it (S13 V10: the session's viewports
	// hand each new one to the viewports of its path, Viewports::track and follow), held for the
	// next follow: a RevealText's place, which a script viewport's device shows and selects. Nothing
	// for a kind that takes none (the menu's and the model's: their windows take theirs).
	virtual void receive(const ViewEvent &event) { (void)event; }

protected:
	ViewportModel(ViewportKind kind, std::string path, ViewportState state);

	// The kind's follow: what it shows now, the action that comes to (a Clear when it shows
	// nothing).
	virtual ViewportAction follow_(const ViewportInput &input, PreviewClock &clock) = 0;
	// The members of a SetViewport the kind takes beside the common ones ("options", "camera").
	virtual bool takes_(const std::string &member) const = 0;
	// The kind's members checked (false, with `error`, for one it refuses), then applied.
	virtual bool check_(const io::JsonValue &json, std::string &error) const = 0;
	virtual void apply_(const io::JsonValue &json, PreviewClock &clock) = 0;
	// The device's report, the kind's part of it.
	virtual void report_(const ViewportDeviceReport &report) { (void)report; }
	// The document state the picture shows from now on.
	void shown(const DocumentBase &document);
	void shown_none();
	// The kind's follow derived a change of its state (viewport_model.h ViewportState: a held
	// window, a framing).
	void state_moved() { ++state_serial_; }

	ViewportState state_;

private:
	ViewportKind kind_;
	std::string path_;
	uint64_t shown_identity_ = 0;
	uint64_t shown_load_ = 0;
	uint64_t shown_revision_ = 0;
	bool shows_document_ = false;
	ViewportAction pending_ = ViewportAction::Keep;
	// A Rebuild held for the gesture `held_for_` (its token) to end.
	bool held_ = false;
	uint64_t held_for_ = 0;
	ChangeClass followed_change_ = ChangeClass::None;
	bool attached_ = false;
	bool holds_ = false; // the attached device holds a picture
	uint64_t builds_ = 0;
	ViewportBuildReport build_; // the attached device's build, as it last said
	uint64_t state_serial_ = 0;
	ViewportState shown_size_; // the device's picture, as it last reported it (0 x 0: none yet)
	bool canvas_sized_ = false;
};

} // namespace opennova::editor
