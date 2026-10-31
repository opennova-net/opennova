#include <runtime/hud/cine_text.h>

#include <runtime/hud/end_round_statistics.h> // epilog_counter_value_text
#include <runtime/hud/feed_format.h> // sprintf_first_s
#include <runtime/hud/hud_frame.h> // wrapped_text_layout
#include <runtime/hud/hud_math.h>

#include <vector>

namespace opennova::hud {

namespace {

// A coordinate of the cine's 1024 x 768 space over the viewport, in the x87's single precision
// and truncated, as each draw computes and ftol's it: the point over its design size times the
// viewport's extent, plus its origin.
float over_width(int32_t v, const CineViewport &view) {
	return static_cast<float>(v) * (1.0f / 1024.0f) * static_cast<float>(view.right - view.left) +
			static_cast<float>(view.left);
}

float over_height(int32_t v, const CineViewport &view) {
	return static_cast<float>(v) * (1.0f / 768.0f) * static_cast<float>(view.bottom - view.top) +
			static_cast<float>(view.top);
}

void append(GameFontRun &into, const GameFontRun &run) {
	into.quads.insert(into.quads.end(), run.quads.begin(), run.quads.end());
	into.underlines.insert(into.underlines.end(), run.underlines.begin(), run.underlines.end());
}

} // namespace

GameFontRun cine_text_fade_run(const GameFont &font, float scale, const std::string &text,
		int32_t x, int32_t y, int32_t box_width, int32_t box_height, uint32_t color,
		const CineViewport &view) {
	GameFontRun out;
	if (font.font() == nullptr || text.empty()) return out;
	// [orig: @0x573041..0x573093, the sums ftol'd @0x5730e2..0x5730f8 / @0x573151..0x573167]
	const float left = over_width(x, view);
	const float top = over_height(y, view);
	const float w = static_cast<float>(box_width) * (1.0f / 1024.0f) *
			static_cast<float>(view.right - view.left);
	const float h = static_cast<float>(box_height) * (1.0f / 768.0f) *
			static_cast<float>(view.bottom - view.top);
	const int x_start = static_cast<int>(left);
	const int y_start = static_cast<int>(top);
	const int x_end = static_cast<int>(left + w);
	const int y_limit = static_cast<int>(top + h);
	std::vector<HudWrappedLine> lines;
	wrapped_text_layout(font, scale, scale, text, x_start, y_start, x_end, y_limit, 0, lines);
	// Mode 4: each line centred on the box's middle; the colour halved keeping its alpha
	// [orig: HUD_DrawWrappedText @0x580dd8..0x580def, @0x580c99..0x580ca8].
	const float centre = static_cast<float>(x_start + ((x_end - x_start) >> 1));
	const uint32_t argb = half_bright_keep_alpha(color);
	GameFontState state;
	state.color = argb;
	state.original_color = argb;
	for (const HudWrappedLine &line : lines) {
		const std::string run = text.substr(line.begin, line.end - line.begin);
		append(out, font.layout(run.c_str(), centre, static_cast<float>(line.y), scale, scale,
				kFontAlignCenter, argb, &state));
	}
	return out;
}

GameFontRun cine_epilog_counter_run(const GameFont &font, float scale, const std::string &label,
		const std::string &value, const std::string &count, int32_t label_x, int32_t value_x,
		int32_t count_x, int32_t row_y, const CineViewport &view) {
	GameFontRun out;
	if (font.font() == nullptr) return out;
	// [orig: @0x573460..0x5734d3; HUD_DrawTextLeft_HalfBright's colour of -1 @0x5804D8..0x5804F0]
	const float y = static_cast<float>(static_cast<int>(over_height(row_y, view)));
	const uint32_t argb = half_bright_argb(0xFFFFFFFFu);
	const auto column = [&](int32_t v) {
		return static_cast<float>(static_cast<int>(over_width(v, view)));
	};
	if (!label.empty())
		append(out, font.layout(label.c_str(), column(label_x), y, scale, scale, 0u, argb));
	if (!value.empty())
		append(out, font.layout(value.c_str(), column(value_x), y, scale, scale, kFontAlignRight,
				argb));
	if (!count.empty())
		append(out, font.layout(count.c_str(), column(count_x), y, scale, scale, kFontAlignRight,
				argb));
	return out;
}

std::string cine_epilog_count_text(int32_t points_target, int32_t points_rate, int32_t points) {
	if (points_target == -1 && points_rate == -1) return std::string();
	return std::to_string(points);
}

std::string cine_event_line(const world::CineEvent &e, const std::string &game_text,
		const std::string &banner) {
	switch (e.text_source) {
	case world::CineTextSource::Banner:
		return banner;
	case world::CineTextSource::GameTextFormat:
		return sprintf_first_s(game_text, e.text);
	case world::CineTextSource::Text:
		return e.text;
	case world::CineTextSource::GameText:
	default:
		return game_text;
	}
}

GameFontRun cine_event_text_run(const world::CineEvent &e, const std::string &game_text,
		const std::string &banner, const GameFont &font, float scale, const CineViewport &view) {
	switch (e.kind) {
	case world::CineEventKind::TextFade: {
		// The y the build's cursor gave, moved by a stored banner (world::CineEvent::banner_y).
		const int32_t y = e.banner_y >= 0 && !banner.empty() ? e.banner_y : e.y;
		return cine_text_fade_run(font, scale, cine_event_line(e, game_text, banner), e.x, y,
				e.box_width, e.box_height, world::cine_text_fade_argb(e), view);
	}
	case world::CineEventKind::EpilogCounter:
		return cine_epilog_counter_run(font, scale, game_text,
				epilog_counter_value_text(e.value, e.max),
				cine_epilog_count_text(e.points_target, e.points_rate, e.points), e.label_x,
				e.value_x, e.count_x, e.row_y, view);
	default:
		return GameFontRun{};
	}
}

int32_t cine_event_font(const world::CineEvent &e) {
	return e.kind == world::CineEventKind::TextFade ? e.font : -2;
}

const char *cine_font_file(const HudLabelFontChoice &choice, int32_t font) {
	switch (font) {
	case -3:
		return choice.impact38_fnt;
	case -2:
		return choice.large_fnt;
	case -1:
		return choice.bold_fnt;
	default:
		return "";
	}
}

float cine_font_scale(const HudLabelFontChoice &choice, int32_t font) {
	return font == -1 ? choice.scale : choice.large_scale;
}

} // namespace opennova::hud
