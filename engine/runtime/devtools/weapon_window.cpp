#include <runtime/devtools/weapon_window.h>
#include <base/io/tick_rate.h>

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace opennova::devtools {
namespace {

namespace wa = world::weapon_action;
namespace wp = world::weapon_phase;

// The logic clock. 1000 / 62.5 is exactly 16, so a tick is 16 ms and the ms
// readouts carry no rounding noise.
constexpr double kMsPerTick = 1000.0 / io::kTickHz;

constexpr float kChannelWidth = 176.0f;  // name at the left, timing right-aligned
constexpr float kRowHeight = 22.0f;
constexpr float kRulerHeight = 30.0f;    // two lines: ticks over ms
constexpr float kHandleHalf = 6.0f;      // grab radius either side of a handle
constexpr float kMinStripPx = 4.0f;      // a {0,0} action stays visible and grabbable
constexpr float kMinZoom = 0.05f;
constexpr float kMaxZoom = 60.0f;
constexpr float kTraceChannelHeight = 20.0f;
constexpr int kTraceChannels = 5;

const char *const kActionNames[wa::kCount] = {
		"IDLE", "EMPTYIDLE", "FIRE", "RECOIL", "RELOAD", "EMPTY",
		"SWITCHTO", "SWITCHFROM", "SWITCHRANK", "SCOPEUP", "SCOPEDOWN", "OVERHEATED"};

const char *action_name(int id) {
	return (id >= 0 && id < wa::kCount) ? kActionNames[id] : "?";
}

const char *phase_name(uint8_t phase) {
	// The pending-reload bit rides on top of the phase value.
	switch (phase & static_cast<uint8_t>(~wp::kReloadPendingBit)) {
		case wp::kNone: return "-";
		case wp::kEntered: return "ENTER";
		case wp::kActive: return "ACTIVE";
		case wp::kDone: return "DONE";
		case wp::kHeld: return "HELD";
		default: return "?";
	}
}

// Twelve evenly spaced hues, so a strip in the dope sheet and the same action
// in the trace read as the same colour.
ImU32 action_color(int id, float saturation, float value, float alpha) {
	const float hue = static_cast<float>(id) / static_cast<float>(wa::kCount);
	return ImGui::ColorConvertFloat4ToU32(
			static_cast<ImVec4>(ImColor::HSV(hue, saturation, value, alpha)));
}

float x_of(float x0, float ppt, double origin, double tick) {
	return x0 + static_cast<float>((tick - origin) * static_cast<double>(ppt));
}

double tick_of(float x0, float ppt, double origin, float x) {
	return origin + static_cast<double>(x - x0) / static_cast<double>(ppt);
}

// The disabled-button contract: an empty `block` enables the button, a
// non-empty one disables it AND becomes the tooltip explaining why.
bool action_button(const char *label, const char *block) {
	const bool blocked = block != nullptr && block[0] != '\0';
	ImGui::BeginDisabled(blocked);
	const bool pressed = ImGui::Button(label);
	ImGui::EndDisabled();
	if (blocked && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled |
										ImGuiHoveredFlags_ForTooltip)) {
		ImGui::SetTooltip("%s", block);
	}
	return pressed;
}

// Case-insensitive ASCII compare. The clip-variant rings key on a LOWERCASED
// name while an ACTION row authors whatever case it likes, so the picker's
// resolve check has to match the way the runtime itself looks a clip up.
bool iequals_ascii(const std::string &a, const std::string &b) {
	if (a.size() != b.size()) return false;
	for (size_t i = 0; i < a.size(); ++i) {
		const unsigned char ca = static_cast<unsigned char>(a[i]);
		const unsigned char cb = static_cast<unsigned char>(b[i]);
		if (std::tolower(ca) != std::tolower(cb)) return false;
	}
	return true;
}

// The coarsest label step that still leaves at least ~72 px between labels.
double ruler_step(float ppt) {
	static const double kSteps[] = {1, 2, 5, 10, 25, 50, 100, 250, 500, 1000, 2500, 5000};
	for (const double step : kSteps) {
		if (step * static_cast<double>(ppt) >= 72.0) return step;
	}
	return kSteps[sizeof(kSteps) / sizeof(kSteps[0]) - 1];
}

// Zoom that fits `ticks` into `width` with a little air on the right.
float fit_zoom(float width, double ticks) {
	return std::clamp(static_cast<float>(static_cast<double>(width) / (std::max(ticks, 1.0) * 1.06)),
			kMinZoom, kMaxZoom);
}

// Faint vertical lines at the ruler's steps, so a strip edge can be read
// against the scale without looking up.
void draw_grid(ImDrawList *dl, float x0, float x1, float y, float height, float ppt,
		double origin) {
	const double step = ruler_step(ppt);
	const double first = std::floor(origin / step) * step;
	const double last = tick_of(x0, ppt, origin, x1);
	for (double t = first; t <= last; t += step) {
		const float x = x_of(x0, ppt, origin, t);
		if (x < x0 || x > x1) continue;
		dl->AddLine(ImVec2(x, y), ImVec2(x, y + height), IM_COL32(255, 255, 255, 14), 1.0f);
	}
}

}  // namespace

// --- records in -----------------------------------------------------------

void WeaponWindow::set_definition(WeaponDefinitionSnapshot definition) {
	if (!definition.valid) {
		// The world unloaded or the weapon cleared: nothing carries over.
		definition_ = WeaponDefinitionSnapshot{};
		live_ = WeaponLiveSnapshot{};
		trace_.clear();
		measure_valid_ = false;
		field_action_ = -1;
		format_rows();
		return;
	}
	// A different weapon is a different definition. The definition is kept
	// across a hide precisely so this comparison still works on the reopen.
	if (!definition_.valid || definition_.weapon_name != definition.weapon_name) {
		trace_.clear();
		measure_valid_ = false;
		fit_pending_ = true;
		field_action_ = -1;
	}
	definition_ = std::move(definition);
	format_rows();
}

void WeaponWindow::set_live(WeaponLiveSnapshot live) {
	if (!live.valid) {
		live_ = WeaponLiveSnapshot{};
		return;
	}
	// The pushed vector holds only what the pump recorded since the last push;
	// the window is what accumulates it into a scrollback. A re-show can hand
	// back ticks already held (the embedder re-primes its cursor), so only
	// ticks newer than the newest held one append; a batch that starts far
	// below it is a restarted logic clock (a round restart, a new world), not
	// a duplicate, and the scrollback restarts with it.
	if (!live.trace.empty()) {
		if (!trace_.empty() && live.trace.front().tick + kTraceCapacity * 2 < trace_.back().tick) {
			trace_.clear();
			measure_valid_ = false;
		}
		size_t appended = 0;
		for (const world::WeaponTraceSample &sample : live.trace) {
			if (!trace_.empty() && sample.tick <= trace_.back().tick) continue;
			trace_.push_back(sample);
			++appended;
		}
		while (trace_.size() > kTraceCapacity) trace_.pop_front();
		// A batch of more than a second is a reopen pulling what the ring
		// recorded while the window was hidden: View All, so the whole burst
		// is on screen instead of its last two seconds riding the playhead.
		if (appended > 62) fit_trace_pending_ = true;
	}
	// Reconcile REC against the ring the embedder actually holds: an arm
	// queued with no world behind it drains and drops, so the switch re-asks
	// until the engine agrees.
	if (live.trace_armed != recording_) {
		WeaponRequest request;
		request.kind = WeaponRequest::Kind::ArmTrace;
		request.armed = recording_;
		enqueue_request(request);
	}
	live_ = std::move(live);
}

