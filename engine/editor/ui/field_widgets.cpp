#include "field_widgets.h"

#include <base/io/strutil.h>
#include <editor/ui/editor_requests.h>
#include <editor/model/field_text.h>
#include <editor/ui/inspector_layout.h>
#include <editor/ui/ui_kit.h>
#include <formats/mns/mns.h>
#include <formats/mnu/mnu_layout.h>

#include <algorithm>
#include <cfloat>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <system_error>
#include <vector>

#include <imgui.h>

namespace opennova::editor::field_widgets {
namespace {

// A list longer than this gets a box that narrows it.
constexpr size_t kFilterFrom = 8;

// A range's end as the field's control writes it.
std::string number_text(const FieldSchema &field, double number) {
	char text[32];
	if (field.type == FieldType::Real) std::snprintf(text, sizeof(text), "%.9g", number);
	else std::snprintf(text, sizeof(text), "%.0f", number);
	return text;
}

// The field's id, then what the schema says of it beyond its name.
std::string details(const FieldSchema &field) {
	std::string tip = field.id;
	if (!field.token.empty() && field.token != field.id) tip += "\nWritten as " + field.token;
	if (!field.unit.empty()) tip += "\nIn " + field.unit;
	if (field.ranged) tip += "\nFrom " + number_text(field, field.min) + " to " + number_text(field, field.max);
	if (!field.description.empty()) tip += "\n" + field.description;
	return tip;
}

unsigned channel(float f) { return unsigned(std::clamp(f, 0.0f, 1.0f) * 255.0f + 0.5f); }

int64_t whole(const Value &value) {
	if (const auto *number = std::get_if<int64_t>(&value)) return *number;
	if (const auto *real = std::get_if<double>(&value)) return int64_t(*real);
	return 0;
}

// Whether a colour field shows its swatch for this value (a HexArgb one names no %VAR%).
bool shows_swatch(const FieldSchema &field, const Value &value) {
	if (field.color == FieldColor::HexArgb) {
		const auto *text = std::get_if<std::string>(&value);
		return text && !mns::is_variable_reference(*text);
	}
	return field.color == FieldColor::PackedRgb && std::holds_alternative<int64_t>(value);
}

// A text field's box: several lines where the field runs over them (not in a table cell),
// a hint where the targets differ.
Edited text(const FieldSchema &field, Value &value, bool compact, bool mixed) {
	Edited out;
	const std::string current = mixed ? std::string() : std::get<std::string>(value);
	std::vector<char> buffer(std::max<size_t>(field.width, 2), 0);
	std::memcpy(buffer.data(), current.data(), std::min(current.size(), buffer.size() - 1));
	const bool changed =
	        field.multiline && !compact
	                ? ImGui::InputTextMultiline("##value", buffer.data(), buffer.size(),
	                                            ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 4.0f +
	                                                                     ImGui::GetStyle().FramePadding.y * 2.0f))
	        : mixed ? ImGui::InputTextWithHint("##value", "(mixed)", buffer.data(), buffer.size())
	                : ImGui::InputText("##value", buffer.data(), buffer.size());
	if (changed) {
		value = std::string(buffer.data());
		out.changed = true;
	}
	out.finished = ImGui::IsItemDeactivatedAfterEdit();
	return out;
}

// What a pick of `choice` sets: a text field's token, a number's value.
Value choice_value(const FieldSchema &field, const FieldChoice &choice) {
	return field.type == FieldType::Text ? Value(choice.name) : Value(choice.value);
}

// The range a number field keeps to: its own where it is ranged, else what its type holds
// (a byte, an unsigned word, a count); false for none.
bool number_bounds(const FieldSchema &field, double &lo, double &hi) {
	if (field.ranged) {
		lo = field.min;
		hi = field.max;
		return true;
	}
	switch (field.type) {
	case FieldType::Byte: lo = 0.0, hi = 255.0; return true;
	case FieldType::Unsigned: lo = 0.0, hi = 4294967295.0; return true;
	case FieldType::Count: lo = 0.0, hi = 9.0e18; return true;
	default: return false;
	}
}

// A typed token as the field takes it: a known one as the table spells it, else a text as
// typed (the box holds no more than the field does), a number of the field's own type when
// the whole of it reads as one within the field's range (false otherwise).
bool typed_value(const FieldSchema &field, const std::string &token, Value &out) {
	for (const FieldChoice &choice : field.choices)
		if (strutil::iequals(token, choice.name)) {
			out = choice_value(field, choice);
			return true;
		}
	if (field.type == FieldType::Text) {
		out = token;
		return true;
	}
	const char *first = token.data(), *last = token.data() + token.size();
	double lo = 0.0, hi = 0.0;
	const bool bounded = number_bounds(field, lo, hi);
	if (field.type == FieldType::Real) {
		double number = 0.0;
		const auto read = std::from_chars(first, last, number);
		if (token.empty() || read.ec != std::errc() || read.ptr != last || !std::isfinite(number)) return false;
		if (bounded && (number < lo || number > hi)) return false;
		out = number;
		return true;
	}
	int64_t number = 0;
	const auto read = std::from_chars(first, last, number); // out of int64's range: result_out_of_range
	if (token.empty() || read.ec != std::errc() || read.ptr != last) return false;
	if (bounded && (double(number) < lo || double(number) > hi)) return false;
	out = number;
	return true;
}

// What the typed box holds: a text field's capacity (its terminator included), a number's
// digits.
size_t typed_capacity(const FieldSchema &field) {
	return field.type == FieldType::Text && field.width > 1 ? field.width : 64;
}

} // namespace

