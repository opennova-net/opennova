#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <base/io/json.h>
#include <editor/model/node.h>
#include <editor/preview/canvas_gesture.h>
#include <editor/preview/preview_clock.h>
#include <editor/preview/viewport_follow.h>
#include <editor/preview/viewport_kinds.h>

namespace opennova::editor {

class CanvasHalf;
class DocumentBase;
class ViewportDevice;
struct SessionView;
struct ViewportDeviceReport;

// What a viewport shows (ADR 0046 S13 V5): a picture of its document (Ready); nothing, there being
// nothing to show (Empty: no project, no document of its kind, no screen selected, an animation no
// model plays); or nothing, the game being unable to read what it should show (Failed: a document
// that cannot be written, a screen missing from what it writes, a model that does not read back).
// The kind's reason says which.
enum class ViewportStatus : uint8_t { Empty, Failed, Ready };
// "empty", "failed", "ready".
const char *viewport_status_token(ViewportStatus status);

// The JSON text of a SetViewport that changes one member of a viewport of `kind`: {"kind": its
// token, member: value} (a canvas's camera, a toolbar's options, the clock, the device's size).
std::string viewport_change(ViewportKind kind, const char *member, io::JsonValue value);

// What changed in a viewport's document since the viewport last followed it (Viewports::follow,
// from the document's identity, load and revision; S13 V8 reads the document's changes_since to say
// more).
enum class ChangeClass : uint8_t {
	None, // the document is as it was
	Unknown, // edited, undone or redone: what changed is not said
	Loaded, // another document at the path, the same one read again, or followed the first time
};

// A viewport's own state beside its kind's (a menu's options, a model's options and camera): the
// size its device draws its picture at, in pixels (a menu's drawn at a size of its own, a model's
// at the size its canvas draws, which its camera projects to). It changes only through a
// SetViewport request (apply) and the kind's own rules as it follows (a menu's held window following
// the selection, a model framed when it first shows).
struct ViewportState {
	int width = 0;
	int height = 0;
};

// What a viewport reads of the session (Viewports::follow, a canvas's planning, the wire): the view,
// the preview clock (Viewports'), the document at its path (null: none open there), and what changed
// in it since the viewport last followed (a canvas's planning and the wire read None: they read the
// viewport as it followed).
struct ViewportInput {
	const SessionView &view;
	const PreviewClock &clock;
	const DocumentBase *document = nullptr;
	ChangeClass change = ChangeClass::None;
};

// What a viewport's planners read (a canvas's gestures, the MCP's drag and command): the input, the
// size the canvas draws the picture at (pixels; the state's where no canvas draws), the grid a drag
// snaps to (a menu's 8-unit grid where it is not 0, a model's grid in metres; 0 free), and the device
// drawing it (null: none; what a drop lands on, ViewportDevice::surface_at).
struct ViewportContext {
	ViewportInput input;
	int width = 0;
	int height = 0;
	float snap = 0.0f;
	const ViewportDevice *device = nullptr;
	// An edit is planned only where the session takes one now: no running operation refuses an edit
	// (S13 A3, SessionView::allows), and the document is open and not blocked.
	bool editable() const;
};

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

// A drag the MCP plans (ADR 0046 S10p5, S9k1): the record `id`, the handle by its token (a menu
// window's "move", "left", ... "bottom_right"; a model marker's "place" or "axis"), by (x, y) from
// where the picture shows it (`by`: a menu's, design units) or to the point (x, y) of the picture
// (a model's, pixels), snapped by `snap` (a menu's grid when it is not 0; a model's grid in metres,
// 0 free).
struct ViewportDrag {
	NodeId id = 0;
	std::string handle;
	bool by = true;
	float x = 0.0f;
	float y = 0.0f;
	float snap = 0.0f;
};

// A viewport (ADR 0046 S13 V5; CONTEXT.md "Viewport"): one document's picture as the game would
// draw it, of one kind (viewport_kinds.h), kept by the session (Viewports) while its document is
// open and shared const on the view, so the windows read it and never change it. Its state (the
// device's size, the kind's options and camera) changes only through a SetViewport request (apply)
// and the kind's own rules as it follows; what follows from it (its status, what its picture is made
// from, the action its device takes next) is made by follow, which the session's devices drive
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

	// --- what it shows ---------------------------------------------------------------------------

	virtual ViewportStatus status() const = 0;
	// The kind's reason (its token on the wire: "no_menu", "unserializable", "ready", ...), the
	// sentence it shows for it ("" when ready) and its detail (the first reason the document cannot
	// be written, the missing screen's name, the animation's file).
	virtual const char *reason() const = 0;
	virtual std::string message() const = 0;
	virtual const std::string &detail() const = 0;
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
	// The MCP's drag planned into requests (a batch of the gesture's steps and its end): false, with
	// why, for a picture that is not the document's own, a record it does not show, a handle the
	// record has not, a drag that writes nothing the session would take.
	virtual bool drag(const ViewportContext &context, const ViewportDrag &drag, CanvasRequests &out,
			std::string &error) const = 0;
	// A command by its name over the records `ids` (the menu's arrange of windows, "align_left" ...;
	// the model's "frame"), planned into requests: false, with why, for a command the kind has not or
	// one it cannot plan.
	virtual bool command(const ViewportContext &context, const std::string &name,
			const std::vector<NodeId> &ids, CanvasRequests &out, std::string &error) const = 0;

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

	// --- the device's -------------------------------------------------------------------------------

	bool attached() const { return attached_; }
	// How many times its device was told to make its picture again (an edit that changes only what
	// the overlays show is not one).
	uint64_t builds() const { return builds_; }

	// --- the session's (Viewports) ------------------------------------------------------------------

	// Follow `input`: what it shows now, and the action its device takes next, merged into the one
	// its device has not taken yet (Rebuild over Update, a Clear that drops a picture the device
	// holds; a Clear of nothing is nothing). The action pending.
	ViewportAction follow(const ViewportInput &input, PreviewClock &clock);
	// A SetViewport's change (`json` an object: kind, device {width, height}, clock {playing, rate,
	// time_ms, ticks}, and the kind's own members): every member checked before any applies; false,
	// nothing changed, with `error` naming the member and what it takes.
	bool apply(const io::JsonValue &json, PreviewClock &clock, std::string &error);
	// What its device does now (Keep: nothing; Keep while no device is attached), then Keep until a
	// follow says otherwise.
	ViewportAction take_action();
	// A device is attached: it holds nothing yet, so its first action makes the picture (Rebuild,
	// when there is one to show); its state is kept. Detached (given up for another, ADR 0046 S13 V5:
	// the device cache's least recently used): nothing pending, its state kept for the next.
	void attach();
	void detach();
	// What the device read as it made its picture (its textures), and where it placed what it drew.
	void device_report(const ViewportDeviceReport &report);

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

	ViewportState state_;

private:
	ViewportKind kind_;
	std::string path_;
	uint64_t shown_identity_ = 0;
	uint64_t shown_load_ = 0;
	uint64_t shown_revision_ = 0;
	bool shows_document_ = false;
	ViewportAction pending_ = ViewportAction::Keep;
	bool attached_ = false;
	bool holds_ = false; // the attached device holds a picture
	uint64_t builds_ = 0;
};

} // namespace opennova::editor
