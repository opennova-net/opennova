#include "catalog_validation.h"
#include <editor/documents/def_catalog_document.h>
#include <editor/documents/source_issue_findings.h>
#include <editor/project/project_files.h>

#include <base/io/strutil.h>

#include <map>

namespace opennova::editor {
using namespace def;
namespace {
const CatalogRow &catalog_row(const Node &node) { return static_cast<const CatalogRow &>(node); }

// What the game does with a record whose name an earlier record of its kind has, in the
// file's order: the game loads the file either way, so each is a warning naming the record
// that wins. The name lookups compare without case.
std::string repeated_name(DefRecordKind kind, const Node &earlier) {
	switch (kind) {
	case DefRecordKind::Item:
		// Both are kept (the load logs "Duplicate names", its strcmp @0x4a1ea4, and goes on)
		// [orig: ItemDefs_LoadAndValidate @ 0x4a1da0]; the lookup by name returns the first
		// [orig: ItemList_FindIndexByPrimaryName @ 0x49e010].
		return "An earlier item has this name (id " + std::to_string(std::get<DefItemDef>(catalog_row(earlier).data).id) +
		       "): the game keeps both, and a lookup by the name finds the earlier one.";
	case DefRecordKind::Weapon:
		// A weapon block of a name the table has reopens that row, reset to its defaults
		// [orig: WeaponDefs_ParseLineCallback @ 0x543680, AvatarDef_FindIndexByName @0x5436e1 ->
		// AdmDef_InitEntryDefaults @0x543722].
		return "An earlier weapon has this name: the game reads this block in its place and keeps nothing of the "
		       "earlier one.";
	case DefRecordKind::Ammo:
		// Every ammo block takes a slot of its own [orig: AmmoDef_AllocateSlot @ 0x409a20]; the
		// lookup by name returns the first [orig: AmmoDef_LookupByName @ 0x409870].
		return "An earlier ammo record has this name: the game keeps both, and a lookup by the name finds the "
		       "earlier one.";
	case DefRecordKind::Carry:
		// A class the table has is found, and its limit written over [orig: WeaponDefs_ParseLineCallback
		// @ 0x543680, sub_540590 @0x543802 -> the store @0x543873].
		return "An earlier carry limit names this class: the game reads this line's limit in its place.";
	default: return "An earlier record has this name.";
	}
}
}
std::vector<Diagnostic> validate_catalog_file(const Document &document) {
	std::vector<Diagnostic> findings;
	const auto *catalog = dynamic_cast<const DefCatalogDocument *>(&document);
	if (!catalog) return findings;
	auto locate = [&](Diagnostic &diagnostic) {
		for (const auto &row : catalog->rows()) {
			if (row->name() == diagnostic.record) {
				diagnostic.row_id = row->id; diagnostic.record_kind = row->kind; return;
			}
			if (def_kind(row->kind) == DefRecordKind::Weapon) {
				const auto &weapon = std::get<DefWeaponDef>(catalog_row(*row).data);
				for (size_t i = 0; i < weapon.actions_count; ++i) if (weapon.actions[i].name == diagnostic.record) {
					diagnostic.row_id = row->id; diagnostic.child_id = row->collections[0][i];
					diagnostic.record_kind = node_kind(DefRecordKind::Action); return;
				}
			}
		}
	};
	// Input the game ignores is dropped on save: a warning. Input the typed model cannot
	// carry blocks the file: an error. On the record the issue names, found by its name.
	source_issue_findings(
			*catalog, "catalog.invalid_input", "catalog.ignored_input", findings, locate);
	if (document.blocked()) return findings;
	for (const auto &issue : document.serialize().issues) {
		auto diagnostic = make_diagnostic(DiagnosticSeverity::Error, "catalog.unserializable",
			issue.message, document.path(), issue.field);
		diagnostic.record = issue.record; locate(diagnostic); findings.push_back(std::move(diagnostic));
	}
	// The first item of each id in the file: both of a repeated id are kept (the load logs
	// "Duplicate ID number" @0x4a1e96 and goes on) [orig: ItemDefs_LoadAndValidate @ 0x4a1da0], and
	// the lookup by id returns the first [orig: ItemList_FindIndexByTypeId @ 0x49e100]. An id a
	// table the scan lists earlier has is graph/use_checks' finding (check_item_ids).
	std::map<int, const Node *> first_of_id;
	std::map<std::string, const Node *> named; // the first record of each kind and name
	for (const auto &row : catalog->rows()) {
		auto add = [&](DiagnosticSeverity severity, const char *code, const std::string &message,
			const std::string &field, NodeAddress address) {
			auto diagnostic = make_diagnostic(severity, code, message, document.path(), field);
			diagnostic.record = row->name(); diagnostic.row_id = address.row;
			diagnostic.child_id = address.child; diagnostic.record_kind = address.kind;
			findings.push_back(std::move(diagnostic));
		};
		const NodeAddress address{row->id, row->kind, 0};
		// On the field that names the record (an item's display_name, a weapon's weapon_name).
		const DefRecordKind kind = def_kind(row->kind);
		const char *name_field = catalog_name_field(kind);
		if (row->name().empty()) {
			add(DiagnosticSeverity::Error, "catalog.name_empty", "Enter a name for this record.", name_field, address);
		} else {
			const auto first = named.emplace(std::to_string(int(row->kind)) + "/" + strutil::to_upper(row->name()), row.get());
			if (!first.second)
				add(DiagnosticSeverity::Warning, "catalog.name_duplicate", repeated_name(kind, *first.first->second), name_field,
				    address);
		}
		if (kind == DefRecordKind::Item) {
			const auto &item = std::get<DefItemDef>(catalog_row(*row).data);
			const auto first = first_of_id.emplace(item.id, row.get());
			if (!first.second)
				add(DiagnosticSeverity::Warning, "catalog.item_identity",
				    "An earlier item, \"" + first.first->second->name() + "\", has id " + std::to_string(item.id) +
				            ": the game keeps both, and a lookup by the id finds the earlier one.",
				    "id", address);
			if (!item.type) add(DiagnosticSeverity::Error, "catalog.item_type", "Choose an item type.", "type", address);
		}
	}
	return findings;
}
} // namespace opennova::editor