std::string field_tip(const FieldSchema &field) { return details(field); }

std::string column_header(const FieldSchema &field) {
	return field_title(field) + (field.unit.empty() ? std::string() : " (" + field.unit + ")");
}

Edited number(const FieldSchema &field, Value &value, bool unit) {
	Edited out;
	const ImGuiStyle &style = ImGui::GetStyle();
	const bool after = unit && !field.unit.empty();
	const float full = ImGui::CalcItemWidth();
	ImGui::SetNextItemWidth(std::max(1.0f, full - (after ? style.ItemInnerSpacing.x + ui_kit::text_width(field.unit.c_str()) : 0.0f)));
	if (field.type == FieldType::Real) {
		double number = std::holds_alternative<double>(value) ? std::get<double>(value) : double(whole(value));
		if (field.ranged) {
			const double lo = field.min, hi = field.max;
			const float speed = field.step > 0.0 ? float(field.step) : float((hi - lo) / 1000.0);
			out.changed = ImGui::DragScalar("##value", ImGuiDataType_Double, &number, speed, &lo, &hi, "%.9g",
			                                ImGuiSliderFlags_AlwaysClamp);
		} else {
			out.changed = ImGui::InputDouble("##value", &number, 0, 0, "%.9g");
		}
		if (out.changed) value = number;
	} else {
		int64_t number = whole(value);
		if (field.ranged) {
			const int64_t lo = int64_t(field.min), hi = int64_t(field.max);
			const float speed = field.step > 0.0 ? float(field.step) : 1.0f;
			out.changed = ImGui::DragScalar("##value", ImGuiDataType_S64, &number, speed, &lo, &hi, nullptr,
			                                ImGuiSliderFlags_AlwaysClamp);
		} else {
			out.changed = ImGui::InputScalar("##value", ImGuiDataType_S64, &number);
		}
		if (out.changed) value = number;
	}
	out.finished = ImGui::IsItemDeactivatedAfterEdit();
	if (after) {
		ImGui::SameLine(0.0f, style.ItemInnerSpacing.x);
		ImGui::TextUnformatted(field.unit.c_str());
	}
	return out;
}

