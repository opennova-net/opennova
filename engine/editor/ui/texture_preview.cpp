#include <editor/ui/texture_preview.h>

#include <algorithm>
#include <string>

#include <imgui.h>

#include <editor/assets/asset_registry.h>
#include <editor/preview/texture_thumbnail_images.h>
#include <editor/preview/texture_thumbnails.h>
#include <editor/project/project_files.h>
#include <editor/session/texture_budget_list.h>
#include <editor/session/texture_budget_list.h>
#include <editor/session/texture_use_index.h>
#include <editor/session/view/session_view.h>
#include <editor/ui/ui_kit.h>

namespace opennova::editor::texture_preview {

namespace {

// The larger picture a tooltip shows.
constexpr float kTooltipSide = 256.0f;

std::shared_ptr<const TextureThumbnail> picture_of(Workspace &workspace, const std::string &file,
                                                   TextureLoadTransform transform) {
	const SessionView &view = workspace.view();
	return view.documents.thumbnails ? view.documents.thumbnails->get(view, file, transform) : nullptr;
}

// The box at `at`, `side` a side: the picture over a checkerboard where it has alpha, its proportions
// kept; framed while it is being made; crossed out where the game cannot load the file.
void draw_box(Workspace &workspace, const TextureThumbnail *picture, ImVec2 at, float side) {
	ImDrawList &paint = *ImGui::GetWindowDrawList();
	const ImVec2 end(at.x + side, at.y + side);
	const ImU32 frame = ImGui::GetColorU32(ImGuiCol_Border);
	paint.AddRectFilled(at, end, ImGui::GetColorU32(ImGuiCol_FrameBg));
	if (!picture) {
		// Being made: three dots.
		const float r = std::max(1.5f, side * 0.04f);
		for (int i = -1; i <= 1; ++i)
			paint.AddCircleFilled(ImVec2(at.x + side * 0.5f + float(i) * r * 3.0f, at.y + side * 0.5f), r, frame);
		paint.AddRect(at, end, frame);
		return;
	}
	if (picture->state != TextureThumbnail::State::Ready || picture->width == 0 || picture->height == 0) {
		const ImU32 red = ImGui::GetColorU32(ui_kit::severity_color(DiagnosticSeverity::Error));
		const float inset = side * 0.25f;
		paint.AddLine(ImVec2(at.x + inset, at.y + inset), ImVec2(end.x - inset, end.y - inset), red, 2.0f);
		paint.AddLine(ImVec2(end.x - inset, at.y + inset), ImVec2(at.x + inset, end.y - inset), red, 2.0f);
		paint.AddRect(at, end, red);
		return;
	}
	const float scale = side / float(std::max(picture->width, picture->height));
	const float width = float(picture->width) * scale, height = float(picture->height) * scale;
	const ImVec2 from(at.x + (side - width) * 0.5f, at.y + (side - height) * 0.5f);
	const ImVec2 to(from.x + width, from.y + height);
	if (picture->translucent) {
		const float cell = std::max(4.0f, side / 8.0f);
		for (float y = from.y; y < to.y; y += cell)
			for (float x = from.x; x < to.x; x += cell) {
				const bool light = (int((x - from.x) / cell) + int((y - from.y) / cell)) % 2 == 0;
				paint.AddRectFilled(ImVec2(x, y), ImVec2(std::min(x + cell, to.x), std::min(y + cell, to.y)),
				                    light ? IM_COL32(158, 158, 158, 255) : IM_COL32(102, 102, 102, 255));
			}
	}
	TextureThumbnailImages *images = workspace.thumbnail_images();
	const uint64_t id = images ? images->texture_id(*picture) : 0;
	if (id) paint.AddImage(ImTextureID(id), from, to);
	else
		paint.AddRectFilled(from, to, IM_COL32(picture->average[0], picture->average[1], picture->average[2], picture->average[3]));
	paint.AddRect(at, end, frame);
}

} // namespace

std::string facts(const TextureThumbnail &thumbnail) {
	std::string out = std::to_string(thumbnail.source_width) + " x " + std::to_string(thumbnail.source_height);
	if (!thumbnail.format.empty()) out += ", " + thumbnail.format;
	if (!thumbnail.texels.empty()) out += "\n" + thumbnail.texels;
	if (thumbnail.levels > 1) out += ", " + std::to_string(thumbnail.levels) + " mip levels";
	return out;
}

void thumbnail(Workspace &workspace, const std::string &file, TextureLoadTransform transform, float side) {
	const std::shared_ptr<const TextureThumbnail> picture = picture_of(workspace, file, transform);
	const ImVec2 at = ImGui::GetCursorScreenPos();
	ImGui::PushID(file.c_str());
	ImGui::InvisibleButton("##thumbnail", ImVec2(side, side));
	ImGui::PopID();
	draw_box(workspace, picture.get(), at, side);
	tooltip(workspace, file, transform, std::string());
}

void picture(Workspace &workspace, const std::string &file, TextureLoadTransform transform, float side) {
	const std::shared_ptr<const TextureThumbnail> made = picture_of(workspace, file, transform);
	const ImVec2 at = ImGui::GetCursorScreenPos();
	ImGui::Dummy(ImVec2(side, side));
	draw_box(workspace, made.get(), at, side);
}

void picture(Workspace &workspace, const TextureThumbnail *made, float side) {
	const ImVec2 at = ImGui::GetCursorScreenPos();
	ImGui::Dummy(ImVec2(side, side));
	draw_box(workspace, made, at, side);
}

void facts_block(Workspace &workspace, const std::string &file, TextureLoadTransform transform, float width) {
	const std::shared_ptr<const TextureThumbnail> made = picture_of(workspace, file, transform);
	ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width);
	ImGui::TextUnformatted(basename_of(file).c_str());
	if (!made) ImGui::TextDisabled("Reading it...");
	else if (made->state != TextureThumbnail::State::Ready)
		ImGui::TextColored(ui_kit::severity_color(DiagnosticSeverity::Error), "The game cannot load it: %s", made->refusal.c_str());
	else {
		ImGui::TextDisabled("%s", facts(*made).c_str());
		if (transform != TextureLoadTransform::None)
			ImGui::TextDisabled("As this use loads it: %s", texture_load_transform_words(transform));
	}
	ImGui::PopTextWrapPos();
}

