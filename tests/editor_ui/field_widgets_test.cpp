// S12 D4 (ADR 0046 S12): the Inspector's field controls (editor/ui/field_widgets) over a null
// ImGui backend, drawn alone in a window. A ranged number clamps what is typed to its range as
// it is typed and shows its unit after it; a group's fields draw on one row, a Channel group
// after its swatch, each member its own control with the choices it is given (as they apply to
// its record, not its schema's); an open list takes a typed token (a known one
// as the table spells it) of the field's own type, within its range and no longer than it holds,
// a closed one narrows to what is typed and takes the one left; and a field's tooltip and its
// column's heading say what the schema says of it.
#include <cstddef>
#include <functional>
#include <string>
#include <variant>
#include <vector>

#include <editor/ui/field_widgets.h>
#include "editor_ui_test_support.h"

#include <imgui.h>
#include <imgui_internal.h>

namespace editor_ui_test {

namespace {

namespace widgets = opennova::editor::field_widgets;

// A frame of one window holding what `draw` draws, the first control 300 wide.
void widget_frame(const std::function<void()> &draw, int count = 1) {
	for (int i = 0; i < count; ++i) {
		ImGui::NewFrame();
		ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
		ImGui::SetNextWindowSize(ImVec2(640.0f, 400.0f));
		ImGui::Begin("widgets");
		ImGui::SetNextItemWidth(300.0f);
		draw();
		ImGui::End();
		ImGui::Render();
	}
}

// What a frame of it writes as text.
std::string logged_widgets(const std::function<void()> &draw) {
	ImGui::NewFrame();
	ImGui::LogToBuffer();
	ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
	ImGui::SetNextWindowSize(ImVec2(640.0f, 400.0f));
	ImGui::Begin("widgets");
	ImGui::SetNextItemWidth(300.0f);
	draw();
	ImGui::End();
	const std::string text = GImGui->LogBuffer.c_str();
	ImGui::LogFinish();
	ImGui::Render();
	return text;
}

ImGuiID widget(std::initializer_list<const char *> steps) { return item_id(Ui::window_id("widgets"), steps); }

// A control given the keyboard as Enter or Tab gives it (a drag's typed form).
void type_into(ImGuiID id, const std::function<void()> &draw) {
	ImGui::ActivateItemByID(id);
	GImGui->NavNextActivateFlags = ImGuiActivateFlags_PreferInput;
	widget_frame(draw, 2);
}

void press_enter(const std::function<void()> &draw) {
	ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, true);
	widget_frame(draw);
	ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, false);
	widget_frame(draw);
}

// Each member's own schema's choices, as group() takes them.
std::vector<const std::vector<FieldChoice> *> schema_choices(const std::vector<FieldUse> &uses) {
	std::vector<const std::vector<FieldChoice> *> out;
	for (const FieldUse &use : uses) out.push_back(&use.schema->choices);
	return out;
}

FieldSchema channel(const char *id, const char *label) {
	FieldSchema field;
	field.id = id;
	field.label = label;
	field.type = FieldType::Integer;
	field.ranged = true;
	field.min = 0.0;
	field.max = 255.0;
	field.color = FieldColor::Channel;
	field.group = "Start colour";
	return field;
}

// Two choices of one name (unit_type's two boats): each its own item, each picked (S12 D10
// review).
void test_choices_of_one_name() {
	NullBackend backend;
	std::string typed; // what an open list's box holds: the window's
	FieldSchema field;
	field.id = "unit_type";
	field.type = FieldType::Integer;
	field.choices = {{"5", 5, "Boat"}, {"6", 6, "Boat"}};
	Value value = int64_t(0);
	const auto draw = [&] { widgets::choice(field, field.choices, value, typed); };
	widget_frame(draw, 2);
	const ImGuiID list = ImHashStr("##Combo_00");
	for (const int index : {1, 0}) {
		ImGui::ActivateItemByID(widget({"##value"}));
		widget_frame(draw, 2);
		ImGui::ActivateItemByID(item_id(pushed(list, index), {"Boat"}));
		widget_frame(draw, 3);
		CHECK(std::get<int64_t>(value) == (index ? 6 : 5), "each boat is picked by its own item");
	}
}

