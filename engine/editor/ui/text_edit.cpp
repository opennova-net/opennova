#include "text_edit.h"

#include <algorithm>
#include <cfloat>
#include <cstring>
#include <utility>

#include <editor/model/document.h>
#include <editor/ui/editor_requests.h>

#include <imgui.h>

namespace opennova::editor::text_edit {
namespace {

// The most lines a cell shows before its box scrolls.
constexpr int kMostLines = 8;

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

bool draw(const char *id, char *buffer, size_t size, ImGuiInputTextFlags flags, const Box &box,
		ImGuiInputTextCallback callback, void *user) {
	// Multi-line first (as the Inspector's form had it: a hint only on one line), at the width the
	// caller set.
	if (box.multiline)
		return ImGui::InputTextMultiline(id, buffer, size, ImVec2(0.0f, box.height), flags, callback, user);
	if (box.hint) return ImGui::InputTextWithHint(id, box.hint, buffer, size, flags, callback, user);
	return ImGui::InputText(id, buffer, size, flags, callback, user);
}

// The bytes a box over a text of `size` bytes holds with a field `width` wide (the terminator
// included): the width, or the text and its terminator where the text is longer; 0 for a width
// of 0 (the box grows with what is typed).
size_t capacity(size_t size, size_t width) {
	return width ? std::max(width, size + 1) : 0;
}

} // namespace

bool edit(const char *id, std::string &text, size_t width, const Box &box) {
	const ImGuiInputTextFlags flags = box.enter_returns ? ImGuiInputTextFlags_EnterReturnsTrue : 0;
	const size_t room = capacity(text.size(), width);
	bool changed = false;
	if (room) {
		// The value's own string is the buffer: `room` bytes with the terminator, the whole value
		// in them; ImGui keeps what is typed within them and refuses the rest whole.
		text.resize(room - 1, '\0');
		changed = draw(id, text.data(), room, flags, box, nullptr, nullptr);
	} else {
		changed = draw(id, text.data(), text.size() + 1, flags | ImGuiInputTextFlags_CallbackResize,
				box, grow, &text);
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
	box.height = ImGui::GetTextLineHeight() * float(std::clamp(lines, 1, kMostLines)) +
	             ImGui::GetStyle().FramePadding.y * 2.0f;
	ImGui::SetNextItemWidth(-FLT_MIN);
	if (edit(id.c_str(), *text, field.width, box))
		window_requests::set(workspace, document, address, field.id, std::move(*text));
	if (ImGui::IsItemDeactivatedAfterEdit()) window_requests::end_edit(workspace, document.path());
	return true;
}

} // namespace opennova::editor::text_edit
