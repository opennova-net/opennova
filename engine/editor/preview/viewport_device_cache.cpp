#include <editor/preview/viewport_device_cache.h>

#include <algorithm>

#include <editor/preview/viewport_model.h>
#include <editor/preview/viewports.h>
#include <editor/session/view/session_view.h>

namespace opennova::editor {

ViewportDeviceCache::ViewportDeviceCache(Factory make, size_t capacity) :
		make_(std::move(make)), capacity_(std::max<size_t>(capacity, 1)) {}

ViewportDeviceCache::~ViewportDeviceCache() = default;

ViewportDeviceCache::Slot *ViewportDeviceCache::slot_(const std::string &path, ViewportKind kind) {
	for (Slot &slot : slots_)
		if (slot.kind == kind && slot.path == path) return &slot;
	return nullptr;
}

ViewportDevice *ViewportDeviceCache::held(const std::string &path, ViewportKind kind) const {
	for (const Slot &slot : slots_)
		if (slot.kind == kind && slot.path == path) return slot.device.get();
	return nullptr;
}

void ViewportDeviceCache::use_(Viewports &viewports, const std::string &path, ViewportKind kind) {
	if (Slot *slot = slot_(path, kind)) {
		slot->used = ++clock_;
		slot->round = round_;
		return;
	}
	if (!make_) return;
	if (slots_.size() >= capacity_) {
		// The least recently used not used this round given up: its viewport keeps its state for the
		// next device. Every device used this round: none is made, and the viewport waits.
		auto oldest = slots_.end();
		for (auto it = slots_.begin(); it != slots_.end(); ++it)
			if (it->round != round_ && (oldest == slots_.end() || it->used < oldest->used)) oldest = it;
		if (oldest == slots_.end()) return;
		viewports.detach(oldest->path, oldest->kind);
		slots_.erase(oldest);
	}
	std::unique_ptr<ViewportDevice> device = make_(kind);
	if (!device) return;
	Slot slot;
	slot.path = path;
	slot.kind = kind;
	slot.device = std::move(device);
	slot.used = ++clock_;
	slot.round = round_;
	slots_.push_back(std::move(slot));
	viewports.attach(path, kind);
}

void ViewportDeviceCache::sync(Viewports &viewports, const SessionView &view) {
	++round_;
	// A device whose viewport went (its document closed), or is another than the one it was attached
	// to (closed and opened again since: made again at the defaults, attached to nothing), goes; the
	// viewport made again is given a device as any other.
	slots_.erase(std::remove_if(slots_.begin(), slots_.end(),
						 [&](const Slot &slot) {
							 const ViewportModel *model = viewports.find(slot.path, slot.kind);
							 return !model || !model->attached();
						 }),
			slots_.end());
	// What the Preview window shows (every kind's target where nothing draws to ask), then what a
	// view asked for.
	for (size_t i = 0; i < kViewportKindCount; ++i) {
		const auto kind = static_cast<ViewportKind>(i);
		if (!pin_all_ && kind != view.documents.preview_shown) continue;
		const std::string &target = view.documents.previews[kind].path;
		if (!target.empty() && viewports.find(target, kind)) use_(viewports, target, kind);
	}
	for (const Want &want : wanted_)
		if (viewports.find(want.path, want.kind)) use_(viewports, want.path, want.kind);
	wanted_.clear();
	// Every viewport with a device follows, and each device takes what its viewport asks, then says
	// where its build stands.
	viewports.follow(view);
	for (Slot &slot : slots_) {
		const ViewportModel *model = viewports.find(slot.path, slot.kind);
		if (!model) continue;
		const ViewportAction action = viewports.take_action(slot.path, slot.kind);
		ViewportDeviceReport report;
		slot.device->take(action, *model, view, viewports.clock(), report);
		report.build = slot.device->build();
		viewports.device_report(slot.path, slot.kind, report);
	}
}

void ViewportDeviceCache::tick(const Viewports &viewports) {
	for (Slot &slot : slots_)
		if (const ViewportModel *model = viewports.find(slot.path, slot.kind))
			slot.device->tick(*model, viewports.clock());
}

void ViewportDeviceCache::step(Viewports &viewports, const std::function<bool()> &more) {
	// The most recently used first: the viewport a canvas drew or a view asked for last is the one
	// looked at, and the frame's budget goes to it before an older (perhaps hidden) one.
	std::vector<Slot *> order;
	order.reserve(slots_.size());
	for (Slot &slot : slots_) order.push_back(&slot);
	std::sort(order.begin(), order.end(), [](const Slot *a, const Slot *b) { return a->used > b->used; });
	// One unit a frame in all, whatever the budget: the first build that has one runs it; each unit
	// after it, of that build or a later one, runs only while `more` says the budget lasts (asked once
	// after each unit, so a budget of N units is N units however many builds share them).
	bool may = true;
	for (Slot *slot : order) {
		if (!may) break;
		const ViewportModel *model = viewports.find(slot->path, slot->kind);
		if (!model) continue;
		bool stepped = false;
		while (may && slot->device->step(*model, viewports.clock())) {
			stepped = true;
			may = more && more();
		}
		if (stepped) viewports.device_build(slot->path, slot->kind, slot->device->build());
	}
}

ViewportDevice *ViewportDeviceCache::device(const std::string &path, ViewportKind kind) {
	if (Slot *slot = slot_(path, kind)) {
		// Asked for since the last sync: kept through the next (its round's).
		slot->used = ++clock_;
		slot->round = round_ + 1;
		return slot->device.get();
	}
	const bool asked = std::any_of(wanted_.begin(), wanted_.end(),
			[&](const Want &want) { return want.kind == kind && want.path == path; });
	if (!asked) wanted_.push_back({ path, kind });
	return nullptr;
}

} // namespace opennova::editor