void tooltip(Workspace &workspace, const std::string &file, TextureLoadTransform transform, const std::string &lead) {
	if (!ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip) || !ImGui::BeginTooltip()) return;
	if (!lead.empty()) ImGui::TextUnformatted(lead.c_str());
	const std::shared_ptr<const TextureThumbnail> picture = picture_of(workspace, file, transform);
	const ImVec2 at = ImGui::GetCursorScreenPos();
	ImGui::Dummy(ImVec2(kTooltipSide, kTooltipSide));
	draw_box(workspace, picture.get(), at, kTooltipSide);
	ImGui::TextUnformatted(file.c_str());
	if (!picture) {
		ImGui::TextDisabled("Reading it...");
	} else if (picture->state != TextureThumbnail::State::Ready) {
		ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + kTooltipSide);
		ImGui::TextColored(ui_kit::severity_color(DiagnosticSeverity::Error), "The game cannot load it: %s",
		                   picture->refusal.c_str());
		ImGui::PopTextWrapPos();
	} else {
		ImGui::TextDisabled("%s", facts(*picture).c_str());
		if (!picture->alpha.empty()) ImGui::TextDisabled("Alpha: %s", picture->alpha.c_str());
		if (transform != TextureLoadTransform::None)
			ImGui::TextDisabled("As this use loads it: %s", texture_load_transform_words(transform));
	}
	ImGui::EndTooltip();
}

