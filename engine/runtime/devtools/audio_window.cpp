#include <runtime/devtools/audio_window.h>

#include <runtime/devtools/debug_control_ids.h>

#include <imgui.h>

#include <algorithm>
#include <cstdio>

namespace opennova::devtools {

namespace {

// A peak in dB as a 0..1 meter over the -60..0 dB range.
float meter(float db) { return std::clamp((db + 60.0f) / 60.0f, 0.0f, 1.0f); }

}  // namespace

void AudioWindow::on_visibility(bool visible) {
	if (!visible) {
		snapshot_ = AudioSnapshot{};
		editing_bus_ = -1;
		format();
	}
}

void AudioWindow::set_snapshot(AudioSnapshot snapshot) {
	snapshot_ = std::move(snapshot);
	volume_edits_.resize(snapshot_.buses.size());
	for (size_t i = 0; i < snapshot_.buses.size(); ++i) {
		if (static_cast<int>(i) != editing_bus_) volume_edits_[i] = snapshot_.buses[i].volume_db;
	}
	format();
}

void AudioWindow::request_volume(const std::string &bus, float volume_db) {
	requests_.push_back({control_id::kSetAudioBusVolume,
			{ControlArg::string(bus), ControlArg::number(std::clamp(volume_db, kVolumeMinDb, kVolumeMaxDb))}});
}

void AudioWindow::request_flag(const char *control_id, const std::string &bus, bool value) {
	requests_.push_back({control_id, {ControlArg::string(bus), ControlArg::boolean(value)}});
}

bool AudioWindow::take_request(ControlRequest &request) {
	if (requests_.empty()) return false;
	request = requests_.front();
	requests_.pop_front();
	return true;
}

void AudioWindow::format() {
	mission_text_.clear();
	if (!snapshot_.valid || !snapshot_.mission_valid) return;
	const AudioSnapshot &s = snapshot_;
	char buf[320];
	std::snprintf(buf, sizeof(buf),
			"markers %d/%d resolved (%lld live) | banks %d | ambient candidates %d/%d validated, %d decode failures | "
			"channels %lld active of %lld (budget %d) | dialogs %d | voice writes %lld | tick %.2f ms",
			s.markers_resolved, s.markers_total, static_cast<long long>(s.markers), s.banks_loaded,
			s.ambient_candidates_validated, s.ambient_candidates, s.ambient_decode_failures,
			static_cast<long long>(s.active_channels), static_cast<long long>(s.physical_channels),
			s.channel_budget, s.dialogs, static_cast<long long>(s.voice_writes),
			static_cast<double>(s.tick_us) / 1000.0);
	mission_text_ = buf;
}

void AudioWindow::draw(ImGuiPass &pass, uint64_t frame_index) {
	(void)pass;
	(void)frame_index;
	if (!snapshot_.valid) {
		ImGui::TextDisabled("No audio device sampled.");
		return;
	}
	if (ImGui::BeginTable("buses", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
		ImGui::TableSetupColumn("bus");
		ImGui::TableSetupColumn("volume dB", ImGuiTableColumnFlags_WidthStretch, 2.0f);
		ImGui::TableSetupColumn("mute");
		ImGui::TableSetupColumn("solo");
		ImGui::TableSetupColumn("bypass");
		ImGui::TableSetupColumn("peak L / R", ImGuiTableColumnFlags_WidthStretch, 2.0f);
		ImGui::TableHeadersRow();
		for (size_t i = 0; i < snapshot_.buses.size(); ++i) {
			const AudioBusRow &bus = snapshot_.buses[i];
			ImGui::PushID(static_cast<int>(i));
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(bus.name.c_str());
			ImGui::TableNextColumn();
			ImGui::SetNextItemWidth(-1.0f);
			ImGui::SliderFloat("##volume", &volume_edits_[i], kVolumeMinDb, kVolumeMaxDb, "%.1f dB");
			if (ImGui::IsItemActivated()) editing_bus_ = static_cast<int>(i);
			if (ImGui::IsItemDeactivatedAfterEdit()) request_volume(bus.name, volume_edits_[i]);
			if (ImGui::IsItemDeactivated()) editing_bus_ = -1;
			ImGui::TableNextColumn();
			bool mute = bus.mute;
			if (ImGui::Checkbox("##mute", &mute)) request_flag(control_id::kSetAudioBusMute, bus.name, mute);
			ImGui::TableNextColumn();
			bool solo = bus.solo;
			if (ImGui::Checkbox("##solo", &solo)) request_flag(control_id::kSetAudioBusSolo, bus.name, solo);
			ImGui::TableNextColumn();
			bool bypass = bus.bypass;
			if (ImGui::Checkbox("##bypass", &bypass)) {
				request_flag(control_id::kSetAudioBusBypass, bus.name, bypass);
			}
			ImGui::TableNextColumn();
			ImGui::ProgressBar(meter(bus.peak_left_db), ImVec2(-1.0f, 6.0f), "");
			ImGui::ProgressBar(meter(bus.peak_right_db), ImVec2(-1.0f, 6.0f), "");
			ImGui::PopID();
		}
		ImGui::EndTable();
	}
	if (snapshot_.mission_valid) {
		ImGui::SeparatorText("Mission audio");
		ImGui::PushTextWrapPos(0.0f);
		ImGui::TextUnformatted(mission_text_.c_str());
		ImGui::PopTextWrapPos();
	}
}

}  // namespace opennova::devtools
