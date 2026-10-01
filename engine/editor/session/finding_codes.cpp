#include "finding_codes.h"

#include <iterator>
#include <string_view>
#include <unordered_map>

#include <editor/documents/document_types.h>

namespace opennova::editor {

namespace {

using F = FindingFix;
using G = FindingGroup;

// Each group's key (the family every code of it starts with, the whole code for the optional
// files) and title, in FindingGroup's order.
struct GroupRow {
	FindingGroup group;
	const char *key;
	const char *title;
};
constexpr GroupRow kGroups[] = {
	{ G::None, "", "" },
	{ G::RequiredFiles, "requirement", "Required files" },
	{ G::OptionalFiles, "requirement.optional_missing", "Optional files" },
	{ G::MissingReferences, "reference", "Missing references" },
	{ G::FilesNotChecked, "graph", "Files not checked" },
	{ G::ProjectFiles, "asset", "Project files" },
	{ G::Project, "project", "Project" },
	{ G::Documents, "document", "Documents" },
	{ G::Imports, "import", "Imports" },
	{ G::Build, "build", "Build" },
	{ G::Play, "play", "Play" },
	{ G::Renames, "rename", "Renames" },
	{ G::NewFiles, "blank", "New files" },
	{ G::CreateMissing, "create_missing", "Create missing files" },
	{ G::EditorSettings, "editor_settings", "Editor settings" },
	{ G::LocalSettings, "local_settings", "Local settings" },
	{ G::Operations, "operation", "Operations" },
	{ G::UnsavedChanges, "unsaved", "Unsaved changes" },
	{ G::Viewports, "viewport", "Viewports" },
	{ G::Catalogs, "catalog", "Catalogs" },
	{ G::StringTables, "strings", "String tables" },
	{ G::Menus, "menu", "Menus" },
	{ G::Stylesheets, "style", "Stylesheets" },
	{ G::Models, "model", "Models" },
	{ G::Animations, "animation", "Animations" },
	{ G::AnimationMaps, "animation_map", "Animation maps" },
	{ G::Scripts, "script", "Scripts" },
	{ G::MusicScripts, "music_script", "Music scripts" },
	{ G::Credits, "credits", "Credits" },
	{ G::Shaders, "shader", "Shaders" },
};

constexpr bool groups_well_formed() {
	for (size_t i = 0; i < std::size(kGroups); ++i) {
		if (static_cast<size_t>(kGroups[i].group) != i) return false;
		for (size_t j = 0; j < i; ++j)
			if (same_finding_token(kGroups[i].key, kGroups[j].key)) return false;
	}
	return true;
}
static_assert(std::size(kGroups) == kFindingGroupCount, "every FindingGroup has exactly one row");
static_assert(groups_well_formed(), "the group rows follow FindingGroup's order, each key its own");

const std::unordered_map<std::string_view, const FindingCodeRow *> &lookup() {
	static const std::unordered_map<std::string_view, const FindingCodeRow *> map = [] {
		std::unordered_map<std::string_view, const FindingCodeRow *> out;
		for (const NamedFindingTable &table : finding_tables())
			for (const FindingCodeRow &row : table.rows) out.emplace(row.token, &row);
		return out;
	}();
	return map;
}

} // namespace

const FindingCodeRow *finding_row(const std::string &token) {
	const auto &map = lookup();
	const auto found = map.find(std::string_view(token));
	return found == map.end() ? nullptr : found->second;
}

std::vector<NamedFindingTable> finding_tables() {
	std::vector<NamedFindingTable> out{ { "core", core_finding_codes() } };
	for (size_t i = 1; i <= kDocumentTypeCount; ++i) {
		const DocumentType *type = registered_document_type(static_cast<DocumentTypeId>(i));
		if (type && type->findings) out.push_back({ type->name, type->findings() });
	}
	return out;
}

const char *finding_owner(const FindingCodeRow *row) {
	for (const NamedFindingTable &table : finding_tables())
		if (table.rows.holds(row)) return table.owner;
	return nullptr;
}

const char *finding_fix_token(FindingFix fixes) {
	switch (fixes) {
	case F::None: return "none";
	case F::Requirement: return "requirement";
	case F::WrongKind: return "wrong_kind";
	case F::Rename: return "rename";
	case F::ResetRow: return "reset_row";
	case F::Reference: return "reference";
	case F::UnimportedTexture: return "unimported_texture";
	case F::Reload: return "reload";
	case F::Reimport: return "reimport";
	case F::Rewrite: return "rewrite";
	}
	return "none";
}

const char *finding_place_token(FindingPlace place) {
	return place == FindingPlace::File ? "file" : "content";
}

const char *finding_group_key(FindingGroup group) {
	const size_t at = static_cast<size_t>(group);
	return at < std::size(kGroups) ? kGroups[at].key : "";
}

const char *finding_group_title(FindingGroup group) {
	const size_t at = static_cast<size_t>(group);
	return at < std::size(kGroups) ? kGroups[at].title : "";
}

const char *finding_source_token(const FindingCodeRow &row) {
	switch (row.source) {
	case FindingSource::Graph: return "graph";
	case FindingSource::RenderCheck: return "render";
	case FindingSource::Own: break;
	}
	return finding_group_key(row.group);
}

const char *finding_problem_token(FindingProblem problem) {
	switch (problem) {
	case FindingProblem::Info: return "info";
	case FindingProblem::Warning: return "warning";
	case FindingProblem::None: break;
	}
	return "none";
}

} // namespace opennova::editor
