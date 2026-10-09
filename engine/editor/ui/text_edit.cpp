#include "text_edit.h"

#include <algorithm>
#include <cfloat>
#include <cstring>
#include <utility>

#include <base/io/strutil.h>
#include <editor/model/document.h>
#include <editor/ui/editor_requests.h>

#include <imgui.h>

namespace opennova::editor::text_edit {
namespace {

// The most lines a cell shows before its box scrolls.
constexpr int kMostLines = 8;
// The most bytes of UTF-8 one character of the game's code page takes (Windows-1252 holds no
// character past U+FFFF).
constexpr size_t kCodePageUtf8 = 3;

// A box of no width: ImGui asks for more room as the text grows past the buffer, and the string
// grows with it.
int grow(ImGuiInputTextCallbackData *data) {
	if (data->EventFlag == ImGuiInputTextFlags_CallbackResize) {
		auto *text = static_cast<std::string *>(data->UserData);
		text->resize(size_t(data->BufTextLen));
		data->Buf = text->data();
	}
	return 0;
}

// The characters of a UTF-8 text: the bytes its stored form takes in the game's code page, one a
// character (a character the code page has no byte for is the document's to refuse, in its words).
size_t characters(const char *text, size_t length) {
	return strutil::utf8_length(std::string_view(text, length));
}

// A code-page box's text as the frame began (the caller's value, which the box's buffer still holds
// while ImGui edits its own) and the field's width in characters, its terminator included.
struct CodePage {
	const char *before = nullptr;
	size_t before_length = 0;
	size_t width = 0;
};

// An edit that takes a code-page box past its width (and grows it: a value already past the width
// may shrink) is refused whole: the text goes back to what the frame began with, the cursor to
// where the two part.
int code_page_edit(ImGuiInputTextCallbackData *data) {
	if (data->EventFlag != ImGuiInputTextFlags_CallbackEdit) return 0;
	const auto *page = static_cast<const CodePage *>(data->UserData);
	const size_t now = characters(data->Buf, size_t(data->BufTextLen));
	if (now < page->width || now <= characters(page->before, page->before_length)) return 0;
	size_t same = 0;
	while (same < page->before_length && same < size_t(data->BufTextLen) && data->Buf[same] == page->before[same])
		++same;
	while (same > 0 && same < page->before_length && (static_cast<unsigned char>(page->before[same]) & 0xC0) == 0x80)
		--same;
	data->DeleteChars(0, data->BufTextLen);
	data->InsertChars(0, page->before, page->before + page->before_length);
	data->CursorPos = data->SelectionStart = data->SelectionEnd = int(same);
	return 0;
}

bool draw(const char *id, char *buffer, size_t size, ImGuiInputTextFlags flags, const Box &box,
		ImGuiInputTextCallback callback, void *user) {
	// Multi-line first (as the Inspector's form had it: a hint only on one line), at the width the
	// caller set.
	if (box.multiline)
		return ImGui::InputTextMultiline(id, buffer, size, ImVec2(0.0f, box.height), flags, callback, user);
	if (box.hint) return ImGui::InputTextWithHint(id, box.hint, buffer, size, flags, callback, user);
	return ImGui::InputText(id, buffer, size, flags, callback, user);
}

} // namespace

bool edit(const char *id, std::string &text, size_t width, const Box &box) {
	ImGuiInputTextFlags flags = box.enter_returns ? ImGuiInputTextFlags_EnterReturnsTrue : 0;
	bool changed = false;
	if (!width) {
		changed = draw(id, text.data(), text.size() + 1, flags | ImGuiInputTextFlags_CallbackResize,
				box, grow, &text);
	} else if (box.code_page) {
		// Room for the width's characters at their widest in UTF-8, or the whole value where it is
		// longer; the width in characters is kept by the edit callback.
		CodePage page{nullptr, text.size(), width};
		const size_t room = std::max(text.size() + 1, kCodePageUtf8 * (width - 1) + 1);
		text.resize(room - 1, '\0');
		page.before = text.data();
		changed = draw(id, text.data(), room, flags | ImGuiInputTextFlags_CallbackEdit, box, code_page_edit,
				&page);
	} else {
		// The value's own string is the buffer: the width's bytes with the terminator, or the whole
		// value and its terminator where it is longer; ImGui refuses an insertion past it whole.
		const size_t room = std::max(width, text.size() + 1);
		text.resize(room - 1, '\0');
		changed = draw(id, text.data(), room, flags, box, nullptr, nullptr);
	}
	// What the box holds, to its terminator (the box writes as it is typed in, a box that answers
	// Enter alone included).
	text.resize(std::strlen(text.c_str()));
	return changed;
}

bool cell(Workspace &workspace, const Document &document, const NodeAddress &address,
		const FieldSchema &field, int lines) {
	Value value;
	if (!document.get(address, field.id, value)) return false;
	auto *text = std::get_if<std::string>(&value);
	if (!text) return false;
	const std::string id = "##" + field.id;
	Box box;
	box.multiline = field.multiline;
	box.code_page = field.code_page;
	box.height = ImGui::GetTextLineHeight() * float(std::clamp(lines, 1, kMostLines)) +
	             ImGui::GetStyle().FramePadding.y * 2.0f;
	ImGui::SetNextItemWidth(-FLT_MIN);
	if (edit(id.c_str(), *text, field.width, box))
		window_requests::set(workspace, document, address, field.id, std::move(*text));
	if (ImGui::IsItemDeactivatedAfterEdit()) window_requests::end_edit(workspace, document.path());
	return true;
}

} // namespace opennova::editor::text_edit