void WeaponWindow::on_visibility(bool visible) {
	shown_ = visible;
	if (!visible) {
		// The live record goes (the embedder stops building it) and the drag
		// state goes, but the definition and the scrollback stay: the natural
		// loop is to close F3, shoot with the real mouse, and reopen to read
		// what happened.
		live_ = WeaponLiveSnapshot{};
		drag_action_ = -1;
		// A hold cannot outlive the UI that shows it.
		queue_fire_held(false);
		// REC is the user's explicit switch. While it is on the engine ring
		// keeps recording through a hide (one sample copy per pump tick), so
		// the reopen shows the last ~16 s; off, the ring is released.
		if (!recording_) {
			WeaponRequest request;
			request.kind = WeaponRequest::Kind::ArmTrace;
			request.armed = false;
			enqueue_request(request);
		}
	} else {
		fit_pending_ = true;
		if (!recording_ && !rec_touched_) set_recording(true);
	}
}

void WeaponWindow::enqueue_request(const WeaponRequest &request) { requests_.push_back(request); }

bool WeaponWindow::take_request(WeaponRequest &request) {
	if (requests_.empty()) return false;
	request = requests_.front();
	requests_.pop_front();
	return true;
}

void WeaponWindow::select_action(int action_id) {
	if (action_id < 0 || action_id >= wa::kCount) return;
	selected_ = action_id;
}

// --- formatting -----------------------------------------------------------

void WeaponWindow::format_rows() {
	for (int id = 0; id < wa::kCount; ++id) {
		const WeaponActionRow &row = definition_.actions[id];
		RowText &text = texts_[static_cast<size_t>(id)];
		text.label = kActionNames[id];
		if (!definition_.valid) {
			text.timing.clear();
			continue;
		}
		// The three slots shipped data never authors bake from generated
		// defaults; an edit to them cannot be written back to a row.
		if (!row.authored) text.label += " *";
		// `auto` and an explicit value are different authorings and read
		// differently: auto shows what it baked to.
		char start[24];
		char end[24];
		if (row.authored && row.authored_delay_start < 0) {
			std::snprintf(start, sizeof(start), "auto(%d)", row.delay_start);
		} else {
			std::snprintf(start, sizeof(start), "%d", row.delay_start);
		}
		if (row.authored && row.authored_delay_end < 0) {
			std::snprintf(end, sizeof(end), "auto(%d)", row.delay_end);
		} else {
			std::snprintf(end, sizeof(end), "%d", row.delay_end);
		}
		char buf[80];
		std::snprintf(buf, sizeof(buf), "%s / %s", start, end);
		text.timing = buf;
	}
}

const char *WeaponWindow::action_label(int action_id) const {
	if (action_id < 0 || action_id >= wa::kCount) return "";
	return texts_[static_cast<size_t>(action_id)].label.c_str();
}

const char *WeaponWindow::action_timing(int action_id) const {
	if (action_id < 0 || action_id >= wa::kCount) return "";
	return texts_[static_cast<size_t>(action_id)].timing.c_str();
}

const char *WeaponWindow::trace_row(int index) const {
	if (index < 0 || index >= trace_count()) return "";
	const world::WeaponTraceSample &s = trace_[static_cast<size_t>(index)];
	char events[112] = {};
	size_t used = 0;
	const auto append = [&](const char *what) {
		if (used >= sizeof(events)) return;
		const int n = std::snprintf(events + used, sizeof(events) - used, "%s%s",
				used == 0 ? "" : " ", what);
		if (n > 0) used += static_cast<size_t>(n);
	};
	if (s.fired) append("fired");
	if (s.dry_fired) append("dry");
	if (s.action_started >= 0) {
		char tmp[40];
		std::snprintf(tmp, sizeof(tmp), "start=%s", action_name(s.action_started));
		append(tmp);
	}
	if (s.action_finished >= 0) {
		char tmp[40];
		std::snprintf(tmp, sizeof(tmp), "end=%s", action_name(s.action_finished));
		append(tmp);
	}
	if (s.action_effect >= 0) append("effect");
	if (s.reload_requested) append("reload_req");
	if (s.reload_applied) append("reload_applied");

	char buf[224];
	std::snprintf(buf, sizeof(buf), "%u %s %s c%d%s%s", static_cast<unsigned>(s.tick),
			action_name(s.current), phase_name(s.phase), s.counter,
			used > 0 ? " " : "", events);
	scratch_ = buf;
	return scratch_.c_str();
}

// --- request helpers ------------------------------------------------------

void WeaponWindow::queue_delays(int action_id, int32_t delay_start, int32_t delay_end,
		bool rebake) {
	WeaponRequest request;
	request.kind = WeaponRequest::Kind::SetActionDelays;
	request.action_id = action_id;
	request.delay_start = delay_start;
	request.delay_end = delay_end;
	request.rebake = rebake;
	enqueue_request(request);
}

void WeaponWindow::request_delay_edit(int action_id, DelayLeg leg, int32_t value) {
	if (action_id < 0 || action_id >= wa::kCount || !definition_.valid) return;
	const WeaponActionRow &row = definition_.actions[action_id];
	value = std::max<int32_t>(0, value);
	// The untouched leg travels in its AUTHORED form so an `auto` stays `auto`;
	// a slot with no authored row has only its baked values to keep.
	const int32_t other_start = row.authored ? row.authored_delay_start : row.delay_start;
	const int32_t other_end = row.authored ? row.authored_delay_end : row.delay_end;
	if (leg == DelayLeg::Start) {
		queue_delays(action_id, value, other_end, false);
	} else {
		queue_delays(action_id, other_start, value, false);
	}
}

void WeaponWindow::request_delay_auto(int action_id, DelayLeg leg, bool is_auto) {
	if (action_id < 0 || action_id >= wa::kCount || !definition_.valid) return;
	const WeaponActionRow &row = definition_.actions[action_id];
	if (!row.authored) return;  // no row to write `auto` into
	// -1 is the parser's `auto` sentinel and needs the re-bake that resolves
	// it from the clip; turning it off freezes the value the clip most
	// recently baked to, which needs no re-bake at all.
	const int32_t start = leg == DelayLeg::Start ? (is_auto ? -1 : row.delay_start)
												: row.authored_delay_start;
	const int32_t end = leg == DelayLeg::End ? (is_auto ? -1 : row.delay_end)
											: row.authored_delay_end;
	queue_delays(action_id, start, end, is_auto);
}

void WeaponWindow::queue_text(int action_id, WeaponRequest::TextField field, const char *text) {
	WeaponRequest request;
	request.kind = WeaponRequest::Kind::SetActionText;
	request.action_id = action_id;
	request.field = field;
	std::snprintf(request.text, sizeof(request.text), "%s", text != nullptr ? text : "");
	enqueue_request(request);
}

void WeaponWindow::queue_trigger(WeaponRequest::Trigger trigger) {
	WeaponRequest request;
	request.kind = WeaponRequest::Kind::TriggerAction;
	request.trigger = trigger;
	enqueue_request(request);
}

void WeaponWindow::queue_fire_held(bool held) {
	WeaponRequest request;
	request.kind = WeaponRequest::Kind::SetFireHeld;
	request.held = held;
	enqueue_request(request);
}

