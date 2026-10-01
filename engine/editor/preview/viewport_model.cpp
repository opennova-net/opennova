#include <editor/preview/viewport_model.h>

#include <cmath>
#include <cstdint>

#include <editor/model/document_base.h>
#include <editor/preview/viewport_device.h>
#include <editor/preview/viewports.h>
#include <editor/session/view/session_view.h>

namespace opennova::editor {

namespace {

using io::JsonValue;
using io::json_whole_in;

// The common members a SetViewport takes, read into what they set: `device` {width, height}, each
// 1..8192; `clock` {playing, rate, time_ms, ticks}.
struct Common {
	bool device = false;
	int64_t width = 0;
	int64_t height = 0;
	bool playing_set = false;
	bool playing = false;
	bool rate_set = false;
	double rate = 1.0;
	int64_t time_ms = -1;
	int64_t ticks = -1;
};

bool read_device(const JsonValue &json, const ViewportState &state, Common &out, std::string &error) {
	if (!json.is_object()) {
		error = "\"device\" is an object, {width, height}.";
		return false;
	}
	out.device = true;
	out.width = state.width;
	out.height = state.height;
	for (const io::JsonMember &member : json.object) {
		if (member.key != "width" && member.key != "height") {
			error = "Unknown device member \"" + member.key + "\" (it takes width, height).";
			return false;
		}
		if (!json_whole_in(member.value, 1.0, 8192.0, member.key == "width" ? out.width : out.height)) {
			error = "device." + member.key + " is a whole number from 1 to 8192.";
			return false;
		}
	}
	return true;
}

bool read_clock(const JsonValue &json, Common &out, std::string &error) {
	if (!json.is_object()) {
		error = "\"clock\" is an object, {playing, rate, time_ms, ticks}.";
		return false;
	}
	for (const io::JsonMember &member : json.object) {
		const JsonValue &value = member.value;
		if (member.key == "playing") {
			if (!value.is_bool()) {
				error = "clock.playing is true or false.";
				return false;
			}
			out.playing_set = true;
			out.playing = value.boolean;
		} else if (member.key == "rate") {
			if (!value.is_number() || !(value.number >= 0.0) || !std::isfinite(value.number)) {
				error = "clock.rate is a number, 0 or more.";
				return false;
			}
			out.rate_set = true;
			out.rate = value.number;
		} else if (member.key == "time_ms") {
			// Any time from 0 on, one past the clock's last millisecond taken as its last.
			if (!value.is_number() || !(value.number >= 0.0)) {
				error = "clock.time_ms is a number, 0 or more.";
				return false;
			}
			out.time_ms = int64_t(std::fmin(std::trunc(value.number), double(UINT32_MAX)));
		} else if (member.key == "ticks") {
			if (!json_whole_in(value, 0.0, double(INT32_MAX), out.ticks)) {
				error = "clock.ticks is a whole number, 0 or more.";
				return false;
			}
		} else {
			error = "Unknown clock member \"" + member.key + "\" (it takes playing, rate, time_ms, ticks).";
			return false;
		}
	}
	return true;
}

// The clock's members read (read_clock) set on `clock`.
void apply_clock(const Common &common, PreviewClock &clock) {
	if (common.playing_set) clock.set_playing(common.playing);
	if (common.rate_set) clock.set_rate(common.rate);
	if (common.time_ms >= 0) clock.seek_ms(uint32_t(common.time_ms));
	if (common.ticks >= 0) clock.seek_ticks(int32_t(common.ticks));
}

} // namespace

const char *viewport_status_token(ViewportStatus status) {
	switch (status) {
	case ViewportStatus::Empty: return "empty";
	case ViewportStatus::Failed: return "failed";
	case ViewportStatus::Ready: return "ready";
	}
	return "empty";
}

std::string viewport_change(ViewportKind kind, const char *member, io::JsonValue value) {
	JsonValue change = JsonValue::make_object();
	change.set("kind", io::json_string(viewport_kind_token(kind)));
	change.set(member, std::move(value));
	return io::json_write(change);
}

bool set_preview_clock(const io::JsonValue &json, PreviewClock &clock, std::string &error) {
	Common common;
	if (!read_clock(json, common, error)) return false;
	apply_clock(common, clock);
	return true;
}

bool ViewportContext::editable() const {
	return input.document && !input.document->blocked() &&
			input.view.allows(EditorRequestKind::EditRecord);
}

std::string ViewportContext::not_editable() const {
	if (!input.document) return "The document is not open.";
	if (input.document->blocked())
		return input.document->path() + " takes no edit until its file is corrected and read again (it holds what "
										"the editor cannot carry).";
	if (!input.view.allows(EditorRequestKind::EditRecord))
		return "The session takes no edit now (an operation holds the documents).";
	return std::string();
}

ViewportContext viewport_context(const SessionView &view, const ViewportModel &model, float snap) {
	static const PreviewClock kStill;
	const DocumentBase *document = nullptr;
	for (const auto &open : view.documents.open)
		if (open && open->path() == model.path()) document = open.get();
	const ViewportState size = model.size();
	return ViewportContext{
		ViewportInput{ view, view.documents.viewports ? view.documents.viewports->clock() : kStill, document,
				ChangeClass::None },
		size.width, size.height, snap, nullptr
	};
}

ViewportModel::ViewportModel(ViewportKind kind, std::string path, ViewportState state) :
		state_(state), kind_(kind), path_(std::move(path)) {}

ViewportModel::~ViewportModel() = default;

ViewportState ViewportModel::size() const {
	return attached_ && shown_size_.width > 0 && shown_size_.height > 0 ? shown_size_ : state_;
}

bool ViewportModel::current(const ViewportInput &input) const {
	return status() == ViewportStatus::Ready && shows_document_ && input.document &&
			input.document->identity() == shown_identity_ &&
			input.document->load_generation() == shown_load_ &&
			input.document->revision() == shown_revision_;
}

io::JsonValue ViewportModel::notes_json(const ViewportInput &) const {
	return JsonValue::make_array();
}

io::JsonValue ViewportModel::render_json(
		const ViewportInput &, NodeId, const JsonPage &, std::string &error) const {
	error = std::string("a ") + viewport_kind_token(kind_) +
			" viewport's picture is its whole document: it renders no row apart (op state reads it).";
	return JsonValue::make_null();
}

void ViewportModel::shown(const DocumentBase &document) {
	shows_document_ = true;
	shown_identity_ = document.identity();
	shown_load_ = document.load_generation();
	shown_revision_ = document.revision();
}

void ViewportModel::shown_none() {
	shows_document_ = false;
	shown_identity_ = shown_load_ = shown_revision_ = 0;
}

ViewportAction ViewportModel::follow(const ViewportInput &input, PreviewClock &clock) {
	followed_change_ = input.change;
	// The token of the gesture open in its document (0: none), where its kind's picture made again
	// waits for one.
	const uint64_t gesture = row().holds_for_gesture ? input.view.documents.gesture_in(path_).token : 0;
	// A Rebuild held for a gesture that ended since (or gave way to another) is due now.
	if (held_ && gesture != held_for_) {
		held_ = false;
		pending_ = ViewportAction::Rebuild;
	}
	switch (follow_(input, clock)) {
	case ViewportAction::Keep: break;
	case ViewportAction::Update:
		if (pending_ == ViewportAction::Keep) pending_ = ViewportAction::Update;
		break;
	case ViewportAction::Rebuild:
		// The device keeps the picture it holds while a gesture is open in the document: it is made
		// again at the gesture's end. A device holding none (never made, or cleared), or with a Rebuild
		// or a Clear to take, makes it now.
		if (gesture && holds_ && (pending_ == ViewportAction::Keep || pending_ == ViewportAction::Update)) {
			held_ = true;
			held_for_ = gesture;
		} else {
			pending_ = ViewportAction::Rebuild;
		}
		break;
	case ViewportAction::Clear:
		// A picture the device holds is dropped; one it was about to make, or held, is not made.
		held_ = false;
		pending_ = holds_ ? ViewportAction::Clear : ViewportAction::Keep;
		break;
	}
	return pending_;
}

bool ViewportModel::apply(const io::JsonValue &json, PreviewClock &clock, std::string &error) {
	if (!json.is_object()) {
		error = "A viewport's change is a JSON object.";
		return false;
	}
	Common common;
	for (const io::JsonMember &member : json.object) {
		if (member.key == "kind") {
			ViewportKind named = ViewportKind::kCount;
			if (!member.value.is_string() || !viewport_kind_from_token(member.value.string, named) ||
					named != kind_) {
				error = std::string("\"kind\" names this viewport's kind, \"") + viewport_kind_token(kind_) + "\".";
				return false;
			}
		} else if (member.key == "device") {
			if (!read_device(member.value, state_, common, error)) return false;
		} else if (member.key == "clock") {
			if (!read_clock(member.value, common, error)) return false;
		} else if (!takes_(member.key)) {
			error = "Unknown viewport member \"" + member.key + "\".";
			return false;
		}
	}
	if (common.device && canvas_sized_) {
		error = "device: a canvas draws this viewport at a size of its own; the device's size is set "
				"only where no canvas sizes the picture (a headless editor, a menu at its Device size).";
		return false;
	}
	if (!check_(json, error)) return false;
	if (common.device) {
		state_.width = int(common.width);
		state_.height = int(common.height);
	}
	apply_clock(common, clock);
	apply_(json, clock);
	++state_serial_;
	return true;
}

ViewportAction ViewportModel::take_action() {
	if (!attached_) return ViewportAction::Keep;
	const ViewportAction action = pending_;
	pending_ = ViewportAction::Keep;
	if (action == ViewportAction::Rebuild) {
		++builds_;
		holds_ = true;
	} else if (action == ViewportAction::Clear) {
		holds_ = false;
	}
	return action;
}

void ViewportModel::attach() {
	attached_ = true;
	holds_ = false;
	held_ = false;
	// The device holds nothing yet: its first action makes the picture, if there is one (a gesture
	// open or not: there is no last picture to keep).
	pending_ = status() == ViewportStatus::Ready ? ViewportAction::Rebuild : ViewportAction::Keep;
}

void ViewportModel::detach() {
	attached_ = false;
	holds_ = false;
	held_ = false;
	pending_ = ViewportAction::Keep;
	shown_size_ = ViewportState();
	canvas_sized_ = false;
}

void ViewportModel::device_report(const ViewportDeviceReport &report) {
	shown_size_ = ViewportState{ report.width, report.height };
	canvas_sized_ = report.canvas_sized;
	report_(report);
}

} // namespace opennova::editor
