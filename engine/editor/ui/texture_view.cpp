#include <editor/ui/texture_view.h>

#include <algorithm>
#include <cfloat>
#include <cstdio>

#include <imgui.h>

#include <editor/assets/asset_kinds.h>
#include <editor/assets/asset_registry.h>
#include <editor/documents/document_types.h>
#include <editor/documents/texture_document.h>
#include <editor/documents/texture_roles.h>
#include <editor/graph/asset_graph.h>
#include <editor/graph/reference_queries.h>
#include <editor/graph/texture_uses.h>
#include <editor/project/project_files.h>
#include <editor/session/texture_use_index.h>
#include <editor/session/view/session_view.h>
#include <editor/ui/editor_requests.h>
#include <editor/ui/texture_viewport_view.h>
#include <editor/ui/ui_kit.h>

namespace opennova::editor {

TextureView::TextureView() : viewport_(std::make_unique<TextureViewportView>()) {}

TextureView::~TextureView() = default;

void TextureView::draw(Workspace &workspace, const DocumentBase &document) {
	// A texture holds no records: a reveal sent to it shows nothing more than the tab itself.
	take_events();
	// The info in a column (about a third of the tab, wide enough to read its sentences, never more
	// than half of it), the picture beside it. Sized every frame: the tab's width follows the dock.
	const float avail = ImGui::GetContentRegionAvail().x;
	const float em = ImGui::GetFontSize();
	const float column = std::min(std::clamp(avail * 0.36f, em * 18.0f, em * 30.0f), avail * 0.5f);
	if (ImGui::BeginChild("texture_info", ImVec2(column, 0.0f), ImGuiChildFlags_Borders))
		draw_info(workspace, document);
	ImGui::EndChild();
	ImGui::SameLine();
	if (ImGui::BeginChild("texture_picture", ImVec2(0.0f, 0.0f))) main_viewport(workspace, document);
	ImGui::EndChild();
}

bool TextureView::main_viewport(Workspace &workspace, const DocumentBase &document) {
	viewport_->draw(workspace, document.path());
	return true;
}

void TextureView::end_frame(Workspace &workspace) {
	viewport_->end_frame(workspace);
}

void TextureView::draw_info(Workspace &workspace, const DocumentBase &document) {
	const auto *texture = dynamic_cast<const TextureDocument *>(&document);
	const TextureImage *image = texture ? texture->image().get() : nullptr;
	ImGui::TextUnformatted(basename_of(document.path()).c_str());
	ui_kit::tooltip(document.path() + "\nRead only: the texture editor shows a texture as the game reads it.");
	if (!image) {
		ui_kit::empty_state("This texture did not read.");
		return;
	}
	ImGui::Spacing();
	if (ImGui::BeginTable("facts", 2, ImGuiTableFlags_SizingStretchProp)) {
		ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthFixed);
		ImGui::TableSetupColumn("words", ImGuiTableColumnFlags_WidthStretch);
		for (const TextureFact &fact : image->facts) {
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::TextDisabled("%s", fact.label.c_str());
			ImGui::TableNextColumn();
			ImGui::PushTextWrapPos(0.0f);
			if (fact.key == "loads" && !image->loads) ImGui::TextColored(ui_kit::severity_color(DiagnosticSeverity::Error), "%s", fact.words.c_str());
			else if (fact.key == "rows" && image->upside_down) ImGui::TextColored(ui_kit::severity_color(DiagnosticSeverity::Warning), "%s", fact.words.c_str());
			else ImGui::TextUnformatted(fact.words.c_str());
			ImGui::PopTextWrapPos();
		}
		ImGui::EndTable();
	}
	if (!image->palette.empty() && ImGui::CollapsingHeader("Palette", ImGuiTreeNodeFlags_DefaultOpen)) draw_palette(image->palette);
	draw_uses(workspace, document);
}