void WeaponWindow::set_recording(bool recording) {
	recording_ = recording;
	WeaponRequest request;
	request.kind = WeaponRequest::Kind::ArmTrace;
	request.armed = recording;
	enqueue_request(request);
}

// --- shared axis chrome ---------------------------------------------------

bool WeaponWindow::handle_axis_input(float x0, float x1, Axis &axis, bool clamp_to_zero) {
	if (!ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows |
				ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)) {
		return false;
	}
	const ImGuiIO &io = ImGui::GetIO();
	bool interacted = false;
	// A pane narrower than its channel column has x1 < x0; clamp to a valid
	// (possibly empty) range rather than hand std::clamp inverted bounds.
	const float mouse_x = std::clamp(io.MousePos.x, x0, std::max(x0, x1));
	if (io.MouseWheel != 0.0f) {
		// Zoom about the cursor so the tick under the pointer stays put.
		const double anchor = tick_of(x0, axis.pixels_per_tick, axis.origin_tick, mouse_x);
		const float factor = io.MouseWheel > 0.0f ? 1.15f : 1.0f / 1.15f;
		axis.pixels_per_tick = std::clamp(axis.pixels_per_tick * factor, kMinZoom, kMaxZoom);
		axis.origin_tick = anchor - static_cast<double>(mouse_x - x0) /
										   static_cast<double>(axis.pixels_per_tick);
		interacted = true;
	}
	if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle) ||
			ImGui::IsMouseDragging(ImGuiMouseButton_Right)) {
		axis.origin_tick -= static_cast<double>(io.MouseDelta.x) /
							static_cast<double>(axis.pixels_per_tick);
		interacted = true;
	}
	if (clamp_to_zero && axis.origin_tick < 0.0) axis.origin_tick = 0.0;
	return interacted;
}

void WeaponWindow::draw_ruler(float x0, float x1, float y, const Axis &axis, double ms_zero_tick) {
	ImDrawList *dl = ImGui::GetWindowDrawList();
	const ImU32 line = IM_COL32(120, 120, 128, 150);
	const ImU32 text = IM_COL32(205, 205, 212, 255);
	const ImU32 dim = IM_COL32(135, 135, 145, 255);
	dl->AddRectFilled(ImVec2(x0, y), ImVec2(x1, y + kRulerHeight), IM_COL32(28, 28, 32, 255));

	const double step = ruler_step(axis.pixels_per_tick);
	const double first = std::floor(axis.origin_tick / step) * step;
	const double last = tick_of(x0, axis.pixels_per_tick, axis.origin_tick, x1);
	for (double t = first; t <= last; t += step) {
		const float x = x_of(x0, axis.pixels_per_tick, axis.origin_tick, t);
		if (x < x0 - 1.0f || x > x1) continue;
		dl->AddLine(ImVec2(x, y + kRulerHeight - 6.0f), ImVec2(x, y + kRulerHeight), line, 1.0f);
		char label[32];
		// Ticks are the unit being edited; ms is the unit being felt.
		std::snprintf(label, sizeof(label), "%.0f", t);
		dl->AddText(ImVec2(x + 3.0f, y + 1.0f), text, label);
		std::snprintf(label, sizeof(label), "%.0f ms", (t - ms_zero_tick) * kMsPerTick);
		dl->AddText(ImVec2(x + 3.0f, y + 15.0f), dim, label);
	}
	dl->AddLine(ImVec2(x0, y + kRulerHeight), ImVec2(x1, y + kRulerHeight), line, 1.0f);
}

void WeaponWindow::fit_dope_sheet(float width) {
	// View All: the longest strip or clip ghost decides the scale.
	double longest = 32.0;
	for (const WeaponActionRow &row : definition_.actions) {
		longest = std::max(longest, static_cast<double>(row.delay_start) + row.delay_end);
		longest = std::max(longest, static_cast<double>(row.clip_ticks));
	}
	def_axis_.pixels_per_tick = fit_zoom(width, longest);
	def_axis_.origin_tick = 0.0;
	fit_pending_ = false;
}

void WeaponWindow::frame_action(float width, int action_id) {
	// View Selected: one strip across ~two thirds of the pane.
	if (action_id < 0 || action_id >= wa::kCount) return;
	const WeaponActionRow &row = definition_.actions[action_id];
	const double span = std::max(8.0,
			std::max(static_cast<double>(row.delay_start) + row.delay_end,
					static_cast<double>(row.clip_ticks)));
	def_axis_.pixels_per_tick = fit_zoom(width * 0.66f, span);
	def_axis_.origin_tick = 0.0;
	fit_pending_ = false;
}

void WeaponWindow::fit_trace(float width) {
	fit_trace_pending_ = false;
	if (trace_.empty()) return;
	const double oldest = static_cast<double>(trace_.front().tick);
	const double newest = static_cast<double>(trace_.back().tick);
	const double span = std::max(32.0, newest - oldest + 1.0);
	trace_axis_.pixels_per_tick = fit_zoom(width, span);
	trace_axis_.origin_tick = oldest - span * 0.01;
	trace_follow_ = false;
}

// --- the dope sheet -------------------------------------------------------

