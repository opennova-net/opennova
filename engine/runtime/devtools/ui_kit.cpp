#include <runtime/devtools/ui_kit.h>

#include <base/io/strutil.h>

#include <cctype>

#include <imgui.h>
#include <imgui_internal.h>

namespace opennova::devtools::ui_kit {

namespace {

// Whether a TipsAbove lives: Dear ImGui is single threaded, one frame is drawn at a time.
bool g_tips_above = false;

// What a label adds to a control's width when it is drawn on its right.
float label_width(const char *label) {
	const float text = ImGui::CalcTextSize(label, nullptr, true).x;
	return text > 0.0f ? ImGui::GetStyle().ItemInnerSpacing.x + text : 0.0f;
}

// Where without_citations took a citation out, until the blanks and the stops about it are settled.
constexpr char kCut = '\x01';

bool hex_char(char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; }
bool digit_char(char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; }
bool word_char(char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_'; }
// A word starts (ends) at `at`: no letter, digit, '_' or '-' before (at) it.
bool starts_word(const std::string &s, size_t at) { return at == 0 || (!word_char(s[at - 1]) && s[at - 1] != '-'); }
bool ends_word(const std::string &s, size_t at) { return at >= s.size() || (!word_char(s[at]) && s[at] != '-'); }

// The end of a "0x..." number at `at`, or `at` when none starts there.
size_t hex_end(const std::string &s, size_t at) {
	if (at + 2 >= s.size() || s[at] != '0' || (s[at + 1] != 'x' && s[at + 1] != 'X') || !hex_char(s[at + 2])) return at;
	size_t end = at + 2;
	while (end < s.size() && hex_char(s[end])) ++end;
	return end;
}

// The end of an address cited at `at` ("@ 0x4ae150" or "@0x4ae150", a range "..0x4ae1a0" or "-0x4ae1a0"
// or an offset "+0x174" after it), or `at` when none is.
size_t address_end(const std::string &s, size_t at) {
	if (s[at] != '@') return at;
	const size_t number = at + 1 < s.size() && s[at + 1] == ' ' ? at + 2 : at + 1;
	size_t end = hex_end(s, number);
	if (end == number) return at;
	for (;;) {
		size_t next = end;
		if (s.compare(end, 2, "..") == 0) next = end + 2;
		else if (end < s.size() && (s[end] == '-' || s[end] == '+')) next = end + 1;
		const size_t more = next == end ? end : hex_end(s, next);
		if (more == next) return end;
		end = more;
	}
}

// Whether s[from, to) names a function of the code: an identifier with a '_' or a "::" in it
// (Entity_ApplyWeaponDamage, CGameFont::DrawText, sub_58A690), never a word of the prose ("box", "Note:").
bool code_name(const std::string &s, size_t from, size_t to) {
	if (from >= to || digit_char(s[from]) || s[to - 1] == ':') return false;
	bool marked = false;
	for (size_t i = from; i < to; ++i) {
		if (!word_char(s[i]) && s[i] != ':') return false;
		marked = marked || s[i] == '_' || (s[i] == ':' && i + 1 < to && s[i + 1] == ':');
	}
	return marked;
}

// The end of a divergence's id at `at` ("D-FNT-3", "D-OCC-8b"), or `at` when none starts there.
size_t divergence_end(const std::string &s, size_t at) {
	if (!starts_word(s, at) || s.compare(at, 2, "D-") != 0) return at;
	size_t i = at + 2;
	if (i >= s.size() || !std::isupper(static_cast<unsigned char>(s[i]))) return at;
	while (i < s.size() && (std::isupper(static_cast<unsigned char>(s[i])) || digit_char(s[i]))) ++i;
	if (i + 1 >= s.size() || s[i] != '-' || !digit_char(s[i + 1])) return at;
	for (++i; i < s.size() && digit_char(s[i]);) ++i;
	if (i < s.size() && std::islower(static_cast<unsigned char>(s[i]))) ++i;
	return ends_word(s, i) ? i : at;
}

// The end of an RE record's name at `at` ("render-occlusion-re", "mis-format-re.md"), or `at`.
size_t record_end(const std::string &s, size_t at) {
	if (!starts_word(s, at)) return at;
	size_t i = at;
	while (i < s.size() && (std::islower(static_cast<unsigned char>(s[i])) || digit_char(s[i]) || s[i] == '-')) ++i;
	if (i - at < 4 || s.compare(i - 3, 3, "-re") != 0) return at;
	if (s.compare(i, 3, ".md") == 0) i += 3;
	return ends_word(s, i) ? i : at;
}

// The ']' that closes the '[' at `open` (a bracket inside it closed first), or npos.
size_t bracket_close(const std::string &s, size_t open) {
	int depth = 0;
	for (size_t i = open; i < s.size(); ++i) {
		if (s[i] == '[') ++depth;
		else if (s[i] == ']' && --depth == 0) return i;
	}
	return std::string::npos;
}

// Whether the bracket s[open, close] is a citation: "[orig: ...]", one that cites an address, or one that
// names a function alone ("[Model_SelectRlodLevel]", never a key's "[ESC_KEY]").
bool cites(const std::string &s, size_t open, size_t close) {
	if (s.compare(open + 1, 5, "orig:") == 0) return true;
	bool lower = false, upper = false;
	for (size_t i = open + 1; i < close; ++i) {
		lower = lower || std::islower(static_cast<unsigned char>(s[i])) != 0;
		upper = upper || std::isupper(static_cast<unsigned char>(s[i])) != 0;
	}
	if (lower && upper && code_name(s, open + 1, close)) return true;
	for (size_t i = s.find('@', open); i != std::string::npos && i < close; i = s.find('@', i + 1))
		if (address_end(s, i) != i) return true;
	return false;
}

// Whether the parenthesis s(from, to) holds nothing but cuts with blanks and stops, or an IDA name alone
// ("sub_58A690").
bool cut_only(const std::string &s, size_t from, size_t to) {
	while (from < to && s[from] == ' ') ++from;
	while (to > from && s[to - 1] == ' ') --to;
	static const char *const kIda[] = {"sub_", "loc_", "word_", "dword_", "qword_", "byte_", "unk_", "off_", "flt_", "dbl_", "stru_"};
	for (const char *prefix : kIda) {
		const size_t n = std::strlen(prefix);
		if (to - from > n + 3 && s.compare(from, n, prefix) == 0 &&
		    std::all_of(s.begin() + std::ptrdiff_t(from + n), s.begin() + std::ptrdiff_t(to), hex_char))
			return true;
	}
	// Cuts, divergence ids and record names, between blanks and stops: never a word of the prose (a mission's
	// "D-DAY-1.bms" outside parentheses stands).
	bool cut = false;
	for (size_t i = from; i < to;) {
		if (s[i] == ' ' || s[i] == ',' || s[i] == ';') {
			++i;
			continue;
		}
		size_t end = s[i] == kCut ? i + 1 : s[i] == 'D' ? divergence_end(s, i) : record_end(s, i);
		if (end == i || end > to) return false;
		cut = true;
		i = end;
	}
	return cut;
}

// The marked text settled: a parenthesis that holds nothing but cuts goes whole with the blank before it;
// each run of cuts with the blanks and stops about it goes where it opens or closes a line, a sentence or
// a parenthesis, and between two words leaves its first stop and a blank.
std::string settle_cuts(const std::string &s) {
	const auto in_run = [](char c) { return c == kCut || c == ' ' || c == ',' || c == ';' || c == ':'; };
	std::string out;
	out.reserve(s.size());
	size_t i = 0;
	while (i < s.size()) {
		if (s[i] == '(') {
			const size_t close = s.find(')', i);
			if (close != std::string::npos && s.find('(', i + 1) > close && cut_only(s, i + 1, close)) {
				while (!out.empty() && out.back() == ' ') out.pop_back();
				for (i = close + 1; out.empty() && i < s.size() && s[i] == ' ';) ++i;
				continue;
			}
		}
		if (!in_run(s[i])) {
			out += s[i++];
			continue;
		}
		size_t end = i;
		bool cut = false;
		char stop = 0;
		for (; end < s.size() && in_run(s[end]); ++end) {
			cut = cut || s[end] == kCut;
			if (!stop && s[end] != ' ' && s[end] != kCut) stop = s[end];
		}
		if (!cut) {
			out.append(s, i, end - i);
		} else {
			const char before = out.empty() ? '\n' : out.back();
			const char after = end < s.size() ? s[end] : '\n';
			const bool opens = before == '\n' || before == '(' || before == '[';
			const bool closes = after == '\n' || after == ')' || after == ']' || after == '.' || after == '!' || after == '?';
			if (!opens && !closes) {
				if (stop) out += stop;
				out += ' ';
			}
		}
		i = end;
	}
	return out;
}

}  // namespace

WrapRow::WrapRow() : right_(ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x) {}

void WrapRow::next(float width) {
	if (first_) {
		first_ = false;
		return;
	}
	if (ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x + width <= right_) ImGui::SameLine();
}

float button_width(const char *label) {
	return ImGui::CalcTextSize(label, nullptr, true).x + ImGui::GetStyle().FramePadding.x * 2.0f;
}

float checkbox_width(const char *label) { return ImGui::GetFrameHeight() + label_width(label); }

float field_width(float width, const char *label) { return width + label_width(label); }

float text_width(const char *text) { return ImGui::CalcTextSize(text).x; }

bool tool(WrapRow &row, const char *label, bool enabled, const std::string &tip, bool small) {
	const float width = button_width(label);
	row.next(width);
	// A label wider than the whole of its line (a row wraps whole controls, so one wider than a line
	// would run past the window) is cut to the line, the button's id the whole label's (###).
	const float room = ImGui::GetContentRegionAvail().x;
	const std::string shown = width > room ? fit(label, room - ImGui::GetStyle().FramePadding.x * 2.0f) + "###" + label
	                                       : std::string(label);
	ImGui::BeginDisabled(!enabled);
	const bool pressed = small ? ImGui::SmallButton(shown.c_str()) : ImGui::Button(shown.c_str());
	// Held back by an enclosing BeginDisabled too (a view the busy gate holds back, S13 A3), however
	// the button was activated.
	const bool held = (ImGui::GetItemFlags() & ImGuiItemFlags_Disabled) != 0;
	ImGui::EndDisabled();
	tooltip(tip);
	return pressed && enabled && !held;
}

bool check(WrapRow &row, const char *label, bool *value, const std::string &tip) {
	row.next(checkbox_width(label));
	const bool changed = ImGui::Checkbox(label, value);
	tooltip(tip);
	return changed;
}

PathField path_field(const char *label, char *buffer, size_t size, const char *browse, float width, bool enabled) {
	PathField out;
	WrapRow row;
	const float field = width > 0.0f ? width : ImGui::CalcItemWidth();
	row.next(field_width(field, label));
	ImGui::SetNextItemWidth(field);
	out.edited = ImGui::InputText(label, buffer, size);
	out.typing = ImGui::IsItemActive();
	row.next(button_width(browse));
	ImGui::BeginDisabled(!enabled);
	out.browse = ImGui::Button(browse) && enabled;
	ImGui::EndDisabled();
	return out;
}

RowTool row_tools(WrapRow &row, const RowTools &t) {
	const bool chosen = t.selected < t.count;
	const bool full = t.max != 0 && t.count >= t.max;
	const std::string at_most = "It holds " + std::to_string(t.max) + " at most.";
	// Why a tool of the selected one cannot act, the first that holds; "" when it can.
	const auto held = [&](bool also, const std::string &because) -> std::string {
		if (!chosen) return t.pick;
		if (t.locked) return t.locked;
		return also ? because : std::string();
	};
	RowTool pressed = RowTool::None;
	if (t.add && tool(row, t.add, !full, full ? at_most : t.add_tip, t.small)) pressed = RowTool::Add;
	if (t.duplicate) {
		const std::string why = held(full, at_most);
		if (tool(row, t.duplicate, why.empty(), why.empty() ? t.duplicate_tip : why, t.small)) pressed = RowTool::Duplicate;
	}
	if (t.remove) {
		const std::string why = held(t.keeps != nullptr, t.keeps ? t.keeps : "");
		if (tool(row, t.remove, why.empty(), why.empty() ? t.remove_tip : why, t.small)) pressed = RowTool::Remove;
	}
	if (t.up) {
		const std::string why = held(t.selected == 0, "It is the first.");
		if (tool(row, t.up, why.empty(), why.empty() ? "Moves the selected one up." : why, t.small)) pressed = RowTool::Up;
	}
	if (t.down) {
		const std::string why = held(t.selected + 1 >= t.count, "It is the last.");
		if (tool(row, t.down, why.empty(), why.empty() ? "Moves the selected one down." : why, t.small))
			pressed = RowTool::Down;
	}
	return pressed;
}

bool filter_box(const char *id, char *text, size_t size, const char *hint, float width, const char *tip, bool ctrl_f) {
	const float clear = ImGui::GetFrameHeight();
	const float gap = ImGui::GetStyle().ItemInnerSpacing.x;
	const float total = width > 0.0f ? width : ImGui::GetContentRegionAvail().x;
	// Ctrl+F reaches the filter of the window that has the focus (the shortcut's route).
	if (ctrl_f && ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_F)) ImGui::SetKeyboardFocusHere();
	ImGui::SetNextItemWidth(std::max(1.0f, total - clear - gap));
	bool changed = ImGui::InputTextWithHint(id, hint, text, size);
	if (tip) tooltip(tip);
	ImGui::SameLine(0.0f, gap);
	ImGui::PushID(id);
	const bool empty = text[0] == '\0';
	ImGui::BeginDisabled(empty);
	const ImVec2 at = ImGui::GetCursorScreenPos();
	if (ImGui::Button("##clear", ImVec2(clear, clear)) && !empty) {
		text[0] = '\0';
		changed = true;
	}
	if (ImGui::IsItemVisible()) {
		const float pad = clear * 0.3f;
		ImDrawList *draw = ImGui::GetWindowDrawList();
		const ImU32 color = ImGui::GetColorU32(ImGuiCol_Text);
		draw->AddLine(ImVec2(at.x + pad, at.y + pad), ImVec2(at.x + clear - pad, at.y + clear - pad), color, 1.5f);
		draw->AddLine(ImVec2(at.x + clear - pad, at.y + pad), ImVec2(at.x + pad, at.y + clear - pad), color, 1.5f);
	}
	ImGui::EndDisabled();
	tooltip(empty ? std::string() : "Clear the filter.");
	ImGui::PopID();
	return changed;
}

std::string fit(const std::string &text, float width) {
	const size_t end = text.find('\n');
	const std::string line = text.substr(0, end);
	if (end == std::string::npos && ImGui::CalcTextSize(line.c_str()).x <= width) return line;
	const char *const begin = line.c_str();
	const char *stop = begin;
	const float dots = ImGui::CalcTextSize("...").x;
	// Narrower than the ellipsis: what fits of the text, without one.
	const float room = width < dots ? std::max(0.0f, width) : width - dots;
	ImGui::GetFont()->CalcTextSizeA(ImGui::GetFontSize(), room, 0.0f, begin, begin + line.size(), &stop);
	return std::string(begin, stop) + (width < dots ? "" : "...");
}

std::string fit_middle(const std::string &text, float width) {
	if (ImGui::CalcTextSize(text.c_str()).x <= width) return text;
	const float dots = ImGui::CalcTextSize("...").x;
	if (width <= dots) return fit(text, width);
	// The end takes three fifths of the room, the start the rest; each cut at a whole character.
	const float room = width - dots;
	size_t tail = text.size();
	while (tail > 0) {
		const size_t back = strutil::utf8_cut(text, tail - 1);
		if (ImGui::CalcTextSize(text.c_str() + back, text.c_str() + text.size()).x > room * 0.6f) break;
		tail = back;
	}
	const float tail_width = ImGui::CalcTextSize(text.c_str() + tail, text.c_str() + text.size()).x;
	const char *const begin = text.c_str();
	const char *stop = begin;
	ImGui::GetFont()->CalcTextSizeA(ImGui::GetFontSize(), room - tail_width, 0.0f, begin, begin + tail, &stop);
	return std::string(begin, stop) + "..." + text.substr(tail);
}

bool fitted_button(const std::string &label, const char *id, float width) {
	const std::string shown = fit(label, width - ImGui::GetStyle().FramePadding.x * 2.0f);
	return ImGui::Button((shown + "###" + id).c_str());
}

void clipped_text(const std::string &text, const std::string &tip) {
	const std::string shown = fit(text, ImGui::GetContentRegionAvail().x);
	ImGui::TextUnformatted(shown.c_str());
	tooltip(!tip.empty() ? tip : shown != text ? text : std::string());
}

bool tooltip_hovered() { return ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled); }