// The palette as swatches, sixteen a row, as wide as the column lets them be.
void TextureView::draw_palette(const std::vector<uint8_t> &palette) {
	const size_t entries = palette.size() / 3;
	const float side = std::clamp((ImGui::GetContentRegionAvail().x - 2.0f) / 16.0f, 6.0f, 18.0f);
	const ImVec2 origin = ImGui::GetCursorScreenPos();
	const size_t rows = (entries + 15) / 16;
	ImGui::InvisibleButton("##palette", ImVec2(side * 16.0f, side * float(rows)));
	const bool hovered = ImGui::IsItemHovered();
	ImDrawList &paint = *ImGui::GetWindowDrawList();
	const ImVec2 mouse = ImGui::GetIO().MousePos;
	for (size_t i = 0; i < entries; ++i) {
		const ImVec2 min(origin.x + float(i % 16) * side, origin.y + float(i / 16) * side);
		const ImVec2 max(min.x + side - 1.0f, min.y + side - 1.0f);
		paint.AddRectFilled(min, max, IM_COL32(palette[i * 3], palette[i * 3 + 1], palette[i * 3 + 2], 255));
		if (hovered && mouse.x >= min.x && mouse.x < min.x + side && mouse.y >= min.y && mouse.y < min.y + side) {
			paint.AddRect(min, max, IM_COL32(255, 255, 255, 255));
			char tip[96];
			std::snprintf(tip, sizeof(tip), "Entry %zu: R %u G %u B %u", i, unsigned(palette[i * 3]), unsigned(palette[i * 3 + 1]),
			              unsigned(palette[i * 3 + 2]));
			ui_kit::tooltip(tip);
		}
	}
}

// What the texture is used as (the session's texture uses, S18): each use by its role and where, a Go to
// on each referrer; a use whose loader opens another file of the name dimmed, saying which.
void TextureView::draw_uses(Workspace &workspace, const DocumentBase &document) {
	const SessionView &view = workspace.view();
	static const std::vector<TextureUse> kNone;
	const std::vector<TextureUse> &uses =
			view.documents.texture_uses ? view.documents.texture_uses->uses_of(view, document.path()) : kNone;
	const std::string heading = "Used as (" + std::to_string(uses.size()) + ")###used_as";
	if (!ImGui::CollapsingHeader(heading.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) return;
	if (!view.findings.graph) {
		ui_kit::empty_state("The project's references are not read yet.");
		return;
	}
	if (uses.empty()) {
		ui_kit::empty_state("Nothing in the project uses this file.",
		                    "A model, a terrain, a sky, a particle effect, the HUD, a definition, a menu or a mission names a "
		                    "texture by its file name; the game opens some by name itself.");
		return;
	}
	for (size_t i = 0; i < uses.size(); ++i) {
		const TextureUse &use = uses[i];
		ImGui::PushID(int(i));
		std::string line = use.words;
		if (!use.reads_file)
			line += use.served.empty() ? " - the game finds nothing for it" : " - the game loads " + basename_of(use.served);
		const std::string shown = ui_kit::fit(line, ImGui::GetContentRegionAvail().x);
		if (!use.reads_file) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
		const bool pressed = ImGui::Selectable((shown + "###use").c_str());
		if (!use.reads_file) ImGui::PopStyleColor();
		ReferenceTarget target;
		if (!use.fixed) {
			const AssetEntry *source = view.project.scan ? view.project.scan->at_path(use.referrer) : nullptr;
			target.label = use.words;
			target.file = use.referrer;
			target.locator = use.locator;
			target.field = use.field;
			target.editable = source && is_editable_kind(source->kind);
		}
		if (pressed && !use.fixed) window_requests::go_to(workspace, target);
		ui_kit::tooltip_lazy([&] {
			std::string tip = line + "\nAs written: " + use.name_written;
			if (!use.load.file.empty()) tip += "\nIts loader opens " + use.load.file;
			if (use.load.transform != TextureLoadTransform::None)
				tip += std::string("\nAnd makes it ") + texture_load_transform_words(use.load.transform);
			if (use.known()) {
				const TextureRoleRow &role = texture_role_row(use.role);
				if (*role.alpha) tip += std::string("\nAlpha: ") + role.alpha;
				if (role.size != TextureSizeRule::Any) tip += "\nSize: " + texture_size_words(role);
				tip += std::string("\nWitness: ") + (use.fixed ? use.fixed_witness.c_str() : role.witness);
			}
			if (!use.fixed)
				tip += target.editable ? "\nA click opens " + target.file + " at it." : "\nA click shows " + target.file + " in Files.";
			return tip;
		});
		ImGui::PopID();
	}
}

std::unique_ptr<DocumentView> make_texture_view() {
	return std::make_unique<TextureView>();
}

} // namespace opennova::editor