// A byte 0..255 in pixels: 300 typed is 255 while it is typed (none past the range is set),
// the unit after the value, and the words its tooltip and its column say.
void test_ranged_number() {
	NullBackend backend;
	std::string typed; // what an open list's box holds: the window's
	FieldSchema field;
	field.id = "alpha_test";
	field.label = "Alpha-test threshold";
	field.token = "alphatest";
	field.type = FieldType::Integer;
	field.ranged = true;
	field.min = 0.0;
	field.max = 255.0;
	field.unit = "px";
	field.description = "Below it a texel is not drawn.";
	Value value = int64_t(10);
	std::vector<int64_t> set;
	bool finished = false;
	const auto draw = [&] {
		const widgets::Edited edited = widgets::number(field, value);
		if (edited.changed) set.push_back(std::get<int64_t>(value));
		finished = finished || edited.finished;
	};
	widget_frame(draw);
	CHECK(logged_widgets(draw).find("px") != std::string::npos, "the unit after the value");
	type_into(widget({"##value"}), draw);
	CHECK(ImGui::GetIO().WantTextInput, "a ranged number takes typing");
	ImGui::GetIO().AddInputCharactersUTF8("300");
	widget_frame(draw, 2);
	press_enter(draw);
	CHECK(!set.empty() && std::get<int64_t>(value) == 255, "300 typed is clamped to 255 as it is typed");
	bool past = false;
	for (const int64_t number : set) past = past || number < 0 || number > 255;
	CHECK(!past && finished, "no value past the range is ever set, and the typing ends the burst");

	const std::string tip = widgets::field_tip(field);
	CHECK(in_order(tip, {"alpha_test", "Written as alphatest", "In px", "From 0 to 255", "Below it a texel"}),
	      "the tooltip: the id, the key the file writes, the unit, the range, the note");
	CHECK(widgets::column_header(field) == "Alpha-test threshold (px)", "a column's heading: the name and the unit");
	FieldSchema bare;
	bare.id = "count";
	CHECK(widgets::field_tip(bare) == "count" && widgets::column_header(bare) == "count", "nothing made up");
}

// A Channel group: the three on one row after the swatch, each its own control (green typed
// changes green alone), the group's unit once after them.
void test_group_row() {
	NullBackend backend;
	std::string typed; // what an open list's box holds: the window's
	std::vector<FieldSchema> fields = {channel("start.r", "Start red"), channel("start.g", "Start green"),
	                                   channel("start.b", "Start blue")};
	// Each as it applies to a record, its schema's own (FieldUse).
	std::vector<FieldUse> uses;
	for (const FieldSchema &field : fields) uses.push_back(field_use(field));
	const std::vector<const std::vector<FieldChoice> *> choices = schema_choices(uses);
	std::vector<Value> values = {int64_t(10), int64_t(20), int64_t(30)};
	size_t changed = SIZE_MAX;
	float top = 0.0f, bottom = 0.0f;
	const auto draw = [&] {
		top = ImGui::GetCursorScreenPos().y;
		size_t at = SIZE_MAX;
		if (widgets::group(uses, choices, values, at, typed).changed) changed = at;
		bottom = ImGui::GetCursorScreenPos().y;
	};
	widget_frame(draw, 2);
	CHECK(bottom - top <= ImGui::GetFrameHeightWithSpacing() + 0.5f, "the group's fields draw on one row");
	type_into(widget({"start.g", "##value"}), draw);
	CHECK(ImGui::GetIO().WantTextInput, "a member takes typing");
	ImGui::GetIO().AddInputCharactersUTF8("99");
	widget_frame(draw, 2);
	press_enter(draw);
	CHECK(changed == 1 && std::get<int64_t>(values[1]) == 99 && std::get<int64_t>(values[0]) == 10 &&
	              std::get<int64_t>(values[2]) == 30,
	      "a member's own control sets it alone");

	// A position: one unit after the row, not one per member.
	std::vector<FieldSchema> position(3);
	const char *ids[] = {"position.x", "position.y", "position.z"};
	for (size_t i = 0; i < 3; ++i) {
		position[i].id = ids[i];
		position[i].type = FieldType::Real;
		position[i].unit = "m";
		position[i].group = "Position";
	}
	std::vector<FieldUse> placed;
	for (const FieldSchema &field : position) placed.push_back(field_use(field));
	const std::vector<const std::vector<FieldChoice> *> placed_choices = schema_choices(placed);
	std::vector<Value> at = {1.0, 2.0, 3.0};
	const auto place = [&] {
		size_t which = SIZE_MAX;
		widgets::group(placed, placed_choices, at, which, typed);
	};
	widget_frame(place);
	CHECK(count_of(logged_widgets(place), "m") == 1, "a unit the members share shows once");
}

