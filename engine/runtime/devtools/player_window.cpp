#include <runtime/devtools/player_window.h>

#include <runtime/devtools/debug_control_ids.h>

#include <imgui.h>

#include <cstdio>

namespace opennova::devtools {

namespace {

const char *const kViewRows[] = {
		control_id::kThirdPersonOnFoot,
		control_id::kBodyInFirstPerson,
		control_id::kForceFpArms,
};

const char *stance_name(int32_t stance) {
	switch (stance) {
		case 0:
			return "standing";
		case 1:
			return "crouched";
		case 2:
			return "prone";
		default:
			return "?";
	}
}

double units(int32_t q16) { return static_cast<double>(q16) / 65536.0; }

}  // namespace

void PlayerWindow::on_visibility(bool visible) {
	if (!visible) {
		snapshot_ = PlayerSnapshot{};
		format();
	}
}

void PlayerWindow::wanted_controls(std::vector<const char *> &out) const {
	for (const char *id : kViewRows) out.push_back(id);
	out.push_back(control_id::kCycleMapMode);
	out.push_back(control_id::kLocalPlayerLook);
	out.push_back(control_id::kSetViewmodelWeapon);
	out.push_back(control_id::kClearViewmodelWeapon);
	out.push_back(control_id::kDeployPick);
	out.push_back(control_id::kCrewLocalPlayer);
}

void PlayerWindow::set_snapshot(PlayerSnapshot snapshot) {
	snapshot_ = std::move(snapshot);
	format();
}

void PlayerWindow::request_look(float dx_px, float dy_px) {
	requests_.push_back({control_id::kLocalPlayerLook, {ControlArg::number(dx_px), ControlArg::number(dy_px)}});
}

void PlayerWindow::request_viewmodel_weapon(const std::string &weapon) {
	requests_.push_back({control_id::kSetViewmodelWeapon, {ControlArg::string(weapon)}});
}

void PlayerWindow::request_deploy_pick(int32_t zone) {
	requests_.push_back({control_id::kDeployPick, {ControlArg::integer(zone)}});
}

void PlayerWindow::request_crew_local_player(int32_t vehicle_ssn) {
	requests_.push_back({control_id::kCrewLocalPlayer, {ControlArg::integer(vehicle_ssn)}});
}

bool PlayerWindow::take_request(ControlRequest &request) {
	if (requests_.empty()) return false;
	request = requests_.front();
	requests_.pop_front();
	return true;
}

void PlayerWindow::format() {
	body_text_.clear();
	if (!snapshot_.valid || !snapshot_.report.valid) return;
	const world::inspect::LocalPlayerReport &r = snapshot_.report;
	char buf[256];
	std::snprintf(buf, sizeof(buf), "%s  handle 0x%04X  team %d  hp %d/%d%s | %s | yaw %d pitch %d roll %d",
			r.name.empty() ? "(unnamed)" : r.name.c_str(), static_cast<unsigned>(r.handle), r.team, r.health,
			r.health_max, r.dead ? "  DEAD" : "", stance_name(r.stance), r.yaw_deg, r.pitch_deg, r.roll_deg);
	body_text_ = buf;
}

void PlayerWindow::draw(ImGuiPass &pass, uint64_t frame_index) {
	(void)pass;
	(void)frame_index;
	if (!snapshot_.valid || !snapshot_.report.valid) {
		ImGui::TextDisabled("No local player (load a mission, or spectate).");
	} else {
		const world::inspect::LocalPlayerReport &r = snapshot_.report;
		ImGui::TextUnformatted(body_text_.c_str());
		ImGui::Text("position (%.2f, %.2f, %.2f) | anim %s", static_cast<double>(r.position.x),
				static_cast<double>(r.position.y), static_cast<double>(r.position.z), r.anim_key.c_str());
		ImGui::Text("motor speed %.3f u/t | velocity (%.3f, %.3f, %.3f) u/t", units(r.motor_speed_q16),
				units(r.velocity_q16[0]), units(r.velocity_q16[1]), units(r.velocity_q16[2]));
		if (r.mounted) {
			ImGui::Text("mounted on 0x%04X seat %d", static_cast<unsigned>(r.mount_target), r.mount_seat);
		}
		ImGui::Text("camera mode %d%s%s%s", r.camera_mode, r.third_person ? " | third person" : "",
				r.scope_engaged ? " | scoped" : "", r.scope_settled ? " (settled)" : "");
		ImGui::Text("weapon %s | action %s | clip %d/%d | reserve %d%s",
				r.weapon.empty() ? "(none)" : r.weapon.c_str(),
				r.weapon_action_name.empty() ? "?" : r.weapon_action_name.c_str(), r.clip, r.clip_capacity,
				r.reserve, r.medic_cooldown_ticks > 0 ? " | medic cooldown" : "");
		if (r.resolve.valid && ImGui::CollapsingHeader("Movement resolver (last pass)")) {
			ImGui::Text("resolved (%.2f, %.2f, %.2f) | foot clearance %.3f", units(r.resolve.pos[0]),
					units(r.resolve.pos[1]), units(r.resolve.pos[2]), units(r.resolve.foot_clearance));
			ImGui::Text("capsule %.2f .. %.2f", units(r.resolve.capsule_bottom), units(r.resolve.capsule_top));
			for (int i = 0; i < 3; ++i) {
				ImGui::Text("point %d (%.2f, %.2f, %.2f) r %.2f", i, units(r.resolve.points[i][0]),
						units(r.resolve.points[i][1]), units(r.resolve.points[i][2]), units(r.resolve.radii[i]));
			}
		}
	}

	ImGui::SeparatorText("View");
	for (const char *id : kViewRows) draw_control(board_, id, requests_);
	draw_control(board_, control_id::kCycleMapMode, requests_);
	const ControlState *look = board_.state(control_id::kLocalPlayerLook);
	ImGui::BeginDisabled(look == nullptr || !look->writable);
	ImGui::TextUnformatted("Look nudge:");
	ImGui::SameLine();
	if (ImGui::ArrowButton("##look_left", ImGuiDir_Left)) request_look(-kLookNudgePx, 0.0f);
	ImGui::SameLine();
	if (ImGui::ArrowButton("##look_right", ImGuiDir_Right)) request_look(kLookNudgePx, 0.0f);
	ImGui::SameLine();
	if (ImGui::ArrowButton("##look_up", ImGuiDir_Up)) request_look(0.0f, -kLookNudgePx);
	ImGui::SameLine();
	if (ImGui::ArrowButton("##look_down", ImGuiDir_Down)) request_look(0.0f, kLookNudgePx);
	ImGui::EndDisabled();

	ImGui::SeparatorText("Viewmodel");
	ImGui::SetNextItemWidth(180.0f);
	ImGui::InputTextWithHint("##viewmodel", "weapon def name", viewmodel_edit_, sizeof(viewmodel_edit_));
	ImGui::SameLine();
	ImGui::BeginDisabled(viewmodel_edit_[0] == '\0');
	if (ImGui::Button("Set viewmodel")) request_viewmodel_weapon(viewmodel_edit_);
	ImGui::EndDisabled();
	ImGui::SameLine();
	draw_control(board_, control_id::kClearViewmodelWeapon, requests_, "Clear");

	ImGui::SeparatorText("Session");
	const ControlState *deploy = board_.state(control_id::kDeployPick);
	ImGui::BeginDisabled(deploy == nullptr || !deploy->writable);
	ImGui::SetNextItemWidth(90.0f);
	ImGui::InputInt("##zone", &deploy_zone_edit_);
	if (deploy_zone_edit_ < 0) deploy_zone_edit_ = 0;
	ImGui::SameLine();
	if (ImGui::Button("Deploy at zone")) request_deploy_pick(deploy_zone_edit_);
	ImGui::EndDisabled();
	if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled | ImGuiHoveredFlags_ForTooltip)) {
		ImGui::SetTooltip("One deployment pick while the deploy screen is owed (0 = the default spawn).");
	}
	const ControlState *crew = board_.state(control_id::kCrewLocalPlayer);
	ImGui::BeginDisabled(crew == nullptr || !crew->writable);
	ImGui::SetNextItemWidth(90.0f);
	ImGui::InputInt("##crew_ssn", &crew_ssn_edit_);
	ImGui::SameLine();
	if (ImGui::Button("Crew vehicle (SSN)") && crew_ssn_edit_ > 0) request_crew_local_player(crew_ssn_edit_);
	ImGui::EndDisabled();
}

}  // namespace opennova::devtools