Edited choice(const FieldSchema &field, Value &value, bool mixed) {
	Edited out;
	out.coalesce = false;
	const FieldChoice *current = mixed ? nullptr : choice_of(field, value);
	const std::string shown = mixed ? std::string("(mixed)") : current ? choice_title(*current) : shown_value(field, value);
	if (ImGui::BeginCombo("##value", shown.c_str(), ImGuiComboFlags_HeightLarge)) {
		// One list is open at a time: the box starts empty each time one opens, as long as the
		// field it types for holds.
		static std::vector<char> typed;
		const bool narrowed = field.open_choices || field.choices.size() > kFilterFrom;
		bool enter = false;
		if (ImGui::IsWindowAppearing() || typed.size() != typed_capacity(field)) {
			typed.assign(typed_capacity(field), '\0');
			if (narrowed) ImGui::SetKeyboardFocusHere();
		}
		if (narrowed) {
			ImGui::SetNextItemWidth(-FLT_MIN);
			enter = ImGui::InputTextWithHint("##typed", field.open_choices ? "Filter, or type a value" : "Filter",
			                                 typed.data(), typed.size(), ImGuiInputTextFlags_EnterReturnsTrue);
		}
		const std::string token = typed.data();
		auto take = [&](Value picked) {
			value = std::move(picked);
			out.changed = true;
			ImGui::CloseCurrentPopup();
		};
		std::vector<const FieldChoice *> listed;
		for (const FieldChoice &option : field.choices)
			if (token.empty() || window_requests::matches(choice_title(option), token.c_str()) ||
			    window_requests::matches(option.name, token.c_str()))
				listed.push_back(&option);
		// An open field's typed value, the first line while no choice is it.
		const bool known = std::any_of(field.choices.begin(), field.choices.end(),
		                               [&](const FieldChoice &option) { return strutil::iequals(token, option.name); });
		Value typed_as;
		const bool typable = field.open_choices && !token.empty() && typed_value(field, token, typed_as);
		if (typable && !known) {
			if (ImGui::Selectable(("Use \"" + token + "\"").c_str()) || enter) take(typed_as);
			ui_kit::tooltip("Written as typed: the file takes a value the list does not know.");
		} else if (enter && (typable || listed.size() == 1)) {
			take(typable ? typed_as : choice_value(field, *listed.front()));
		}
		// Each choice under its place in the field's list: two of one name (two kinds of boat) are
		// two items.
		for (const FieldChoice *option : listed) {
			ImGui::PushID(static_cast<int>(option - field.choices.data()));
			if (ImGui::Selectable(choice_title(*option).c_str(), current == option) && !out.changed)
				take(choice_value(field, *option));
			if (!option->label.empty())
				ui_kit::tooltip(option->name.empty() ? "(not written)" : option->name);
			ImGui::PopID();
		}
		if (listed.empty() && !(typable && !known)) ImGui::TextDisabled("Nothing matches.");
		ImGui::EndCombo();
	}
	if (current && !current->label.empty()) ui_kit::tooltip(current->name.empty() ? "(not written)" : current->name);
	return out;
}

float swatch_width() { return ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x; }

Edited swatch(const FieldSchema &field, Value &value) {
	Edited out;
	if (!shows_swatch(field, value)) return out;
	if (field.color == FieldColor::HexArgb) {
		const uint32_t word = mnu::color_value(std::get<std::string>(value));
		float rgba[4] = {float((word >> 16) & 0xFF) / 255.0f, float((word >> 8) & 0xFF) / 255.0f,
		                 float(word & 0xFF) / 255.0f, float((word >> 24) & 0xFF) / 255.0f};
		if (ImGui::ColorEdit4("##swatch", rgba,
		                      ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_AlphaPreviewHalf)) {
			char text[16];
			std::snprintf(text, sizeof(text), "%02X%02X%02X%02X", channel(rgba[3]), channel(rgba[0]), channel(rgba[1]),
			              channel(rgba[2]));
			value = std::string(text);
			out.changed = true;
		}
	} else {
		const int64_t word = std::get<int64_t>(value);
		float rgb[3] = {float((word >> 16) & 0xFF) / 255.0f, float((word >> 8) & 0xFF) / 255.0f, float(word & 0xFF) / 255.0f};
		if (ImGui::ColorEdit3("##swatch", rgb, ImGuiColorEditFlags_NoInputs)) {
			value = int64_t((channel(rgb[0]) << 16) | (channel(rgb[1]) << 8) | channel(rgb[2]));
			out.changed = true;
		}
	}
	out.finished = ImGui::IsItemDeactivatedAfterEdit();
	return out;
}