void WeaponWindow::draw_dope_sheet(float height) {
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
	const bool open = ImGui::BeginChild("##weapon_dope", ImVec2(0.0f, height),
			ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
	ImGui::PopStyleVar();
	if (!open) {
		ImGui::EndChild();
		return;
	}

	const ImVec2 origin = ImGui::GetCursorScreenPos();
	const float width = ImGui::GetContentRegionAvail().x;
	const float x0 = origin.x + kChannelWidth;
	const float x1 = std::max(origin.x + width, x0);
	const float axis_w = std::max(x1 - x0, 1.0f);
	ImDrawList *dl = ImGui::GetWindowDrawList();
	const bool pane_hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows |
			ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);

	if (fit_pending_) fit_dope_sheet(axis_w);
	handle_axis_input(x0, x1, def_axis_, true);
	if (pane_hovered && !ImGui::GetIO().WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Home, false)) {
		fit_dope_sheet(axis_w);
	}

	draw_ruler(x0, x1, origin.y, def_axis_, 0.0);
	const float rows_y = origin.y + kRulerHeight;
	const float rows_h = kRowHeight * wa::kCount;
	const float ppt = def_axis_.pixels_per_tick;
	const double org = def_axis_.origin_tick;
	const ImVec2 mouse = ImGui::GetIO().MousePos;

	// Row bands across the full width, with the selected and hovered rows lit.
	for (int id = 0; id < wa::kCount; ++id) {
		const float ry = rows_y + static_cast<float>(id) * kRowHeight;
		dl->AddRectFilled(ImVec2(origin.x, ry), ImVec2(x1, ry + kRowHeight - 1.0f),
				(id & 1) ? IM_COL32(38, 38, 44, 255) : IM_COL32(32, 32, 38, 255));
		const bool hovered = pane_hovered && mouse.y >= ry && mouse.y < ry + kRowHeight &&
				mouse.x >= origin.x && mouse.x < x1;
		if (id == selected_) {
			dl->AddRectFilled(ImVec2(origin.x, ry), ImVec2(x1, ry + kRowHeight - 1.0f),
					IM_COL32(80, 110, 160, 48));
		} else if (hovered) {
			dl->AddRectFilled(ImVec2(origin.x, ry), ImVec2(x1, ry + kRowHeight - 1.0f),
					IM_COL32(255, 255, 255, 10));
		}
	}

	// Content is clipped to the axis area; the channel column is drawn after
	// it, where a strip scrolled off the left cannot paint over the names.
	dl->PushClipRect(ImVec2(x0, rows_y), ImVec2(x1, rows_y + rows_h), true);
	draw_grid(dl, x0, x1, rows_y, rows_h, ppt, org);
	for (int id = 0; id < wa::kCount; ++id) {
		const WeaponActionRow &row = definition_.actions[id];
		const float ry = rows_y + static_cast<float>(id) * kRowHeight;
		const bool selected = id == selected_;

		// The resolved clip as a ghost bar: this is what an `auto` delay bakes
		// from, so a mismatch against the strip is the thing worth seeing.
		if (row.clip_ticks > 0) {
			const float gx0 = x_of(x0, ppt, org, 0.0);
			const float gx1 = x_of(x0, ppt, org, row.clip_ticks);
			dl->AddRectFilled(ImVec2(gx0, ry + kRowHeight - 6.0f),
					ImVec2(std::max(gx1, gx0 + 1.0f), ry + kRowHeight - 3.0f),
					IM_COL32(140, 140, 150, 110));
		}

		// The strip: [delay_start | delay_end], split at the arbiter tick where
		// the counter reaches 0 and the handler makes its decision.
		const double t_arbiter = row.delay_start;
		const double t_end = static_cast<double>(row.delay_start) + row.delay_end;
		const float sx0 = x_of(x0, ppt, org, 0.0);
		float sxa = x_of(x0, ppt, org, t_arbiter);
		float sx1 = x_of(x0, ppt, org, t_end);
		if (sx1 - sx0 < kMinStripPx) sx1 = sx0 + kMinStripPx;
		sxa = std::clamp(sxa, sx0, sx1);
		const float top = ry + 3.0f;
		const float bottom = ry + kRowHeight - 7.0f;
		dl->AddRectFilled(ImVec2(sx0, top), ImVec2(sxa, bottom),
				action_color(id, 0.45f, 0.42f, 1.0f), 2.0f);
		dl->AddRectFilled(ImVec2(sxa, top), ImVec2(sx1, bottom),
				action_color(id, 0.62f, 0.72f, 1.0f), 2.0f);
		dl->AddLine(ImVec2(sxa, ry + 1.0f), ImVec2(sxa, ry + kRowHeight - 5.0f),
				IM_COL32(250, 240, 190, 230), 2.0f);
		if (selected) {
			dl->AddRect(ImVec2(sx0 - 1.0f, top - 1.0f), ImVec2(sx1 + 1.0f, bottom + 1.0f),
					IM_COL32(255, 255, 255, 190), 2.0f, 0, 1.5f);
		}

		// The legs, at the ticks they are predicted to fire: the begin sound on
		// entry, the end sound and the effect at the arbiter. The trace pane
		// below shows where they ACTUALLY landed.
		const ImU32 sound_col = IM_COL32(150, 220, 255, 235);
		const ImU32 effect_col = IM_COL32(255, 190, 120, 235);
		if (!row.soundset.empty()) {
			dl->AddCircleFilled(ImVec2(sx0 + 2.0f, ry + 5.0f), 3.0f, sound_col);
		}
		if (!row.soundsetend.empty()) {
			dl->AddCircleFilled(ImVec2(sxa, ry + 5.0f), 3.0f, sound_col);
			dl->AddText(ImVec2(sxa + 6.0f, ry + 1.0f), sound_col, row.soundsetend.c_str());
		}
		if (!row.particle.empty()) {
			dl->AddTriangleFilled(ImVec2(sxa - 3.0f, ry + kRowHeight - 4.0f),
					ImVec2(sxa + 3.0f, ry + kRowHeight - 4.0f), ImVec2(sxa, ry + kRowHeight - 9.0f),
					effect_col);
		}

		// Hit zones. The row-select button is submitted first with overlap
		// allowed so the two drag handles on top of it win the hit test.
		ImGui::PushID(id);
		ImGui::SetCursorScreenPos(ImVec2(origin.x, ry));
		ImGui::SetNextItemAllowOverlap();
		ImGui::InvisibleButton("##row", ImVec2(std::max(x1 - origin.x, 1.0f), kRowHeight - 1.0f));
		if (ImGui::IsItemClicked()) select_action(id);
		if (ImGui::IsItemHovered()) {
			if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) frame_action(axis_w, id);
			ImGui::SetTooltip("%s%s\nDELAYSTART %d  (%.0f ms)\nDELAYEND   %d  (%.0f ms)\n"
							  "clip %s%d ticks\n\ndrag the divider or the right edge to retime\n"
							  "double-click to frame",
					kActionNames[id], row.authored ? "" : "  (not authored)", row.delay_start,
					row.delay_start * kMsPerTick, row.delay_end, row.delay_end * kMsPerTick,
					row.clip_ticks > 0 ? "" : "unresolved, ", row.clip_ticks);
		}

		const int32_t total = row.delay_start + row.delay_end;
		// handle 1 = the arbiter divider (delaystart), handle 2 = the right edge
		// (delayend).
		for (int handle = 1; handle <= 2; ++handle) {
			const float hx = handle == 1 ? sxa : sx1;
			ImGui::SetCursorScreenPos(ImVec2(hx - kHandleHalf, ry + 1.0f));
			ImGui::PushID(handle);
			ImGui::InvisibleButton("##h", ImVec2(kHandleHalf * 2.0f, kRowHeight - 3.0f));
			const bool hovered = ImGui::IsItemHovered();
			if (hovered || (drag_action_ == id && drag_handle_ == handle)) {
				ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
				dl->AddLine(ImVec2(hx, ry + 1.0f), ImVec2(hx, ry + kRowHeight - 4.0f),
						IM_COL32(255, 255, 255, 220), 2.0f);
			}
			if (ImGui::IsItemActivated()) {
				select_action(id);
				drag_action_ = id;
				drag_handle_ = handle;
				drag_start_value_ = handle == 1 ? row.delay_start : row.delay_end;
				drag_start_other_ = handle == 1 ? total : row.delay_start;
				drag_accum_ = 0.0f;
			}
			if (ImGui::IsItemActive() && drag_action_ == id && drag_handle_ == handle) {
				drag_accum_ += ImGui::GetIO().MouseDelta.x;
				const int32_t delta =
						static_cast<int32_t>(std::lround(drag_accum_ / static_cast<double>(ppt)));
				int32_t next_start = row.delay_start;
				int32_t next_end = row.delay_end;
				if (handle == 1) {
					next_start = std::max(0, drag_start_value_ + delta);
					if (ImGui::GetIO().KeyCtrl) {
						// Ctrl keeps the total duration and moves only the
						// split: both legs go explicit.
						next_start = std::min(next_start, drag_start_other_);
						next_end = drag_start_other_ - next_start;
						if (next_start != row.delay_start || next_end != row.delay_end) {
							queue_delays(id, next_start, next_end, false);
						}
					} else if (next_start != row.delay_start) {
						request_delay_edit(id, DelayLeg::Start, next_start);
					}
				} else {
					next_end = std::max(0, drag_start_value_ + delta);
					if (next_end != row.delay_end) request_delay_edit(id, DelayLeg::End, next_end);
				}
				ImGui::SetTooltip("%s  %s\nDELAYSTART %d  (%.0f ms)\nDELAYEND   %d  (%.0f ms)",
						kActionNames[id], handle == 1 ? "delaystart" : "delayend", next_start,
						next_start * kMsPerTick, next_end, next_end * kMsPerTick);
			}
			if (ImGui::IsItemDeactivated() && drag_action_ == id && drag_handle_ == handle) {
				drag_action_ = -1;
			}
			ImGui::PopID();
		}
		ImGui::PopID();
	}

	// The live playhead: where the running action's counter currently sits.
	if (live_.valid && live_.current >= 0 && live_.current < wa::kCount) {
		const WeaponActionRow &live = definition_.actions[live_.current];
		// The counter runs DOWN, so elapsed is the far side of whichever
		// segment is ticking; phase tells which.
		const bool in_tail = (live_.phase & static_cast<uint8_t>(~wp::kReloadPendingBit)) ==
				wp::kDone;
		const double elapsed = in_tail
				? static_cast<double>(live.delay_start) + (live.delay_end - live_.counter)
				: static_cast<double>(live.delay_start - live_.counter);
		const float px = x_of(x0, ppt, org, std::max(elapsed, 0.0));
		const float ry = rows_y + static_cast<float>(live_.current) * kRowHeight;
		dl->AddLine(ImVec2(px, ry), ImVec2(px, ry + kRowHeight - 1.0f),
				IM_COL32(255, 90, 90, 240), 2.0f);
	}
	dl->PopClipRect();

	// The channel column: the name at the left, the authored/baked timing
	// right-aligned beside the axis so every track reads its numbers.
	for (int id = 0; id < wa::kCount; ++id) {
		const float ry = rows_y + static_cast<float>(id) * kRowHeight;
		const RowText &text = texts_[static_cast<size_t>(id)];
		dl->AddText(ImVec2(origin.x + 4.0f, ry + 4.0f),
				definition_.actions[id].authored ? IM_COL32(215, 215, 222, 255)
												 : IM_COL32(130, 130, 138, 255),
				text.label.c_str());
		const ImVec2 size = ImGui::CalcTextSize(text.timing.c_str());
		dl->AddText(ImVec2(x0 - 6.0f - size.x, ry + 4.0f), IM_COL32(160, 165, 175, 255),
				text.timing.c_str());
	}

	ImGui::SetCursorScreenPos(ImVec2(origin.x, rows_y + rows_h));
	ImGui::EndChild();
}

