#include <runtime/devtools/ai_window.h>

#include <runtime/devtools/entities_window.h>

#include <imgui.h>

#include <cstdarg>
#include <cstdio>
#include <utility>

namespace opennova::devtools {

namespace {

const char *alert_name(int32_t alert) {
	switch (alert) {
		case 0: return "GREEN";
		case 1: return "YELLOW";
		case 2: return "RED";
		default: return "?";
	}
}

ImVec4 alert_color(int32_t alert) {
	switch (alert) {
		case 1: return ImVec4(1.0f, 0.85f, 0.25f, 1.0f);
		case 2: return ImVec4(1.0f, 0.30f, 0.25f, 1.0f);
		default: return ImVec4(0.35f, 1.0f, 0.45f, 1.0f);
	}
}

const char *profile_type_name(int32_t type) {
	switch (type) {
		case 1: return "HELO";
		case 2: return "GROUND";
		case 3: return "ORGANIC";
		default: return "unresolved";
	}
}

void push_line(std::vector<std::string> &lines, const char *fmt, ...) {
	char buf[192];
	va_list args;
	va_start(args, fmt);
	std::vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);
	lines.emplace_back(buf);
}

}  // namespace

AiWindow::AiWindow(EntitiesWindow &entities) : entities_(entities) {
	using Element = AiOverlayLayer::Element;
	const Element elements[kLayerCount] = {Element::Labels, Element::Routes, Element::Targets,
			Element::Rings};
	for (int i = 0; i < kLayerCount; ++i) {
		layers_[static_cast<size_t>(i)] = std::make_unique<AiOverlayLayer>(snapshot_, entities_, elements[i]);
	}
}

bool AiWindow::any_layer_enabled() const {
	for (const auto &layer : layers_) {
		if (layer->enabled()) return true;
	}
	return false;
}

const char *AiWindow::brain_text(int row) const {
	if (row < 0 || row >= brain_count()) return "";
	return brain_rows_[static_cast<size_t>(row)].c_str();
}

void AiWindow::on_visibility(bool visible) {
	shown_ = visible;
	if (!visible) {
		// Drop the records so a closed window holds nothing; the embedder's
		// needs_ai_debug gate stops the pushes on the same edge. A Game-view
		// layer still on keeps the record it draws.
		if (!any_layer_enabled()) {
			snapshot_ = AiDebugSnapshot{};
			format_snapshot();
		}
		detail_ = EntityDetailSnapshot{};
		format_detail();
	}
}

void AiWindow::set_snapshot(AiDebugSnapshot snapshot) {
	snapshot_ = std::move(snapshot);
	if (!snapshot_.valid) snapshot_ = AiDebugSnapshot{};
	format_snapshot();
}

void AiWindow::set_detail(EntityDetailSnapshot detail) {
	if (!detail.card.valid) {
		detail_ = EntityDetailSnapshot{};
		format_detail();
		return;
	}
	// A card for a selection that moved is stale, whatever it says (the
	// Entity Properties acceptance rule).
	if (detail.card.handle != entities_.selected_handle()) return;
	detail_ = std::move(detail);
	format_detail();
}

void AiWindow::clear_detail() {
	detail_ = EntityDetailSnapshot{};
	format_detail();
}

bool AiWindow::detail_valid() const {
	return detail_.card.valid && detail_.card.has_ai &&
			detail_.card.handle == entities_.selected_handle();
}

const char *AiWindow::group_text(int row) const {
	if (row < 0 || row >= static_cast<int>(group_rows_.size())) return "";
	return group_rows_[static_cast<size_t>(row)].c_str();
}

const char *AiWindow::channel_text(int row) const {
	if (row < 0 || row >= static_cast<int>(channel_rows_.size())) return "";
	return channel_rows_[static_cast<size_t>(row)].c_str();
}

const char *AiWindow::detail_line(int row) const {
	if (row < 0 || row >= static_cast<int>(detail_lines_.size())) return "";
	return detail_lines_[static_cast<size_t>(row)].c_str();
}

