#include <editor/ui/texture_viewport_view.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

#include <imgui.h>

#include <editor/preview/texture_viewport.h>
#include <editor/session/request_factories.h>
#include <editor/session/texture_use_index.h>
#include <editor/session/view/session_view.h>
#include <editor/ui/ui_kit.h>
#include <editor/ui/workspace.h>

namespace opennova::editor {

namespace {

// The channels' names in the toolbar, in TextureChannels' order, and what each shows.
constexpr const char *kChannelNames[] = {"Colour", "Red", "Green", "Blue", "Alpha", "Colour and alpha"};
constexpr const char *kChannelTips[] = {
	"The colour, its alpha left out.",
	"The red channel as grey.",
	"The green channel as grey.",
	"The blue channel as grey.",
	"The alpha channel as grey: white opaque, black transparent.",
	"The colour over a checkerboard by its alpha: what shows through where it is transparent.",
};

std::string percent(float scale) {
	char text[32];
	std::snprintf(text, sizeof(text), "%g%%", std::round(scale * 1000.0f) / 10.0f);
	return text;
}

std::string level_words(const TextureImage &image, size_t level) {
	return "Level " + std::to_string(level) + ": " + std::to_string(image.levels[level].width) + " x " +
	       std::to_string(image.levels[level].height);
}

// A combo as wide as its widest choice, its arrow and its padding: none is cut.
float combo_width(float widest_text) {
	return widest_text + ImGui::GetFrameHeight() + ImGui::GetStyle().FramePadding.x * 2.0f;
}

} // namespace

TextureViewportView::TextureViewportView() : ViewportView(ViewportKind::Texture) {}

void TextureViewportView::draw_ready(Workspace &workspace, const ViewportModel &viewport, ViewportContext &context) {
	const auto &model = static_cast<const TextureViewport &>(viewport);
	const TextureImage *image = model.image().get();
	ui_kit::WrapRow row;
	// The zoom: fitted, one texel a pixel about the middle shown now, and the scale it is at.
	const TexturePlacement placed = model.placement(context.width, context.height);
	if (ui_kit::tool(row, "Fit", !model.camera().fit, model.camera().fit ? "The whole texture shows already." : "Show the whole texture (F, or a double click on it)."))
		workspace.request(request::set_viewport(model.path(), texture_camera_change(TextureCamera{})));
	if (ui_kit::tool(row, "1:1", true, "One texel a pixel, about the middle shown now.")) {
		TextureCamera camera;
		camera.fit = false;
		camera.scale = 1.0f;
		camera.x = placed.x;
		camera.y = placed.y;
		workspace.request(request::set_viewport(model.path(), texture_camera_change(camera)));
	}
	const std::string zoom = percent(placed.scale);
	row.next(ui_kit::text_width(zoom.c_str()));
	ImGui::AlignTextToFramePadding();
	ImGui::TextDisabled("%s", zoom.c_str());
	ui_kit::tooltip("The zoom: the wheel steps it about the pointer; a drag pans.");
	// The channels.
	TextureViewportOptions options = model.options();
	float widest = 0.0f;
	for (const char *name : kChannelNames) widest = std::max(widest, ui_kit::text_width(name));
	const float channels_width = combo_width(widest);
	row.next(ui_kit::field_width(channels_width, "##channels"));
	ImGui::SetNextItemWidth(channels_width);
	if (ImGui::BeginCombo("##channels", kChannelNames[size_t(options.channels)])) {
		for (size_t i = 0; i < 6; ++i) {
			if (ImGui::Selectable(kChannelNames[i], size_t(options.channels) == i)) options.channels = TextureChannels(i);
			ui_kit::tooltip(kChannelTips[i]);
		}
		ImGui::EndCombo();
	}
	ui_kit::tooltip(std::string("Shows: ") + kChannelTips[size_t(options.channels)]);
	// The mip level, where the file holds more than one.
	if (image && image->levels.size() > 1) {
		const size_t shown = model.shown_level();
		const std::string label = level_words(*image, shown);
		// The first level's words are the widest (its sides the longest numbers).
		const float level_width = combo_width(ui_kit::text_width(level_words(*image, 0).c_str()));
		row.next(ui_kit::field_width(level_width, "##level"));
		ImGui::SetNextItemWidth(level_width);
		if (ImGui::BeginCombo("##level", label.c_str())) {
			for (size_t i = 0; i < image->levels.size(); ++i)
				if (ImGui::Selectable(level_words(*image, i).c_str(), i == shown)) options.level = int(i);
			ImGui::EndCombo();
		}
		ui_kit::tooltip("The mip level shown, at the texture's size: the game samples a smaller level as the "
		                "texture shrinks on screen.");
	}
	// What it is shown as: the file, or one of its uses as the game draws it (what the use's loader makes
	// of its texels, a cut-out's test, a tile atlas's cells).
	static const std::vector<TextureUse> kNone;
	const SessionView &view = workspace.view();
	const std::vector<TextureUse> &uses =
	        view.documents.texture_uses ? view.documents.texture_uses->uses_of(view, model.path()) : kNone;
	if (!uses.empty()) {
		const char *file_words = "As the file holds it";
		const float as_width = std::min(combo_width(ui_kit::text_width("As the game draws it for 00 uses")),
		                                std::max(ImGui::GetContentRegionAvail().x, ImGui::GetFontSize() * 10.0f));
		const TextureShownUse &shown = model.shown_use();
		const std::string label = shown.index >= 0 ? "As: " + shown.words : std::string(file_words);
		row.next(ui_kit::field_width(as_width, "##as_used"));
		ImGui::SetNextItemWidth(as_width);
		if (ImGui::BeginCombo("##as_used", ui_kit::fit(label, as_width - ImGui::GetFrameHeight()).c_str())) {
			if (ImGui::Selectable(file_words, options.as_used < 0)) options.as_used = -1;
			ui_kit::tooltip("The texels as the file's reader decodes them.");
			for (size_t i = 0; i < uses.size(); ++i) {
				const std::string item = uses[i].words + "###use" + std::to_string(i);
				if (ImGui::Selectable(item.c_str(), options.as_used == int(i))) options.as_used = int(i);
				ui_kit::tooltip("As the game draws it for this use: " + uses[i].words);
			}
			ImGui::EndCombo();
		}
		ui_kit::tooltip(shown.index >= 0 ? "Shows the texture as the game draws it for " + shown.words +
		                                           (shown.cutout >= 0 ? ": what its cut-out keeps opaque, what it discards clear" : "") +
		                                           (shown.cells > 0 ? ": the cells the game cuts it in drawn over it" : "") + "."
		                                 : std::string("Shows the texture as the file holds it; pick a use to see it as the game "
		                                               "draws it there."));
	}
	if (options != model.options()) workspace.request(request::set_viewport(model.path(), texture_options_change(options)));
	canvas(workspace, viewport, context, std::max(48.0f, ImGui::GetContentRegionAvail().y));
}

} // namespace opennova::editor
