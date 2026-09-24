#include <runtime/devtools/particles_window.h>

#include <runtime/devtools/debug_control_ids.h>

#include <imgui.h>

#include <cstdio>

namespace opennova::devtools {

void ParticlesWindow::on_visibility(bool visible) {
	if (!visible) {
		snapshot_ = ParticleSnapshot{};
		peak_ = 0;
		format();
	}
}

void ParticlesWindow::wanted_controls(std::vector<const char *> &out) const {
	out.push_back(control_id::kHideParticles);
}

void ParticlesWindow::set_snapshot(ParticleSnapshot snapshot) {
	snapshot_ = std::move(snapshot);
	// The peak latches until the count returns to zero [orig:
	// Debug_DrawParticleStats @0x44c840 — the peak word resets on a zero
	// current count].
	const std::size_t current = snapshot_.valid ? snapshot_.scene.live_particle_count : 0;
	if (current == 0) {
		peak_ = 0;
	} else if (current > peak_) {
		peak_ = current;
	}
	format();
}

bool ParticlesWindow::take_request(ControlRequest &request) {
	if (requests_.empty()) return false;
	request = requests_.front();
	requests_.pop_front();
	return true;
}

const char *ParticlesWindow::emitter_row(int row) const {
	if (row < 0 || row >= emitter_row_count()) return "";
	return emitter_rows_[static_cast<size_t>(row)].c_str();
}

void ParticlesWindow::format() {
	count_text_.clear();
	emitter_rows_.clear();
	if (!snapshot_.valid) return;
	char buf[192];
	// [orig: Debug_DrawParticleStats @0x44c840 — "Current Particle Count:
	//  %ld / %ld"]
	std::snprintf(buf, sizeof(buf), "Current Particle Count:  %zu / %zu",
			snapshot_.scene.live_particle_count, peak_);
	count_text_ = buf;
	long index = 0;
	for (const particle::EffectGroupDebugSnapshot &group : snapshot_.scene.groups) {
		bool first = true;
		for (const particle::EffectEmitterDebugSnapshot &emitter : group.emitters) {
			if (first) {
				std::snprintf(buf, sizeof(buf), "%02ld   %s  (%s, %zu alive)", index,
						group.effect_name.c_str(), emitter.definition_name.c_str(), emitter.alive_particle_count);
			} else {
				std::snprintf(buf, sizeof(buf), "      %s  (%zu alive%s)", emitter.definition_name.c_str(),
						emitter.alive_particle_count, emitter.emitting ? "" : ", done");
			}
			emitter_rows_.emplace_back(buf);
			first = false;
		}
		++index;
	}
}

void ParticlesWindow::draw(ImGuiPass &pass, uint64_t frame_index) {
	(void)pass;
	(void)frame_index;
	draw_control(board_, control_id::kHideParticles, requests_);
	if (!snapshot_.valid) {
		ImGui::TextDisabled("No effect scene pushed (load a mission).");
		return;
	}
	const particle::EffectDebugSnapshot &s = snapshot_.scene;
	ImGui::TextUnformatted(count_text_.c_str());
	ImGui::Text("groups %zu | emitters %zu | pools: groups %zu, emitters %zu high water",
			s.live_group_count, s.live_emitter_count, s.group_pool_high_water, s.emitter_pool_high_water);
	ImGui::Text("spawns suppressed %zu, rejected %zu, over capacity %zu | %zu effects interned",
			s.suppressed_spawn_count, s.rejected_spawn_count, s.capacity_rejection_count,
			s.interned_effect_count);
	ImGui::Separator();
	if (ImGui::BeginChild("emitters", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None)) {
		ImGuiListClipper clipper;
		clipper.Begin(emitter_row_count());
		while (clipper.Step()) {
			for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
				ImGui::TextUnformatted(emitter_rows_[static_cast<size_t>(i)].c_str());
			}
		}
		clipper.End();
	}
	ImGui::EndChild();
}

}  // namespace opennova::devtools
