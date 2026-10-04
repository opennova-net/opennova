#include "catalog_validation.h"
#include <editor/documents/def_catalog_document.h>
#include <editor/documents/source_issue_findings.h>
#include <editor/project/project_files.h>

#include <base/io/strutil.h>

#include <map>
#include <unordered_map>

namespace opennova::editor {
using namespace def;
namespace {
constexpr FindingCodeEntry<CatalogFinding> kFindingEntries[] = {
	{ CatalogFinding::InvalidInput, { "catalog.invalid_input", FindingFix::None, nullptr, true } },
	// Input the game ignores, which a save keeps as the file has it (the file's notes, def_notes.h): said,
	// nothing to fix.
	{ CatalogFinding::IgnoredInput, { "catalog.ignored_input" } },
	{ CatalogFinding::Unserializable, { "catalog.unserializable", FindingFix::None, nullptr, true } },
	// A record with no name, an item of type 0: the editor's own rules, no refusal of the game's
	// witnessed (the gate follows retail, ADR 0046 S14): listed.
	{ CatalogFinding::NameEmpty, listed_code("catalog.name_empty") },
	{ CatalogFinding::NameDuplicate, { "catalog.name_duplicate" } },
	{ CatalogFinding::ItemIdentity, { "catalog.item_identity" } },
	{ CatalogFinding::ItemType, listed_code("catalog.item_type") },
};
static_assert(std::size(kFindingEntries) == static_cast<size_t>(CatalogFinding::kCount),
		"every CatalogFinding has exactly one row");
static_assert(finding_entries_well_formed(kFindingEntries),
		"the catalog's rows follow CatalogFinding's order, each token its own");
constexpr auto kFindingRows = finding_rows(kFindingEntries, FindingGroup::Catalogs);
static_assert(finding_rows_well_formed(kFindingRows), "every row of the table takes its group");

const CatalogRow &catalog_row(const Node &node) { return static_cast<const CatalogRow &>(node); }

// What the game does with a record whose name an earlier record of its kind has, in the file's
// order: the game loads the file either way, so each is a warning naming the record that wins, one
// row per kind its game witnesses (a new family adds its row). The name lookups compare without case.
struct RepeatedName {
	DefRecordKind kind;
	const char *message; // "{id}": the earlier record's id (an item's)
};
constexpr RepeatedName kRepeatedNames[] = {
	// Both are kept (the load logs "Duplicate names", its strcmp @0x4a1ea4, and goes on) [orig:
	// ItemDefs_LoadAndValidate @ 0x4a1da0]; the lookup by name returns the first [orig:
	// ItemList_FindIndexByPrimaryName @ 0x49e010].
	{DefRecordKind::Item,
	 "An earlier item has this name (id {id}): the game keeps both, and a lookup by the name finds the earlier one."},
	// A weapon block of a name the table has reopens that row, reset to its defaults [orig:
	// WeaponDefs_ParseLineCallback @ 0x543680, AvatarDef_FindIndexByName @0x5436e1 -> AdmDef_InitEntryDefaults
	// @0x543722].
	{DefRecordKind::Weapon,
	 "An earlier weapon has this name: the game reads this block in its place and keeps nothing of the earlier one."},
	// Every ammo block takes a slot of its own [orig: AmmoDef_AllocateSlot @ 0x409a20]; the lookup by name
	// returns the first [orig: AmmoDef_LookupByName @ 0x409870].
	{DefRecordKind::Ammo,
	 "An earlier ammo record has this name: the game keeps both, and a lookup by the name finds the earlier one."},
	// A class the table has is found, and its limit written over [orig: WeaponDefs_ParseLineCallback @
	// 0x543680, sub_540590 @0x543802 -> the store @0x543873].
	{DefRecordKind::Carry, "An earlier carry limit names this class: the game reads this line's limit in its place."},
	// The lookup by name returns the first row of it [orig: PowerUpDef_FindByName @0x442660, stricmp over the
	// rows in order]: a later one no item binds.
	{DefRecordKind::Powerup,
	 "An earlier powerup has this name: an item's powerupdef finds the earlier one, and no item binds this one."},
};

std::string repeated_name(DefRecordKind kind, const Node &earlier) {
	for (const RepeatedName &row : kRepeatedNames) {
		if (row.kind != kind) continue;
		std::string message = row.message;
		const size_t id = message.find("{id}");
		if (id != std::string::npos)
			message.replace(id, 4, std::to_string(catalog_row(earlier).native.as<DefItemDef>().id));
		return message;
	}
	return "An earlier record has this name.";
}
}
const FindingCodeRow &finding_code(CatalogFinding code) {
	return kFindingRows[static_cast<size_t>(code)];
}

FindingTable catalog_finding_codes() { return { kFindingRows.data(), kFindingRows.size() }; }

std::vector<Diagnostic> validate_catalog_file(const DocumentBase &document) {
	std::vector<Diagnostic> findings;
	const auto *catalog = dynamic_cast<const DefCatalogDocument *>(&document);
	if (!catalog) return findings;
	// A finding's record by its name: the first record of the name as a walk of the rows meets it,
	// a row's own name before its weapon's actions'. The names are read once, for the first finding
	// its locator left unplaced (a walk per finding read every row's name for each).
	std::unordered_map<std::string, NodeAddress> by_name;
	bool indexed = false;
	auto locate = [&](Diagnostic &diagnostic) {
		if (diagnostic.row_id) return; // placed by its locator, wherever that record is now
		if (!indexed) {
			indexed = true;
			for (const auto &row : catalog->rows()) {
				by_name.emplace(row->name(), NodeAddress{row->id, row->kind, 0});
				if (def_kind(row->kind) != DefRecordKind::Weapon) continue;
				const auto &weapon = catalog_row(*row).native.as<DefWeaponDef>();
				const std::vector<RecordIds> &actions = catalog_row(*row).ids.lists[0];
				for (size_t i = 0; i < weapon.actions_count && i < actions.size(); ++i)
					by_name.emplace(weapon.actions[i].name,
					                NodeAddress{row->id, node_kind(DefRecordKind::Action), actions[i].id});
			}
		}
		const auto found = by_name.find(diagnostic.record);
		if (found == by_name.end()) return;
		diagnostic.row_id = found->second.row;
		diagnostic.child_id = found->second.child;
		diagnostic.record_kind = found->second.kind;
	};
	// Input the game ignores is kept on save: a warning. Input the typed model cannot
	// carry blocks the file: an error. On the record the issue names, found by its name.
	source_issue_findings(
			*catalog, finding_code(CatalogFinding::InvalidInput),
			finding_code(CatalogFinding::IgnoredInput), findings, locate);
	if (document.blocked()) return findings;
	for (const auto &issue : document.serialize().issues) {
		auto diagnostic = make_finding(CatalogFinding::Unserializable, DiagnosticSeverity::Error,
			issue.message, document.path(), issue.field);
		diagnostic.record = issue.record; locate(diagnostic); findings.push_back(std::move(diagnostic));
	}
	// The first item of each id in the file: both of a repeated id are kept (the load logs
	// "Duplicate ID number" @0x4a1e96 and goes on) [orig: ItemDefs_LoadAndValidate @ 0x4a1da0], and
	// the lookup by id returns the first [orig: ItemList_FindIndexByTypeId @ 0x49e100]. Two item
	// tables are two files of one name, of which the game reads one (asset.name.duplicate): an id
	// is compared within its table alone.
	std::map<int, const Node *> first_of_id;
	std::map<std::string, const Node *> named; // the first record of each kind and name
	for (const auto &row : catalog->rows()) {
		auto add = [&](DiagnosticSeverity severity, CatalogFinding code, const std::string &message,
			const std::string &field, NodeAddress address) {
			auto diagnostic = make_finding(code, severity, message, document.path(), field);
			diagnostic.record = row->name(); diagnostic.row_id = address.row;
			diagnostic.child_id = address.child; diagnostic.record_kind = address.kind;
			findings.push_back(std::move(diagnostic));
		};
		const NodeAddress address{row->id, row->kind, 0};
		// On the field that names the record (an item's display_name, a weapon's weapon_name).
		const DefRecordKind kind = def_kind(row->kind);
		const char *name_field = catalog_name_field(row->kind);
		if (row->name().empty()) {
			add(DiagnosticSeverity::Error, CatalogFinding::NameEmpty, "Enter a name for this record.", name_field, address);
		} else {
			const auto first = named.emplace(std::to_string(int(row->kind)) + "/" + strutil::to_upper(row->name()), row.get());
			if (!first.second)
				add(DiagnosticSeverity::Warning, CatalogFinding::NameDuplicate, repeated_name(kind, *first.first->second), name_field,
				    address);
		}
		if (kind == DefRecordKind::Item) {
			const auto &item = catalog_row(*row).native.as<DefItemDef>();
			const auto first = first_of_id.emplace(item.id, row.get());
			if (!first.second)
				add(DiagnosticSeverity::Warning, CatalogFinding::ItemIdentity,
				    "An earlier item, \"" + first.first->second->name() + "\", has id " + std::to_string(item.id) +
				            ": the game keeps both, and a lookup by the id finds the earlier one.",
				    "id", address);
			if (!item.type) add(DiagnosticSeverity::Error, CatalogFinding::ItemType, "Choose an item type.", "type", address);
		}
	}
	return findings;
}
} // namespace opennova::editor
