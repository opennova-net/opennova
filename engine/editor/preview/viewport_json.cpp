#include <editor/preview/viewport_json.h>

#include <memory>
#include <vector>

#include <editor/model/document_base.h>
#include <editor/preview/preview_clock.h>
#include <editor/preview/viewport_model.h>
#include <editor/preview/viewports.h>
#include <editor/session/view/session_view.h>

namespace opennova::editor {

namespace {

using io::JsonValue;
using io::json_number;
using io::json_string;

const DocumentBase *open_at(const SessionView &view, const std::string &path) {
	for (const auto &document : view.documents.open)
		if (document && document->path() == path) return document.get();
	return nullptr;
}

JsonValue envelope(const SessionView &view, const ViewportModel &model, const PreviewClock &clock,
		const JsonPage &page) {
	const DocumentBase *document = model.path().empty() ? nullptr : open_at(view, model.path());
	const ViewportInput input{ view, clock, document, ChangeClass::None };
	JsonValue out = JsonValue::make_object();
	out.set("kind", json_string(model.row().token));
	out.set("path", json_string(model.path()));
	out.set("as_saved", JsonValue::make_bool(model.row().as_saved));
	out.set("status", json_string(viewport_status_token(model.status())));
	out.set("reason", json_string(model.reason()));
	out.set("message", json_string(model.message()));
	out.set("detail", json_string(model.detail()));
	out.set("revision", json_number(double(document ? document->revision() : 0)));
	out.set("shown_revision", json_number(double(model.shown_revision())));
	out.set("current", JsonValue::make_bool(model.current(input)));
	out.set("builds", json_number(double(model.builds())));
	out.set("units", json_string(model.units()));
	JsonValue device = JsonValue::make_object();
	device.set("attached", JsonValue::make_bool(model.attached()));
	device.set("width", json_number(model.state().width));
	device.set("height", json_number(model.state().height));
	out.set("device", std::move(device));
	out.set("options", model.options_json());
	out.set("camera", model.camera_json());
	JsonValue timing = JsonValue::make_object();
	timing.set("playing", JsonValue::make_bool(clock.playing()));
	timing.set("rate", json_number(clock.rate()));
	timing.set("time_ms", json_number(double(clock.ms())));
	timing.set("ticks", json_number(double(clock.ticks())));
	out.set("clock", std::move(timing));
	out.set("body", model.body_json(input));
	// A page of the items and, by the same page, of the notes.
	JsonValue items = model.items_json(input);
	JsonValue notes = model.notes_json(input);
	const auto page_of = [&page](JsonValue &list) {
		const size_t total = list.array.size();
		std::vector<JsonValue> kept(list.array.begin() + std::ptrdiff_t(page.first(total)),
				list.array.begin() + std::ptrdiff_t(page.last(total)));
		list.array = std::move(kept);
		return total;
	};
	const size_t item_total = page_of(items);
	const size_t note_total = page_of(notes);
	out.set("items", std::move(items));
	out.set("notes", std::move(notes));
	set_page(out, page, item_total, note_total);
	out.set("note_count", json_number(double(note_total)));
	return out;
}

} // namespace

io::JsonValue viewport_to_json(const SessionView &view, const ViewportModel *model, ViewportKind kind,
		const JsonPage &page) {
	static const PreviewClock kStill;
	const PreviewClock &clock = view.documents.viewports ? view.documents.viewports->clock() : kStill;
	if (model) return envelope(view, *model, clock, page);
	// The kind's empty viewport: one over no document of it, followed once.
	std::unique_ptr<ViewportModel> empty = viewport_kind_row(kind).make(std::string());
	PreviewClock still = clock;
	empty->follow(ViewportInput{ view, still, nullptr, ChangeClass::Loaded }, still);
	return envelope(view, *empty, clock, page);
}

io::JsonValue viewport_hit_to_json(const ViewportHit &hit) {
	JsonValue out = JsonValue::make_object();
	out.set("kind", json_string(hit.kind));
	out.set("index", json_number(hit.index));
	out.set("id", json_number(double(hit.id)));
	out.set("name", json_string(hit.name));
	out.set("current", JsonValue::make_bool(hit.current));
	return out;
}

} // namespace opennova::editor
