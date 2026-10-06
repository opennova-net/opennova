#include <editor/ui/texture_view.h>

#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <cstring>

#include <imgui.h>
#include <imgui_internal.h>

#include <base/io/strutil.h>
#include <editor/assets/asset_kinds.h>
#include <editor/assets/asset_registry.h>
#include <editor/documents/document_types.h>
#include <editor/documents/texture_budget.h>
#include <editor/documents/texture_document.h>
#include <editor/documents/texture_image.h>
#include <editor/documents/texture_roles.h>
#include <editor/graph/asset_graph.h>
#include <editor/graph/reference_queries.h>
#include <editor/graph/texture_uses.h>
#include <editor/import/texture_import.h>
#include <editor/import/texture_source.h>
#include <editor/project/project_files.h>
#include <editor/session/request_factories.h>
#include <editor/session/texture_show_use.h>
#include <editor/session/texture_use_index.h>
#include <editor/session/view/session_view.h>
#include <editor/ui/editor_requests.h>
#include <editor/ui/reference_picker.h>
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
	const ImVec2 origin = ImGui::GetCursorScreenPos();
	const ImVec2 size = ImGui::GetContentRegionAvail();
	const float avail = size.x;
	const float em = ImGui::GetFontSize();
	const float column = std::min(std::clamp(avail * 0.36f, em * 18.0f, em * 30.0f), avail * 0.5f);
	if (ImGui::BeginChild("texture_info", ImVec2(column, 0.0f), ImGuiChildFlags_Borders))
		draw_info(workspace, document);
	ImGui::EndChild();
	ImGui::SameLine();
	if (ImGui::BeginChild("texture_picture", ImVec2(0.0f, 0.0f))) {
		main_viewport(workspace, document);
		// A project image dragged from Files onto the picture: Replace (S18).
		const ImGuiID target = ImGui::GetID("##replace_drop");
		if (ImGui::BeginDragDropTargetCustom(ImRect(ImGui::GetWindowPos(),
		                                            ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowSize().x,
		                                                   ImGui::GetWindowPos().y + ImGui::GetWindowSize().y)),
		                                     target)) {
			const ImGuiPayload *dragged = ImGui::GetDragDropPayload();
			if (dragged && dragged->IsDataType(kFileDragPayload) && dragged->Data) {
				const char *data = static_cast<const char *>(dragged->Data);
				const std::string path(data, strnlen(data, size_t(dragged->DataSize)));
				if (replaceable_image(path) && path != document.path() && ImGui::AcceptDragDropPayload(kFileDragPayload))
					workspace.request(request::preview_texture_source(document.path(), path));
			}
			ImGui::EndDragDropTarget();
		}
	}
	ImGui::EndChild();
	// An image the OS dropped on the tab: Replace, asked first (S18).
	std::vector<std::string> dropped;
	if (workspace.take_dropped_files(origin.x, origin.y, origin.x + size.x, origin.y + size.y, dropped)) {
		for (const std::string &path : dropped)
			if (replaceable_image(path)) {
				workspace.request(request::preview_texture_source(document.path(), path));
				break;
			}
	}
}