void AiWindow::format_snapshot() {
	brain_rows_.clear();
	brain_alerts_.clear();
	group_rows_.clear();
	group_alerts_.clear();
	channel_rows_.clear();
	if (!snapshot_.valid) {
		counters_ = "No world.";
		return;
	}
	const world::inspect::AiSystemCounters &c = snapshot_.report.counters;
	char buf[192];
	std::snprintf(buf, sizeof(buf),
			"brains %d  events %d  rel_ops %d  find_target %d  mission gaps %u / %llu calls",
			c.brain_count, c.event_count,
			c.rel_ops, c.find_target_calls, c.runtime_gap_sites,
            static_cast<unsigned long long>(c.runtime_gap_calls));
	counters_ = buf;
	brain_rows_.reserve(snapshot_.report.rows.size());
	for (const world::inspect::AiOverlayRow &r : snapshot_.report.rows) {
		char target[64] = "-";
		if (r.target_valid) {
			std::snprintf(target, sizeof(target), "%s", r.target_name.empty() ? "?" : r.target_name.c_str());
		}
		char route[32] = "-";
		if (r.wp_channel > 0) std::snprintf(route, sizeof(route), "ch %d node %d", r.wp_channel, r.wp_node);
		std::snprintf(buf, sizeof(buf), "%-16s  G%02d  %-6s  %-20s  -> %-16s  %s",
				r.name.empty() ? "(unnamed)" : r.name.c_str(), r.group_id,
				r.alive ? alert_name(r.alert) : "DEAD",
				r.state_name.empty() ? "?" : r.state_name.c_str(), target, route);
		brain_rows_.emplace_back(buf);
		brain_alerts_.push_back(r.alive ? r.alert : -1);
	}
	group_rows_.reserve(snapshot_.report.groups.size());
	for (const world::inspect::AiGroupRow &g : snapshot_.report.groups) {
		std::snprintf(buf, sizeof(buf), "G%02d  %-6s  alive %d/%d", g.id,
				alert_name(g.alert), g.live_count, g.initial_count);
		group_rows_.emplace_back(buf);
		group_alerts_.push_back(g.alert);
	}
	channel_rows_.reserve(snapshot_.report.channels.size());
	for (const world::inspect::AiNavChannelRow &ch : snapshot_.report.channels) {
		// loopflag bit0 set = one-shot (terminate at path end).
		std::snprintf(buf, sizeof(buf), "ch %d  %s  nodes %d  followers %d",
				ch.index, (ch.loopflag & 1) != 0 ? "once" : "loop",
				static_cast<int>(ch.nodes.size()), ch.followers);
		channel_rows_.emplace_back(buf);
	}
}

void AiWindow::format_detail() {
	detail_lines_.clear();
	if (!detail_.card.valid || !detail_.card.has_ai) return;
	const world::inspect::AiDetail &d = detail_.card.ai;
	std::vector<std::string> &out = detail_lines_;

	push_line(out, "[Identity]");
	push_line(out, "%s  handle 0x%04X  ai_index %d",
			d.name.empty() ? "(unnamed)" : d.name.c_str(),
			static_cast<unsigned>(detail_.card.handle), detail_.card.ai_index);
	push_line(out, "state %d %s  pending %d", d.state, d.state_name.c_str(),
			d.pending_state);
	push_line(out, "alert %s (slot)  %s (brain)  group %d  team %d",
			alert_name(d.alert), alert_name(d.alert_brain), d.group_id, d.team);

	push_line(out, "[Movement]");
	if (d.infantry) {
		push_line(out, "infantry motor  move_mode %d  airborne %s",
				d.infantry_move_mode, d.airborne ? "yes" : "no");
		push_line(out, "root dx/dy %d/%d  resolve dx/dy %d/%d", d.root_dx,
				d.root_dy, d.res_dx, d.res_dy);
	}
	push_line(out, "wp channel %d node %d dist %d  route %d #%d", d.wp_channel,
			d.wp_node, d.wp_distance, d.waypoint_id, d.wp_number);
	push_line(out, "slot route %d cmd %d node %d", d.s35, d.s37, d.s38);
	push_line(out, "out_speed %d  speeds %d/%d", d.out_speed, d.speed_a,
			d.speed_b);
	push_line(out, "work pos %.1f %.1f %.1f  heading 0x%08X", d.work_pos.x,
			d.work_pos.y, d.work_pos.z, static_cast<unsigned>(d.work_heading));

	push_line(out, "[Combat]");
	if (d.target_valid) {
		push_line(out, "target %s (0x%04X)",
				d.target_name.empty() ? "?" : d.target_name.c_str(),
				static_cast<unsigned>(d.target_handle));
	} else {
		push_line(out, "target none");
	}
	if (d.priority_target_valid)
		push_line(out, "priority target 0x%04X",
				static_cast<unsigned>(d.priority_target_handle));
	// The brain timers cover the SM/vehicle chain; the infantry pass keeps
	// its own (damage/combat_move below).
	push_line(out, "combat_timer %d  fire_delay %d  retarget %d",
			d.combat_timer, d.fire_delay, d.retarget_timer);
	push_line(out, "cooldown a/b %d/%d  turret yaw 0x%08X pitch 0x%08X",
			d.cooldown_a, d.cooldown_b, static_cast<unsigned>(d.turret_yaw),
			static_cast<unsigned>(d.turret_pitch));
	push_line(out, "sight %.1f u  attack %.1f u", d.sight_range_u,
			d.attack_range_u);
	push_line(out, "ammo %d clip %d magazine %d", d.ammo_primary, d.clip_size,
			d.magazine);
	if (d.infantry) {
		push_line(out, "aim %s heading 0x%08X pitch 0x%08X",
				d.aim_valid ? "valid" : "none",
				static_cast<unsigned>(d.aim_heading),
				static_cast<unsigned>(d.aim_pitch));
		push_line(out, "damage_timer %d  same_target %d  combat_move %d",
				d.damage_timer, d.same_target_ticks, d.combat_move_timer);
	}
	if (d.muzzle_valid)
		push_line(out, "muzzle %.1f %.1f %.1f", d.muzzle.x, d.muzzle.y,
				d.muzzle.z);

	push_line(out, "[Profile]");
	push_line(out, "type %s  control 0x%X%s%s%s",
			profile_type_name(d.profile_type),
			static_cast<unsigned>(d.slot_control_bits),
			(d.slot_control_bits & 0x1) != 0 ? " BLIND" : "",
			(d.slot_control_bits & 0x8) != 0 ? " COWARD" : "",
			(d.slot_control_bits & 0x200) != 0 ? " BERSERK" : "");
	push_line(out, "class priority a/g/o/d %d/%d/%d/%d",
			d.profile_class_priority[0], d.profile_class_priority[1],
			d.profile_class_priority[2], d.profile_class_priority[3]);
	push_line(out, "fov %d/%d  range %d/%d  approach %.1f",
			d.profile_fov_primary, d.profile_fov_secondary,
			d.profile_range_primary, d.profile_range_secondary,
			static_cast<float>(d.profile_approach_cap) / 65536.0f);
}

