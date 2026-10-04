#include <editor/ui/texture_view.h>

#include <algorithm>
#include <cfloat>
#include <cstdio>

#include <imgui.h>

#include <base/io/strutil.h>
#include <editor/assets/asset_kinds.h>
#include <editor/assets/asset_registry.h>
#include <editor/documents/document_types.h>
#include <editor/documents/texture_document.h>
#include <editor/documents/texture_image.h>
#include <editor/documents/texture_roles.h>
#include <editor/graph/asset_graph.h>
#include <editor/graph/reference_queries.h>
#include <editor/graph/texture_uses.h>
#include <editor/import/texture_import.h>
#include <editor/project/project_files.h>
#include <editor/session/request_factories.h>
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
	ui_kit::tooltip(document.path() + "\nShown as the game reads it. Its edits are whole-image ones (Edit, below); paint it in "
	                                  "your own program.");
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
	draw_import(workspace, document);
	if (!import_.imported) draw_edits(workspace, document, *image);
	draw_uses(workspace, document);
}

// The whole-image edits of a texture no import makes (ADR 0046 S18, texture_operation): each one undo
// step, the file made anew through the editor's writers in the form it is stored in, Save writing it.
void TextureView::draw_edits(Workspace &workspace, const DocumentBase &document, const TextureImage &image) {
	if (!ImGui::CollapsingHeader("Edit###texture_edit")) return;
	const SessionView &view = workspace.view();
	const bool allowed = view.allows(EditorRequestKind::TextureOperation) && image.loads && image.decoded;
	const std::string &path = document.path();
	const auto operate = [&](const char *operation, std::vector<std::pair<std::string, std::string>> params) {
		if (allowed) workspace.request(request::texture_operation(path, operation, std::move(params)));
	};
	const std::string extension = strutil::to_lower(utf8_of(path_of(path).extension()));
	const uint32_t w = image.width(), h = image.height();
	ImGui::BeginDisabled(!allowed);
	ImGui::PushTextWrapPos(0.0f);
	// Its rows, where the game draws it upside down.
	if (image.upside_down) {
		if (ImGui::Button("Save it bottom first")) operate("reorder_rows", {});
		ui_kit::tooltip("Its rows are stored top first, and the game reads every TGA bottom up: written bottom first, the "
		                "game draws it the way up its header meant.");
	}
	// Its size: halved as the game halves, or its sides to powers of two.
	ImGui::TextDisabled("Size");
	if (w > 1 && h > 1) {
		if (ImGui::Button("Halve")) operate("resize", {{"size", std::to_string(w / 2) + "x" + std::to_string(h / 2)}});
		ui_kit::tooltip("Each texel the 2 x 2 box of the four under it, as the game halves a texture.");
		ImGui::SameLine();
	}
	if (ImGui::Button("Powers of two")) operate("resize", {{"size", "pow2_down"}});
	ui_kit::tooltip("Each side down to a power of two.");
	// Its alpha (a PCX holds none; a 24-bit TGA's edit says to store it as 32-bit first).
	if (extension != ".pcx") {
		ImGui::TextDisabled("Alpha");
		if (ImGui::Button("Opaque")) operate("alpha", {{"alpha", "opaque"}});
		ImGui::SameLine();
		if (ImGui::Button("Invert")) operate("alpha", {{"alpha", "invert"}});
		ImGui::SameLine();
		if (ImGui::Button("From brightness")) operate("alpha", {{"alpha", "luminance"}});
		ui_kit::tooltip("Each texel's alpha its brightness, (85 x (r + g + b)) >> 8, as the game makes a sky PCX's.");
	}
	// Its stored form, within its extension.
	if (extension == ".tga") {
		ImGui::TextDisabled("Stored as");
		if (ImGui::Button("32-bit")) operate("format", {{"format", "tga"}});
		ImGui::SameLine();
		if (ImGui::Button("24-bit")) operate("format", {{"format", "tga24"}});
		ui_kit::tooltip("Colour alone: a terrain colour map's form.");
	} else if (extension == ".dds") {
		ImGui::TextDisabled("Stored as");
		if (ImGui::Button("DXT5 with mips")) operate("format", {{"dds", "dxt5"}, {"mips", "full"}});
		ImGui::SameLine();
		if (ImGui::Button("DXT1")) operate("format", {{"dds", "dxt1"}, {"mips", "full"}});
		ImGui::SameLine();
		if (ImGui::Button("A8R8G8B8")) operate("format", {{"dds", "argb"}});
	}
	// An 8-bit PCX's indices, which a foliage or char map reads as data.
	if (extension == ".pcx" && !image.palette.empty()) {
		ImGui::TextDisabled("Move a palette index");
		ImGui::SetNextItemWidth(ImGui::GetFontSize() * 4.0f);
		ImGui::InputInt("##from", &remap_from_, 0);
		ImGui::SameLine();
		ImGui::TextUnformatted("to");
		ImGui::SameLine();
		ImGui::SetNextItemWidth(ImGui::GetFontSize() * 4.0f);
		ImGui::InputInt("##to", &remap_to_, 0);
		remap_from_ = std::clamp(remap_from_, 0, 255);
		remap_to_ = std::clamp(remap_to_, 0, 255);
		ImGui::SameLine();
		if (ImGui::Button("Move")) operate("remap_palette", {{std::to_string(remap_from_), std::to_string(remap_to_)}});
		ui_kit::tooltip("Every texel of the first index takes the second, the palette as it is: a foliage map's codes.");
	}
	ImGui::PopTextWrapPos();
	ImGui::EndDisabled();
}

