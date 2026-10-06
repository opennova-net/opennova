#include <editor/ui/texture_viewport_view.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

#include <imgui.h>

#include <editor/documents/texture_budget.h>
#include <editor/preview/texture_viewport.h>
#include <runtime/renderer/texture_dxt.h>
#include <editor/session/request_factories.h>
#include <editor/session/texture_use_index.h>
#include <editor/session/view/session_view.h>
#include <editor/ui/ui_kit.h>
#include <editor/ui/workspace.h>

namespace opennova::editor {

namespace {

// The channels' names in the toolbar, in TextureChannels' order, and what each shows.
constexpr const char *kChannelNames[] = {"Colour", "Red", "Green", "Blue", "Alpha", "Colour and alpha", "Normals, lit"};
constexpr const char *kChannelTips[] = {
	"The colour, its alpha left out.",
	"The red channel as grey.",
	"The green channel as grey.",
	"The blue channel as grey.",
	"The alpha channel as grey: white opaque, black transparent.",
	"The colour over a checkerboard by its alpha: what shows through where it is transparent.",
	"Its colour read as a normal map (red across, green down the texture as the game's tangent frame runs, blue out "
	"of it) and lit from the light's direction: the relief it gives a surface.",
};
constexpr size_t kChannelCount = sizeof(kChannelNames) / sizeof(kChannelNames[0]);

// The object texture detail's choices in the toolbar: as stored, then each level, full first.
constexpr const char *kDetailNames[] = {"As stored", "Detail 3 (full)", "Detail 2", "Detail 1", "Detail 0 (lowest)"};

std::string percent(float scale) {
	char text[32];
	std::snprintf(text, sizeof(text), "%g%%", std::round(scale * 1000.0f) / 10.0f);
	return text;
}

std::string level_words(uint32_t width, uint32_t height, size_t level) {
	return "Level " + std::to_string(level) + ": " + std::to_string(std::max(1u, width >> level)) + " x " +
	       std::to_string(std::max(1u, height >> level));
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
		for (size_t i = 0; i < kChannelCount; ++i) {
			if (ImGui::Selectable(kChannelNames[i], size_t(options.channels) == i)) options.channels = TextureChannels(i);
			ui_kit::tooltip(kChannelTips[i]);
		}
		ImGui::EndCombo();
	}
	ui_kit::tooltip(std::string("Shows: ") + kChannelTips[size_t(options.channels)]);
	// The light a normal map is lit from, round the picture.
	if (options.channels == TextureChannels::Normals) {
		const float light_width = ImGui::GetFontSize() * 7.0f;
		row.next(ui_kit::field_width(light_width, "##light"));
		ImGui::SetNextItemWidth(light_width);
		float light = options.light;
		if (ImGui::SliderFloat("##light", &light, 0.0f, 360.0f, "light %.0f deg")) options.light = light;
		ui_kit::tooltip("Where the light comes from, round the picture: 0 from its right, 90 from its top, 180 from its left.");
	}
	// The mip level: the file's chain (a DDS's), or the chain the game builds of a texture made from pixels.
	const TextureImage *source = model.source().get();
	const bool built = source && source->reader != TextureReader::Dds && image && image->levels.size() == 1;
	const uint32_t chain = image ? (image->levels.size() > 1 ? uint32_t(image->levels.size())
	                                                         : built ? renderer::texture_level_count(image->width(), image->height(), 0) : 1u)
	                             : 1u;
	if (image && chain > 1) {
		const size_t shown = std::min(size_t(std::max(options.level, 0)), size_t(chain - 1));
		const std::string label = level_words(image->width(), image->height(), shown);
		// The first level's words are the widest (its sides the longest numbers).
		const float level_width = combo_width(ui_kit::text_width(level_words(image->width(), image->height(), 0).c_str()));
		row.next(ui_kit::field_width(level_width, "##level"));
		ImGui::SetNextItemWidth(level_width);
		if (ImGui::BeginCombo("##level", label.c_str())) {
			for (size_t i = 0; i < chain; ++i)
				if (ImGui::Selectable(level_words(image->width(), image->height(), i).c_str(), i == shown)) options.level = int(i);
			ImGui::EndCombo();
		}
		const bool game_built = source && source->reader != TextureReader::Dds;
		ui_kit::tooltip(game_built ? "The mip level shown, at the texture's size: the chain the game builds of it, each level the box "
		                             "filter of the one before; the game samples a smaller level as the texture shrinks on screen."
		                           : "The mip level shown, at the texture's size: the chain the file holds; the game samples a "
		                             "smaller level as the texture shrinks on screen.");
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
	// The object texture detail the device texture is shown at, where a model row's use costs it.
	const TextureBudget *budget = nullptr;
	for (const TextureUse &use : uses)
		if (use.budget.known) {
			budget = &use.budget;
			break;
		}
	if (budget) {
		float widest_detail = 0.0f;
		for (const char *name : kDetailNames) widest_detail = std::max(widest_detail, ui_kit::text_width(name));
		const float detail_width = combo_width(widest_detail);
		row.next(ui_kit::field_width(detail_width, "##detail"));
		ImGui::SetNextItemWidth(detail_width);
		const size_t current = options.detail < 0 ? 0 : size_t(4 - options.detail);
		if (ImGui::BeginCombo("##detail", kDetailNames[current])) {
			for (size_t i = 0; i < 5; ++i)
				if (ImGui::Selectable(kDetailNames[i], i == current)) options.detail = i == 0 ? -1 : int(4 - i);
			ImGui::EndCombo();
		}
		ui_kit::tooltip("The device texture the game makes at the options' object texture detail (game.cfg object_texdetail, 0 "
		                "the lowest), for the use shown or the first model row: a diffuse or detail texture is halved once "
		                "or twice below full detail, a normal map never.");
	}
	if (options != model.options()) workspace.request(request::set_viewport(model.path(), texture_options_change(options)));
	// What the picture is, in words: the device texture at the detail shown; the alpha as the use shown reads it.
	ImGui::PushTextWrapPos(0.0f);
	if (model.shown_detail() >= 0)
		ImGui::TextDisabled("At object texture detail %d the game makes %s.", model.shown_detail(),
		                    device_texture_words(model.shown_device()).c_str());
	const TextureShownUse &use_shown = model.shown_use();
	if (use_shown.index >= 0 && !use_shown.alpha_words.empty()) ImGui::TextDisabled("Alpha: %s.", use_shown.alpha_words.c_str());
	// What the use's role reads of it: a line (a particle graphic's atlas page), and a legend (a blend map's weights,
	// a foliage map's codes), each row its swatch, its share and what the game does with it; six rows show, more scroll.
	const TextureRoleView &role = model.role_view();
	if (!role.words.empty()) ImGui::TextDisabled("%s", role.words.c_str());
	if (!role.title.empty()) {
		ImGui::TextDisabled("%s:", role.title.c_str());
		const float line = ImGui::GetTextLineHeightWithSpacing();
		const float height = line * float(std::min<size_t>(std::max<size_t>(role.legend.size(), 1), 6)) + ImGui::GetStyle().WindowPadding.y;
		if (ImGui::BeginChild("legend", ImVec2(0.0f, height))) {
			ImGui::PushTextWrapPos(0.0f);
			for (size_t i = 0; i < role.legend.size(); ++i) {
				const TextureLegendRow &entry = role.legend[i];
				ImGui::PushID(int(i));
				const float side = ImGui::GetTextLineHeight();
				ImGui::ColorButton("##swatch", ImVec4(entry.rgb[0] / 255.0f, entry.rgb[1] / 255.0f, entry.rgb[2] / 255.0f, 1.0f),
				                   ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoPicker | ImGuiColorEditFlags_NoDragDrop,
				                   ImVec2(side, side));
				ImGui::SameLine();
				char share[32];
				std::snprintf(share, sizeof(share), "%.1f%%", entry.share * 100.0);
				ImGui::TextUnformatted((entry.key + " (" + share + "): " + entry.words).c_str());
				ImGui::PopID();
			}
			ImGui::PopTextWrapPos();
		}
		ImGui::EndChild();
	}
	ImGui::PopTextWrapPos();
	canvas(workspace, viewport, context, std::max(48.0f, ImGui::GetContentRegionAvail().y));
}

} // namespace opennova::editor