void AiWindow::draw_detail_pane() {
	ImGui::SeparatorText("Selected brain");
	if (detail_lines_.empty()) {
		ImGui::TextDisabled(
				"No AI selected. Pick a row in Entities, or Shift+F6 in the world.");
		return;
	}
	for (const std::string &line : detail_lines_) {
		if (!line.empty() && line.front() == '[') {
			ImGui::SeparatorText(line.c_str());
		} else {
			ImGui::TextUnformatted(line.c_str());
		}
	}
}

void AiWindow::draw_tables() {
    if (ImGui::CollapsingHeader("Mission runtime gaps")) {
        for (const auto &gap : snapshot_.report.runtime_gaps) {
            const auto &site = gap.origin;
            const char *kind = "unknown";
            switch (site.kind) {
                case world::RuntimeGapKind::WacCommand: kind = "WAC command"; break;
                case world::RuntimeGapKind::WacOpcode: kind = "WAC opcode"; break;
                case world::RuntimeGapKind::WacInstructionLimit: kind = "WAC instruction limit"; break;
                case world::RuntimeGapKind::BmsAction: kind = "BMS action"; break;
            }
            ImGui::Text("%s %d/%d, event %d, site %d: %llu calls (ticks %u..%u)",
                    kind, site.code, site.subcode, site.event, site.site,
                    static_cast<unsigned long long>(gap.count), gap.first_tick, gap.last_tick);
            if (!site.source.empty()) ImGui::TextDisabled("%s:%d", site.source.c_str(), site.line);
            ImGui::TextDisabled("arguments: %d, %d, %d, %d", gap.arguments[0],
                    gap.arguments[1], gap.arguments[2], gap.arguments[3]);
        }
        if (snapshot_.report.runtime_gaps.empty()) ImGui::TextDisabled("No runtime gaps observed.");
    }
	ImGui::SeparatorText("Groups");
	if (group_rows_.empty()) {
		ImGui::TextDisabled("No groups with members.");
	} else {
		for (size_t i = 0; i < group_rows_.size(); ++i) {
			ImGui::TextColored(alert_color(group_alerts_[i]), "%s",
					group_rows_[i].c_str());
		}
	}
	ImGui::SeparatorText("Routes");
	if (channel_rows_.empty()) {
		ImGui::TextDisabled("No nav channels.");
	} else {
		for (const std::string &row : channel_rows_)
			ImGui::TextUnformatted(row.c_str());
	}
}

void AiWindow::draw(ImGuiPass &pass, uint64_t) {
	// The Game-view layers, toggled here or from the Overlays menu.
	ImGui::TextUnformatted("Game view:");
	for (auto &layer : layers_) {
		ImGui::SameLine();
		bool on = layer->enabled();
		if (ImGui::Checkbox(layer->label(), &on)) pass.set_overlay_enabled(*layer, on);
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("%s", layer->tooltip());
	}
	ImGui::TextUnformatted(counters_.c_str());
	if (!snapshot_.valid) return;
	draw_detail_pane();
	char header[48];
	std::snprintf(header, sizeof(header), "Brains (%d)###brains", brain_count());
	if (ImGui::CollapsingHeader(header)) {
		if (ImGui::BeginChild("brains", ImVec2(0.0f, 220.0f), ImGuiChildFlags_None,
					ImGuiWindowFlags_HorizontalScrollbar)) {
			ImGuiListClipper clipper;
			clipper.Begin(brain_count());
			while (clipper.Step()) {
				for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
					const int alert = brain_alerts_[static_cast<size_t>(i)];
					if (alert < 0) {
						ImGui::TextDisabled("%s", brain_rows_[static_cast<size_t>(i)].c_str());
					} else {
						ImGui::TextColored(alert_color(alert), "%s", brain_rows_[static_cast<size_t>(i)].c_str());
					}
				}
			}
			clipper.End();
		}
		ImGui::EndChild();
	}
	draw_tables();
}

}  // namespace opennova::devtools
