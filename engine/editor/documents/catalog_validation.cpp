#include "catalog_validation.h"
#include <editor/project/project_files.h>
#include <base/gameprofile/gameprofile.h>
#include <base/vfs/vfs_decode.h>
#include <formats/rtxt/rtxt.h>
#include <runtime/replication/item_replication_catalog.h>

#include <filesystem>
#include <map>
#include <set>

namespace opennova::editor {
using namespace def;
namespace {
std::string key(const std::string &value) { return normalized_logical_name(value); }
bool has_asset(const AssetScan &scan, const std::string &name, AssetKind kind, const char *extension) {
	const auto *asset = scan.find(name);
	if (!asset) asset = scan.find(name + extension);
	return asset && asset->kind == kind;
}
}
std::vector<Diagnostic> validate_catalogs(const ProjectPaths &paths, const ProjectDocument &project,
	const AssetScan &scan, const std::vector<std::shared_ptr<const EditableDocument>> &open) {
	std::vector<Diagnostic> findings;
	std::vector<std::shared_ptr<const EditableDocument>> documents;
	for (const auto &asset : scan.entries) {
		if (!is_catalog_kind(asset.kind)) continue;
		std::shared_ptr<const EditableDocument> document;
		for (const auto &candidate : open) if (candidate->path() == asset.relative_path) document = candidate;
		if (!document) {
			auto loaded = std::make_shared<EditableDocument>(); Diagnostic error;
			if (!loaded->load((std::filesystem::path(paths.root) / asset.relative_path).generic_string(),
				asset.relative_path, asset.kind, project.target_game, error)) { findings.push_back(error); continue; }
			document = loaded;
		}
		documents.push_back(document);
	}
	std::set<std::string> weapons, ammo;
	std::set<int> item_ids;
	std::vector<DefItemDef> item_records;
	for (const auto &document : documents) for (const auto &row : document->rows()) {
		if (row->kind == DefRecordKind::Weapon) weapons.insert(key(row->name()));
		if (row->kind == DefRecordKind::Ammo) ammo.insert(key(row->name()));
		if (row->kind == DefRecordKind::Item) {
			const auto &item = std::get<DefItemDef>(row->data);
			item_ids.insert(item.id); item_records.push_back(item);
		}
	}
	rtxt::File game_text;
	if (const auto *asset = scan.find("gametext.bin")) {
		std::vector<uint8_t> bytes; std::string error;
		if (read_file_bytes((std::filesystem::path(paths.root) / asset->relative_path).generic_string(), bytes, error) &&
			vfs_decode_payload(bytes, gameprofile::gameprofile_scr_policy_for_code(project.target_game.c_str())))
			rtxt::parse(bytes.data(), bytes.size(), game_text, error);
	}
	DefItemsFile item_file{}; item_file.entries = item_records.data(); item_file.count = item_records.size();
	const auto replication = replication::ItemReplicationCatalog::from_items_def(item_file);
    std::set<int> duplicate_ids;
    for (const auto &issue : replication.issues()) duplicate_ids.insert(issue.definition_id);
	for (const auto &document : documents) {
        auto locate = [&](Diagnostic &diagnostic) {
            for (const auto &row : document->rows()) {
                if (row->name() == diagnostic.record) {
                    diagnostic.row_id = row->id; diagnostic.record_kind = row->kind; return;
                }
                if (row->kind == DefRecordKind::Weapon) {
                    const auto &weapon = std::get<DefWeaponDef>(row->data);
                    for (size_t i = 0; i < weapon.actions_count; ++i) if (weapon.actions[i].name == diagnostic.record) {
                        diagnostic.row_id = row->id; diagnostic.child_id = row->children[i];
                        diagnostic.record_kind = DefRecordKind::Action; return;
                    }
                }
            }
        };
        for (const auto &issue : document->parse_issues()) {
			auto diagnostic = make_diagnostic(DiagnosticSeverity::Error,
				issue.code == DefIssueCode::UnknownProperty ? "catalog.unknown_property" : "catalog.invalid_input",
				issue.message, document->path(), issue.field);
			diagnostic.line = issue.line; diagnostic.record = issue.record; locate(diagnostic);
			findings.push_back(std::move(diagnostic));
		}
		if (document->blocked()) continue;
        for (const auto &issue : document->serialize().diagnostics) {
            auto diagnostic = make_diagnostic(DiagnosticSeverity::Error, "catalog.unserializable",
                issue.message, document->path(), issue.field);
            diagnostic.record = issue.record; locate(diagnostic); findings.push_back(std::move(diagnostic));
        }
		std::set<std::string> names;
		for (const auto &row : document->rows()) {
			auto add = [&](DiagnosticSeverity severity, const char *code, const std::string &message,
				const std::string &field, CatalogAddress address) {
				auto diagnostic = make_diagnostic(severity, code, message, document->path(), field);
				diagnostic.record = row->name(); diagnostic.row_id = address.row;
				diagnostic.child_id = address.child; diagnostic.record_kind = address.kind;
				findings.push_back(std::move(diagnostic));
			};
			const CatalogAddress address{row->id, row->kind, 0};
			if (row->name().empty())
				add(DiagnosticSeverity::Error, "catalog.name_empty", "Enter a name for this record.", "name", address);
			else if (!names.insert(std::to_string(int(row->kind)) + "/" + key(row->name())).second)
				add(DiagnosticSeverity::Error, "catalog.name_duplicate", "A record with this name already exists.", "name", address);
			if (row->kind == DefRecordKind::Item) {
				const auto &item = std::get<DefItemDef>(row->data);
				if (duplicate_ids.count(item.id))
                    add(DiagnosticSeverity::Error, "catalog.item_identity", "Duplicate item identity " + std::to_string(item.id) + ".", "id", address);
                if (!item.type) add(DiagnosticSeverity::Error, "catalog.item_type", "Choose an item type.", "type", address);
			}
			auto inspect = [&](CatalogAddress target) {
				const void *record = document->record(target);
				if (!record) return;
				for (const auto &field : def_fields(target.kind)) {
					if (field.reference == DefReference::None) continue;
					const DefValue value = def_get(record, field);
					std::string symbol;
					if (const auto *text = std::get_if<std::string>(&value)) symbol = *text;
					else if (const auto *id = std::get_if<int64_t>(&value)) { if (*id) symbol = std::to_string(*id); }
					if (symbol.empty() || key(symbol) == "NONE" || key(symbol) == "NULL") continue;
					bool found = true, advisory = false;
					switch (field.reference) {
					case DefReference::Model: found = has_asset(scan, symbol, AssetKind::Model, ".3di"); break;
					case DefReference::AnimationMap: found = has_asset(scan, symbol, AssetKind::AnimationMap, ".adm"); break;
					case DefReference::AiProfile: found = has_asset(scan, symbol, AssetKind::AiProfile, ".aip"); break;
					case DefReference::Texture:
						found = has_asset(scan, symbol, AssetKind::Texture, ".tga") ||
							has_asset(scan, symbol, AssetKind::Texture, ".pcx") || has_asset(scan, symbol, AssetKind::Texture, ".dds"); break;
					case DefReference::Ammo: found = ammo.count(key(symbol)) != 0; break;
					case DefReference::Weapon: found = weapons.count(key(symbol)) != 0; break;
					case DefReference::Item: found = item_ids.count(int(std::get<int64_t>(value))) != 0; break;
					case DefReference::GameText: found = game_text.find_in_section("WepDes", symbol) != nullptr; break;
					case DefReference::OtherText:
						found = field.id == "attach_text_id" ? game_text.find_in_section("Overlays", symbol) != nullptr : false;
						advisory = field.id != "attach_text_id"; break;
					case DefReference::Sound: case DefReference::Particle: found = false; advisory = true; break;
					default: break;
					}
					if (!found) add(advisory ? DiagnosticSeverity::Warning : DiagnosticSeverity::Error,
						advisory ? "catalog.reference_unverified" : "catalog.reference_missing",
						advisory ? "Verify " + field.id + " reference '" + symbol + "'; this catalog does not resolve that symbol table yet." :
							"Missing " + field.id + " reference '" + symbol + "'.", field.id, target);
				}
			};
			inspect(address);
			const auto child_kind = row->kind == DefRecordKind::Item ? DefRecordKind::Attachment :
				row->kind == DefRecordKind::Weapon ? DefRecordKind::Action : DefRecordKind::Effect;
			for (CatalogId id : row->children) inspect({row->id, child_kind, id});
			for (CatalogId id : row->sights) inspect({row->id, DefRecordKind::Sight, id});
		}
	}
	return findings;
}
} // namespace opennova::editor