TipsAbove::TipsAbove() : before_(g_tips_above) { g_tips_above = true; }

TipsAbove::~TipsAbove() { g_tips_above = before_; }

std::string without_citations(const std::string &text) {
	if (text.find_first_of("@[(") == std::string::npos) return text;
	// Each citation marked by a cut: a citing bracket whole, an address with the function it names; an unclosed
	// "[orig:" stops the marking, the rest kept as it is. A parenthesis of divergence ids, record names and cuts
	// alone goes as the cuts settle.
	std::string marked;
	marked.reserve(text.size());
	bool cut = false;
	size_t i = 0;
	while (i < text.size()) {
		const char c = text[i];
		size_t end = i;
		if (c == '[') {
			const size_t close = bracket_close(text, i);
			if (close == std::string::npos && text.compare(i + 1, 5, "orig:") == 0) break;
			if (close != std::string::npos && cites(text, i, close)) end = close + 1;
		} else if (c == '@') {
			end = address_end(text, i);
			if (end != i) {
				// The function it cites before it, with the blank between them.
				size_t name = marked.size();
				while (name > 0 && marked[name - 1] == ' ') --name;
				size_t from = name;
				while (from > 0 && (word_char(marked[from - 1]) || marked[from - 1] == ':')) --from;
				marked.resize(code_name(marked, from, name) ? from : name);
			}
		}
		if (end == i) {
			marked += c;
			++i;
			continue;
		}
		marked += kCut;
		cut = true;
		i = end;
	}
	marked.append(text, i, std::string::npos);
	return cut || marked.find('(') != std::string::npos ? settle_cuts(marked) : text;
}