bool TextureView::replaceable_image(const std::string &path) {
	const std::string extension = strutil::to_lower(utf8_of(path_of(path).extension()));
	return extension == ".png" || extension == ".tga" || extension == ".pcx";
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
	                                  "your own program (Edit in its program).");
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
	// Made from an image the modder brings (S18): a Replace, which keeps the name every referrer writes.
	const SessionView &view = workspace.view();
	ui_kit::WrapRow made;
	if (ui_kit::tool(made, "Replace with image...", view.allows(EditorRequestKind::PreviewTextureSource),
	                 "Makes " + basename_of(document.path()) +
	                         " from a PNG, a TGA or a PCX of yours, in the form it is stored in, under the name every file "
	                         "that uses it writes, after showing you what changes. The file it replaces is kept under " +
	                         std::string(kReplacedFolder) + "/. Dropping an image on this tab does the same.")) {
		EditorRequest pick = request::pick_file(PickPurpose::TextureImage);
		pick.path = document.path();
		workspace.request(std::move(pick));
	}
	// Painted in the modder's own program (S18): what it saves comes back as the editor gains the focus. A
	// texture with a source opens it; one without is given one, asked first.
	const AssetEntry *entry = view.project.scan ? view.project.scan->at_path(document.path()) : nullptr;
	const bool has_source = (entry && !entry->imported_from.empty()) ||
	                        strutil::to_lower(utf8_of(path_of(document.path()).extension())) == ".png";
	const bool unsaved = document.dirty();
	if (ui_kit::tool(made, "Edit in its program",
	                 view.allows(has_source ? EditorRequestKind::OpenTextureSource : EditorRequestKind::PreviewTextureSource) && !unsaved,
	                 unsaved ? "Save or discard its edits first: its program edits the file as saved."
	                         : "Opens the image this texture is made from in the program your system has for it: its "
	                           "import's source, or a PNG the game reads itself. A texture stored in another form gets a "
	                           "source of its own once, in art/, which its import turns back into this file (you are shown "
	                           "what changes first). What the program saves is imported again when you come back to the "
	                           "editor."))
		workspace.request(has_source ? request::open_texture_source(document.path()) : request::preview_texture_source(document.path()));
	if (!image->palette.empty() && ImGui::CollapsingHeader("Palette", ImGuiTreeNodeFlags_DefaultOpen)) draw_palette(image->palette);
	draw_import(workspace, document);
	if (!import_.imported) draw_edits(workspace, document, *image);
	draw_uses(workspace, document);
}

