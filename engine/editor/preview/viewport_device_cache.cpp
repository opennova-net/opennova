#include <editor/preview/viewport_device_cache.h>

#include <algorithm>

#include <editor/preview/viewport_model.h>
#include <editor/preview/viewports.h>
#include <editor/session/view/session_view.h>

namespace opennova::editor {

ViewportDeviceCache::ViewportDeviceCache(Factory make, size_t capacity, KindLimit limit) :
		make_(std::move(make)), capacity_(std::max<size_t>(capacity, 1)), limit_(std::move(limit)) {
	if (!limit_) limit_ = [](ViewportKind kind) { return viewport_kind_row(kind).devices; };
}

ViewportDeviceCache::~ViewportDeviceCache() = default;

ViewportDevice *ViewportDeviceCache::most_recently_used() const {
	const Slot *newest = nullptr;
	for (const Slot &slot : slots_)
		if (!newest || slot.asked > newest->asked) newest = &slot;
	return newest ? newest->device.get() : nullptr;
}

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
	// The least recently used not used this round given up: its viewport keeps its state for the
	// next device. Every device used this round: none is made, and the viewport waits. Of the kind
	// first, where the kind keeps fewer than the cache (S14), then of all past the capacity.
	const auto give_up = [&](bool of_kind) {
		auto oldest = slots_.end();
		for (auto it = slots_.begin(); it != slots_.end(); ++it) {
			if (it->round == round_ || (of_kind && it->kind != kind)) continue;
			if (oldest == slots_.end() || it->used < oldest->used) oldest = it;
		}
		if (oldest == slots_.end()) return false;
		viewports.detach(oldest->path, oldest->kind);
		slots_.erase(oldest);
		return true;
	};
	const size_t limit = limit_(kind);
	if (limit > 0) {
		size_t held = 0;
		for (const Slot &slot : slots_)
			if (slot.kind == kind) ++held;
		if (held >= limit && !give_up(true)) return;
	}
	if (slots_.size() >= capacity_ && !give_up(false)) return;
	std::unique_ptr<ViewportDevice> device = make_(kind);
	if (!device) return;
	Slot slot;
	slot.path = path;
	slot.kind = kind;
	slot.device = std::move(device);
	slot.used = ++clock_;
	slot.asked = slot.used;
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
	// What the Preview window shows (every kind's target where nothing draws to ask, and the active
	// document's Main view, which its tab would draw: S13 V10), then what a view asked for.
	for (size_t i = 0; i < kViewportKindCount; ++i) {
		const auto kind = static_cast<ViewportKind>(i);
		if (!pin_all_ && kind != view.documents.preview_shown) continue;
		const std::string &target = view.documents.previews[kind].path;
		if (!target.empty() && viewports.find(target, kind)) use_(viewports, target, kind);
	}
	if (pin_all_ && !view.documents.active.empty())
		for (size_t i = 0; i < kViewportKindCount; ++i) {
			const auto kind = static_cast<ViewportKind>(i);
			if (viewport_kind_row(kind).role == ViewportRole::Main && viewports.find(view.documents.active, kind))
				use_(viewports, view.documents.active, kind);
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

void ViewportDeviceCache::arbitrate(const Viewports &viewports) {
	// The devices whose draws asked to render this frame, and each state's latest render among them.
	std::vector<Slot *> asked;
	struct State {
		uint64_t state = 0;
		uint64_t rendered = 0;
	};
	std::vector<State> states;
	for (Slot &slot : slots_) {
		if (!slot.device->render_asked()) continue;
		asked.push_back(&slot);
		const uint64_t state = slot.device->scene_state(), rendered = slot.device->rendered_frame();
		auto found = std::find_if(states.begin(), states.end(), [state](const State &s) { return s.state == state; });
		if (found == states.end()) states.push_back(State{ state, rendered });
		else found->rendered = std::max(found->rendered, rendered);
	}
	if (asked.empty()) return;
	// The state that rendered longest ago wins; a tie goes to the state published last, else to the
	// first held.
	const State *winner = &states.front();
	for (const State &state : states) {
		if (state.rendered < winner->rendered ||
				(state.rendered == winner->rendered && published_any_ && state.state == published_ && winner->state != published_))
			winner = &state;
	}
	const uint64_t state = winner->state;
	for (Slot *slot : asked)
		if (slot->device->scene_state() != state) slot->device->withhold_render();
	if (!published_any_ || published_ != state) {
		for (Slot *slot : asked)
			if (slot->device->scene_state() == state) {
				slot->device->publish_scene_state();
				break;
			}
		published_ = state;
		published_any_ = true;
	}
	for (Slot *slot : asked) {
		if (slot->device->scene_state() != state) continue;
		if (const ViewportModel *model = viewports.find(slot->path, slot->kind)) slot->device->present(*model, viewports.clock());
	}
}

ViewportDevice *ViewportDeviceCache::device(const std::string &path, ViewportKind kind) {
	if (Slot *slot = slot_(path, kind)) {
		// Asked for since the last sync: kept through the next (its round's).
		slot->used = ++clock_;
		slot->asked = slot->used;
		slot->round = round_ + 1;
		return slot->device.get();
	}
	const bool asked = std::any_of(wanted_.begin(), wanted_.end(),
			[&](const Want &want) { return want.kind == kind && want.path == path; });
	if (!asked) wanted_.push_back({ path, kind });
	return nullptr;
}

} // namespace opennova::editor
