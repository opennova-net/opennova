#include "simulation/epilog_cine_record.h"

#include "util/color_convert.h"
#include "util/record_bind.h"
#include "util/string_convert.h"

#include <runtime/hud/end_round_statistics.h>

using namespace godot;
using opennova::to_gd;

// --- CineEventRecord ------------------------------------------------------------

String CineEventRecord::get_image() const { return to_gd(value_.image); }
String CineEventRecord::get_text_section() const { return to_gd(value_.text_section); }
String CineEventRecord::get_text_key() const { return to_gd(value_.text_key); }
String CineEventRecord::get_label_key() const { return to_gd(value_.label_key); }

String CineEventRecord::get_value_text() const {
	return to_gd(opennova::hud::epilog_counter_value_text(value_.value, value_.max));
}

Color CineEventRecord::get_draw_color() const {
	return opennova::color_from_argb(opennova::world::cine_event_draw_argb(value_));
}

void CineEventRecord::_bind_methods() {
	BIND_ENUM_CONSTANT(KIND_CACHE_CAMERA);
	BIND_ENUM_CONSTANT(KIND_LETTERBOX);
	BIND_ENUM_CONSTANT(KIND_EDIT_FADE);
	BIND_ENUM_CONSTANT(KIND_IMAGE_FADE);
	BIND_ENUM_CONSTANT(KIND_TEXT_FADE);
	BIND_ENUM_CONSTANT(KIND_EPILOG_COUNTER);
	BIND_ENUM_CONSTANT(FADE_SOURCE_IMAGE);
	BIND_ENUM_CONSTANT(FADE_SOURCE_SOLID_COLOR);
	BIND_ENUM_CONSTANT(TEXT_SOURCE_GAME_TEXT);
	BIND_ENUM_CONSTANT(TEXT_SOURCE_BANNER);
	OPENNOVA_RECORD_READ_ONLY(CineEventRecord, Variant::INT, kind)
	OPENNOVA_RECORD_READ_ONLY(CineEventRecord, Variant::INT, start)
	OPENNOVA_RECORD_READ_ONLY(CineEventRecord, Variant::INT, duration)
	OPENNOVA_RECORD_READ_ONLY(CineEventRecord, Variant::BOOL, second_pass)
	OPENNOVA_RECORD_READ_ONLY(CineEventRecord, Variant::FLOAT, alpha)
	OPENNOVA_RECORD_READ_ONLY(CineEventRecord, Variant::STRING, image)
	OPENNOVA_RECORD_READ_ONLY(CineEventRecord, Variant::INT, fade_source)
	OPENNOVA_RECORD_READ_ONLY(CineEventRecord, Variant::INT, solid_color)
	OPENNOVA_RECORD_READ_ONLY(CineEventRecord, Variant::INT, text_source)
	OPENNOVA_RECORD_READ_ONLY(CineEventRecord, Variant::STRING, text_section)
	OPENNOVA_RECORD_READ_ONLY(CineEventRecord, Variant::STRING, text_key)
	OPENNOVA_RECORD_READ_ONLY(CineEventRecord, Variant::INT, x)
	OPENNOVA_RECORD_READ_ONLY(CineEventRecord, Variant::INT, y)
	OPENNOVA_RECORD_READ_ONLY(CineEventRecord, Variant::INT, font)
	OPENNOVA_RECORD_READ_ONLY(CineEventRecord, Variant::INT, box_width)
	OPENNOVA_RECORD_READ_ONLY(CineEventRecord, Variant::INT, box_height)
	OPENNOVA_RECORD_READ_ONLY(CineEventRecord, Variant::INT, color_mask)
	OPENNOVA_RECORD_READ_ONLY(CineEventRecord, Variant::STRING, label_key)
	OPENNOVA_RECORD_READ_ONLY(CineEventRecord, Variant::INT, label_x)
	OPENNOVA_RECORD_READ_ONLY(CineEventRecord, Variant::INT, value_x)
	OPENNOVA_RECORD_READ_ONLY(CineEventRecord, Variant::INT, row_y)
	OPENNOVA_RECORD_READ_ONLY(CineEventRecord, Variant::STRING, value_text)
	OPENNOVA_RECORD_READ_ONLY(CineEventRecord, Variant::COLOR, draw_color)
	ClassDB::bind_method(D_METHOD("is_live_at", "frame"), &CineEventRecord::is_live_at);
}

// --- EpilogCineState ------------------------------------------------------------

void EpilogCineState::assign(const opennova::world::EpilogCine &p_value) {
	mode_ = static_cast<int>(p_value.mode);
	frame_ = p_value.frame;
	active_ = p_value.active;
	screen_active_ = p_value.screen_active;
	bars_fading_ = p_value.bars_fading;
	bars_held_ = p_value.bars_held;
	bars_alpha_ = p_value.bars_alpha;
	events_.clear();
	for (const opennova::world::CineEvent &e : p_value.events) {
		Ref<CineEventRecord> row;
		row.instantiate();
		row->assign(e);
		events_.push_back(row);
	}
}

int EpilogCineState::bar_height(int p_width, int p_height) {
	return opennova::world::epilog_bar_height(p_width, p_height);
}

void EpilogCineState::_bind_methods() {
	BIND_ENUM_CONSTANT(MODE_NONE);
	BIND_ENUM_CONSTANT(MODE_WIN);
	BIND_ENUM_CONSTANT(MODE_LOSE);
	OPENNOVA_RECORD_READ_ONLY(EpilogCineState, Variant::INT, mode)
	OPENNOVA_RECORD_READ_ONLY(EpilogCineState, Variant::INT, frame)
	OPENNOVA_RECORD_READ_ONLY(EpilogCineState, Variant::BOOL, active)
	OPENNOVA_RECORD_READ_ONLY(EpilogCineState, Variant::BOOL, screen_active)
	OPENNOVA_RECORD_READ_ONLY(EpilogCineState, Variant::BOOL, bars_fading)
	OPENNOVA_RECORD_READ_ONLY(EpilogCineState, Variant::BOOL, bars_held)
	OPENNOVA_RECORD_READ_ONLY(EpilogCineState, Variant::FLOAT, bars_alpha)
	OPENNOVA_RECORD_READ_ONLY_ROWS(EpilogCineState, events, CineEventRecord)
	ClassDB::bind_static_method("EpilogCineState", D_METHOD("bar_height", "width", "height"),
			&EpilogCineState::bar_height);
}
