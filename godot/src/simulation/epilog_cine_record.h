#pragma once

// The SP end-of-round cine as the shell draws it (Simulation::get_epilog_cine;
// engine: runtime/world/epilog_cine.h): the timeline frame, the stage flags,
// the cinematic bars and every timeline event with its live alpha. Read-only
// records; the engine owns the schedule, the stage machines and the values.

#include <runtime/world/epilog_cine.h>

#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/typed_array.hpp>

namespace godot {

// One timeline event (world::CineEvent).
class CineEventRecord : public RefCounted {
	GDCLASS(CineEventRecord, RefCounted)

	opennova::world::CineEvent value_;

protected:
	static void _bind_methods();

public:
	enum Kind {
		KIND_CACHE_CAMERA = static_cast<int>(opennova::world::CineEventKind::CacheCamera),
		KIND_LETTERBOX = static_cast<int>(opennova::world::CineEventKind::Letterbox),
		KIND_EDIT_FADE = static_cast<int>(opennova::world::CineEventKind::EditFade),
		KIND_IMAGE_FADE = static_cast<int>(opennova::world::CineEventKind::ImageFade),
		KIND_TEXT_FADE = static_cast<int>(opennova::world::CineEventKind::TextFade),
		KIND_EPILOG_COUNTER = static_cast<int>(opennova::world::CineEventKind::EpilogCounter),
	};
	enum FadeSource {
		FADE_SOURCE_IMAGE = static_cast<int>(opennova::world::CineFadeSource::Image),
		FADE_SOURCE_SOLID_COLOR = static_cast<int>(opennova::world::CineFadeSource::SolidColor),
	};
	enum TextSource {
		TEXT_SOURCE_GAME_TEXT = static_cast<int>(opennova::world::CineTextSource::GameText),
		TEXT_SOURCE_BANNER = static_cast<int>(opennova::world::CineTextSource::Banner),
	};

	void assign(const opennova::world::CineEvent &p_value) { value_ = p_value; }

	int get_kind() const { return static_cast<int>(value_.kind); }
	int get_start() const { return value_.start; }
	int get_duration() const { return value_.duration; }
	// Bit 0: drawn in the second pass, over the first pass's images and fades.
	bool get_second_pass() const { return (value_.flags & 1) != 0; }
	float get_alpha() const { return value_.alpha; }
	String get_image() const;
	int get_fade_source() const { return static_cast<int>(value_.fade_source); }
	int get_solid_color() const { return static_cast<int>(value_.solid_color); }
	int get_text_source() const { return static_cast<int>(value_.text_source); }
	String get_text_section() const;
	String get_text_key() const;
	int get_x() const { return value_.x; }
	int get_y() const { return value_.y; }
	int get_font() const { return value_.font; }
	int get_box_width() const { return value_.box_width; }
	int get_box_height() const { return value_.box_height; }
	int get_color_mask() const { return static_cast<int>(value_.color_mask); }
	String get_label_key() const;
	int get_label_x() const { return value_.label_x; }
	int get_value_x() const { return value_.value_x; }
	int get_row_y() const { return value_.row_y; }
	// The counter's value column as CineEventEpilogCounter_Draw composes it
	// (hud::epilog_counter_value_text: empty for a negative value).
	String get_value_text() const;
	// The colour the node draws in (world::cine_event_draw_argb).
	Color get_draw_color() const;
	bool is_live_at(int p_frame) const { return value_.live_at(p_frame); }
};

// The cine (world::EpilogCine).
class EpilogCineState : public RefCounted {
	GDCLASS(EpilogCineState, RefCounted)

	int mode_ = 0;
	int frame_ = -1;
	bool active_ = false;
	bool screen_active_ = false;
	bool bars_fading_ = false;
	bool bars_held_ = false;
	float bars_alpha_ = 0.0f;
	TypedArray<CineEventRecord> events_;

protected:
	static void _bind_methods();

public:
	enum Mode {
		MODE_NONE = static_cast<int>(opennova::world::EpilogCineMode::None),
		MODE_WIN = static_cast<int>(opennova::world::EpilogCineMode::Win),
		MODE_LOSE = static_cast<int>(opennova::world::EpilogCineMode::Lose),
	};

	void assign(const opennova::world::EpilogCine &p_value);

	int get_mode() const { return mode_; }
	int get_frame() const { return frame_; }
	bool get_active() const { return active_; }
	bool get_screen_active() const { return screen_active_; }
	// The bars draw while fading (at bars_alpha) or held (opaque).
	bool get_bars_fading() const { return bars_fading_; }
	bool get_bars_held() const { return bars_held_; }
	float get_bars_alpha() const { return bars_alpha_; }
	TypedArray<CineEventRecord> get_events() const { return events_; }
	// The cinematic bars' height for a display (world::epilog_bar_height).
	static int bar_height(int p_width, int p_height);
};

} // namespace godot

VARIANT_ENUM_CAST(godot::CineEventRecord::Kind);
VARIANT_ENUM_CAST(godot::CineEventRecord::FadeSource);
VARIANT_ENUM_CAST(godot::CineEventRecord::TextSource);
VARIANT_ENUM_CAST(godot::EpilogCineState::Mode);
