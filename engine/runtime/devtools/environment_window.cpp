#include <runtime/devtools/environment_window.h>

#include <runtime/devtools/debug_control_ids.h>
#include <runtime/world/weather_state.h>

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

constexpr int kNoTarget = -1;
constexpr int kLightningTarget = -2;

uint32_t pack_rgb(const std::array<float, 3> &rgb) {
	const auto byte = [](float v) -> uint32_t {
		const int scaled = static_cast<int>(v * 255.0f + 0.5f);
		return static_cast<uint32_t>(scaled < 0 ? 0 : (scaled > 255 ? 255 : scaled));
	};
	return (byte(rgb[0]) << 16) | (byte(rgb[1]) << 8) | byte(rgb[2]);
}

ImVec4 swatch(uint32_t packed) {
	return ImVec4(static_cast<float>((packed >> 16) & 0xFFu) / 255.0f,
			static_cast<float>((packed >> 8) & 0xFFu) / 255.0f,
			static_cast<float>(packed & 0xFFu) / 255.0f, 1.0f);
}

}  // namespace

void EnvironmentWindow::on_visibility(bool visible) {
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
		// The commands write targets, so the strip seeds from the ramps'
		// goals: sunfade's current never leaves 0, skyspeed's ramps.
		sky_speed_edit_ = snapshot_.sky_speed_target;
		sun_fade_pct_edit_ = snapshot_.sun_fade_target_pct;
		sky_height_edit_ = snapshot_.sky_height_metres;
		minute_edit_ = snapshot_.minute_of_day;
		fog_type_edit_ = snapshot_.fog_type;
		wind_edit_ = snapshot_.wind_scale;
		wind_pct_edit_ = (snapshot_.wind_scale * 100 + 128) / 256;
		color_fade_seconds_edit_ = snapshot_.color_fade_seconds;
		// quake(n) arms 6 * n ticks; a running quake seeds its remainder.
		if (snapshot_.quake_ticks > 0) quake_seconds_edit_ = (snapshot_.quake_ticks + 5) / 6;
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
		for (std::string &row : extra_rows_) row.clear();
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

	// The live weather state the retail page does not print.
	char buf[96];
	int e = 0;
	std::snprintf(buf, sizeof(buf), "Clock: %02d:%02d (minute %d)", s.minute_of_day / 60,
			s.minute_of_day % 60, s.minute_of_day);
	extra_rows_[e++] = buf;
	std::snprintf(buf, sizeof(buf), "Wind: %d (%d%% of 256)", s.wind_scale,
			(s.wind_scale * 100 + 128) / 256);
	extra_rows_[e++] = buf;
	std::snprintf(buf, sizeof(buf), "Rain target: %d%% (%s)", s.rain_target_pct,
			s.precipitation_kind == 1 ? "snow" : "rain");
	extra_rows_[e++] = buf;
	extra_rows_[e++] = int_row("Overcast target", s.overcast_target_pct, "%");
	extra_rows_[e++] = int_row("Fog target", s.fog_target_metres, "m");
	std::snprintf(buf, sizeof(buf), "SkySpeed target: %d | SunFade target: %d%%",
			s.sky_speed_target, s.sun_fade_target_pct);
	extra_rows_[e++] = buf;
	std::snprintf(buf, sizeof(buf), "Lightning: timers %d / %d, level %d", s.lightning_timer_a,
			s.lightning_timer_b, s.lightning_level);
	extra_rows_[e++] = buf;
	extra_rows_[e++] = int_row("Quake", s.quake_ticks, " ticks left");
}

int EnvironmentWindow::row_count() const {
	return snapshot_.valid ? kRowCount : 0;
}

const char *EnvironmentWindow::row_text(int row) const {
	if (row < 0 || row >= row_count()) return "";
	return rows_[static_cast<size_t>(row)].c_str();
}

int EnvironmentWindow::extra_row_count() const {
	return snapshot_.valid ? kExtraRowCount : 0;
}

const char *EnvironmentWindow::extra_row_text(int row) const {
	if (row < 0 || row >= extra_row_count()) return "";
	return extra_rows_[static_cast<size_t>(row)].c_str();
}

void EnvironmentWindow::request_sky_height(int32_t metres) {
	// skyheight's parameter IS the 16.16 target (the WAC parser's << 16 does
	// not apply to the command), so whole metres shift here.
	enqueue_request({control_id::kEnvironmentSkyHeight,
			{ControlArg::integer(static_cast<int64_t>(metres) * 65536)}});
}

void EnvironmentWindow::request_clock_scrub(int32_t minute_of_day) {
	enqueue_request({control_id::kEnvironmentTimeOfDay, {ControlArg::number(minute_of_day)}});
}

void EnvironmentWindow::request_wind_strength(int32_t percent) {
	enqueue_request({control_id::kEnvironmentWindStrength, {ControlArg::number(percent)}});
}

void EnvironmentWindow::request_weather_snapshot() {
	enqueue_request({control_id::kEnvironmentWeatherSnapshot, {}});
}

void EnvironmentWindow::enqueue_request(const ControlRequest &request) {
	requests_.push_back(request);
}

