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

// The preview clock the view's viewports run on (a still one in a view no session made).
const PreviewClock &clock_of(const SessionView &view) {
	static const PreviewClock kStill;
	return view.documents.viewports ? view.documents.viewports->clock() : kStill;
}

// What the viewport reads of its document as it followed.
ViewportInput input_of(const SessionView &view, const ViewportModel &model, const PreviewClock &clock) {
	const DocumentBase *document = model.path().empty() ? nullptr : open_at(view, model.path());
	return ViewportInput{ view, clock, document, ChangeClass::None };
}

// The page of `list` kept in it; its whole length.
size_t page_of(JsonValue &list, const JsonPage &page) {
	const size_t total = list.array.size();
	std::vector<JsonValue> kept(list.array.begin() + std::ptrdiff_t(page.first(total)),
			list.array.begin() + std::ptrdiff_t(page.last(total)));
	list.array = std::move(kept);
	return total;
}

JsonValue envelope(const SessionView &view, const ViewportModel &model, const PreviewClock &clock,
		const JsonPage &page) {
	const ViewportInput input = input_of(view, model, clock);
	JsonValue out = JsonValue::make_object();
	out.set("kind", json_string(viewport_kind_token(model.kind())));
	out.set("path", json_string(model.path()));
	out.set("as_saved", JsonValue::make_bool(model.row().as_saved));
	out.set("status", json_string(viewport_status_token(model.status())));
	out.set("reason", json_string(model.reason()));
	out.set("message", json_string(model.message()));
	out.set("detail", json_string(model.detail()));
	out.set("revision", json_number(double(input.document ? input.document->revision() : 0)));
	out.set("shown_revision", json_number(double(model.shown_revision())));
	out.set("current", JsonValue::make_bool(model.current(input)));
	out.set("builds", json_number(double(model.builds())));
	out.set("units", json_string(model.units()));
	JsonValue device = JsonValue::make_object();
	device.set("attached", JsonValue::make_bool(model.attached()));
	const ViewportState size = model.size();
	device.set("width", json_number(size.width));
	device.set("height", json_number(size.height));
	device.set("canvas_sized", JsonValue::make_bool(model.canvas_sized()));
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
	const size_t item_total = page_of(items, page);
	const size_t note_total = page_of(notes, page);
	out.set("items", std::move(items));
	out.set("notes", std::move(notes));
	set_page(out, page, item_total, note_total);
	out.set("note_count", json_number(double(note_total)));
	return out;
}

// A page of one of a viewport's lists, `key` its name.
JsonValue list_page(const SessionView &view, const ViewportModel &model, const char *key, JsonValue list,
		const JsonPage &page) {
	const ViewportInput input = input_of(view, model, clock_of(view));
	JsonValue out = JsonValue::make_object();
	out.set("kind", json_string(viewport_kind_token(model.kind())));
	out.set("path", json_string(model.path()));
	out.set("status", json_string(viewport_status_token(model.status())));
	out.set("reason", json_string(model.reason()));
	out.set("current", JsonValue::make_bool(model.current(input)));
	out.set("shown_revision", json_number(double(model.shown_revision())));
	const size_t total = page_of(list, page);
	out.set(key, std::move(list));
	set_page(out, page, total);
	return out;
}

} // namespace

io::JsonValue viewport_to_json(const SessionView &view, const ViewportModel *model, ViewportKind kind,
		const JsonPage &page) {
	const PreviewClock &clock = clock_of(view);
	if (model) return envelope(view, *model, clock, page);
	// The kind's empty viewport: one over no document of it, followed once.
	std::unique_ptr<ViewportModel> empty = viewport_kind_row(kind).make(std::string());
	PreviewClock still = clock;
	empty->follow(ViewportInput{ view, still, nullptr, ChangeClass::Loaded }, still);
	return envelope(view, *empty, clock, page);
}

io::JsonValue viewport_items_to_json(const SessionView &view, const ViewportModel &model, const JsonPage &page) {
	return list_page(view, model, "items", model.items_json(input_of(view, model, clock_of(view))), page);
}

io::JsonValue viewport_notes_to_json(const SessionView &view, const ViewportModel &model, const JsonPage &page) {
	return list_page(view, model, "notes", model.notes_json(input_of(view, model, clock_of(view))), page);
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
