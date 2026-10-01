#include <editor/preview/viewports.h>

#include <algorithm>

#include <editor/assets/asset_kinds.h>
#include <editor/model/document_base.h>
#include <editor/preview/viewport_device.h>
#include <editor/preview/viewport_model.h>
#include <editor/session/view/session_view.h>

namespace opennova::editor {

namespace {

const DocumentBase *open_at(const SessionView &view, const std::string &path) {
	for (const auto &document : view.documents.open)
		if (document && document->path() == path) return document.get();
	return nullptr;
}

DocumentTypeId type_of(const DocumentBase &document) {
	return asset_kind_row(document.kind()).document;
}

} // namespace

Viewports::Viewports() = default;
Viewports::~Viewports() = default;

Viewports::Slot *Viewports::slot_(const std::string &path, ViewportKind kind) {
	for (Slot &slot : slots_)
		if (slot.model->kind() == kind && slot.model->path() == path) return &slot;
	return nullptr;
}

const ViewportModel *Viewports::find(const std::string &path, ViewportKind kind) const {
	for (const Slot &slot : slots_)
		if (slot.model->kind() == kind && slot.model->path() == path) return slot.model.get();
	return nullptr;
}

ViewportModel *Viewports::find(const std::string &path, ViewportKind kind) {
	Slot *slot = slot_(path, kind);
	return slot ? slot->model.get() : nullptr;
}

const ViewportModel &Viewports::at(size_t index) const {
	return *slots_[index].model;
}

ViewportModel &Viewports::ensure(const std::string &path, ViewportKind kind) {
	if (Slot *slot = slot_(path, kind)) return *slot->model;
	Slot slot;
	slot.model = viewport_kind_row(kind).make(path);
	slots_.push_back(std::move(slot));
	return *slots_.back().model;
}

void Viewports::track(const SessionView &view) {
	// A viewport whose document is no longer open, or is open as a type its kind does not show.
	slots_.erase(std::remove_if(slots_.begin(), slots_.end(),
						 [&](const Slot &slot) {
							 const DocumentBase *document = open_at(view, slot.model->path());
							 return !document ||
									 !viewport_kind_shows(slot.model->kind(), type_of(*document));
						 }),
			slots_.end());
	for (size_t i = 0; i < kViewportKindCount; ++i) {
		const auto kind = static_cast<ViewportKind>(i);
		const std::string &target = view.documents.previews[kind].path;
		const DocumentBase *document = target.empty() ? nullptr : open_at(view, target);
		if (document && viewport_kind_shows(kind, type_of(*document))) ensure(target, kind);
	}
	// A document whose type a Main-role kind shows owns that viewport while it is open (its tab's).
	for (const auto &document : view.documents.open) {
		const ViewportKind main = document ? main_viewport_kind(type_of(*document)) : ViewportKind::kCount;
		if (main != ViewportKind::kCount) ensure(document->path(), main);
	}
}

void Viewports::follow_(const SessionView &view, Slot &slot) {
	const DocumentBase *document = open_at(view, slot.model->path());
	// What changed in the document since the viewport last followed it.
	ChangeClass change = ChangeClass::None;
	if (!document) {
		change = ChangeClass::Loaded;
		slot.followed = false;
	} else {
		if (!slot.followed || document->identity() != slot.identity ||
				document->load_generation() != slot.load)
			change = ChangeClass::Loaded;
		else if (document->revision() != slot.revision)
			change = ChangeClass::Unknown;
		slot.followed = true;
		slot.identity = document->identity();
		slot.load = document->load_generation();
		slot.revision = document->revision();
	}
	// What the follow derives (its held window, a framing, the clock sought) said once it has.
	const uint64_t serial = slot.model->state_serial();
	const PreviewClock clock = clock_;
	slot.model->follow(ViewportInput{ view, clock_, document, change }, clock_);
	const bool clock_moved = clock.playing() != clock_.playing() || clock.ms() != clock_.ms() ||
			clock.ticks() != clock_.ticks() || clock.rate() != clock_.rate();
	if ((slot.model->state_serial() != serial || clock_moved) && on_derived_change_) on_derived_change_();
}

void Viewports::follow(const SessionView &view) {
	for (Slot &slot : slots_)
		if (slot.model->attached()) follow_(view, slot);
}

ViewportModel *Viewports::follow_one(
		const SessionView &view, const std::string &path, ViewportKind kind) {
	Slot *slot = slot_(path, kind);
	if (!slot) return nullptr;
	follow_(view, *slot);
	return slot->model.get();
}

void Viewports::attach(const std::string &path, ViewportKind kind) {
	if (ViewportModel *model = find(path, kind)) model->attach();
}

void Viewports::detach(const std::string &path, ViewportKind kind) {
	if (ViewportModel *model = find(path, kind)) model->detach();
}

ViewportAction Viewports::take_action(const std::string &path, ViewportKind kind) {
	ViewportModel *model = find(path, kind);
	return model ? model->take_action() : ViewportAction::Keep;
}

void Viewports::device_report(
		const std::string &path, ViewportKind kind, const ViewportDeviceReport &report) {
	ViewportModel *model = find(path, kind);
	if (model && model->device_report(report) && on_derived_change_) on_derived_change_();
}

void Viewports::device_build(const std::string &path, ViewportKind kind, const ViewportBuildReport &build) {
	ViewportModel *model = find(path, kind);
	if (model && model->device_build(build) && on_derived_change_) on_derived_change_();
}

bool Viewports::set(const SessionView &view, const std::string &path, const io::JsonValue &json,
		std::string &error) {
	if (!json.is_object()) {
		error = "A viewport's change is a JSON object.";
		return false;
	}
	ViewportKind kind = ViewportKind::kCount;
	if (const io::JsonValue *named = json.get("kind")) {
		if (!named->is_string() || !viewport_kind_from_token(named->string, kind)) {
			error = "\"kind\" names a viewport kind (menu, model).";
			return false;
		}
	}
	// No path: the kind's Preview target, the document the Preview window shows.
	std::string at = path;
	if (at.empty()) {
		if (kind == ViewportKind::kCount) {
			error = "set_viewport names its document (path), or the kind whose Preview it changes.";
			return false;
		}
		at = view.documents.previews[kind].path;
		if (at.empty()) {
			error = std::string("The Preview shows no ") + viewport_kind_token(kind) +
					": name the document (path).";
			return false;
		}
	}
	const DocumentBase *document = open_at(view, at);
	if (!document) {
		error = "No document is open at " + at + ".";
		return false;
	}
	const DocumentTypeId type = type_of(*document);
	if (kind == ViewportKind::kCount) {
		// The one kind that shows the document.
		size_t showing = 0;
		for (size_t i = 0; i < kViewportKindCount; ++i) {
			if (!viewport_kind_shows(static_cast<ViewportKind>(i), type)) continue;
			kind = static_cast<ViewportKind>(i);
			++showing;
		}
		if (showing != 1) {
			error = showing ? at + " shows in several viewports: name the kind."
							: at + " shows in no viewport.";
			return false;
		}
	} else if (!viewport_kind_shows(kind, type)) {
		error = at + " does not show in a " + viewport_kind_token(kind) + " viewport.";
		return false;
	}
	// The viewport the change is for, made only once the change applies to it (a refused change makes
	// none).
	if (ViewportModel *held = find(at, kind)) return held->apply(json, clock_, error);
	std::unique_ptr<ViewportModel> made = viewport_kind_row(kind).make(at);
	if (!made->apply(json, clock_, error)) return false;
	Slot slot;
	slot.model = std::move(made);
	slots_.push_back(std::move(slot));
	return true;
}

} // namespace opennova::editor
