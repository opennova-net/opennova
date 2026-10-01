#pragma once

// The viewports' tests' fakes (S13 V5): a device that draws nothing and records what its viewport
// asked, the device cache over such devices, and the requests a planner made, served through a
// session. The Shell's devices are godot/src/authoring's; these stand in for them in a ctest.

#include <memory>
#include <string>
#include <vector>

#include <editor/assets/project_asset_source.h>
#include <editor/preview/viewport_follow.h>

#include <editor/preview/canvas_gesture.h>
#include <editor/preview/menu_viewport.h>
#include <editor/preview/model_viewport.h>
#include <editor/preview/viewport_device.h>
#include <editor/preview/viewport_device_cache.h>
#include <editor/preview/viewport_model.h>
#include <editor/preview/viewports.h>
#include <editor/session/project_session.h>
#include <editor/session/view/session_view.h>

namespace editor_test {

using opennova::editor::ViewportAction;

// A device that draws nothing: the actions its viewport asked of it, in order, and the size it was
// drawn at. It reports its size as the Shell's does: the one a canvas drew it at since the last pump
// (and whether that was a size of the canvas's own), else its viewport's state's. A menu's reports
// where its picture placed each widget: where the viewport's own compile did (the Shell's MenuFrame
// draws the same screen alike). It reads `reads` through the project's files as it makes its
// picture, as a model's device reads its textures, and reports them. It makes its picture whole as it
// takes a Rebuild, or (S13 V6) with `units` set builds it over that many steps, one unit a step as the
// Shell's frames step it, drawing the last picture it built (`shown`, a generation) until the build
// ends, failing at unit `fail_at` when that is set; a Rebuild drops the one in flight, a Clear drops it
// with the picture.
struct FakeDevice final : opennova::editor::ViewportDevice {
	std::vector<ViewportAction> taken;
	std::vector<std::string> reads;
	int draws = 0;
	int width = 0;
	int height = 0;
	bool drawn = false; // a canvas drew it since the last pump
	bool canvas_sized = false;
	// A build over steps (S13 V6): its units (0: made whole as it is taken) and the unit it fails at
	// (0: none); its build as build() says, the generation of the picture it draws (0: none), the
	// units it ran, and the Actions it took while a build ran.
	uint64_t units = 0;
	uint64_t fail_at = 0;
	opennova::editor::ViewportBuildReport built;
	uint64_t shown = 0;
	int steps = 0;
	std::vector<ViewportAction> taken_building;
	// What a model viewport's level option was as its last build ended (-2: none ended): the state
	// the build applied.
	int ended_lod = -2;
	void draw(const opennova::editor::ViewportPicture &picture) override {
		++draws;
		width = picture.width;
		height = picture.height;
		drawn = true;
		canvas_sized = picture.canvas_sized;
	}
	opennova::editor::ViewportBuildReport build() const override { return built; }
	bool step(const opennova::editor::ViewportModel &model, const opennova::editor::PreviewClock &) override {
		if (!built.loading) return false;
		++steps;
		++built.progress.done;
		if (fail_at != 0 && built.progress.done == fail_at) {
			built.loading = false;
			built.failed = true;
			built.message = "Unit " + std::to_string(fail_at) + " failed.";
		} else if (built.progress.done == built.progress.total) {
			built.loading = false;
			shown = built.generation;
			const auto *shown_model = dynamic_cast<const opennova::editor::ModelViewport *>(&model);
			ended_lod = shown_model ? shown_model->options().lod : -1;
		}
		return true;
	}
	void take(ViewportAction action, const opennova::editor::ViewportModel &model,
			const opennova::editor::SessionView &view, const opennova::editor::PreviewClock &,
			opennova::editor::ViewportDeviceReport &report) override {
		taken.push_back(action);
		if (built.loading) taken_building.push_back(action);
		if (action == ViewportAction::Rebuild) {
			built = opennova::editor::ViewportBuildReport();
			built.generation = model.builds();
			if (units != 0) {
				built.loading = true;
				built.progress.total = units;
				built.progress.label = "units";
			} else {
				shown = built.generation;
			}
		} else if (action == ViewportAction::Clear) {
			built.loading = built.failed = false;
			shown = 0;
		}
		report.width = drawn ? width : model.state().width;
		report.height = drawn ? height : model.state().height;
		report.canvas_sized = drawn && canvas_sized;
		drawn = false;
		if (!reads.empty() && view.findings.assets) {
			opennova::editor::StampedFiles files(view.findings.assets);
			std::vector<uint8_t> bytes;
			for (const std::string &name : reads) files.read(name, bytes);
			report.files = files.stamps();
		}
		const auto *menu = dynamic_cast<const opennova::editor::MenuViewport *>(&model);
		if (!menu || menu->status() != opennova::editor::ViewportStatus::Ready) return;
		const opennova::menu::MenuFrameCompiler &compiler = menu->render().compiler();
		report.rects.resize(size_t(compiler.widget_count() > 0 ? compiler.widget_count() : 0));
		for (int index = 0; index < compiler.widget_count(); ++index) {
			opennova::mnu::RectEdges rect{};
			auto &placed = report.rects[size_t(index)];
			placed.placed = compiler.widget_rect(index, menu->render().state(), &rect);
			placed.left = rect.left;
			placed.top = rect.top;
			placed.right = rect.right;
			placed.bottom = rect.bottom;
		}
	}
	void tick(const opennova::editor::ViewportModel &, const opennova::editor::PreviewClock &) override {}
	// The last action it took (Keep before any).
	ViewportAction last() const { return taken.empty() ? ViewportAction::Keep : taken.back(); }
	// The actions taken since `from`, Keep left out.
	std::vector<ViewportAction> since(size_t from) const {
		std::vector<ViewportAction> out;
		for (size_t i = from; i < taken.size(); ++i)
			if (taken[i] != ViewportAction::Keep) out.push_back(taken[i]);
		return out;
	}
};

// The Shell's devices over fakes: every device it made, still held or given up, and how many it
// made; the units each one it makes builds its picture over (S13 V6; 0: made whole as it is taken).
struct FakeDevices {
	std::vector<FakeDevice *> made;
	uint64_t units = 0;
	opennova::editor::ViewportDeviceCache cache{ [this](opennova::editor::ViewportKind) {
		auto device = std::make_unique<FakeDevice>();
		device->units = units;
		made.push_back(device.get());
		return std::unique_ptr<opennova::editor::ViewportDevice>(std::move(device));
	} };
	// The Shell's pump after the session's poll.
	void sync(opennova::editor::ProjectSession &session) { cache.sync(session.viewports(), session.view()); }
	// A frame of the Shell's (S13 V6): its pump, then the builds stepped, `units` units a frame (the
	// budget's: each build in flight runs at least one).
	void frame(opennova::editor::ProjectSession &session, int units = 1) {
		sync(session);
		int left = units;
		cache.step(session.viewports(), [&left] { return --left > 0; });
	}
	FakeDevice *held(const std::string &path, opennova::editor::ViewportKind kind) const {
		return static_cast<FakeDevice *>(cache.held(path, kind));
	}
	// The last action the device of (path, kind) took (Keep: none, or no device).
	ViewportAction last(const std::string &path, opennova::editor::ViewportKind kind) const {
		const FakeDevice *device = held(path, kind);
		return device ? device->last() : ViewportAction::Keep;
	}
};

// The requests a planner made, gathered.
struct Gathered final : opennova::editor::CanvasRequests {
	std::vector<opennova::editor::EditorRequest> requests;
	void request(opennova::editor::EditorRequest request) override { requests.push_back(std::move(request)); }
};

// The requests served in order through the session: true when there was one and each was done.
inline bool serve(opennova::editor::ProjectSession &session,
		const std::vector<opennova::editor::EditorRequest> &requests) {
	bool done = !requests.empty();
	for (const opennova::editor::EditorRequest &request : requests) {
		session.handle(request);
		done = done && session.outcome().done();
	}
	return done;
}

// What the planners read of the viewport of `kind` over `path` (the view, its clock, the document
// open there), at the size its state says.
inline opennova::editor::ViewportContext viewport_context(const opennova::editor::ProjectSession &session,
		const opennova::editor::ViewportModel &model, float snap = 0.0f) {
	const opennova::editor::SessionView &view = session.view();
	const opennova::editor::DocumentBase *document = nullptr;
	for (const auto &open : view.documents.open)
		if (open && open->path() == model.path()) document = open.get();
	return opennova::editor::ViewportContext{
		opennova::editor::ViewportInput{ view, view.documents.viewports->clock(), document,
				opennova::editor::ChangeClass::None },
		model.size().width, model.size().height, snap, nullptr
	};
}

} // namespace editor_test