// A member draws the choices it is given, as they apply to its record (Document::choices_on, a
// record's own list; S13 D2's review), not its schema's: a member whose schema lists none shows
// its value by the choice's name and takes a pick of another, its neighbour given none its number.
void test_group_choices() {
	NullBackend backend;
	std::string typed; // what an open list's box holds: the window's
	std::vector<FieldSchema> fields(2);
	fields[0].id = "spawn.team";
	fields[1].id = "spawn.count";
	for (FieldSchema &field : fields) {
		field.type = FieldType::Integer;
		field.group = "Spawn";
	}
	std::vector<FieldUse> uses;
	for (const FieldSchema &field : fields) uses.push_back(field_use(field));
	const std::vector<FieldChoice> teams = {{"BLUE", 1, "Blue team"}, {"RED", 2, "Red team"}}, none;
	const std::vector<const std::vector<FieldChoice> *> choices = {&teams, &none};
	std::vector<Value> values = {int64_t(2), int64_t(7)};
	size_t changed = SIZE_MAX;
	const auto draw = [&] {
		size_t at = SIZE_MAX;
		if (widgets::group(uses, choices, values, at, typed).changed) changed = at;
	};
	widget_frame(draw, 2);
	CHECK(logged_widgets(draw).find("Red team") != std::string::npos, "a member shows its value by its own choice's name");
	ImGui::ActivateItemByID(widget({"spawn.team", "##value"}));
	widget_frame(draw, 2);
	ImGui::ActivateItemByID(item_id(pushed(ImHashStr("##Combo_00"), 0), {"Blue team"}));
	widget_frame(draw, 3);
	CHECK(changed == 0 && std::get<int64_t>(values[0]) == 1 && std::get<int64_t>(values[1]) == 7,
	      "a member takes a pick of its own choices, alone");
}

// An open list takes a typed token: one it lacks as typed, one it knows as the table spells
// it; a closed long list narrows to what is typed and takes the one left.
void test_open_choice() {
	NullBackend backend;
	std::string typed; // what an open list's box holds: the window's
	FieldSchema field;
	field.id = "charfilter[0]";
	field.type = FieldType::Text;
	field.width = 32;
	field.choices = {{"medic", 1}, {"sniper", 2}};
	field.open_choices = true;
	Value value = std::string("medic");
	int picks = 0;
	const auto draw = [&] {
		if (widgets::choice(field, field.choices, value, typed).changed) ++picks;
	};
	widget_frame(draw);
	ImGui::ActivateItemByID(widget({"##value"}));
	widget_frame(draw, 3);
	CHECK(ImGui::GetIO().WantTextInput, "an open list's box has the keyboard");
	ImGui::GetIO().AddInputCharactersUTF8("pilot");
	widget_frame(draw);
	press_enter(draw);
	CHECK(picks == 1 && std::get<std::string>(value) == "pilot", "Enter takes the typed token, written as typed");
	ImGui::ActivateItemByID(widget({"##value"}));
	widget_frame(draw, 3);
	ImGui::GetIO().AddInputCharactersUTF8("SNIPER");
	widget_frame(draw);
	press_enter(draw);
	CHECK(picks == 2 && std::get<std::string>(value) == "sniper", "a known token typed is the table's spelling");

	FieldSchema closed;
	closed.id = "kind";
	closed.type = FieldType::Integer;
	const char *names[] = {"one", "two", "three", "four", "five", "six", "seven", "eight", "nine"};
	for (int i = 0; i < 9; ++i) closed.choices.push_back({names[i], i + 1});
	Value number = int64_t(1);
	const auto list = [&] { widgets::choice(closed, closed.choices, number, typed); };
	widget_frame(list);
	ImGui::ActivateItemByID(widget({"##value"}));
	widget_frame(list, 3);
	ImGui::GetIO().AddInputCharactersUTF8("nin");
	widget_frame(list);
	press_enter(list);
	CHECK(std::get<int64_t>(number) == 9, "a long list narrows to what is typed; Enter takes the one left");
	ImGui::ActivateItemByID(widget({"##value"}));
	widget_frame(list, 3);
	ImGui::GetIO().AddInputCharactersUTF8("ten");
	widget_frame(list);
	press_enter(list);
	CHECK(std::get<int64_t>(number) == 9, "a closed list takes no value it lacks");
}