// The whole-image edits of a texture no import makes (ADR 0046 S18, texture_operation): each one undo
// step, the file made anew through the editor's writers in the form it is stored in, Save writing it.
// Each group's tools wrap to the column's width.
void TextureView::draw_edits(Workspace &workspace, const DocumentBase &document, const TextureImage &image) {
	if (!ImGui::CollapsingHeader("Edit###texture_edit")) return;
	const SessionView &view = workspace.view();
	const bool allowed = view.allows(EditorRequestKind::TextureOperation) && image.loads && image.decoded;
	const std::string &path = document.path();
	const auto operate = [&](const char *operation, std::vector<std::pair<std::string, std::string>> params) {
		workspace.request(request::texture_operation(path, operation, std::move(params)));
	};
	const std::string extension = strutil::to_lower(utf8_of(path_of(path).extension()));
	const uint32_t w = image.width(), h = image.height();
	// Its rows, where the game draws it upside down.
	if (image.upside_down) {
		ui_kit::WrapRow rows;
		if (ui_kit::tool(rows, "Save it bottom first", allowed,
		                 "Its rows are stored top first, and the game reads every TGA bottom up: written bottom first, the "
		                 "game draws it the way up its header meant."))
			operate("reorder_rows", {});
	}
	// Its size: halved as the game halves, or its sides to powers of two.
	ImGui::TextDisabled("Size");
	ui_kit::WrapRow size;
	if (w > 1 && h > 1 &&
	    ui_kit::tool(size, "Halve", allowed, "Each texel the 2 x 2 box of the four under it, as the game halves a texture."))
		operate("resize", {{"size", std::to_string(w / 2) + "x" + std::to_string(h / 2)}});
	if (ui_kit::tool(size, "Powers of two", allowed, "Each side down to a power of two."))
		operate("resize", {{"size", "pow2_down"}});
	// Its alpha (a PCX holds none; a 24-bit TGA's edit says to store it as 32-bit first).
	if (extension != ".pcx") {
		ImGui::TextDisabled("Alpha");
		ui_kit::WrapRow alpha;
		if (ui_kit::tool(alpha, "Opaque", allowed, "Every texel opaque.")) operate("alpha", {{"alpha", "opaque"}});
		if (ui_kit::tool(alpha, "Invert", allowed, "Each texel's alpha turned over: clear where it was solid.")) operate("alpha", {{"alpha", "invert"}});
		if (ui_kit::tool(alpha, "From brightness", allowed,
		                 "Each texel's alpha its brightness, (85 x (r + g + b)) >> 8, as the game makes a sky PCX's."))
			operate("alpha", {{"alpha", "luminance"}});
	}
	// Its stored form, within its extension.
	if (extension == ".tga" || extension == ".dds") {
		ImGui::TextDisabled("Stored as");
		ui_kit::WrapRow form;
		if (extension == ".tga") {
			if (ui_kit::tool(form, "32-bit", allowed, "Colour and alpha.")) operate("format", {{"format", "tga"}});
			if (ui_kit::tool(form, "24-bit", allowed, "Colour alone: a terrain colour map's form.")) operate("format", {{"format", "tga24"}});
		} else {
			if (ui_kit::tool(form, "DXT5 with mips", allowed, "The form of the game's own model textures."))
				operate("format", {{"dds", "dxt5"}, {"mips", "full"}});
			if (ui_kit::tool(form, "DXT1", allowed, "Half DXT5's size, alpha on or off.")) operate("format", {{"dds", "dxt1"}, {"mips", "full"}});
			if (ui_kit::tool(form, "A8R8G8B8", allowed, "Uncompressed, one level.")) operate("format", {{"dds", "argb"}});
		}
	}
	// An 8-bit PCX's indices, which a foliage or char map reads as data.
	if (extension == ".pcx" && !image.palette.empty()) {
		// The two indices are the workspace's (the MCP gaps lane: workspace.document's remap_from and remap_to).
		const WorkspaceView::DocumentView &held = view.workspace.document(path);
		if (remap_held_.follow(std::make_pair(held.remap_from, held.remap_to))) {
			remap_from_ = held.remap_from;
			remap_to_ = held.remap_to;
		}
		const std::pair<int, int> before(remap_from_, remap_to_);
		ImGui::TextDisabled("Move a palette index");
		ImGui::BeginDisabled(!allowed);
		ImGui::SetNextItemWidth(ImGui::GetFontSize() * 4.0f);
		ImGui::InputInt("##from", &remap_from_, 0);
		ImGui::SameLine();
		ImGui::TextUnformatted("to");
		ImGui::SameLine();
		ImGui::SetNextItemWidth(ImGui::GetFontSize() * 4.0f);
		ImGui::InputInt("##to", &remap_to_, 0);
		ImGui::EndDisabled();
		remap_from_ = std::clamp(remap_from_, 0, 255);
		remap_to_ = std::clamp(remap_to_, 0, 255);
		if (std::make_pair(remap_from_, remap_to_) != before) {
			io::JsonValue members = io::JsonValue::make_object();
			members.set("path", io::JsonValue::make_string(path));
			members.set("remap_from", io::JsonValue::make_number(remap_from_));
			members.set("remap_to", io::JsonValue::make_number(remap_to_));
			window_requests::set_workspace(workspace, "document", std::move(members));
		}
		ui_kit::WrapRow move;
		if (ui_kit::tool(move, "Move", allowed,
		                 "Every texel of the first index takes the second, the palette as it is: a foliage map's codes."))
			operate("remap_palette", {{std::to_string(remap_from_), std::to_string(remap_to_)}});
	}
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
	// No one file serves them all: the uses that ask otherwise given a copy of their own (split_texture),
	// each file's import then made as its own uses ask.
	if (!needs.split_referrers.empty()) {
		const std::string copy = free_texture_copy_name(view, document.path());
		std::string files;
		for (const std::string &file : needs.split_referrers) files += (files.empty() ? "" : ", ") + basename_of(file);
		ui_kit::WrapRow split;
		if (ui_kit::tool(split, "Split into two files", !copy.empty() && view.allows(EditorRequestKind::SplitTexture),
		                 "Makes " + copy + ", a copy of " + basename_of(document.path()) + ", which the uses in " + files +
		                         " name from then on; each file is then made as its own uses ask. Undo does not take it back."))
			workspace.request(request::split_texture(document.path(), copy, needs.split_referrers));
	}
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
		// What else is done at a use: shown where the game draws it (S18, show_use: on its model, in its
		// menu, on a mission's terrain, else as it draws it); its file given a copy of the texture of its own
		// (split_texture), where another file uses the texture too.
		if (!use.fixed && ImGui::BeginPopupContextItem("use_menu")) {
			UsePlace place;
			std::string why;
			const bool placed = texture_use_place(view, document.path(), use, int(i), place, why);
			if (ImGui::MenuItem(placed ? place.label.c_str() : "Show where the game draws it", nullptr, false,
			                    placed && view.allows(EditorRequestKind::ShowUse)))
				workspace.request(request::show_use(document.path(), use.referrer, use.locator.empty() ? use.record : use.locator,
				                                    use.field));
			ui_kit::tooltip(placed ? place.picture == UsePicture::AsUsed
			                                 ? "Nothing in the editor draws " + basename_of(use.referrer) +
			                                           "'s picture: this texture's view shows it as that use makes it."
			                                 : "Opens " + basename_of(place.open) + " where it draws this texture."
			                       : why);
			const bool others = std::any_of(uses.begin(), uses.end(), [&](const TextureUse &each) {
				return !each.fixed && each.referrer != use.referrer;
			});
			const std::string copy = free_texture_copy_name(view, document.path());
			if (ImGui::MenuItem(("Give " + basename_of(use.referrer) + " a copy of its own").c_str(), nullptr, false,
			                    others && !copy.empty() && view.allows(EditorRequestKind::SplitTexture)))
				workspace.request(request::split_texture(document.path(), copy, {use.referrer}));
			ui_kit::tooltip("Makes " + copy + ", a copy of " + basename_of(document.path()) + ", which " +
			                basename_of(use.referrer) + " names from then on; the other uses keep this file.");
			ImGui::EndPopup();
		}
		const auto use_tooltip = [&] {
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
		};
		ui_kit::tooltip_lazy(use_tooltip);
		// What it costs the game (S18, the texture budget): its device texture at full object texture detail,
		// in the warning colour past what the use check says of, and each detail level in its tooltip.
		if (use.budget.known) draw_budget(use.budget);
		ImGui::PopID();
	}
}

