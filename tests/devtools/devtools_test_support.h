// Shared scaffolding for the dev-tools ctests (devtools_test,
// devtools_windows_test, devtools_overlay_test): the failure counter and
// CHECK macro, a null ImGui backend (a display size and a built font atlas,
// no platform or renderer), a fake Game viewport, the allocator hooks the
// pass hand-off takes, and the control-request id compare.
#pragma once

#include <runtime/devtools/control_request.h>
#include <runtime/devtools/game_dev_tools.h>
#include <runtime/devtools/game_window.h>

#include <imgui.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace devtools_test {

inline int g_failures = 0;

#define CHECK(cond, message)                                                          \
	do {                                                                              \
		if (!(cond)) {                                                                \
			std::printf("FAIL %s:%d: %s (%s)\n", __FILE__, __LINE__, message, #cond); \
			++devtools_test::g_failures;                                              \
		}                                                                             \
	} while (0)

// A headless ImGui frame: the null example's setup (a display size and a
// built font atlas), no platform or renderer backend.
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

struct FakeGameViewport : opennova::devtools::GameViewport {
	int width = 0;
	int height = 0;
	int draws = 0;

	void draw(int requested_width, int requested_height) override {
		width = requested_width;
		height = requested_height;
		++draws;
	}
};

inline void *test_alloc(size_t size, void *) { return std::malloc(size); }
inline void test_free(void *ptr, void *) { std::free(ptr); }

// A drained control request names a row by its wire id (the constants are
// string literals, so the check compares text, never addresses).
inline bool is_control(const opennova::devtools::ControlRequest &request, const char *id) {
	return std::strcmp(request.id, id) == 0;
}

// The registered window with this title, or -1.
inline int find_window(const opennova::devtools::GameDevTools &tools, const char *title) {
	for (int i = 0; i < tools.pass().window_count(); ++i) {
		if (std::strcmp(tools.pass().window(i).title(), title) == 0) return i;
	}
	return -1;
}

// One layout pass of an attached, open tools instance.
inline bool draw_once(opennova::devtools::GameDevTools &tools, uint64_t frame) {
	ImGui::NewFrame();
	const bool drew = tools.pass().draw_frame(frame);
	ImGui::Render();
	return drew;
}

inline int report(const char *name) {
	if (g_failures != 0) {
		std::printf("%d failure(s)\n", g_failures);
		return 1;
	}
	std::printf("%s: OK\n", name);
	return 0;
}

}  // namespace devtools_test