void tooltip(const std::string &cited) {
	if (cited.empty() || !tooltip_hovered()) return;
	const std::string text = without_citations(cited);
	if (text.empty()) return;
	const float wrap = ImGui::GetFontSize() * 40.0f;
	if (g_tips_above) {
		// Above the item, its bottom edge on the item's top edge (a hair clear of it) and its left on
		// the item's, held within the window's viewport; where the room above the item does not hold
		// it, Dear ImGui places it by the pointer as it places any tooltip.
		const ImGuiStyle &style = ImGui::GetStyle();
		const ImVec2 size = ImGui::CalcTextSize(text.c_str(), nullptr, false, wrap);
		const float width = size.x + style.WindowPadding.x * 2.0f, height = size.y + style.WindowPadding.y * 2.0f;
		const ImGuiViewport *viewport = ImGui::GetWindowViewport();
		const ImVec2 item = ImGui::GetItemRectMin();
		const float bottom = item.y - 2.0f;
		if (bottom - height >= viewport->Pos.y) {
			const float left = std::min(item.x, viewport->Pos.x + viewport->Size.x - width);
			ImGui::SetNextWindowPos(ImVec2(std::max(left, viewport->Pos.x), bottom), ImGuiCond_Always, ImVec2(0.0f, 1.0f));
		}
	}
	// In place of a tooltip set before it this frame, as SetTooltip does: a table's header sets
	// its own for a label it cut, which BeginTooltip would add this one to.
	ImGui::BeginTooltipEx(ImGuiTooltipFlags_OverridePrevious, ImGuiWindowFlags_None);
	ImGui::PushTextWrapPos(wrap);
	ImGui::TextUnformatted(text.c_str());
	ImGui::PopTextWrapPos();
	ImGui::EndTooltip();
}

