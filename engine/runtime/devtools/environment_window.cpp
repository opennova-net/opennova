#include <runtime/devtools/environment_window.h>

#include <imgui.h>

#include <cstdio>

namespace opennova::devtools {

namespace {

std::string rgb_row(const char *label, uint32_t packed) {
	char buf[64];
	std::snprintf(buf, sizeof(buf), "%s: (%u, %u, %u)", label,
			(packed >> 16) & 0xFFu, (packed >> 8) & 0xFFu, packed & 0xFFu);
	return buf;
}

std::string int_row(const char *label, int32_t value, const char *suffix = "") {
	char buf[64];
	std::snprintf(buf, sizeof(buf), "%s: %d%s", label, value, suffix);
	return buf;
}

ImVec4 swatch(uint32_t packed) {
	return ImVec4(static_cast<float>((packed >> 16) & 0xFFu) / 255.0f,
			static_cast<float>((packed >> 8) & 0xFFu) / 255.0f,
			static_cast<float>(packed & 0xFFu) / 255.0f, 1.0f);
}

}  // namespace

void EnvironmentWindow::on_visibility(bool visible) {
	shown_ = visible;
	if (!visible) {
		// Drop the snapshot so a closed window holds nothing; the embedder's
		// needs_environment_snapshot gate stops the pushes on the same edge.
		snapshot_ = EnvironmentSnapshot{};
		seeded_ = false;
		format_rows();
	}
}

void EnvironmentWindow::set_snapshot(const EnvironmentSnapshot &snapshot) {
	snapshot_ = snapshot;
	if (snapshot_.valid && !seeded_) {
		// Seed the control strip from the live values (an untouched Apply is
		// a no-op-shaped write).
		rain_pct_edit_ = snapshot_.rain_target_pct;
		overcast_pct_edit_ = snapshot_.overcast_target_pct;
		fog_metres_edit_ = snapshot_.fog_target_metres;
		sky_speed_edit_ = snapshot_.sky_speed;
		minute_edit_ = snapshot_.minute_of_day;
		fog_type_edit_ = snapshot_.fog_type;
		wind_edit_ = snapshot_.wind_scale;
		color_fade_seconds_edit_ = snapshot_.color_fade_seconds;
		seeded_ = true;
	}
	if (!snapshot_.valid) {
		seeded_ = false;
	}
	format_rows();
}

void EnvironmentWindow::format_rows() {
	if (!snapshot_.valid) {
		for (std::string &row : rows_) row.clear();
		return;
	}
	const EnvironmentSnapshot &s = snapshot_;
	// The retail page in its two-column reading order [orig:
	// Debug_DrawEnvironmentValues @ 0x4ef000 — the sprintf formats verbatim].
	char blink[6] = {'-', '-', '-', '-', '-', '\0'};
	if (s.blink_flags & 0x02u) blink[0] = 'V';
	if (s.blink_flags & 0x04u) blink[1] = 'S';
	if (s.blink_flags & 0x08u) blink[2] = 'W';
	if (s.blink_flags & 0x10u) blink[3] = 'L';
	if (s.blink_flags & 0x20u) blink[4] = 'O';
	int i = 0;
	rows_[i++] = "Env: " + s.env_name;
	rows_[i++] = "Trn: " + s.trn_name;
	rows_[i++] = std::string("Blink: ") + blink;
	rows_[i++] = int_row("Fogtype", s.fog_type);
	rows_[i++] = int_row("Fogdist", s.fog_dist_metres, "m");
	rows_[i++] = int_row("ColorFade", s.color_fade_seconds, " seconds");
	rows_[i++] = int_row("SunFade", s.sun_fade_pct, "%");
	rows_[i++] = int_row("MoonLight", s.night ? 1 : 0);
	rows_[i++] = rgb_row("Fog", s.fog_rgb);
	rows_[i++] = rgb_row("SkyFog", s.skyfog_rgb);
	rows_[i++] = rgb_row("Cloud", s.cloud_rgb);
	rows_[i++] = int_row("FOV", s.fov_degrees, " degrees");
	rows_[i++] = rgb_row("Sun", s.sun_rgb);
	rows_[i++] = rgb_row("Lightning", s.lightning_rgb);
	rows_[i++] = rgb_row("Sky", s.sky_rgb);
	rows_[i++] = rgb_row("Ground", s.ground_rgb);
	rows_[i++] = rgb_row("Ceiling", s.ceiling_rgb);
	rows_[i++] = rgb_row("Floor", s.floor_rgb);
	rows_[i++] = int_row("SkyHeight", s.sky_height_metres, "m");
	rows_[i++] = int_row("SkySpeed", s.sky_speed, "m");
	rows_[i++] = rgb_row("OutDoor", s.outdoor_rgb);
	rows_[i++] = rgb_row("InDoor", s.indoor_rgb);
	rows_[i++] = rgb_row("Gain", s.gain_rgb);
	rows_[i++] = rgb_row("Iris", s.iris_rgb);
	rows_[i++] = int_row("Rain", s.rain_pct, "%");
	rows_[i++] = int_row("Overcast", s.overcast_pct, "%");
	rows_[i++] = int_row("Complexity", s.complexity);
	// [orig: sprintf(text_buf, "  DCB: %i", 692) — the literal]
	rows_[i++] = int_row("DCB", 692);
	rows_[i++] = int_row("Quake", s.quake_ticks, " ticks");
	rows_[i++] = int_row("Precipitation", s.precipitation_kind);
}

int EnvironmentWindow::row_count() const {
	return snapshot_.valid ? kRowCount : 0;
}

const char *EnvironmentWindow::row_text(int row) const {
	if (row < 0 || row >= row_count()) return "";
	return rows_[static_cast<size_t>(row)].c_str();
}

void EnvironmentWindow::enqueue_request(const EnvironmentRequest &request) {
	requests_.push_back(request);
}

bool EnvironmentWindow::take_request(EnvironmentRequest &request) {
	if (requests_.empty()) return false;
	request = requests_.front();
	requests_.pop_front();
	return true;
}

void EnvironmentWindow::draw_rows() {
	// Two columns like the retail page: left rows at x 10, right rows at
	// x 200, the color rows with a swatch beside their text.
	const uint32_t swatches[kRowCount] = {
		0, 0, 0, 0, 0, 0, 0, 0,
		snapshot_.fog_rgb, snapshot_.skyfog_rgb, snapshot_.cloud_rgb, 0,
		snapshot_.sun_rgb, snapshot_.lightning_rgb, snapshot_.sky_rgb, snapshot_.ground_rgb,
		snapshot_.ceiling_rgb, snapshot_.floor_rgb, 0, 0,
		snapshot_.outdoor_rgb, snapshot_.indoor_rgb, snapshot_.gain_rgb, snapshot_.iris_rgb,
		0, 0, 0, 0, 0, 0,
	};
	const bool swatched[kRowCount] = {
		false, false, false, false, false, false, false, false,
		true, true, true, false, true, true, true, true, true, true, false, false,
		true, true, true, true, false, false, false, false, false, false,
	};
	if (ImGui::BeginTable("environment_rows", 2, ImGuiTableFlags_SizingStretchSame)) {
		for (int i = 0; i < kRowCount; ++i) {
			if ((i & 1) == 0) ImGui::TableNextRow();
			ImGui::TableNextColumn();
			if (swatched[i]) {
				ImGui::ColorButton("##swatch", swatch(swatches[i]),
						ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoPicker,
						ImVec2(ImGui::GetFrameHeight(), ImGui::GetFrameHeight()));
				ImGui::SameLine();
			}
			ImGui::PushID(i);
			ImGui::TextUnformatted(rows_[static_cast<size_t>(i)].c_str());
			ImGui::PopID();
		}
		ImGui::EndTable();
	}
}

void EnvironmentWindow::draw_controls() {
	ImGui::Separator();
	ImGui::TextUnformatted(snapshot_.authority
					? "Weather commands (the WAC handlers)"
					: "Weather commands land on the authority; a joiner only observes");
	ImGui::BeginDisabled(!snapshot_.authority);
	const float w = 96.0f;
	ImGui::SetNextItemWidth(w);
	ImGui::InputInt("Seconds##transition", &seconds_edit_);
	if (seconds_edit_ < 0) seconds_edit_ = 0;

	ImGui::SetNextItemWidth(w);
	ImGui::SliderInt("##rain", &rain_pct_edit_, 0, 100, "%d%%");
	ImGui::SameLine();
	if (ImGui::Button("Rain")) enqueue_request({EnvironmentRequest::Kind::Rain, rain_pct_edit_, seconds_edit_});
	ImGui::SameLine();
	if (ImGui::Button("Snow")) enqueue_request({EnvironmentRequest::Kind::Snow, rain_pct_edit_, seconds_edit_});

	ImGui::SetNextItemWidth(w);
	ImGui::SliderInt("##overcast", &overcast_pct_edit_, 0, 100, "%d%%");
	ImGui::SameLine();
	if (ImGui::Button("Overcast")) enqueue_request({EnvironmentRequest::Kind::Overcast, overcast_pct_edit_, seconds_edit_});

	ImGui::SetNextItemWidth(w);
	ImGui::InputInt("##fog", &fog_metres_edit_);
	ImGui::SameLine();
	if (ImGui::Button("Fog dist")) enqueue_request({EnvironmentRequest::Kind::FogDistance, fog_metres_edit_, 0});
	ImGui::SameLine();
	if (ImGui::Button("Move fog")) enqueue_request({EnvironmentRequest::Kind::MoveFog, fog_metres_edit_, seconds_edit_});
	ImGui::SameLine();
	ImGui::SetNextItemWidth(w);
	ImGui::SliderInt("##fogtype", &fog_type_edit_, 0, 3, "type %d");
	ImGui::SameLine();
	if (ImGui::Button("Fog type")) enqueue_request({EnvironmentRequest::Kind::FogType, fog_type_edit_, 0});

	ImGui::SetNextItemWidth(w);
	ImGui::InputInt("##skyspeed", &sky_speed_edit_);
	ImGui::SameLine();
	if (ImGui::Button("Sky speed")) enqueue_request({EnvironmentRequest::Kind::SkySpeed, sky_speed_edit_, 0});
	ImGui::SameLine();
	ImGui::SetNextItemWidth(w);
	ImGui::InputInt("##wind", &wind_edit_);
	ImGui::SameLine();
	if (ImGui::Button("Wind")) enqueue_request({EnvironmentRequest::Kind::WindScale, wind_edit_, 0});

	ImGui::SetNextItemWidth(w);
	ImGui::SliderInt("##minute", &minute_edit_, 0, 1439, "%d min");
	ImGui::SameLine();
	if (ImGui::Button("Time of day")) enqueue_request({EnvironmentRequest::Kind::TimeOfDayMinutes, minute_edit_, 0});

	ImGui::SetNextItemWidth(w);
	ImGui::InputInt("##quake", &quake_seconds_edit_);
	ImGui::SameLine();
	if (ImGui::Button("Quake")) enqueue_request({EnvironmentRequest::Kind::Quake, quake_seconds_edit_, 0});
	ImGui::SameLine();
	if (ImGui::Button("Flash")) enqueue_request({EnvironmentRequest::Kind::Flash, 0, 0});
	ImGui::SameLine();
	if (ImGui::Button("Far flash")) enqueue_request({EnvironmentRequest::Kind::FarFlash, 0, 0});

	ImGui::SetNextItemWidth(w);
	ImGui::SliderInt("##sunfade", &sun_fade_pct_edit_, 0, 100, "%d%%");
	ImGui::SameLine();
	if (ImGui::Button("Sun fade")) enqueue_request({EnvironmentRequest::Kind::SunFade, sun_fade_pct_edit_, seconds_edit_});
	ImGui::SameLine();
	ImGui::SetNextItemWidth(w);
	ImGui::InputInt("##colorfade", &color_fade_seconds_edit_);
	ImGui::SameLine();
	if (ImGui::Button("Color fade")) enqueue_request({EnvironmentRequest::Kind::ColorFade, color_fade_seconds_edit_, 0});
	ImGui::EndDisabled();
}

void EnvironmentWindow::draw(ImGuiPass &pass, uint64_t frame_index) {
	(void)pass;
	(void)frame_index;
	if (!snapshot_.valid) {
		ImGui::TextUnformatted("No environment pushed (load a mission).");
		return;
	}
	ImGui::Text("Script & Env Values | logic tick %llu (%.2f s readings)",
			static_cast<unsigned long long>(snapshot_.logic_tick), kRefreshSeconds);
	draw_rows();
	draw_controls();
}

}  // namespace opennova::devtools