// An open list's typed value is the field's own: a number of its type within its range (300 is
// no index of a 0..255 field, 1.5 a real one's value), a text no longer than the field holds
// (a 150-byte token into a 256-byte field whole, a long one into an 8-byte field cut to 7).
void test_typed_values() {
	NullBackend backend;
	std::string box; // what an open list's box holds: the window's
	auto typed = [&box](FieldSchema field, Value value, const char *text) {
		const auto draw = [&] { widgets::choice(field, field.choices, value, box); };
		widget_frame(draw);
		ImGui::ActivateItemByID(widget({"##value"}));
		widget_frame(draw, 3);
		ImGui::GetIO().AddInputCharactersUTF8(text);
		widget_frame(draw);
		press_enter(draw);
		ImGui::ClosePopupsExceptModals();
		widget_frame(draw);
		return value;
	};
	FieldSchema index;
	index.id = "part";
	index.type = FieldType::Integer;
	index.ranged = true;
	index.max = 255.0;
	index.open_choices = true;
	index.choices = {{"0", 0, "Part 0"}};
	CHECK(std::get<int64_t>(typed(index, int64_t(0), "300")) == 0, "an index past the field's range is not taken");
	CHECK(std::get<int64_t>(typed(index, int64_t(0), "12")) == 12, "an index within it is");
	CHECK(std::get<int64_t>(typed(index, int64_t(0), "99999999999999999999")) == 0, "past int64: not taken");
	FieldSchema real;
	real.id = "rate";
	real.type = FieldType::Real;
	real.open_choices = true;
	real.choices = {{"1", 1, "One"}};
	const Value rate = typed(real, 0.0, "1.5");
	CHECK(std::holds_alternative<double>(rate) && std::get<double>(rate) == 1.5, "a real field takes a real");
	FieldSchema small;
	small.id = "token";
	small.type = FieldType::Text;
	small.width = 8;
	small.open_choices = true;
	small.choices = {{"medic", 1}};
	CHECK(std::get<std::string>(typed(small, std::string("medic"), "abcdefghij")) == "abcdefg",
	      "a token no longer than the field holds");
	FieldSchema wide = small;
	wide.width = 256;
	const std::string long_token(150, 'x');
	CHECK(std::get<std::string>(typed(wide, std::string("medic"), long_token.c_str())) == long_token,
	      "a long token a wide field holds is taken whole");
}

// S13 V3: a text box over a value longer than its field holds (a UTF-8 text past the schema's
// width, which the format may store in fewer bytes: five e-acutes are ten bytes of UTF-8, five of
// Windows-1252) shows it whole and never cuts it (ui/text_edit): drawn, nothing is set; a Backspace
// at its end takes its last character whole (eight bytes left, no half sequence), and once it is
// no longer than the field, a typed character past the width is refused whole. A box of a field
// wide enough takes what is typed.
void test_long_value_whole() {
	NullBackend backend;
	std::string typed; // what an open list's box holds: the window's
	FieldSchema field;
	field.id = "text";
	field.type = FieldType::Text;
	field.width = 9; // eight bytes and the terminator
	const FieldUse use = field_use(field);
	const std::vector<FieldChoice> none;
	const std::string e = "\xC3\xA9"; // one e-acute, two bytes of UTF-8
	Value value = e + e + e + e + e;
	int sets = 0;
	const auto draw = [&] {
		if (widgets::value(use, none, value, false, typed).changed) ++sets;
	};
	widget_frame(draw, 3);
	CHECK(sets == 0 && std::get<std::string>(value) == e + e + e + e + e, "drawn, the value is whole and nothing is set");
	const auto press = [&](ImGuiKey key) {
		ImGui::GetIO().AddKeyEvent(key, true);
		widget_frame(draw);
		ImGui::GetIO().AddKeyEvent(key, false);
		widget_frame(draw);
	};
	type_into(widget({"##value"}), draw);
	press(ImGuiKey_End);
	press(ImGuiKey_Backspace);
	CHECK(sets == 1 && std::get<std::string>(value) == e + e + e + e,
	      "a Backspace takes the last character whole: eight bytes, no half sequence");
	ImGui::GetIO().AddInputCharactersUTF8("x");
	widget_frame(draw, 2);
	CHECK(sets == 1 && std::get<std::string>(value) == e + e + e + e, "a character past the field's width is refused");
	ImGui::ClearActiveID();
	widget_frame(draw);
	// Wide enough: typed at the end, one Set a character.
	field.width = 64;
	const FieldUse wide = field_use(field);
	const auto draw_wide = [&] {
		if (widgets::value(wide, none, value, false, typed).changed) ++sets;
	};
	type_into(widget({"##value"}), draw_wide);
	ImGui::GetIO().AddKeyEvent(ImGuiKey_End, true);
	widget_frame(draw_wide);
	ImGui::GetIO().AddKeyEvent(ImGuiKey_End, false);
	widget_frame(draw_wide);
	ImGui::GetIO().AddInputCharactersUTF8("x");
	widget_frame(draw_wide, 2);
	CHECK(std::get<std::string>(value) == e + e + e + e + "x", "a field wide enough takes it");
	ImGui::ClearActiveID();
	widget_frame(draw_wide);
}

} // namespace

void run_field_widget_tests() {
	test_choices_of_one_name();
	test_ranged_number();
	test_group_row();
	test_group_choices();
	test_open_choice();
	test_typed_values();
	test_long_value_whole();
}

} // namespace editor_ui_test
