// THE CHAT INPUT LINE element: the open capture's titled stdbox with the
// typed line and its blinking cursor. The capture, its prompt and the
// dispatch color are hud/hud_chat_entry.h's.
// [orig: StdCtype_Destructor (IDB misnomer — the chat input line) @0x5b8f30,
//  called from HUD_DrawOverlayPanels @0x5c014e as (ctx, 50, dword_24C190C)
//  whenever g_InputCaptureMode is nonzero and not the save-rename mode 3
//  (@0x5c0132..0x5c0143)]

#include <runtime/hud/hud_frame.h>

namespace opennova::hud {

namespace {

// The call's corner [orig: `push 32h` @0x5c014b; dword_24C190C = 0x260
// (Renderer_SetDisplayModeWithFallback @0x58763a) — the caller's y, 608 on
// the scene frame; the status page passes (50, 480) @0x50b24b..0x50b25a] and
// the drawer's box and text offsets [orig: HUD_DrawLabelBox(ctx, x, y,
// 1024 - x, y + 64, prompt, color) @0x5b8f86..0x5b8f99; the text at (x + 32,
// y + 24) @0x5b8fd9..0x5b8fdd].
constexpr float kChatInputX = 50.0f;
constexpr float kChatInputHeight = 64.0f;
constexpr float kChatInputTextDx = 32.0f;
constexpr float kChatInputTextDy = 24.0f;

} // namespace

void HudFrameCompiler::element_chat_input(const HudFrameState &state, float w, float h,
		float y) {
	const HudChatInputState &ci = state.chat_input;
	if (!ci.shown) return;
	// The stdbox with the prompt as its title, in the dispatch color
	// [orig: @0x5b8f99].
	emit_label_box(kChatInputX, y, 1024.0f - kChatInputX,
			y + kChatInputHeight, ci.prompt, ci.color, w, h);
	// The typed line plus a '_' cursor while bits 4..5 of the main-frame
	// counter are set [orig: `test byte ptr dword_A8705C, 30h` @0x5b8fbb, the
	// '_' store @0x5b8fc6], in the bold slot, half-bright, with inline tags
	// OFF so typed markup shows as typed [orig: HUD_DrawTextLeftScaled(bold,
	// .., 0x101) @0x5b8fe6 -> HUD_DrawTextLeft_HalfBright: flag 0x100 passes
	// the tags-disabled bit @0x5804e1..0x5804e9].
	std::string line = ci.text;
	if ((ci.frame & 0x30u) != 0u) line.push_back('_');
	if (line.empty()) {
		++draw_list_.elements_drawn;
		return;
	}
	emit_slot_text(label_font_bold_, label_scale_, line.c_str(),
			sx(kChatInputX + kChatInputTextDx, w), sy(y + kChatInputTextDy, h),
			half_bright_argb(ci.color), kFontTagsDisabled);
	++draw_list_.elements_drawn;
}

} // namespace opennova::hud