// --- the trace ------------------------------------------------------------

void WeaponWindow::draw_trace(float height) {
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
	const bool open = ImGui::BeginChild("##weapon_trace", ImVec2(0.0f, height),
			ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
	ImGui::PopStyleVar();
	if (!open) {
		ImGui::EndChild();
		return;
	}

	const ImVec2 origin = ImGui::GetCursorScreenPos();
	const float width = ImGui::GetContentRegionAvail().x;
	const float x0 = origin.x + kChannelWidth;
	const float x1 = std::max(origin.x + width, x0);
	const float axis_w = std::max(x1 - x0, 1.0f);
	ImDrawList *dl = ImGui::GetWindowDrawList();

	if (trace_.empty()) {
		ImGui::SetCursorScreenPos(ImVec2(origin.x + 6.0f, origin.y + 6.0f));
		ImGui::TextDisabled(recording_
						? "Recording. Fire, reload or switch to fill the trace."
						: "Trace disarmed.");
		ImGui::EndChild();
		return;
	}

	const bool pane_hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows |
			ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
	const double newest = static_cast<double>(trace_.back().tick);
	if (fit_trace_pending_) fit_trace(axis_w);
	if (pane_hovered && !ImGui::GetIO().WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Home, false)) {
		fit_trace(axis_w);
	}
	if (trace_follow_) {
		// Ride the newest tick with a small margin so the playhead is not glued
		// to the right edge.
		const double visible = static_cast<double>(axis_w) / static_cast<double>(trace_axis_.pixels_per_tick);
		trace_axis_.origin_tick = newest - visible + std::min(visible * 0.1, 12.0);
	}
	// Any manual pan or zoom drops follow; the checkbox re-arms it.
	if (handle_axis_input(x0, x1, trace_axis_, false)) trace_follow_ = false;

	draw_ruler(x0, x1, origin.y, trace_axis_, newest);
	const float ppt = trace_axis_.pixels_per_tick;
	const double org = trace_axis_.origin_tick;

	// The ruler doubles as the measure strip: drag across it to read a span in
	// ticks, ms and rounds-per-minute.
	ImGui::SetCursorScreenPos(ImVec2(x0, origin.y));
	ImGui::InvisibleButton("##measure", ImVec2(axis_w, kRulerHeight));
	if (ImGui::IsItemActivated()) {
		measuring_ = true;
		measure_from_ = tick_of(x0, ppt, org, ImGui::GetIO().MousePos.x);
		measure_to_ = measure_from_;
		measure_valid_ = true;
	}
	if (ImGui::IsItemActive() && measuring_) {
		measure_to_ = tick_of(x0, ppt, org, ImGui::GetIO().MousePos.x);
	}
	if (ImGui::IsItemDeactivated()) measuring_ = false;
	if (ImGui::IsItemHovered() && !measuring_) {
		ImGui::SetTooltip("drag to measure a span (ticks, ms, rpm)");
	}

	const float ch_h = kTraceChannelHeight;
	const float body_y = origin.y + kRulerHeight;
	const float body_h = ch_h * kTraceChannels;
	const char *const channel_names[kTraceChannels] = {"FSM", "anim", "sound", "effect", "phase"};
	const auto channel_y = [&](int i) { return body_y + ch_h * static_cast<float>(i); };

	for (int i = 0; i < kTraceChannels; ++i) {
		dl->AddRectFilled(ImVec2(x0, channel_y(i)), ImVec2(x1, channel_y(i) + ch_h - 1.0f),
				IM_COL32(30, 30, 36, 255));
	}

	// The body is one hit zone: hovering a tick reads it out in full, and a
	// click selects the action that was current on it in the dope sheet.
	ImGui::SetCursorScreenPos(ImVec2(x0, body_y));
	ImGui::InvisibleButton("##body", ImVec2(axis_w, body_h));
	if (ImGui::IsItemHovered()) {
		const double at = std::floor(tick_of(x0, ppt, org, ImGui::GetIO().MousePos.x));
		// Recent samples are the likely target, so walk back from the newest.
		int found = -1;
		for (int i = static_cast<int>(trace_.size()) - 1; i >= 0; --i) {
			if (static_cast<double>(trace_[static_cast<size_t>(i)].tick) == at) {
				found = i;
				break;
			}
		}
		if (found >= 0) {
			const world::WeaponTraceSample &s = trace_[static_cast<size_t>(found)];
			char extra[96];
			if (s.heat > 0) {
				std::snprintf(extra, sizeof(extra), "clip %d  reserve %d  heat %d", s.clip, s.reserve, s.heat);
			} else {
				std::snprintf(extra, sizeof(extra), "clip %d  reserve %d", s.clip, s.reserve);
			}
			ImGui::SetTooltip("%s\nanim %s%s%s\n%s", trace_row(found),
					s.anim_key[0] != '\0' ? s.anim_key : "-",
					s.anim_key[0] != '\0' ? (s.advance_anim ? "  (advancing)" : "  (held)") : "",
					s.anim_variant > 0 ? "  variant" : "", extra);
			if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) select_action(s.current);
		}
	}

	// Content inside the axis area only; the channel names are drawn after it.
	dl->PushClipRect(ImVec2(x0, body_y), ImVec2(x1, body_y + body_h), true);
	draw_grid(dl, x0, x1, body_y, body_h, ppt, org);

	// Runs: consecutive samples on contiguous ticks holding the same value. A
	// gap in ticks (a paused sim, a dead player) breaks the run rather than
	// drawing a strip across time that was never pumped.
	const auto same_action = [&](size_t a, size_t b) { return trace_[a].current == trace_[b].current; };
	const auto same_anim = [&](size_t a, size_t b) {
		return std::strcmp(trace_[a].anim_key, trace_[b].anim_key) == 0 &&
				trace_[a].anim_variant == trace_[b].anim_variant;
	};
	const auto for_each_run = [&](auto same, auto emit) {
		size_t i = 0;
		while (i < trace_.size()) {
			size_t j = i + 1;
			while (j < trace_.size() && trace_[j].tick == trace_[j - 1].tick + 1 && same(i, j)) ++j;
			const float rx0 = x_of(x0, ppt, org, static_cast<double>(trace_[i].tick));
			const float rx1 = std::max(x_of(x0, ppt, org, static_cast<double>(trace_[j - 1].tick) + 1.0),
					rx0 + 2.0f);
			if (rx1 >= x0 && rx0 <= x1) emit(i, j, rx0, rx1);
			i = j;
		}
	};
	const auto run_label = [&](float rx0, float rx1, float y, const char *label, ImU32 col) {
		dl->PushClipRect(ImVec2(rx0, y), ImVec2(rx1, y + ch_h), true);
		dl->AddText(ImVec2(rx0 + 3.0f, y + 3.0f), col, label);
		dl->PopClipRect();
	};

	// FSM: the action that was current, in its dope-sheet colour.
	for_each_run(same_action, [&](size_t i, size_t, float rx0, float rx1) {
		const float y = channel_y(0);
		dl->AddRectFilled(ImVec2(rx0, y + 2.0f), ImVec2(rx1, y + ch_h - 3.0f),
				action_color(trace_[i].current, 0.62f, 0.72f, 1.0f), 2.0f);
		if (trace_[i].current == selected_) {
			dl->AddRect(ImVec2(rx0, y + 1.0f), ImVec2(rx1, y + ch_h - 2.0f),
					IM_COL32(255, 255, 255, 190), 2.0f, 0, 1.5f);
		}
		run_label(rx0, rx1, y, action_name(trace_[i].current), IM_COL32(15, 15, 18, 255));
	});

	// anim: the clip the channel is holding, dim where the channel is frozen
	// and bright on the ticks it actually stepped — retail clocks the viewmodel
	// from the action counter, so a held clip is the normal case, not a bug.
	for_each_run(same_anim, [&](size_t i, size_t j, float rx0, float rx1) {
		if (trace_[i].anim_key[0] == '\0') return;
		const float y = channel_y(1);
		dl->AddRectFilled(ImVec2(rx0, y + 2.0f), ImVec2(rx1, y + ch_h - 3.0f),
				IM_COL32(52, 66, 84, 235), 2.0f);
		for (size_t k = i; k < j; ++k) {
			if (!trace_[k].advance_anim) continue;
			const float ax0 = x_of(x0, ppt, org, static_cast<double>(trace_[k].tick));
			const float ax1 = std::max(x_of(x0, ppt, org, static_cast<double>(trace_[k].tick) + 1.0),
					ax0 + 1.0f);
			dl->AddRectFilled(ImVec2(ax0, y + 2.0f), ImVec2(ax1, y + ch_h - 3.0f),
					IM_COL32(96, 140, 190, 255));
		}
		char label[80];
		if (trace_[i].anim_variant > 0) {
			std::snprintf(label, sizeof(label), "%s #%d", trace_[i].anim_key, trace_[i].anim_variant);
		} else {
			std::snprintf(label, sizeof(label), "%s", trace_[i].anim_key);
		}
		run_label(rx0, rx1, y, label, IM_COL32(225, 232, 240, 255));
	});

	// Event markers, at the tick they actually fired.
	for (const world::WeaponTraceSample &s : trace_) {
		const float mx = x_of(x0, ppt, org, static_cast<double>(s.tick) + 0.5);
		if (mx < x0 - 40.0f || mx > x1) continue;
		const auto marker = [&](float y, ImU32 col, const char *label) {
			dl->AddLine(ImVec2(mx, y + 2.0f), ImVec2(mx, y + ch_h - 3.0f), col, 2.0f);
			if (label != nullptr && label[0] != '\0') {
				dl->AddText(ImVec2(mx + 3.0f, y + 3.0f), col, label);
			}
		};
		if (s.action_started >= 0 && s.action_started < wa::kCount) {
			const std::string &set = definition_.actions[s.action_started].soundset;
			if (!set.empty()) marker(channel_y(2), IM_COL32(150, 220, 255, 245), set.c_str());
		}
		if (s.action_finished >= 0 && s.action_finished < wa::kCount) {
			// The gunshot lives on the END leg for most shipped fire rows.
			const std::string &set = definition_.actions[s.action_finished].soundsetend;
			if (!set.empty()) marker(channel_y(2), IM_COL32(120, 255, 200, 245), set.c_str());
		}
		if (s.fired) marker(channel_y(3), IM_COL32(255, 120, 110, 245), "fired");
		if (s.dry_fired) marker(channel_y(3), IM_COL32(200, 200, 120, 245), "dry");
		if (s.action_effect >= 0) marker(channel_y(3), IM_COL32(255, 190, 120, 245), "effect");
		if (s.reload_applied) marker(channel_y(3), IM_COL32(160, 220, 160, 245), "reload");
		// The phase band is a solid colour per tick, so a single-tick ENTER is
		// still a visible sliver.
		const float px0 = x_of(x0, ppt, org, static_cast<double>(s.tick));
		const float px1 = std::max(x_of(x0, ppt, org, static_cast<double>(s.tick) + 1.0), px0 + 1.0f);
		ImU32 phase_col = IM_COL32(60, 60, 68, 255);
		switch (s.phase & static_cast<uint8_t>(~wp::kReloadPendingBit)) {
			case wp::kEntered: phase_col = IM_COL32(240, 200, 90, 255); break;
			case wp::kActive: phase_col = IM_COL32(90, 200, 120, 255); break;
			case wp::kDone: phase_col = IM_COL32(90, 130, 210, 255); break;
			case wp::kHeld: phase_col = IM_COL32(190, 120, 210, 255); break;
			default: break;
		}
		dl->AddRectFilled(ImVec2(px0, channel_y(4) + 4.0f), ImVec2(px1, channel_y(4) + ch_h - 5.0f),
				phase_col);
	}

	// The measured span.
	if (measure_valid_) {
		const double a = std::min(measure_from_, measure_to_);
		const double b = std::max(measure_from_, measure_to_);
		const float mx0 = x_of(x0, ppt, org, a);
		const float mx1 = x_of(x0, ppt, org, b);
		dl->AddRectFilled(ImVec2(mx0, body_y), ImVec2(mx1, body_y + body_h),
				IM_COL32(255, 255, 255, 26));
		dl->AddLine(ImVec2(mx0, body_y), ImVec2(mx0, body_y + body_h), IM_COL32(255, 255, 255, 160));
		dl->AddLine(ImVec2(mx1, body_y), ImVec2(mx1, body_y + body_h), IM_COL32(255, 255, 255, 160));
		const double span = std::round(b - a);
		char label[96];
		if (span >= 1.0) {
			std::snprintf(label, sizeof(label), "%.0f ticks  %.0f ms  %.0f rpm", span,
					span * kMsPerTick, io::kTickHz / span * 60.0);
		} else {
			std::snprintf(label, sizeof(label), "%.0f ticks", span);
		}
		dl->AddText(ImVec2(mx0 + 4.0f, body_y + body_h - 16.0f), IM_COL32(255, 255, 255, 230), label);
	}

	// The playhead sits on the newest recorded tick.
	const float head_x = x_of(x0, ppt, org, newest + 1.0);
	dl->AddLine(ImVec2(head_x, body_y), ImVec2(head_x, body_y + body_h),
			IM_COL32(255, 90, 90, 240), 2.0f);
	dl->PopClipRect();

	for (int i = 0; i < kTraceChannels; ++i) {
		dl->AddText(ImVec2(origin.x + 4.0f, channel_y(i) + 3.0f), IM_COL32(190, 190, 198, 255),
				channel_names[i]);
	}
	// The phase legend, in the channel column of the phase row.
	{
		const float y = channel_y(4) + 3.0f;
		float x = origin.x + 46.0f;
		const struct { ImU32 col; const char *name; } legend[] = {
				{IM_COL32(240, 200, 90, 255), "enter"}, {IM_COL32(90, 200, 120, 255), "active"},
				{IM_COL32(90, 130, 210, 255), "done"}};
		for (const auto &item : legend) {
			dl->AddRectFilled(ImVec2(x, y + 3.0f), ImVec2(x + 8.0f, y + 11.0f), item.col);
			dl->AddText(ImVec2(x + 11.0f, y), IM_COL32(150, 150, 160, 255), item.name);
			x += 11.0f + ImGui::CalcTextSize(item.name).x + 8.0f;
		}
	}

	ImGui::SetCursorScreenPos(ImVec2(origin.x, body_y + body_h));
	ImGui::EndChild();
}