bool EnvironmentWindow::take_request(ControlRequest &request) {
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
		0, 0, 0, 0,
	};
	const bool swatched[kRowCount] = {
		false, false, false, false, false, false, false, false,
		true, true, true, false, true, true, true, true, true, true, false, false,
		true, true, true, true, false, false, false, false,
	};
	// The color rows a WAC handler targets open a picker that drives that
	// handler (fogcolor/skyfogcolor/cloud/sun/lightning/sky/ground/ceiling/
	// floor/gain); the derived blocks (outdoor, indoor, iris) stay read-only.
	// While the mission carries a TOD keyframe table the compute re-snaps
	// the five keyframed rows every tick, so their handlers are inert and
	// their pickers stay closed too [orig: Environment_ComputeTimeOfDayColors
	// @ 0x57e078..0x57e3c9 overwrites what WacCmd_Sun @ 0x4edcd0 wrote; the
	// early return @ 0x57de8a without a table leaves the handlers live].
	const int block_targets[kRowCount] = {
		kNoTarget, kNoTarget, kNoTarget, kNoTarget, kNoTarget, kNoTarget, kNoTarget, kNoTarget,
		static_cast<int>(world::WeatherColorTarget::Fog),
		static_cast<int>(world::WeatherColorTarget::SkyFog),
		static_cast<int>(world::WeatherColorTarget::Cloud), kNoTarget,
		static_cast<int>(world::WeatherColorTarget::Sun), kLightningTarget,
		static_cast<int>(world::WeatherColorTarget::Sky),
		static_cast<int>(world::WeatherColorTarget::Ground),
		static_cast<int>(world::WeatherColorTarget::Ceiling),
		static_cast<int>(world::WeatherColorTarget::Floor), kNoTarget, kNoTarget,
		kNoTarget, kNoTarget,
		static_cast<int>(world::WeatherColorTarget::Gain), kNoTarget,
		kNoTarget, kNoTarget, kNoTarget, kNoTarget,
	};
	const bool keyframed_rows[kRowCount] = {
		false, false, false, false, false, false, false, false,
		true, true, false, false, true, false, true, true, false, false, false, false,
		false, false, false, false, false, false, false, false,
	};
	const ImVec2 swatch_size(ImGui::GetFrameHeight(), ImGui::GetFrameHeight());
	if (ImGui::BeginTable("environment_rows", 2, ImGuiTableFlags_SizingStretchSame)) {
		for (int i = 0; i < kRowCount; ++i) {
			if ((i & 1) == 0) ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::PushID(i);
			const bool inert = keyframed_rows[i] && snapshot_.tod_keyframed;
			if (swatched[i] && (block_targets[i] == kNoTarget || !snapshot_.authority || inert)) {
				ImGui::ColorButton("##swatch", swatch(swatches[i]),
						ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoPicker, swatch_size);
				ImGui::SameLine();
			} else if (swatched[i]) {
				std::array<float, 3> &rgb = color_edit_[static_cast<size_t>(i)];
				const bool open = ImGui::IsPopupOpen("picker");
				if (!open) {
					const ImVec4 current = swatch(swatches[i]);
					rgb = {current.x, current.y, current.z};
				}
				if (ImGui::ColorButton("##swatch", ImVec4(rgb[0], rgb[1], rgb[2], 1.0f),
							ImGuiColorEditFlags_NoTooltip, swatch_size)) {
					ImGui::OpenPopup("picker");
				}
				if (ImGui::BeginPopup("picker")) {
					if (ImGui::ColorPicker3("##color", rgb.data(),
								ImGuiColorEditFlags_NoSidePreview | ImGuiColorEditFlags_NoSmallPreview |
										ImGuiColorEditFlags_DisplayRGB)) {
						const int32_t packed = static_cast<int32_t>(pack_rgb(rgb));
						if (block_targets[i] == kLightningTarget) {
							enqueue_request({control_id::kEnvironmentLightningColor,
									{ControlArg::integer(packed)}});
						} else {
							enqueue_request({control_id::kEnvironmentBlockColor,
									{ControlArg::integer(block_targets[i]), ControlArg::integer(packed)}});
						}
					}
					ImGui::EndPopup();
				}
				ImGui::SameLine();
			}
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

	// Every button is one debug-control row invoked with the WAC arguments
	// (debug_control_ids.h); the drain hands them to the table unchanged.
	const auto one = [](int32_t a) { return std::vector<ControlArg>{ControlArg::integer(a)}; };
	const auto two = [](int32_t a, int32_t b) {
		return std::vector<ControlArg>{ControlArg::integer(a), ControlArg::integer(b)};
	};
	ImGui::SetNextItemWidth(w);
	ImGui::SliderInt("##precip", &rain_pct_edit_, 0, 100, "precip %d%%");
	if (ImGui::IsItemHovered()) {
		ImGui::SetTooltip("Rain and Snow write the one precipitation channel; the button picks the kind.");
	}
	ImGui::SameLine();
	if (ImGui::Button("Rain")) enqueue_request({control_id::kEnvironmentRain, two(rain_pct_edit_, seconds_edit_)});
	ImGui::SameLine();
	if (ImGui::Button("Snow")) enqueue_request({control_id::kEnvironmentSnow, two(rain_pct_edit_, seconds_edit_)});

	ImGui::SetNextItemWidth(w);
	ImGui::SliderInt("##overcast", &overcast_pct_edit_, 0, 100, "%d%%");
	ImGui::SameLine();
	if (ImGui::Button("Overcast")) enqueue_request({control_id::kEnvironmentOvercast, two(overcast_pct_edit_, seconds_edit_)});

	ImGui::SetNextItemWidth(w);
	ImGui::InputInt("##fog", &fog_metres_edit_);
	ImGui::SameLine();
	if (ImGui::Button("Fog dist")) enqueue_request({control_id::kEnvironmentFogDistance, one(fog_metres_edit_)});
	ImGui::SameLine();
	if (ImGui::Button("Move fog")) enqueue_request({control_id::kEnvironmentMoveFog, two(fog_metres_edit_, seconds_edit_)});
	ImGui::SameLine();
	ImGui::SetNextItemWidth(w);
	ImGui::SliderInt("##fogtype", &fog_type_edit_, 0, 3, "type %d");
	ImGui::SameLine();
	if (ImGui::Button("Fog type")) enqueue_request({control_id::kEnvironmentFogType, one(fog_type_edit_)});

	ImGui::SetNextItemWidth(w);
	ImGui::InputInt("##skyspeed", &sky_speed_edit_);
	ImGui::SameLine();
	if (ImGui::Button("Sky speed")) enqueue_request({control_id::kEnvironmentSkySpeed, one(sky_speed_edit_)});
	ImGui::SameLine();
	ImGui::SetNextItemWidth(w);
	ImGui::InputInt("##skyheight", &sky_height_edit_);
	ImGui::SameLine();
	if (ImGui::Button("Sky height")) request_sky_height(sky_height_edit_);

	ImGui::SetNextItemWidth(w);
	ImGui::InputInt("##wind", &wind_edit_);
	ImGui::SameLine();
	if (ImGui::Button("Wind")) enqueue_request({control_id::kEnvironmentWindScale, one(wind_edit_)});
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("wind(value): 256 is the retail default.");
	ImGui::SameLine();
	ImGui::SetNextItemWidth(w);
	ImGui::SliderInt("##windpct", &wind_pct_edit_, 0, 100, "%d%%");
	ImGui::SameLine();
	if (ImGui::Button("Wind %")) request_wind_strength(wind_pct_edit_);

	ImGui::SetNextItemWidth(w);
	ImGui::SliderInt("##minute", &minute_edit_, 0, 1439, "%d min");
	ImGui::SameLine();
	if (ImGui::Button("Time of day")) enqueue_request({control_id::kEnvironmentTimeOfDayMinutes, one(minute_edit_)});
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("tod(minute): the WAC command's clock math.");
	ImGui::SameLine();
	if (ImGui::Button("Scrub")) request_clock_scrub(minute_edit_);
	if (ImGui::IsItemHovered()) {
		ImGui::SetTooltip("Set the mission clock to exactly this minute and resync the colors.");
	}

	ImGui::SetNextItemWidth(w);
	ImGui::InputInt("##quake", &quake_seconds_edit_);
	ImGui::SameLine();
	if (ImGui::Button("Quake")) enqueue_request({control_id::kEnvironmentQuake, one(quake_seconds_edit_)});
	ImGui::SameLine();
	// flash / farflash are the lightning rows' handlers (the short and the
	// long strike), so the buttons invoke those rows.
	if (ImGui::Button("Flash")) enqueue_request({control_id::kEnvironmentLightningShort, {}});
	ImGui::SameLine();
	if (ImGui::Button("Far flash")) enqueue_request({control_id::kEnvironmentLightningLong, {}});

	ImGui::SetNextItemWidth(w);
	ImGui::SliderInt("##sunfade", &sun_fade_pct_edit_, 0, 100, "%d%%");
	ImGui::SameLine();
	if (ImGui::Button("Sun fade")) enqueue_request({control_id::kEnvironmentSunFade, two(sun_fade_pct_edit_, seconds_edit_)});
	ImGui::SameLine();
	ImGui::SetNextItemWidth(w);
	ImGui::InputInt("##colorfade", &color_fade_seconds_edit_);
	ImGui::SameLine();
	if (ImGui::Button("Color fade")) enqueue_request({control_id::kEnvironmentColorFade, one(color_fade_seconds_edit_)});
	ImGui::EndDisabled();
	// A read, no authority needed: the weather home lands as the command's
	// reported result.
	if (ImGui::Button("Weather snapshot")) request_weather_snapshot();
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
	if (ImGui::CollapsingHeader("Live weather (beyond the retail page)",
				ImGuiTreeNodeFlags_DefaultOpen)) {
		for (int i = 0; i < extra_row_count(); ++i) {
			ImGui::TextUnformatted(extra_rows_[static_cast<size_t>(i)].c_str());
		}
	}
	draw_controls();
}

}  // namespace opennova::devtools
