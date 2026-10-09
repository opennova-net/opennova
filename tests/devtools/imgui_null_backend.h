#pragma once
// A headless Dear ImGui frame for the ImGui ctests (the dev tools' windows, and the
// editor's on the editor trunk, PR #665): the null example's setup (a display size
// and a built font atlas, no platform or renderer backend), and the allocator hooks
// a pass's attach_imgui hand-off takes. It lives here because ImGui headers stay
// under engine/runtime/devtools/ and tests/devtools/ (ADR 0042 d6).
#include <imgui.h>

#include <cstddef>
#include <cstdlib>

namespace imgui_test {

struct NullBackend {
	ImGuiContext *context = nullptr;

	NullBackend() {
		context = ImGui::CreateContext();
		ImGuiIO &io = ImGui::GetIO();
		io.IniFilename = nullptr;
		io.DisplaySize = ImVec2(1280.0f, 720.0f);
		io.DeltaTime = 1.0f / 60.0f;
		unsigned char *pixels = nullptr;
		int width = 0;
		int height = 0;
		io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
	}
	~NullBackend() { ImGui::DestroyContext(context); }
};

inline void *test_alloc(size_t size, void *) { return std::malloc(size); }
inline void test_free(void *ptr, void *) { std::free(ptr); }

} // namespace imgui_test