// --- header, transport, properties ---------------------------------------

void WeaponWindow::draw_header() {
	ImGui::Text("%s", definition_.weapon_name.empty() ? "(unnamed)" : definition_.weapon_name.c_str());
	ImGui::SameLine();
	ImGui::TextDisabled("adm %d", definition_.adm_index);
	ImGui::SameLine();
	if (definition_.clip_capacity < 0) {
		// No magazine to track: the def authored no clipsize, so the FSM runs
		// its infinite-ammo legs and a reload can never be requested.
		ImGui::TextColored(ImVec4(0.95f, 0.65f, 0.4f, 1.0f), "| no clipsize");
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
			ImGui::SetTooltip("The weapon.def entry authors no `clipsize`, so the FSM tracks no\n"
							  "magazine and the reload gate refuses. (The parser key is\n"
							  "`clipsize`; a `clip` line is not one and lands in raw_lines.)");
		}
	} else {
		ImGui::Text("| clip %d/%d  reserve %d", live_.clip, definition_.clip_capacity, live_.reserve);
	}
	ImGui::SameLine();
	ImGui::Text("| %s %s c%d", action_name(live_.current), phase_name(live_.phase), live_.counter);
	ImGui::SameLine();
	ImGui::TextDisabled("| tick %llu", static_cast<unsigned long long>(live_.logic_tick));
	ImGui::SameLine();
	ImGui::TextDisabled("| %s%s", definition_.auto_fire ? "auto" : "semi",
			definition_.burst3 ? " burst3" : "");
	// Only the emplaced and vehicle heavy guns author a heat model; for every
	// infantry weapon this row would be a permanent zero, so it is not drawn.
	if (live_.heat > 0) {
		ImGui::SameLine();
		ImGui::Text("| heat %d", live_.heat);
	}
}

