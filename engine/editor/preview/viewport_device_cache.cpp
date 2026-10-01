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
		return;
	}
	std::unique_ptr<ViewportDevice> device = make_ ? make_(kind) : nullptr;
	if (!device) return;
	if (slots_.size() >= capacity_) {
		// The least recently used given up: its viewport keeps its state for the next device.
		const auto oldest = std::min_element(slots_.begin(), slots_.end(),
				[](const Slot &a, const Slot &b) { return a.used < b.used; });
		viewports.detach(oldest->path, oldest->kind);
		slots_.erase(oldest);
	}
	Slot slot;
	slot.path = path;
	slot.kind = kind;
	slot.device = std::move(device);
	slot.used = ++clock_;
	slots_.push_back(std::move(slot));
	viewports.attach(path, kind);
}

void ViewportDeviceCache::sync(Viewports &viewports, const SessionView &view) {
	// A device whose viewport went (its document closed) goes with it.
	slots_.erase(std::remove_if(slots_.begin(), slots_.end(),
						 [&](const Slot &slot) { return !viewports.find(slot.path, slot.kind); }),
			slots_.end());
	// What the Preview window follows, then what a canvas asked for.
	for (size_t i = 0; i < kViewportKindCount; ++i) {
		const auto kind = static_cast<ViewportKind>(i);
		const std::string &target = view.documents.previews[kind].path;
		if (!target.empty() && viewports.find(target, kind)) use_(viewports, target, kind);
	}
	for (const Want &want : wanted_)
		if (viewports.find(want.path, want.kind)) use_(viewports, want.path, want.kind);
	wanted_.clear();
	// Every viewport with a device follows, and each device takes what its viewport asks.
	viewports.follow(view);
	for (Slot &slot : slots_) {
		const ViewportModel *model = viewports.find(slot.path, slot.kind);
		if (!model) continue;
		const ViewportAction action = viewports.take_action(slot.path, slot.kind);
		ViewportDeviceReport report;
		slot.device->take(action, *model, view, viewports.clock(), report);
		viewports.device_report(slot.path, slot.kind, report);
	}
}

void ViewportDeviceCache::tick(const Viewports &viewports) {
	for (Slot &slot : slots_)
		if (const ViewportModel *model = viewports.find(slot.path, slot.kind))
			slot.device->tick(*model, viewports.clock());
}

ViewportDevice *ViewportDeviceCache::device(const std::string &path, ViewportKind kind) {
	if (Slot *slot = slot_(path, kind)) {
		slot->used = ++clock_;
		return slot->device.get();
	}
	const bool asked = std::any_of(wanted_.begin(), wanted_.end(),
			[&](const Want &want) { return want.kind == kind && want.path == path; });
	if (!asked) wanted_.push_back({ path, kind });
	return nullptr;
}

} // namespace opennova::editor
