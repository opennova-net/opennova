#pragma once

#include <cstddef>
#include <cstdint>
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
// viewport and never change it: its state changes only through a SetViewport request (set), which
// the session serves; what follows from it changes as it follows the view, which the Shell's
// devices drive (ViewportDeviceCache), as they attach, detach, take actions and report.
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
	// its document since its last follow (ChangeClass), then the kind's follow. A viewport with no
	// device follows when one attaches, or when it is read (follow_one).
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

	// A SetViewport (request_kinds.cpp): the viewport of the kind `json` names ("kind", optional
	// where one kind shows the document) over the document open at `path` (""; the kind's Preview
	// target), made when there is none, changed as ViewportModel::apply says. False, nothing
	// changed, with `error` naming what is wrong: the change, the kind, a document not open, a kind
	// that does not show it.
	bool set(const SessionView &view, const std::string &path, const io::JsonValue &json,
			std::string &error);

	// `seconds` of the Shell's frames pass (the clock's while it plays).
	void advance(double seconds) { clock_.advance(seconds); }
	const PreviewClock &clock() const { return clock_; }

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

	std::vector<Slot> slots_;
	PreviewClock clock_;
};

} // namespace opennova::editor
