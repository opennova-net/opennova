#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <base/io/json.h>
#include <editor/preview/preview_clock.h>
#include <editor/preview/viewport_follow.h>
#include <editor/preview/viewport_kinds.h>

namespace opennova::editor {

class ViewportModel;
struct SessionView;
struct ViewportDeviceReport;

// The session's viewports (ADR 0046 S13 V5; CONTEXT.md "Viewport"): one per (document, kind), a
// document owning several where kinds show it (a mission's 3D view and its map), kept while the
// document is open; and the one preview clock they all read (CONTEXT.md "Preview clock"). The
// session owns them and the view shares them const (DocumentsView::viewports), so the windows read a
// viewport and never change it: every change a person or a client makes to its state is a
// SetViewport request (set), which the session serves, and the three changes its follow derives
// (viewport_model.h's ViewportState) are the only others; each moves the view's Viewports concern
// (set_on_derived_change for the derived ones). What follows from the state changes as the viewport
// follows the view, which the Shell's devices drive (ViewportDeviceCache), as they attach, detach,
// take actions and report.
class Viewports {
public:
	Viewports();
	~Viewports();
	Viewports(const Viewports &) = delete;
	Viewports &operator=(const Viewports &) = delete;

	// After every change of the view (SessionCore::touch): a viewport for each kind's Preview
	// target (view.documents.previews), made as a target first names it, and for each open document
	// a Main-role kind shows (its tab's); a viewport whose document closed (or is open as a type its
	// kind does not show) gone, its state with it.
	void track(const SessionView &view);
	// The viewport of `kind` over the document at `path`, made at the kind's defaults when there
	// is none (kept until the document closes).
	ViewportModel &ensure(const std::string &path, ViewportKind kind);
	const ViewportModel *find(const std::string &path, ViewportKind kind) const;
	ViewportModel *find(const std::string &path, ViewportKind kind);
	size_t size() const { return slots_.size(); }
	const ViewportModel &at(size_t index) const;

	// Every viewport a device is attached to follows the view (the Shell's pump): what changed in
	// its document since its last follow (ChangeClass, with the change set the document answers
	// since that state: S13 V8), then the kind's follow. A viewport with no device follows when one
	// attaches, or when it is read (follow_one).
	void follow(const SessionView &view);
	// The viewport of `kind` over `path` followed now (a reader of its envelope, a test), null when
	// none is kept.
	ViewportModel *follow_one(const SessionView &view, const std::string &path, ViewportKind kind);
	// The device's half (ViewportDeviceCache): attached and detached (ViewportModel::attach), its
	// action taken, its report given.
	void attach(const std::string &path, ViewportKind kind);
	void detach(const std::string &path, ViewportKind kind);
	ViewportAction take_action(const std::string &path, ViewportKind kind);
	void device_report(const std::string &path, ViewportKind kind, const ViewportDeviceReport &report);

	// A SetViewport (request_kinds.cpp): the viewport of the kind `json` names ("kind"; left out, the
	// kind the document shows in, default_viewport_kind) over the document open at `path` (its own
	// path, as the session found it; "" the active document), made when there is none, changed as
	// ViewportModel::apply says. False, nothing changed, with `error` naming what is wrong: the
	// change, the kind, a document not open, a kind that does not show it.
	bool set(const SessionView &view, const std::string &path, const io::JsonValue &json,
			std::string &error);
	// The viewport a read of the document open at `path` (its own path; "" the active document)
	// addresses, and an edit in a viewport plans over (S13 V7: the viewport query, EditInViewport): of
	// the kind `named` (kCount: the kind its type shows in, default_viewport_kind), kept, or made at
	// the kind's defaults as a SetViewport makes one, then followed now (follow_one), so a first read
	// of a document makes its viewport and its follow may derive a change (the Viewports concern
	// moving). Null, with `error` naming why, for no document open there, a type that shows in no
	// viewport, or a kind that does not show it.
	ViewportModel *resolve(const SessionView &view, const std::string &path, ViewportKind named, std::string &error);

	// `seconds` of the Shell's frames pass (the clock's while it plays).
	void advance(double seconds) { clock_.advance(seconds); }
	const PreviewClock &clock() const { return clock_; }
	// A SetViewport's clock alone, named by no document (S13 V7: the editor MCP's seek): the preview
	// clock set as set_preview_clock says; false, nothing changed, with `error`.
	bool set_clock(const io::JsonValue &json, std::string &error);
	// Told when a follow derived a change of a viewport's state or the clock (viewport_model.h
	// ViewportState: a held window, a framing, a clip's clock sought): the session moves its
	// Viewports concern.
	void set_on_derived_change(std::function<void()> notify) { on_derived_change_ = std::move(notify); }

private:
	struct Slot {
		std::unique_ptr<ViewportModel> model;
		// The document state the viewport last followed (its ChangeClass is against it).
		bool followed = false;
		uint64_t identity = 0;
		uint64_t load = 0;
		uint64_t revision = 0;
	};
	Slot *slot_(const std::string &path, ViewportKind kind);
	void follow_(const SessionView &view, Slot &slot);
	// The document a set or a resolve names (`path`, "" the active one; open, else `error`) and the
	// kind it addresses (`named`, else default_viewport_kind; one that shows it, else `error`).
	bool addressed_(const SessionView &view, const std::string &path, ViewportKind named, std::string &at,
			ViewportKind &kind, std::string &error) const;

	std::vector<Slot> slots_;
	PreviewClock clock_;
	std::function<void()> on_derived_change_;
};

} // namespace opennova::editor