Edited channel_swatch(std::vector<Value> &values) {
	Edited out;
	if (values.size() != 3) return out;
	float rgb[3];
	for (size_t i = 0; i < 3; ++i) rgb[i] = float(std::clamp<int64_t>(whole(values[i]), 0, 255)) / 255.0f;
	if (ImGui::ColorEdit3("##swatch", rgb, ImGuiColorEditFlags_NoInputs)) {
		for (size_t i = 0; i < 3; ++i) values[i] = int64_t(channel(rgb[i]));
		out.changed = true;
	}
	out.finished = ImGui::IsItemDeactivatedAfterEdit();
	return out;
}

Edited value(const FieldSchema &field, Value &value, bool compact, bool mixed) {
	if (is_yes_no(field)) {
		Edited out;
		out.coalesce = false;
		bool on = std::get<int64_t>(value) != 0;
		if (ImGui::Checkbox("##value", &on)) {
			value = int64_t(on ? 1 : 0);
			out.changed = true;
		}
		return out;
	}
	if (!field.choices.empty()) return choice(field, value, mixed);
	// A colour's swatch first, its value in what is left of the width.
	float width = ImGui::CalcItemWidth();
	Edited picked;
	if (shows_swatch(field, value)) {
		picked = swatch(field, value);
		ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
		width -= swatch_width();
	}
	ImGui::SetNextItemWidth(std::max(1.0f, width));
	const Edited typed = field.type == FieldType::Text ? text(field, value, compact, mixed) : number(field, value, !compact);
	return picked.changed || picked.finished ? picked : typed;
}

Edited group(const std::vector<FieldSchema> &fields, std::vector<Value> &values, size_t &changed,
             const std::vector<bool> &mixed) {
	Edited out;
	changed = SIZE_MAX;
	if (fields.empty() || values.size() != fields.size()) return out;
	const ImGuiStyle &style = ImGui::GetStyle();
	float width = ImGui::CalcItemWidth();
	const bool channels = fields.size() == 3 && std::all_of(fields.begin(), fields.end(), [](const FieldSchema &field) {
		                      return field.color == FieldColor::Channel;
	                      });
	if (channels) {
		ImGui::BeginDisabled(std::any_of(fields.begin(), fields.end(), [](const FieldSchema &f) { return f.read_only; }));
		out = channel_swatch(values);
		ImGui::EndDisabled();
		ImGui::SameLine(0.0f, style.ItemInnerSpacing.x);
		width -= swatch_width();
	}
	// One unit after the row when every member has it.
	const std::string &unit = fields.front().unit;
	const bool one_unit = !unit.empty() && std::all_of(fields.begin(), fields.end(), [&](const FieldSchema &field) {
		return field.unit == unit;
	});
	if (one_unit) width -= style.ItemInnerSpacing.x + ui_kit::text_width(unit.c_str());
	const float count = float(fields.size());
	const float cell = std::max(1.0f, (width - style.ItemInnerSpacing.x * (count - 1.0f)) / count);
	for (size_t i = 0; i < fields.size(); ++i) {
		if (i) ImGui::SameLine(0.0f, style.ItemInnerSpacing.x);
		ImGui::PushID(fields[i].id.c_str());
		ImGui::BeginDisabled(fields[i].read_only);
		ImGui::SetNextItemWidth(cell);
		const Edited one = value(fields[i], values[i], true, i < mixed.size() && mixed[i]);
		ui_kit::tooltip_lazy([&] { return field_title(fields[i]) + "\n" + details(fields[i]); });
		ImGui::EndDisabled();
		ImGui::PopID();
		if (one.changed) {
			out = one;
			changed = i;
		} else if (one.finished && !out.changed) {
			out.finished = true;
		}
	}
	if (one_unit) {
		ImGui::SameLine(0.0f, style.ItemInnerSpacing.x);
		ImGui::TextUnformatted(unit.c_str());
	}
	return out;
}

} // namespace opennova::editor::field_widgets