// How the texture is made, where an import makes it (ADR 0046 S18): its source; each option of its
// importer that applies, a control whose change sets it (set_import_options); what its uses ask of it,
// with why, and the one click that makes it so; what no one file serves.
void TextureView::draw_import(Workspace &workspace, const DocumentBase &document) {
	const SessionView &view = workspace.view();
	const RevisionKey key = revision_key(view.revisions, {ViewConcern::Files, ViewConcern::Graph, ViewConcern::Documents,
	                                                      ViewConcern::DocumentSet});
	if (!import_.made || import_.key != key || import_.path != document.path()) {
		import_ = ImportShown();
		import_.made = true;
		import_.key = key;
		import_.path = document.path();
		std::string error;
		import_.imported = texture_import_state(view, document.path(), import_.state, error);
		if (import_.imported)
			for (const ImportOptionRow &row : import_.state.importer->options)
				if (row.values.empty()) import_.drafts[row.key] = import_option_value(import_.state, row);
	}
	if (!import_.imported) return;
	const TextureImportState &state = import_.state;
	const std::string heading = "Made from " + basename_of(state.source) + "###made_from";
	if (!ImGui::CollapsingHeader(heading.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) return;
	ImGui::PushTextWrapPos(0.0f);
	ImGui::TextDisabled("Its import makes this file from %s: change how it is made here.", state.source.c_str());
	ImGui::PopTextWrapPos();
	// Set while the busy gate takes a set_import_options (an operation holding the files waits).
	const bool allowed = view.allows(EditorRequestKind::SetImportOptions);
	const auto set = [&](const std::string &option, const std::string &value) {
		if (allowed) workspace.request(request::set_import_options(state.source, {{option, value}}));
	};
	ImGui::BeginDisabled(!allowed);
	if (ImGui::BeginTable("import_options", 2, ImGuiTableFlags_SizingStretchProp)) {
		ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthFixed);
		ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch);
		for (const ImportOptionRow &row : state.importer->options) {
			if (!import_option_applies_now(state, row)) continue;
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::TextDisabled("%s", row.label.c_str());
			ui_kit::tooltip(row.words);
			ImGui::TableNextColumn();
			ImGui::PushID(row.key.c_str());
			ImGui::SetNextItemWidth(-FLT_MIN);
			const std::string value = import_option_value(state, row);
			if (row.values.empty()) {
				// A free value (the file name): typed, set when the field is left.
				std::string &draft = import_.drafts[row.key];
				char buffer[64];
				std::snprintf(buffer, sizeof(buffer), "%s", draft.c_str());
				const std::string hint = image_import_output_name(state.source, image_import_settings(state.sidecar.options));
				if (ImGui::InputTextWithHint("##free", hint.c_str(), buffer, sizeof(buffer))) draft = buffer;
				if (ImGui::IsItemDeactivatedAfterEdit() && draft != value) set(row.key, draft);
				ui_kit::tooltip(row.words + "\nTakes " + import_option_takes(row) + ".");
			} else {
				const ImportOptionValue *current = nullptr;
				for (const ImportOptionValue &each : row.values)
					if (each.token == strutil::to_lower(value)) current = &each;
				// A value of a free form (threshold:128, fit:512x512) shows as it is, among the tokens.
				if (ImGui::BeginCombo("##choice", current ? current->words.c_str() : value.c_str())) {
					for (const ImportOptionValue &each : row.values) {
						if (ImGui::Selectable(each.words.c_str(), current == &each) && current != &each) set(row.key, each.token);
						ui_kit::tooltip(each.token);
					}
					ImGui::EndCombo();
				}
				ui_kit::tooltip(row.words + "\nTakes " + import_option_takes(row) + ".");
			}
			ImGui::PopID();
		}
		ImGui::EndTable();
	}
	// What its uses ask, and the one click that makes it so.
	const TextureImportNeeds &needs = state.needs;
	std::vector<std::pair<std::string, std::string>> asked;
	for (const auto &[option, value] : needs.options) {
		const ImportOptionRow *row = import_option_row(state.importer->options, option);
		if (row && strutil::to_lower(import_option_value(state, *row)) != strutil::to_lower(value)) asked.emplace_back(option, value);
	}
	ImGui::PushTextWrapPos(0.0f);
	if (!asked.empty()) {
		ImGui::TextUnformatted("Its uses ask for:");
		for (const std::string &reason : needs.reasons) ImGui::BulletText("%s", reason.c_str());
		if (ImGui::Button("Make it as its uses ask") && allowed)
			workspace.request(request::set_import_options(state.source, {needs.options.begin(), needs.options.end()}));
	} else if (needs.uses > 0 && needs.conflicts.empty()) {
		ImGui::TextDisabled("It is made as its uses ask.");
	}
	for (const std::string &conflict : needs.conflicts)
		ImGui::TextColored(ui_kit::severity_color(DiagnosticSeverity::Warning), "%s", conflict.c_str());
	ImGui::PopTextWrapPos();
	ImGui::EndDisabled();
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
