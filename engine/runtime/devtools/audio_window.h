// The Audio window: the audio buses with live peak meters and their
// volume / mute / solo / bypass (the debug-control table's four set_audio_bus_*
// rows), and the mission audio's setup and per-tick counters (the ambient
// markers, the candidate sounds, the physical channel pool against its
// budget, the dialogs). Records in: the AudioSnapshot the embedder samples
// from the audio device (the buses exist only because Godot's AudioServer
// does) and the mission audio node's stats.
#pragma once

#include <runtime/devtools/control_request.h>
#include <runtime/devtools/imgui_pass.h>

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace opennova::devtools {

struct AudioBusRow {
	std::string name;
	float volume_db = 0.0f;
	bool mute = false;
	bool solo = false;
	bool bypass = false;
	float peak_left_db = -200.0f;
	float peak_right_db = -200.0f;
};

struct AudioSnapshot {
	bool valid = false;
	std::vector<AudioBusRow> buses;
	// The loaded mission's audio (setup + the last tick).
	bool mission_valid = false;
	int32_t markers_total = 0;
	int32_t markers_resolved = 0;
	int32_t banks_loaded = 0;
	int32_t ambient_candidates = 0;
	int32_t ambient_candidates_validated = 0;
	int32_t ambient_decode_failures = 0;
	int32_t channel_budget = 0;
	int32_t dialogs = 0;
	int64_t tick_us = 0;
	int64_t markers = 0;
	int64_t voice_writes = 0;
	int64_t physical_channels = 0;
	int64_t active_channels = 0;
};

class AudioWindow : public Window {
public:
	// The meters move with the sound; the buses refresh ten times a second.
	static constexpr double kRefreshSeconds = 0.1;
	static constexpr float kVolumeMinDb = -80.0f;
	static constexpr float kVolumeMaxDb = 24.0f;

	const char *title() const override { return "Audio"; }
	MenuGroup menu_group() const override { return MenuGroup::Render; }
	WindowSizeHint preferred_size() const override { return {600.0f, 360.0f}; }
	void draw(ImGuiPass &pass, uint64_t frame_index) override;
	void on_visibility(bool visible) override;

	void set_snapshot(AudioSnapshot snapshot);
	bool snapshot_valid() const { return snapshot_.valid; }

	// The bus rows' requests (the set_audio_bus_* rows, by bus name).
	void request_volume(const std::string &bus, float volume_db);
	void request_flag(const char *control_id, const std::string &bus, bool value);
	bool take_request(ControlRequest &request);

	const std::string &mission_text() const { return mission_text_; }

private:
	void format();

	AudioSnapshot snapshot_{};
	std::string mission_text_;
	std::deque<ControlRequest> requests_;
	std::vector<float> volume_edits_;
	int editing_bus_ = -1;
};

}  // namespace opennova::devtools