void WeaponWindow::draw_transport() {
	const char *fire_block = live_.fire_block.c_str();
	const bool fire_ok = live_.fire_block.empty();
	if (action_button("Fire", fire_block)) queue_trigger(WeaponRequest::Trigger::Fire);
	ImGui::SameLine();
	bool held = live_.fire_held;
	ImGui::BeginDisabled(!fire_ok);
	if (ImGui::Checkbox("Hold", &held)) queue_fire_held(held);
	ImGui::EndDisabled();
	if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled)) {
		if (fire_ok) {
			ImGui::SetTooltip("Hold the trigger: on an auto weapon the recoil window's deferred\n"
							  "re-queue sustains the volley, exactly as a held mouse button does.\n"
							  "Released when the window hides.");
		} else {
			ImGui::SetTooltip("%s", fire_block);
		}
	}
	ImGui::SameLine();
	// Strict preview: the buttons carry the FSM's own refusal rather than
	// faking ammo to keep themselves enabled.
	if (action_button("Reload", fire_ok ? live_.reload_block.c_str() : fire_block)) {
		queue_trigger(WeaponRequest::Trigger::Reload);
	}
	ImGui::SameLine();
	if (action_button("Scope toggle", fire_ok ? live_.scope_block.c_str() : fire_block)) {
		queue_trigger(WeaponRequest::Trigger::ScopeToggle);
	}
	ImGui::SameLine();
	if (action_button("Next weapon", fire_block)) queue_trigger(WeaponRequest::Trigger::NextWeapon);
	ImGui::SameLine();
	if (action_button("Prev weapon", fire_block)) queue_trigger(WeaponRequest::Trigger::PrevWeapon);

	ImGui::SameLine();
	ImGui::TextDisabled("|");
	ImGui::SameLine();
	bool recording = recording_;
	if (ImGui::Checkbox("REC", &recording)) {
		rec_touched_ = true;
		set_recording(recording);
	}
	if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
		ImGui::SetTooltip("Record the FSM at every 62.5 Hz pump tick. Stays on while the window\n"
						  "is hidden, so you can close F3, shoot, and reopen to read the burst.");
	}
	ImGui::SameLine();
	ImGui::Checkbox("Follow", &trace_follow_);
	ImGui::SameLine();
	if (ImGui::Button("Fit")) {
		fit_pending_ = true;
		fit_trace_pending_ = !trace_.empty();
	}
	if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
		ImGui::SetTooltip("View All on both panes (Home over a pane fits just that one)");
	}
	ImGui::SameLine();
	if (ImGui::Button("Clear trace")) {
		trace_.clear();
		measure_valid_ = false;
		WeaponRequest request;
		request.kind = WeaponRequest::Kind::ClearTrace;
		enqueue_request(request);
	}
	ImGui::SameLine();
	if (ImGui::Button("Copy ACTION blocks")) {
		// Edits are live-only and never reach disk; this is only so the current
		// numbers can be carried out of the window without retyping them.
		std::string out;
		for (int id = 0; id < wa::kCount; ++id) {
			const WeaponActionRow &row = definition_.actions[id];
			if (!row.authored) continue;
			char block[768];
			char start[24];
			char end[24];
			if (row.authored_delay_start < 0) {
				std::snprintf(start, sizeof(start), "auto");
			} else {
				std::snprintf(start, sizeof(start), "%d", row.authored_delay_start);
			}
			if (row.authored_delay_end < 0) {
				std::snprintf(end, sizeof(end), "auto");
			} else {
				std::snprintf(end, sizeof(end), "%d", row.authored_delay_end);
			}
			std::snprintf(block, sizeof(block),
					"\tACTION\t\"%s\"\n\tDELAYSTART\t%s\n\tDELAYEND\t%s\n\tANIM\t\t%s\n"
					"\tFUNCTION\t%s\n",
					row.authored_name.c_str(), start, end, row.anim_key.c_str(),
					row.function.c_str());
			out += block;
			if (!row.soundset.empty()) out += "\tSOUNDSET\t" + row.soundset + "\n";
			if (!row.soundsetend.empty()) out += "\tSOUNDSETEND\t" + row.soundsetend + "\n";
			if (!row.particle.empty()) out += "\tPARTICLE\t" + row.particle + "\n";
			if (!row.particle_userpoint.empty()) {
				out += "\tPARTICLEUSERPOINT\t" + row.particle_userpoint + "\n";
			}
			out += "\tEND\n\n";
		}
		ImGui::SetClipboardText(out.c_str());
	}
	if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
		ImGui::SetTooltip("Edits never reach disk. This copies the current ACTION blocks in\n"
						  "weapon.def shape so the numbers can be carried out by hand.");
	}
	ImGui::TextDisabled("wheel: zoom   middle/right drag: pan   Home: fit   double-click a track: frame it"
						"   drag the trace ruler: measure");
}

