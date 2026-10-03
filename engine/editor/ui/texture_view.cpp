#include <editor/ui/texture_view.h>

#include <algorithm>
#include <cfloat>
#include <cstdio>

#include <imgui.h>

#include <editor/documents/texture_document.h>
#include <editor/graph/asset_graph.h>
#include <editor/graph/display_names.h>
#include <editor/graph/reference_queries.h>
#include <editor/project/project_files.h>
#include <editor/session/view/session_view.h>
#include <editor/ui/editor_requests.h>
#include <editor/ui/inspector_layout.h>
#include <editor/ui/texture_viewport_view.h>
#include <editor/ui/ui_kit.h>

namespace opennova::editor {

namespace {

const Document *open_records(const SessionView &view, const std::string &path) {
	for (const auto &document : view.documents.open)
		if (document && document->path() == path) return records_of(*document);
	return nullptr;
}

// A use of the texture in words: the record that names it (its title where its file is open, else its
// name as the graph read it), the file, and the field.
std::string use_words(const SessionView &view, const GraphEdge &edge) {
	std::string record = edge.record;
	if (const Document *source = open_records(view, edge.source)) {
		if (view.findings.graph) {
			const GraphNameSource names(*view.findings.graph);
			const std::string title = record_display(*source, edge.address, &names);
			if (!title.empty()) record = title;
		}
	}
	const std::string file = basename_of(edge.source);
	return (record.empty() ? file : record + " in " + file) + " - " + edge_field_title(view, edge);
}

} // namespace

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
	draw_users(workspace, document);
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

// What uses the texture: the graph's references to the file, each a Go to.
void TextureView::draw_users(Workspace &workspace, const DocumentBase &document) {
	const SessionView &view = workspace.view();
	const RevisionKey key = revision_key(view.revisions, {ViewConcern::Graph, ViewConcern::Files, ViewConcern::Documents,
	                                                      ViewConcern::DocumentSet});
	if (users_.key != key || users_.file != document.path()) {
		users_.key = key;
		users_.file = document.path();
		users_.edges = view.findings.graph ? view.findings.graph->usages_of(document.path()) : std::vector<const GraphEdge *>();
		users_.lines.clear();
		for (const GraphEdge *edge : users_.edges) users_.lines.push_back(use_words(view, *edge));
	}
	const std::string heading = "Used by (" + std::to_string(users_.edges.size()) + ")###used_by";
	if (!ImGui::CollapsingHeader(heading.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) return;
	if (!view.findings.graph) {
		ui_kit::empty_state("The project's references are not read yet.");
		return;
	}
	if (users_.edges.empty()) {
		ui_kit::empty_state("No file of the project names it.", "A model, a definition, a menu or a mission names a texture by its file name.");
		return;
	}
	for (size_t i = 0; i < users_.edges.size(); ++i) {
		const GraphEdge &edge = *users_.edges[i];
		ImGui::PushID(int(i));
		const bool pressed = ImGui::Selectable((ui_kit::fit(users_.lines[i], ImGui::GetContentRegionAvail().x) + "###use").c_str());
		if (pressed || ImGui::IsItemHovered()) {
			const ReferenceTarget target = usage_target(*view.project.scan, edge);
			if (pressed) window_requests::go_to(workspace, target);
			ui_kit::tooltip(users_.lines[i] + "\nas written: " + edge.value + "\n" +
			                (target.editable ? "Open " + target.file + " at it." : "Show " + target.file + " in Files."));
		}
		ImGui::PopID();
	}
}

std::unique_ptr<DocumentView> make_texture_view() {
	return std::make_unique<TextureView>();
}

} // namespace opennova::editor