void reference_field(Workspace &workspace, const FieldUse &field, const Value &value, bool compact) {
	const SessionView &view = workspace.view();
	if (!view.findings.graph || !is_texture_reference(field.reference)) return;
	const TextureReferenceLoad load = texture_reference(*view.findings.graph, field, value);
	if (!load.texture) return;
	const float line = ImGui::GetFrameHeight();
	if (compact) {
		if (load.file.empty()) return;
		ImGui::SameLine();
		thumbnail(workspace, load.file, load.transform, line);
		return;
	}
	const float side = line * 3.0f;
	if (load.file.empty()) {
		// Nothing the game loads: the box empty, and why.
		const ImVec2 at = ImGui::GetCursorScreenPos();
		ImGui::Dummy(ImVec2(side, side));
		ImDrawList &paint = *ImGui::GetWindowDrawList();
		paint.AddRect(at, ImVec2(at.x + side, at.y + side), ImGui::GetColorU32(ui_kit::reference_color(ReferenceStatus::Missing)));
		ImGui::SameLine();
		ImGui::BeginGroup();
		ImGui::PushTextWrapPos(0.0f);
		ImGui::TextColored(ui_kit::reference_color(ReferenceStatus::Missing), "Not found");
		ImGui::TextDisabled("The project has no file the game loads for %s.", load.name.c_str());
		ImGui::PopTextWrapPos();
		ImGui::EndGroup();
		return;
	}
	// Its words beside it while they have a few words' room, else under it.
	const bool beside =
			ImGui::GetContentRegionAvail().x - side - ImGui::GetStyle().ItemSpacing.x >= ImGui::GetFontSize() * 14.0f;
	thumbnail(workspace, load.file, load.transform, side);
	if (beside) ImGui::SameLine();
	ImGui::BeginGroup();
	ImGui::PushTextWrapPos(0.0f);
	const std::shared_ptr<const TextureThumbnail> picture = picture_of(workspace, load.file, load.transform);
	const std::string name = basename_of(load.file);
	if (!picture) {
		ImGui::TextUnformatted(name.c_str());
		ImGui::TextDisabled("Reading it...");
	} else if (picture->state != TextureThumbnail::State::Ready) {
		ImGui::TextUnformatted(name.c_str());
		ImGui::TextColored(ui_kit::severity_color(DiagnosticSeverity::Error), "The game cannot load it: %s",
		                   picture->refusal.c_str());
	} else {
		// The file the loader opens, said where it is not the name written (a .tga's .dds).
		ImGui::TextUnformatted(pff::normalized_logical_name(name) == pff::normalized_logical_name(load.name)
		                               ? name.c_str()
		                               : (name + " (the game loads it for " + load.name + ")").c_str());
		ImGui::TextDisabled("%s", facts(*picture).c_str());
		if (load.transform != TextureLoadTransform::None)
			ImGui::TextDisabled("As this use loads it: %s", texture_load_transform_words(load.transform));
	}
	ImGui::PopTextWrapPos();
	ImGui::EndGroup();
}

bool file_tooltip(Workspace &workspace, const std::string &file, const std::string &lead) {
	const SessionView &view = workspace.view();
	const AssetEntry *entry = view.project.scan ? view.project.scan->at_path(file) : nullptr;
	if (!entry || entry->kind != AssetKind::Texture) return false;
	if (!ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) return true;
	// What uses it (S18: the session's texture uses), its first use in words and how many more.
	std::string words = lead;
	if (view.documents.texture_uses) {
		const std::vector<TextureUse> &uses = view.documents.texture_uses->uses_of(view, entry->relative_path);
		const std::string used = uses.empty() ? std::string("Nothing in the project uses it.")
		                                      : "Used as " + uses.front().words +
		                                                (uses.size() > 1 ? " and " + std::to_string(uses.size() - 1) + " more" : "") + ".";
		words += (words.empty() ? "" : "\n") + used;
	}
	// What the game's texture of it costs (S18, the texture budget).
	const std::string cost = texture_file_budget_words(view, entry->relative_path);
	if (!cost.empty()) words += (words.empty() ? "" : "\n") + cost;
	tooltip(workspace, entry->relative_path, TextureLoadTransform::None, words);
	return true;
}

bool record_tooltip(Workspace &workspace, const Document &document, const NodeAddress &address, const std::string &lead) {
	const SessionView &view = workspace.view();
	if (!view.findings.graph || !ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) return false;
	for (const FieldSchema &schema : document.fields(address.kind)) {
		const FieldUse field = document.field_on(address, schema);
		Value value;
		if (!is_texture_reference(field.reference) || !document.present(address, schema.id) ||
		    !document.get(address, schema.id, value))
			continue;
		const TextureReferenceLoad load = texture_reference(*view.findings.graph, field, value);
		if (load.file.empty()) continue;
		tooltip(workspace, load.file, load.transform, lead);
		return true;
	}
	return false;
}

} // namespace opennova::editor::texture_preview