void WeaponWindow::draw_properties() {
	const int id = selected_;
	if (id < 0 || id >= wa::kCount) return;
	const WeaponActionRow &row = definition_.actions[id];

	// Every field below is keyed by the selected action, so a selection change
	// (a row click earlier this frame, a trace click) makes ImGui drop the old
	// action's active field instead of committing its half-typed text here.
	ImGui::PushID(id);
	if (field_action_ != id) {
		field_action_ = id;
		field_active_ = {};
	}

	ImGui::Separator();
	ImGui::Text("%s", kActionNames[id]);
	ImGui::SameLine();
	if (row.authored) {
		ImGui::TextDisabled("authored as \"%s\"", row.authored_name.c_str());
	} else {
		ImGui::TextDisabled("not authored: baked from defaults; edits are live only");
	}

	// Delays. `auto` is a distinct authoring, so it gets a checkbox rather than
	// a magic value in the number field.
	const auto delay_field = [&](const char *label, int32_t baked, int32_t authored, DelayLeg leg) {
		ImGui::PushID(label);
		bool is_auto = row.authored && authored < 0;
		int value = baked;
		ImGui::SetNextItemWidth(110.0f);
		ImGui::BeginDisabled(is_auto);
		const bool changed = ImGui::DragInt("##v", &value, 0.25f, 0, 4096, "%d ticks");
		ImGui::EndDisabled();
		ImGui::SameLine();
		ImGui::TextDisabled("%.0f ms", value * kMsPerTick);
		ImGui::SameLine();
		ImGui::BeginDisabled(!row.authored);
		if (ImGui::Checkbox("auto", &is_auto)) request_delay_auto(id, leg, is_auto);
		ImGui::EndDisabled();
		ImGui::SameLine();
		ImGui::TextUnformatted(label);
		ImGui::PopID();
		if (changed && !is_auto) request_delay_edit(id, leg, value);
	};
	delay_field("DELAYSTART", row.delay_start, row.authored_delay_start, DelayLeg::Start);
	delay_field("DELAYEND", row.delay_end, row.authored_delay_end, DelayLeg::End);

	// The five name fields, reseeded from the definition on any frame they are
	// not the active item.
	const auto name_field = [&](const char *label, WeaponRequest::TextField field,
									const std::string &current,
									const std::vector<std::string> *catalog,
									const char *missing_label) {
		const size_t slot = static_cast<size_t>(field);
		char *buf = field_buf_[slot].data();
		ImGui::PushID(label);
		if (!field_active_[slot]) {
			std::snprintf(buf, field_buf_[slot].size(), "%s", current.c_str());
		}
		ImGui::SetNextItemWidth(220.0f);
		bool commit = ImGui::InputText("##t", buf, field_buf_[slot].size(),
				ImGuiInputTextFlags_EnterReturnsTrue);
		field_active_[slot] = ImGui::IsItemActive();
		commit = commit || ImGui::IsItemDeactivatedAfterEdit();
		if (commit) queue_text(id, field, buf);
		if (catalog != nullptr && !catalog->empty()) {
			ImGui::SameLine();
			if (ImGui::BeginCombo("##pick", "", ImGuiComboFlags_NoPreview)) {
				for (const std::string &name : *catalog) {
					if (ImGui::Selectable(name.c_str(), iequals_ascii(name, current))) {
						queue_text(id, field, name.c_str());
					}
				}
				ImGui::EndCombo();
			}
			// Whether the authored name is one the runtime actually resolved is
			// the difference between a working leg and a silent one.
			ImGui::SameLine();
			bool known = false;
			for (const std::string &name : *catalog) {
				if (iequals_ascii(name, current)) {
					known = true;
					break;
				}
			}
			if (current.empty()) {
				ImGui::TextDisabled("(none)");
			} else if (known) {
				ImGui::TextColored(ImVec4(0.4f, 0.85f, 0.5f, 1.0f), "resolves");
			} else {
				ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.4f, 1.0f), "%s", missing_label);
			}
		}
		ImGui::SameLine();
		ImGui::TextUnformatted(label);
		ImGui::PopID();
	};
	name_field("ANIM", WeaponRequest::TextField::Anim, row.anim_key, &definition_.clip_keys,
			"no clip");
	name_field("SOUNDSET", WeaponRequest::TextField::SoundSet, row.soundset, nullptr, "");
	name_field("SOUNDSETEND", WeaponRequest::TextField::SoundSetEnd, row.soundsetend, nullptr, "");
	name_field("PARTICLE", WeaponRequest::TextField::Particle, row.particle, nullptr, "");
	name_field("PARTICLEUSERPOINT", WeaponRequest::TextField::ParticleUserPoint,
			row.particle_userpoint, nullptr, "");

	ImGui::TextDisabled("FUNCTION %s", row.function.empty() ? "-" : row.function.c_str());
	if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
		ImGui::SetTooltip(
				"Read-only: the handler registry is not ported (divergence D-WPN-1).\n"
				"Every shipped row names the standard handler for its own suffix, so\n"
				"the per-state behaviour is fixed and editing this would change nothing.");
	}
	if (row.clip_ticks > 0) {
		ImGui::SameLine();
		ImGui::TextDisabled("| clip %d ticks (%.0f ms)", row.clip_ticks,
				row.clip_ticks * kMsPerTick);
	}
	ImGui::PopID();
}

// --- the frame ------------------------------------------------------------

void WeaponWindow::draw(ImGuiPass &pass, uint64_t frame_index) {
	(void)pass;
	(void)frame_index;
	if (!definition_.valid) {
		ImGui::TextUnformatted("No weapon installed. Load a mission and spawn.");
		return;
	}
	draw_header();
	draw_transport();
	ImGui::Separator();

	// The dope sheet takes its natural height (ruler + one row per action) so
	// the twelve tracks never scroll out from under the ruler; the trace takes
	// whatever is left above the properties panel.
	const float dope_h = kRulerHeight + kRowHeight * wa::kCount + 2.0f;
	const float props_h = ImGui::GetFrameHeightWithSpacing() * 8.5f;
	const float spacing = ImGui::GetStyle().ItemSpacing.y;
	const float trace_h = std::max(
			ImGui::GetContentRegionAvail().y - dope_h - props_h - spacing * 2.0f,
			kRulerHeight + kTraceChannelHeight * kTraceChannels + 2.0f);

	draw_dope_sheet(dope_h);
	draw_trace(trace_h);
	draw_properties();
}

}  // namespace opennova::devtools
