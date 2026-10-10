#include <editor/ui/font_viewport_view.h>

#include <algorithm>
#include <string>
#include <vector>

#include <imgui.h>

#include <editor/preview/font_viewport.h>
#include <editor/session/request_factories.h>
#include <editor/ui/ui_kit.h>
#include <editor/ui/workspace.h>

namespace opennova::editor {

namespace {

// A combo as wide as its widest choice, its arrow and its padding: none is cut.
float combo_width(float widest_text) {
	return widest_text + ImGui::GetFrameHeight() + ImGui::GetStyle().FramePadding.x * 2.0f;
}

std::string page_words(int page, uint32_t pages) {
	return page < 0 ? std::string("Text") : "Page " + std::to_string(page + 1) + " of " + std::to_string(pages);
}

} // namespace

FontViewportView::FontViewportView() : ViewportView(ViewportKind::Font) {}

void FontViewportView::draw_ready(Workspace &workspace, const ViewportModel &viewport, ViewportContext &context) {
	const auto &model = static_cast<const FontViewport &>(viewport);
	FontViewportOptions options = model.options();
	const uint32_t pages = model.picture() ? model.picture()->font.num_pages : 0;
	ui_kit::WrapRow row;
	// The text or a page.
	const float page_width = combo_width(ui_kit::text_width(page_words(15, 16).c_str()));
	row.next(ui_kit::field_width(page_width, "##page"));
	ImGui::SetNextItemWidth(page_width);
	if (ImGui::BeginCombo("##page", page_words(model.shown_page(), pages).c_str())) {
		if (ImGui::Selectable("Text", model.shown_page() < 0)) options.page = -1;
		ui_kit::tooltip("The text below, as the game draws it with this font.");
		for (uint32_t p = 0; p < pages; ++p)
			if (ImGui::Selectable(page_words(int(p), pages).c_str(), model.shown_page() == int(p))) options.page = int(p);
		ImGui::EndCombo();
	}
	ui_kit::tooltip("What the picture shows: the text drawn as the game draws it, or a page of the font with its glyphs' "
	                "rects (a click on a glyph selects it).");
	// The zoom: picture pixels a game pixel.
	const float zoom_width = combo_width(ui_kit::text_width("8x"));
	row.next(ui_kit::field_width(zoom_width, "##zoom"));
	ImGui::SetNextItemWidth(zoom_width);
	if (ImGui::BeginCombo("##zoom", (std::to_string(options.zoom) + "x").c_str())) {
		for (int z = 1; z <= kFontZoomMost; ++z)
			if (ImGui::Selectable((std::to_string(z) + "x").c_str(), options.zoom == z)) options.zoom = z;
		ImGui::EndCombo();
	}
	ui_kit::tooltip("Each pixel the game draws this many pixels across, texel for texel.");
	if (model.shown_page() < 0) {
		// The colour the text is asked in, and the drawer's styles.
		float rgb[3] = {float((options.color >> 16) & 0xFF) / 255.0f, float((options.color >> 8) & 0xFF) / 255.0f,
		                float(options.color & 0xFF) / 255.0f};
		row.next(ImGui::GetFrameHeight());
		if (ImGui::ColorEdit3("##color", rgb, ImGuiColorEditFlags_NoInputs))
			options.color = (uint32_t(rgb[0] * 255.0f + 0.5f) << 16) | (uint32_t(rgb[1] * 255.0f + 0.5f) << 8) |
			                uint32_t(rgb[2] * 255.0f + 0.5f);
		ui_kit::tooltip("The colour the text is asked in: every caller halves it and the page's MODULATE2X doubles it "
		                "back, so it shows as asked where the page is white.");
		row.next(ui_kit::checkbox_width("Bold"));
		ImGui::Checkbox("Bold", &options.bold);
		ui_kit::tooltip("The drawer's bold: each glyph struck again a pixel right and a pixel up.");
		row.next(ui_kit::checkbox_width("Italic"));
		ImGui::Checkbox("Italic", &options.italic);
		ui_kit::tooltip("The drawer's italic: each glyph's top sheared right by an eighth of its height.");
		row.next(ui_kit::checkbox_width("Underline"));
		ImGui::Checkbox("Underline", &options.underline);
		ui_kit::tooltip("The drawer's underline: a line under the run in twice the colour.");
	}
	if (options != model.options()) workspace.request(request::set_viewport(model.path(), font_options_change(options)));
	if (model.shown_page() < 0) {
		// The text it draws, sent once it changes; a new path starts from the options' text.
		if (text_path_ != model.path()) {
			text_path_ = model.path();
			text_ = model.options().text;
		}
		std::vector<char> buffer(text_.begin(), text_.end());
		buffer.resize(text_.size() + 1024, '\0');
		const float lines = ImGui::GetTextLineHeightWithSpacing() * 3.0f + ImGui::GetStyle().FramePadding.y * 2.0f;
		if (ImGui::InputTextMultiline("##text", buffer.data(), buffer.size(), ImVec2(-1.0f, lines))) {
			text_ = buffer.data();
			FontViewportOptions typed = model.options();
			typed.text = text_;
			workspace.request(request::set_viewport(model.path(), font_options_change(typed)));
		}
		ui_kit::tooltip("The text drawn, its bytes the game's code page; a format tag (<B>, <cRRGGBB>) is read as the "
		                "game reads one.");
	}
	canvas(workspace, viewport, context, std::max(48.0f, ImGui::GetContentRegionAvail().y));
}

} // namespace opennova::editor