void empty_state(const char *text, const char *hint) {
	const std::string line = text;
	const std::string shown = fit(line, ImGui::GetContentRegionAvail().x);
	ImGui::TextDisabled("%s", shown.c_str());
	tooltip(shown == line ? std::string(hint ? hint : "") : hint ? line + "\n" + hint : line);
}

void quiet_text(const std::string &text, const std::string &tip) {
	ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
	clipped_text(text, tip);
	ImGui::PopStyleColor();
}

void help_mark(const std::string &tip) {
	ImGui::SameLine();
	ImGui::TextDisabled("(?)");
	tooltip(tip);
}

bool nothing_matches(char *filter, const char *what) {
	const std::string said = std::string("No ") + what + " matches \"" + filter + "\".";
	const ImGuiStyle &style = ImGui::GetStyle();
	const float clear = ImGui::CalcTextSize("Clear").x + style.FramePadding.x * 2.0f + style.ItemSpacing.x;
	const std::string shown = fit(said, ImGui::GetContentRegionAvail().x - clear);
	ImGui::TextDisabled("%s", shown.c_str());
	tooltip(shown != said ? said : std::string());
	ImGui::SameLine();
	const bool cleared = ImGui::SmallButton("Clear##nothing");
	tooltip("Clear the filter.");
	if (cleared) filter[0] = '\0';
	return cleared;
}

bool fitted_selectable(const std::string &line, const char *id, bool selected) {
	const std::string shown = fit(line, ImGui::GetContentRegionAvail().x);
	const bool pressed = ImGui::Selectable((shown + "###" + id).c_str(), selected);
	if (shown != line) tooltip(line);
	return pressed;
}

bool tool_popup(WrapRow &row, const char *label, const char *popup, bool enabled, const std::string &tip, bool small) {
	if (tool(row, label, enabled, tip, small)) ImGui::OpenPopup(popup);
	return ImGui::BeginPopup(popup);
}

}  // namespace opennova::devtools::ui_kit