// A use's budget under its line: what the game makes of the file at full object texture detail and what
// its .dds would take, the levels below in the tooltip.
void TextureView::draw_budget(const TextureBudget &budget) {
	const renderer::DeviceTexture &full = budget.full();
	std::string line = "In the game: " + texture_bytes_words(full.bytes);
	if (budget.offers_dds) line += ", " + texture_bytes_words(budget.as_dds.bytes) + " as its .dds";
	ImGui::Indent();
	ImGui::PushTextWrapPos(0.0f);
	if (full.bytes > kTextureMemoryWarnBytes) ImGui::TextColored(ui_kit::severity_color(DiagnosticSeverity::Warning), "%s", line.c_str());
	else ImGui::TextDisabled("%s", line.c_str());
	ImGui::PopTextWrapPos();
	ImGui::Unindent();
	ui_kit::tooltip_lazy([&] {
		std::string tip = "What the game makes of " + budget.file + " for this use, every level of which it keeps in its memory:";
		for (int level = renderer::kObjectTexDetailFull; level >= 0; --level)
			tip += "\nObject texture detail " + std::to_string(level) +
			       (level == renderer::kObjectTexDetailFull ? " (full)" : level == 0 ? " (the lowest)" : "") + ": " +
			       device_texture_words(budget.detail[level]);
		if (budget.offers_dds)
			tip += "\nAs a " + std::string(renderer::device_texture_format_name(budget.as_dds.format)) +
			       " .dds beside it, which its loader reads first: " + device_texture_words(budget.as_dds);
		tip += "\nThe detail is the options' object texture detail (game.cfg object_texdetail).";
		return tip;
	});
}

std::unique_ptr<DocumentView> make_texture_view() {
	return std::make_unique<TextureView>();
}

} // namespace opennova::editor
