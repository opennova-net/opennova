#include "catalog_validation.h"
#include <editor/documents/def_catalog_document.h>
#include <editor/documents/source_issue_findings.h>
#include <editor/project/project_files.h>

#include <base/io/strutil.h>

#include <formats/def/reserved_items.h>

#include <map>
#include <set>
#include <unordered_map>

namespace opennova::editor {
using namespace def;
namespace {
constexpr FindingCodeEntry<CatalogFinding> kFindingEntries[] = {
	// Input the typed record cannot carry, which the game's reader reads on past (an unknown word keeps the old
	// value, a fifth addeweap is ignored, an unclosed item block is registered): unwritable_code, a closed file
	// packed as stored, its Save refused.
	{ CatalogFinding::InvalidInput, unwritable_code("catalog.invalid_input") },
	// Input the game ignores, which a save writes as the file has it (the file's modeled layout,
	// def_notes.h): said, nothing to fix.
	{ CatalogFinding::IgnoredInput, { "catalog.ignored_input" } },
	{ CatalogFinding::Unserializable, { "catalog.unserializable", FindingFix::None, nullptr, true } },
	// A record with no name, an item of type 0: the editor's own rules, no refusal of the game's
	// witnessed (the gate follows retail, ADR 0046 S14): listed.
	{ CatalogFinding::NameEmpty, listed_code("catalog.name_empty") },
	// A name an earlier record of its kind has: a name of its own (DI-11, Diagnostic::planned); an id an
	// earlier item has: an id of its own (the reserved-id rule's free id, set: nothing reaches it by id).
	{ CatalogFinding::NameDuplicate, { "catalog.name_duplicate", FindingFix::EditRecord } },
	{ CatalogFinding::ItemIdentity, { "catalog.item_identity", FindingFix::ItemId } },
	{ CatalogFinding::ItemType, listed_code("catalog.item_type") },
	// The ids and rows the engine fixes (itemdef-re.md, "The ids and rows the engine fixes"): the game loads
	// the file either way, so each is listed. What the engine keeps an item's id for (an Info: the
	// Inspector's hint); an item of another kind on such an id, or named as one the engine keeps under
	// another id (Use an id of its own, or that id: Rename everywhere); a first row that is no Null
	// marker (Add one first); an edit refused for either (the refusal's own code).
	{ CatalogFinding::ReservedId, listed_code("catalog.reserved_id") },
	{ CatalogFinding::ReservedKind, listed_code("catalog.reserved_kind", FindingFix::ItemId) },
	{ CatalogFinding::ReservedName, listed_code("catalog.reserved_name", FindingFix::ItemId) },
	{ CatalogFinding::FirstRow, listed_code("catalog.first_row", FindingFix::FallbackRow) },
	{ CatalogFinding::ReservedRefused, listed_code("catalog.reserved_refused") },
	// Input the game's reader stops at, or corrupts the record over (DefCatalogDocument's game_stops): it gates.
	{ CatalogFinding::ReaderStops,
	  game_stops_code("catalog.reader_stops",
	                  "its reader stops at a block opened inside an open one, reading nothing past it (\"weapon didn't "
	                  "have an end\" [orig: WeaponDefs_ParseLineCallback @ 0x5436ad..0x5436d2]; the ammo reader's "
	                  "\"definition missing end\" [orig: AmmoDef_LoadAll @ 0x40b0b0]), and a fifth sights row lands on "
	                  "the record's row count [orig: WeaponDefs_ParseLineCallback, the count bump @ 0x544b11..0x544b20]") },
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

// What the ids and rows the engine fixes make of an item (formats/def/reserved_items.h; itemdef-re.md,
// "The ids and rows the engine fixes"), on the item a lookup by its id finds; `ids` every item id of the
// file, `first_row` whether it is the file's first item. The game loads the file either way: an item of
// another kind on an id the engine keeps for a place or an objective is an error that refuses no build,
// one on an id it keeps for a model or an actor a warning.
template <typename Add>
void reserved_findings(const DefItemDef &item, const std::set<int> &ids, bool first_row, Add &add,
                       const NodeAddress &address) {
	using def::ReservedItemRule;
	const std::string id = std::to_string(item.id);
	const def::ReservedItem *held = def::reserved_item_by_id(item.id);
	if (held && def::reserved_item_kind_matches(*held, item.type)) {
		add(DiagnosticSeverity::Info, CatalogFinding::ReservedId,
		    "The engine keeps id " + id + " for " + reserved_item_words(*held) + ": " + held->use, "id", address);
	} else if (held) {
		const bool refuse = held->rule == ReservedItemRule::Refuse;
		add(refuse ? DiagnosticSeverity::Error : DiagnosticSeverity::Warning, CatalogFinding::ReservedKind,
		    "The engine keeps id " + id + " for " + reserved_item_words(*held) + ": " + held->use + " This item is " +
		            item_kind_words(item.type) +
		            (refuse ? ", never what the engine looks for there: give it an id of its own."
		                    : ", which the engine takes in its place."),
		    "id", address);
	} else if (const def::ReservedItem *named = reserved_item_named(item.display_name);
	           named && !ids.count(def::DEF_ITEM_ID_BASE + named->type)) {
		// Named as a place or an objective the engine finds by an id the file lacks.
		add(DiagnosticSeverity::Warning, CatalogFinding::ReservedName,
		    "This item is named as " + reserved_item_words(*named) + ", which the engine finds only by id " +
		            std::to_string(def::DEF_ITEM_ID_BASE + named->type) + ": " + named->use + " On id " + id +
		            " the engine takes it for an item like any other.",
		    "id", address);
	}
	// The row every lookup that finds nothing resolves to [orig: ItemList_FindIndexByTypeId @ 0x49e100,
	// 0 on a miss], which a player of a class with no item spawns as [orig: Entity_SpawnFromAnimSlotProperty
	// @ 0x43c390, @0x43C3CA] and the pool walks and the script checks take for no item [orig:
	// Entity_UpdateVehiclePhysics @ 0x48BDD9; Entity_IsOnTopOfChain @ 0x4F1A35]: retail's is a marker no
	// mission places, "Null" (id 100000).
	if (!first_row || (item.type == DEF_ITEM_TYPE_MARKER && !held)) return;
	const std::string is = held ? reserved_item_words(*held) + " (id " + id + ")" : item_kind_words(item.type);
	add(DiagnosticSeverity::Warning, CatalogFinding::FirstRow,
	    "The engine gives every id it finds no item of the first row's item, a player of a class with no item "
	    "spawns as it, and its script checks take an entity of it for no item at all. Retail's first row is a "
	    "marker no mission places (\"Null\", id 100000); this one is " + is + ".",
	    held ? "id" : "type", address);
}

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

// A repeated name's fix (DI-11): a name of its own, the one a Duplicate's copy of the record would take
// (catalog_copy_name), `taken` (the names of its kind in the file, upper case) grown by it so the next
// fix gives another; what the game then makes of the two by their kind's rule (kRepeatedNames). None for
// a carry limit, whose name is a class the game's weapons belong to, nor for a name of no copy rule.
void own_name_fix(Diagnostic &d, const Node &row, const NodeAddress &address, std::vector<std::string> &taken) {
	const DefRecordKind kind = def_kind(row.kind);
	if (kind == DefRecordKind::Carry) return;
	const std::string own = catalog_copy_name(row.kind, row.name(), taken, catalog_row(row).native.data());
	if (own.empty() || strutil::to_upper(own) == strutil::to_upper(row.name())) return;
	taken.push_back(strutil::to_upper(own));
	const std::string name = row.name();
	std::string after;
	switch (kind) {
	case DefRecordKind::Weapon:
		after = "the game then keeps both weapons, the earlier " + name + " as written and this one as " + own + ".";
		break;
	case DefRecordKind::Ammo:
		after = "a lookup by the name then finds it; a weapon naming " + name + " still fires the earlier one.";
		break;
	case DefRecordKind::Powerup:
		after = "an item's powerupdef can then bind it; one naming " + name + " still binds the earlier one.";
		break;
	default: after = "a lookup by the name then finds it; one of " + name + " still finds the earlier one."; break;
	}
	Edit set;
	set.address = address;
	set.field = catalog_name_field(row.kind);
	set.value = own;
	d.planned.push_back({"Name it " + own, "Sets its name to " + own + ", the name a copy of it takes: " + after, {set}});
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
			finding_code(CatalogFinding::IgnoredInput), findings, locate,
			"The game reads on past it [orig: ItemDef_ParseProperty @ 0x49eb00; WeaponDefs_ParseLineCallback @ 0x543680]: "
			"a build packs the file as it stands, and a save is refused while the input stands.",
			&finding_code(CatalogFinding::ReaderStops));
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
	std::set<int> item_ids;                     // every item's id: a reserved one the file has
	const Node *first_item = nullptr;           // the row every lookup that finds nothing resolves to
	// Every name of each kind (upper case), which a name of its own a fix gives keeps clear of.
	std::map<NodeKind, std::vector<std::string>> names_of;
	for (const auto &row : catalog->rows()) {
		names_of[row->kind].push_back(strutil::to_upper(row->name()));
		if (def_kind(row->kind) != DefRecordKind::Item) continue;
		item_ids.insert(catalog_row(*row).native.as<DefItemDef>().id);
		if (!first_item) first_item = row.get();
	}
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
			if (!first.second) {
				add(DiagnosticSeverity::Warning, CatalogFinding::NameDuplicate, repeated_name(kind, *first.first->second), name_field,
				    address);
				own_name_fix(findings.back(), *row, address, names_of[row->kind]);
			}
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
			// The id the engine looks this item up by, on the item a lookup finds (the first of its id).
			if (first.second) reserved_findings(item, item_ids, row.get() == first_item, add, address);
		}
	}
	return findings;
}
} // namespace opennova::editor
