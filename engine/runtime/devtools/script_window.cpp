#include <runtime/devtools/script_window.h>

#include <formats/mission/bms.h>
#include <runtime/devtools/debug_control_ids.h>

#include <imgui.h>

#include <cstdio>

namespace opennova::devtools {

void ScriptWindow::on_visibility(bool visible) {
	if (!visible) {
		snapshot_ = ScriptSnapshot{};
		editing_index_ = -1;
		format();
	}
}

void ScriptWindow::wanted_controls(std::vector<const char *> &out) const {
	out.push_back(control_id::kRuntimeWacPaused);
	out.push_back(control_id::kSetMissionVariable);
	out.push_back(control_id::kFireEvent);
}

void ScriptWindow::set_snapshot(ScriptSnapshot snapshot) {
	snapshot_ = std::move(snapshot);
	format();
}

void ScriptWindow::request_set_mission_variable(int32_t index, int32_t value) {
	requests_.push_back({control_id::kSetMissionVariable,
			{ControlArg::integer(index), ControlArg::integer(value)}});
}

void ScriptWindow::request_fire_event(int32_t index) {
	requests_.push_back({control_id::kFireEvent, {ControlArg::integer(index)}});
}

bool ScriptWindow::take_request(ControlRequest &request) {
	if (requests_.empty()) return false;
	request = requests_.front();
	requests_.pop_front();
	return true;
}

const char *ScriptWindow::gap_text(int row) const {
	if (row < 0 || row >= gap_count()) return "";
	return gap_rows_[static_cast<size_t>(row)].c_str();
}

std::string ScriptWindow::bms_truth_text(int row) const {
	const std::vector<mission::ScriptBmsEventRow> &rows = snapshot_.report.bms_events;
	if (!snapshot_.valid || row < 0 || row >= static_cast<int>(rows.size())) return std::string();
	const mission::ScriptBmsEventRow &e = rows[static_cast<size_t>(row)];
	if (e.trigger_truth.empty()) return "(no triggers)";
	std::string out;
	out.reserve(e.trigger_truth.size());
	for (int8_t t : e.trigger_truth) out.push_back(t < 0 ? '-' : (t != 0 ? 'T' : 'F'));
	return out;
}

void ScriptWindow::format() {
	summary_.clear();
	gap_rows_.clear();
	if (!snapshot_.valid) return;
	const mission::ScriptDebugReport &r = snapshot_.report;
	char buf[256];
	if (r.wac_loaded) {
		std::snprintf(buf, sizeof(buf),
				"WAC %s | %u code words | runs %u | time %u | divider %d/62 | acc %d | rng 0x%08X | dispatches %llu",
				r.wac_paused ? "PAUSED" : "running", r.code_words, r.runs, r.time, r.divider,
				r.accumulator, r.rng_seed, static_cast<unsigned long long>(r.dispatch_count));
	} else {
		std::snprintf(buf, sizeof(buf), "no WAC program%s",
				r.diagnostics.empty() ? "" : " (the compile failed)");
	}
	summary_ = buf;
	for (const world::RuntimeGap &gap : r.runtime_gaps) {
		const world::RuntimeGapSite &site = gap.origin;
		const char *kind = "unknown";
		switch (site.kind) {
			case world::RuntimeGapKind::WacCommand:
				kind = "WAC command";
				break;
			case world::RuntimeGapKind::WacOpcode:
				kind = "WAC opcode";
				break;
			case world::RuntimeGapKind::WacInstructionLimit:
				kind = "WAC instruction limit";
				break;
			case world::RuntimeGapKind::BmsAction:
				kind = "BMS action";
				break;
		}
		std::string where;
		if (!site.source.empty()) where = " @ " + site.source + ":" + std::to_string(site.line);
		std::snprintf(buf, sizeof(buf), "%s %d/%d, event %d, site %d: %llu calls (ticks %u..%u), args %d %d %d %d",
				kind, site.code, site.subcode, site.event, site.site, static_cast<unsigned long long>(gap.count),
				gap.first_tick, gap.last_tick, gap.arguments[0], gap.arguments[1], gap.arguments[2],
				gap.arguments[3]);
		gap_rows_.emplace_back(std::string(buf) + where);
	}
}

void ScriptWindow::draw_variables() {
	const mission::ScriptDebugReport &r = snapshot_.report;
	const ControlState *edit_row = board_.state(control_id::kSetMissionVariable);
	const bool can_edit = snapshot_.authority && edit_row != nullptr && edit_row->writable;
	ImGui::Checkbox("Non-zero only", &nonzero_only_);
	ImGui::SameLine();
	ImGui::Checkbox("Globals (G0..G255)", &show_globals_);
	if (!can_edit) {
		ImGui::SameLine();
		ImGui::TextDisabled("(read-only%s)", snapshot_.authority ? "" : ": a joiner does not own the scripts");
	}
	const bool globals = show_globals_;
	const int count = globals ? static_cast<int>(r.global_vars.size()) : static_cast<int>(r.mission_vars.size());
	if (ImGui::BeginTable("vars", 3,
				ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp,
				ImVec2(0.0f, 260.0f))) {
		ImGui::TableSetupScrollFreeze(0, 1);
		ImGui::TableSetupColumn(globals ? "G" : "V");
		ImGui::TableSetupColumn("value");
		ImGui::TableSetupColumn("as 16.16");
		ImGui::TableHeadersRow();
		for (int i = 0; i < count; ++i) {
			const int32_t value = globals ? r.global_vars[static_cast<size_t>(i)] : r.mission_vars[static_cast<size_t>(i)];
			if (nonzero_only_ && value == 0 && editing_index_ != i) continue;
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::Text("%s%d", globals ? "G" : "V", i);
			ImGui::TableNextColumn();
			ImGui::PushID(i);
			if (!globals && editing_index_ == i) {
				ImGui::SetNextItemWidth(-1.0f);
				// Written as it is typed or stepped, set once it is left (Enter, Tab, a click elsewhere, a step
				// button let go; Esc puts back what it held). No EnterReturnsTrue: InputScalar asserts on it.
				ImGui::InputInt("##edit", &edit_value_, 1, 65536);
				if (ImGui::IsItemDeactivatedAfterEdit()) {
					if (edit_value_ != value) request_set_mission_variable(i, edit_value_);
					editing_index_ = -1;
				} else if (ImGui::IsItemDeactivated()) {
					editing_index_ = -1;
				}
			} else {
				ImGui::Text("%d", value);
				if (!globals && can_edit && ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
					editing_index_ = i;
					edit_value_ = value;
				}
				if (!globals && can_edit && ImGui::IsItemHovered()) {
					ImGui::SetTooltip("Double-click to edit (Enter, or leaving it, applies).");
				}
			}
			ImGui::PopID();
			ImGui::TableNextColumn();
			ImGui::Text("%.4f", static_cast<double>(value) / 65536.0);
		}
		ImGui::EndTable();
	}
}

void ScriptWindow::draw(ImGuiPass &pass, uint64_t frame_index) {
	(void)pass;
	(void)frame_index;
	if (!snapshot_.valid) {
		ImGui::TextUnformatted("No mission scripts pushed (load a mission).");
		return;
	}
	const mission::ScriptDebugReport &r = snapshot_.report;
	draw_control(board_, control_id::kRuntimeWacPaused, requests_, "Pause WAC");
	ImGui::SameLine();
	ImGui::TextDisabled("logic tick %llu", static_cast<unsigned long long>(snapshot_.logic_tick));
	ImGui::TextUnformatted(summary_.c_str());
	// The retail script-state page's one line: the first compile error.
	if (!r.first_error.empty()) {
		ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f), "Script error: %s", r.first_error.c_str());
	} else if (r.wac_loaded) {
		ImGui::TextColored(ImVec4(0.55f, 0.9f, 0.55f, 1.0f), "Compiled clean (%d sources)",
				static_cast<int>(r.source_names.size()));
	}

	char header[96];
	std::snprintf(header, sizeof(header), "Compile diagnostics (%d)###diag", static_cast<int>(r.diagnostics.size()));
	if (!r.diagnostics.empty() && ImGui::CollapsingHeader(header)) {
		for (const mission::ScriptWacDiagnosticRow &d : r.diagnostics) {
			if (d.error) {
				ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f), "%s", d.text.c_str());
			} else {
				ImGui::TextUnformatted(d.text.c_str());
			}
		}
	}

	std::snprintf(header, sizeof(header), "WAC events (%d)###wac", static_cast<int>(r.wac_events.size()));
	if (ImGui::CollapsingHeader(header)) {
		if (ImGui::BeginTable("wac_events", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY,
					ImVec2(0.0f, 200.0f))) {
			ImGui::TableSetupScrollFreeze(0, 1);
			ImGui::TableSetupColumn("event");
			ImGui::TableSetupColumn("fired");
			ImGui::TableSetupColumn("last tick");
			ImGui::TableSetupColumn("active");
			ImGui::TableSetupColumn("ever");
			ImGui::TableHeadersRow();
			for (size_t i = 0; i < r.wac_events.size(); ++i) {
				const mission::ScriptWacEventRow &e = r.wac_events[i];
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				if (static_cast<int>(i) == r.current_event) {
					ImGui::TextColored(ImVec4(0.45f, 0.85f, 1.0f, 1.0f), "%d *", static_cast<int>(i));
				} else {
					ImGui::Text("%d", static_cast<int>(i));
				}
				ImGui::TableNextColumn();
				ImGui::Text("%u", e.fired_count);
				ImGui::TableNextColumn();
				ImGui::Text("%u", e.last_fired_tick);
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(e.active ? "yes" : "");
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(e.ever_fired ? "yes" : "");
			}
			ImGui::EndTable();
		}
	}

	std::snprintf(header, sizeof(header), "BMS events (%d)###bms", static_cast<int>(r.bms_events.size()));
	if (ImGui::CollapsingHeader(header)) {
		ImGui::Text("AWOL quanta %d | second time through %s", r.awol_count,
				r.second_time_through ? "yes" : "no");
		const ControlState *fire_row = board_.state(control_id::kFireEvent);
		const bool can_fire = snapshot_.authority && fire_row != nullptr && fire_row->writable;
		if (ImGui::BeginTable("bms_events", 8, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY,
					ImVec2(0.0f, 200.0f))) {
			ImGui::TableSetupScrollFreeze(0, 1);
			ImGui::TableSetupColumn("event");
			ImGui::TableSetupColumn("trig/act");
			ImGui::TableSetupColumn("triggers");
			ImGui::TableSetupColumn("checked");
			ImGui::TableSetupColumn("latch");
			ImGui::TableSetupColumn("repeat");
			ImGui::TableSetupColumn("activate");
			ImGui::TableSetupColumn("");
			ImGui::TableHeadersRow();
			for (size_t i = 0; i < r.bms_events.size(); ++i) {
				const mission::ScriptBmsEventRow &e = r.bms_events[i];
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				ImGui::Text("%d", e.index);
				ImGui::TableNextColumn();
				ImGui::Text("%d / %d", e.trigger_count, e.action_count);
				ImGui::TableNextColumn();
				const std::string truth = bms_truth_text(static_cast<int>(i));
				ImGui::TextUnformatted(truth.c_str());
				if (ImGui::IsItemHovered()) {
					ImGui::SetTooltip("Each trigger's last term as the chain ran it (after its own negation):\n"
									  "T true, F false, - not evaluated by the last run.");
				}
				ImGui::TableNextColumn();
				if (e.last_checked_tick >= 0) {
					ImGui::Text("%lld", static_cast<long long>(e.last_checked_tick));
				}
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(e.fired ? "fired" : (e.active ? "set" : ""));
				ImGui::TableNextColumn();
				ImGui::Text("%u / %u", e.repeat_countdown, e.repeat_reload);
				ImGui::TableNextColumn();
				ImGui::Text("%u / %u", e.activate_countdown, e.activate_reload);
				ImGui::TableNextColumn();
				const auto flagged = [&e](bms::EventFlags flag) {
					return (e.flags & static_cast<uint32_t>(flag)) != 0;
				};
				// The refusals fire_event makes, shown before it is asked (mission::force_fire).
				if (e.fire_pending) {
					ImGui::TextDisabled("fire pending");
				} else if (flagged(bms::EventFlags::PreMission) || flagged(bms::EventFlags::PostMission)) {
					ImGui::TextDisabled(flagged(bms::EventFlags::PreMission) ? "pre-mission" : "post-mission");
				} else if (e.active && !flagged(bms::EventFlags::ResetAfter)) {
					ImGui::TextDisabled("latched");
				} else if (can_fire) {
					ImGui::PushID(static_cast<int>(i));
					if (ImGui::SmallButton("Fire")) request_fire_event(e.index);
					if (ImGui::IsItemHovered()) {
						ImGui::SetTooltip("The chain reads true at its next run; the delay, latch and repeat "
										  "then run as the event's own.");
					}
					ImGui::PopID();
				}
			}
			ImGui::EndTable();
		}
	}

	if (ImGui::CollapsingHeader("Variables", ImGuiTreeNodeFlags_DefaultOpen)) {
		draw_variables();
	}

	std::snprintf(header, sizeof(header), "Runtime gaps (%d sites, %llu calls)###gaps", gap_count(),
			static_cast<unsigned long long>(r.runtime_gap_calls));
	if (ImGui::CollapsingHeader(header)) {
		ImGui::TextDisabled("Script commands the port met that are not witnessed yet.");
		for (const std::string &row : gap_rows_) ImGui::TextUnformatted(row.c_str());
	}
}

}  // namespace opennova::devtools
