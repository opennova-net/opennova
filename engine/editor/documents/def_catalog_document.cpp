#include "def_catalog_document.h"

#include <base/io/strutil.h>
#include <editor/assets/asset_kinds.h>
#include <editor/documents/catalog_validation.h>
#include <editor/graph/reference_kinds.h>
#include <editor/documents/texture_roles.h>
#include <editor/model/diagnostic.h>
#include <editor/project/project_files.h>
#include <formats/grm/grm.h>
#include <runtime/renderer/texture_load_rules.h>

#include <array>
#include <filesystem>
#include <optional>
#include <set>

namespace opennova::editor {
using namespace def;
namespace {

// Whether the game's own reader stops at the input, or corrupts the record over it, by the def readers'
// sites (catalog.reader_stops): a `weapon` line inside an open weapon ends the walk [orig:
// WeaponDefs_ParseLineCallback @ 0x5436ad..0x5436d2], as an `ammo` line inside an open ammo does [orig:
// AmmoDef_LoadAll @ 0x40b0b0] (each a MalformedBlock on its line's key, formats/def/def_weapons.cpp and
// def_ammo.cpp); a fifth `sights` row lands on the row count [orig: WeaponDefs_ParseLineCallback, the count
// bump @ 0x544b11..0x544b20] (an Unrepresentable on its key). Any other blocking input the game reads on past.
bool game_stops(const DefIssue &issue) {
	if (issue.code == DefIssueCode::MalformedBlock)
		return strutil::iequals(issue.field, "weapon") || strutil::iequals(issue.field, "ammo");
	return issue.code == DefIssueCode::Unrepresentable && strutil::iequals(issue.field, "sights");
}

std::vector<SourceIssue> source_issues(const DefParseReport &report) {
	std::vector<SourceIssue> issues;
	for (const DefIssue &issue : report) {
		issues.push_back({issue.blocks(), issue.line, issue.record, issue.field, issue.message});
		issues.back().game_stops = issue.blocks() && game_stops(issue);
	}
	return issues;
}

const ItemsFileState *items_state(const FileState *state) { return dynamic_cast<const ItemsFileState *>(state); }

// A def's texture field's loader argument (ADR 0046 S18, documents/texture_roles.h): its role by the field
// (renderer::def_texture_field_role, its witnesses); none (-1) for a field whose loader the game is not
// witnessed using: the name as written. (An item's shadow_texture the game never loads: refine_field marks it
// ignored.)
int32_t def_texture_role_arg(const std::string &field) {
	renderer::TextureRoleId role = renderer::TextureRoleId::kCount;
	return renderer::def_texture_field_role(field, role) ? texture_role_arg(role) : -1;
}

} // namespace

bool is_catalog_kind(AssetKind kind) {
	return asset_kind_row(kind).document == DocumentTypeId::Catalog;
}

// --- the row -----------------------------------------------------------------------------------------

CatalogRow::CatalogRow(NodeKind k) : native(def_kind(k)) { kind = k; }

CatalogRow::CatalogRow(NodeKind k, CatalogRecord record) : native(std::move(record)) { kind = k; }

RecordHandle CatalogRow::record() const { return {kind, const_cast<void *>(native.data())}; }

size_t CatalogRow::footprint() const { return sizeof(CatalogRow) + native.footprint() + ids_footprint(); }

std::string CatalogRow::name() const {
	const TableKind *own = catalog_table().kind(kind);
	const size_t place = own ? own->find(catalog_name_field(kind)) : TableKind::npos;
	Value value;
	if (place == TableKind::npos || !own->value(place).get(record(), value)) return std::string();
	const auto *text = std::get_if<std::string>(&value);
	return text ? *text : std::string();
}

// --- the declarations ----------------------------------------------------------------------------------

const void *DefCatalogDocument::record(const NodeAddress &address) const {
	const Node *node = row(address.row);
	const RecordHandle handle = node ? record_in(*node, address) : RecordHandle();
	return handle.data;
}

const std::vector<int> &DefCatalogDocument::spawn_ids() const {
	static const std::vector<int> none;
	const auto *state = items_state(file_state());
	return state ? state->spawn_ids : none;
}

const std::vector<RecordKindRow> &DefCatalogDocument::kinds() const {
	// Each family's kinds, in the table's order, made once.
	static const std::array<std::vector<RecordKindRow>, kAssetKindCount> per_kind = [] {
		std::array<std::vector<RecordKindRow>, kAssetKindCount> out;
		for (size_t k = 0; k < kAssetKindCount; ++k) {
			const CatalogFamily *family = catalog_family(AssetKind(k));
			if (!family) continue;
			for (const RecordKindRow &row : catalog_table().kinds())
				if (family->holds(row.kind)) out[k].push_back(row);
		}
		return out;
	}();
	// A file of no family the catalog opens (never one it loads) answers every kind of the table.
	return family() ? per_kind[size_t(kind())] : catalog_table().kinds();
}

bool DefCatalogDocument::parse(const std::vector<uint8_t> &bytes, std::vector<std::shared_ptr<Node>> &rows,
                               std::shared_ptr<const FileState> &state, std::vector<SourceIssue> &issues,
                               Diagnostic &error) {
	const CatalogFamily *own = family();
	if (!is_catalog_kind(kind()) || !own) {
		error = make_finding(CoreFinding::DocumentKind, DiagnosticSeverity::Error, "This file has no catalog editor.", path());
		return false;
	}
	DefParseReport report;
	std::vector<CatalogRecord> records;
	if (!own->parse(bytes, records, state, report)) {
		error = make_finding(CoreFinding::DocumentParse, DiagnosticSeverity::Error, "The file could not be read.", path());
		return false;
	}
	for (CatalogRecord &record : records) {
		const NodeKind row_kind = node_kind(record.kind());
		auto row = std::make_shared<CatalogRow>(row_kind, std::move(record));
		shape(*row);
		rows.push_back(row);
	}
	issues = source_issues(report);
	return true;
}

SerializeResult DefCatalogDocument::serialize() const {
	SerializeResult result;
	if (blocked()) {
		for (const auto &issue : issues()) if (issue.blocks) result.issues.push_back(issue);
		return result;
	}
	const CatalogFamily *own = family();
	if (!own) return result;
	std::vector<const CatalogRecord *> records;
	for (const auto &row : rows()) records.push_back(&static_cast<const CatalogRow &>(*row).native);
	DefWriteResult written = own->write(records, file_state());
	result.text = std::move(written.text);
	result.issues = source_issues(written.diagnostics);
	for (const std::string &record : written.rewritten) result.notes.push_back(record);
	for (const std::string &record : written.reordered)
		result.notes.push_back(record + ": written in the table's order, as its lines in the file's order would read "
		                                "back otherwise (an edit they cannot carry as they stand).");
	return result;
}

std::string DefCatalogDocument::save_words() const {
	// What the writer makes of the file's form (def_notes.h: the layout modeled as the file is read, def.h's
	// DefLineOrder and DefLayout): it generates every line from the records and that data, never the file's
	// bytes (ADR 0003, the maintainer's ruling of 2026-10-04; itemdef-re.md D-ITEMDEF-4).
	const size_t ignored = ignored_lines();
	std::string words = "Saving writes the file in the form it was read in: its spacing, its comments, how each number "
	                    "or word is spelled";
	if (ignored)
		words += ", and the " + std::to_string(ignored) + (ignored == 1 ? " thing" : " things") +
		         " in it the game skips (Problems lists each)";
	words += ". A changed line keeps its spacing and its comment, the words that changed in the editor's form; a new "
	         "line or record is the editor's, after the lines of its record; a record whose edit its lines in the file's "
	         "form cannot carry is written in the editor's form, its comment lines and the lines the game skips kept, "
	         "and Output names it and what it does not keep";
	return words + ". The game reads the same " + (kind() == AssetKind::ItemDefs ? "items" : kind() == AssetKind::WeaponDefs ? "weapons"
	                                                     : kind() == AssetKind::AmmoDefs ? "ammo" : "rows") + ".";
}

std::shared_ptr<Node> DefCatalogDocument::make_node(
		NodeKind kind, NodeId id, const std::vector<std::shared_ptr<const Node>> &rows,
		std::string &error) {
	const RecordKindRow *top = kind_row(kind);
	if (!top || !top->top) {
		error = "This catalog cannot add that kind of record.";
		return nullptr;
	}
	auto row = std::make_shared<CatalogRow>(kind);
	const TableKind &own = *catalog_table().kind(kind);
	const size_t name = own.find(catalog_name_field(kind));
	std::string ignored;
	if (name != TableKind::npos) own.value(name).set(row->record(), std::string("New_") + std::to_string(id), ignored);
	const CatalogKindRow &rules = catalog_kind_row(kind);
	if (rules.made) {
		std::vector<const void *> others;
		for (const auto &other : rows)
			if (other->kind == kind) others.push_back(static_cast<const CatalogRow &>(*other).native.data());
		rules.made(row->native.data(), others, item_ids_elsewhere_);
	}
	shape(*row);
	return row;
}

void DefCatalogDocument::set_names_used_elsewhere(ReferenceKind kind, const std::vector<std::string> &names) {
	if (kind != ReferenceKind::Item) return;
	item_ids_elsewhere_.clear();
	for (const std::string &name : names)
		if (const std::optional<int> id = strutil::parse_int(name)) item_ids_elsewhere_.push_back(*id);
}

bool DefCatalogDocument::accept_step(const EditStep &step, const StagedRows &rows, StepRefusal &refusal) const {
	if (!duplicate_refusal_.empty()) {
		refusal.message = duplicate_refusal_;
		duplicate_refusal_.clear();
		return false;
	}
	// A place or an objective the engine finds by its id keeps it (ReservedItemRule::Refuse): a step that
	// moves such an item off its id, or gives its id to an item of another kind, is refused; one that
	// leaves a mismatch the file already had as it was is not (retail's own items.def repeats 102044 on a
	// powerup row, which stays editable). Every other field of the item stays free.
	for (const RowSwap &swap : step.swaps) {
		if (!swap.after || def_kind(swap.after->kind) != DefRecordKind::Item) continue;
		const auto &after = static_cast<const CatalogRow &>(*swap.after).native.as<DefItemDef>();
		const DefItemDef *before = swap.before && def_kind(swap.before->kind) == DefRecordKind::Item
		                                   ? &static_cast<const CatalogRow &>(*swap.before).native.as<DefItemDef>()
		                                   : nullptr;
		std::string why, field;
		if (reserved_change_allowed(before, after, why, field)) continue;
		refusal.message = why;
		refusal.code = &finding_code(CatalogFinding::ReservedRefused);
		refusal.record = {swap.after->id, swap.after->kind, 0};
		refusal.record_name = swap.after->name();
		refusal.field = field;
		return false;
	}
	return TableDocument::accept_step(step, rows, refusal);
}

// --- the ids the engine fixes ---------------------------------------------------------------------------

// "a marker", "an effect", "an item of no type": an item's type as a message names it.
std::string item_kind_words(int type) {
	const std::string name = type ? def_item_type_name(type) : "item of no type";
	return (std::string("aeiou").find(name.front()) != std::string::npos ? "an " : "a ") + name;
}

// "the Insertion point, a marker" / "the Parachute": a reserved row as a message names it.
std::string reserved_item_words(const ReservedItem &row) {
	std::string words = std::string("the ") + row.label;
	if (row.kind) words += ", " + item_kind_words(row.kind);
	return words;
}

bool reserved_change_allowed(const DefItemDef *before, const DefItemDef &after, std::string &why, std::string &field) {
	// Off its id: an item the engine finds by its id, of the kind it looks for, moved to another.
	if (before && before->id != after.id) {
		const ReservedItem *held = reserved_item_by_id(before->id);
		if (held && held->rule == ReservedItemRule::Refuse && reserved_item_kind_matches(*held, before->type)) {
			why = "The engine finds " + reserved_item_words(*held) + ", by id " + std::to_string(before->id) + ": " +
			      held->use + " This item keeps the id; add another item for a new one.";
			field = "id";
			return false;
		}
	}
	// Onto its id, or there with another kind: refused where the step makes the mismatch.
	const ReservedItem *taken = reserved_item_by_id(after.id);
	if (!taken || taken->rule != ReservedItemRule::Refuse || reserved_item_kind_matches(*taken, after.type)) return true;
	if (before && before->id == after.id && !reserved_item_kind_matches(*taken, before->type)) return true;
	std::string what = item_kind_words(after.type);
	what[0] = 'A';
	why = "The engine keeps id " + std::to_string(after.id) + " for " + reserved_item_words(*taken) + ": " + taken->use +
	      " " + what + " is never what it looks for there; give this item an id of its own.";
	field = before && before->id == after.id ? "type" : "id";
	return false;
}

const ReservedItem *reserved_item_named(const std::string &name) {
	const std::string wanted = strutil::to_upper(strutil::trim(name));
	if (wanted.empty()) return nullptr;
	size_t count = 0;
	const ReservedItem *rows = reserved_items(&count);
	// The places and objectives retail ships a row of, by its name there or the editor's words for it (a
	// flag of no retail row is named "Flag", as a decoration flag may be).
	for (size_t i = 0; i < count; ++i)
		if (rows[i].rule == ReservedItemRule::Refuse && *rows[i].retail &&
		    (strutil::to_upper(rows[i].retail) == wanted || strutil::to_upper(rows[i].label) == wanted))
			return &rows[i];
	return nullptr;
}

std::vector<Edit> reserved_item_add_edits(const ReservedItem &row) {
	std::vector<Edit> edits;
	Edit add;
	add.operation = EditOperation::Add;
	add.address.kind = node_kind(DefRecordKind::Item);
	add.field = "display_name";
	add.value = std::string(row.label);
	edits.push_back(std::move(add));
	// The new item is a marker of an id of its own (made_item): its id and its kind the engine's.
	Edit id;
	id.address = {batch_made(0), node_kind(DefRecordKind::Item), 0};
	id.field = "id";
	id.value = int64_t(DEF_ITEM_ID_BASE + row.type);
	edits.push_back(std::move(id));
	if (row.kind && row.kind != DEF_ITEM_TYPE_MARKER) {
		Edit kind;
		kind.address = {batch_made(0), node_kind(DefRecordKind::Item), 0};
		kind.field = "type";
		kind.value = int64_t(row.kind);
		edits.push_back(std::move(kind));
	}
	return edits;
}

void DefCatalogDocument::prepare_record(const Node &, const ListChange &change, DetachedRecord &record) const {
	if (change.operation == EditOperation::Duplicate && record.data) def_clear_notes(def_kind(record.kind), record.data.get());
}

void DefCatalogDocument::prepare_duplicate(Node &copy, const Node &,
                                           const std::vector<std::shared_ptr<const Node>> &rows) const {
	auto &row = static_cast<CatalogRow &>(copy);
	// A copy is written in the writer's own form: the file's modeled lines are its original's (def_notes.h).
	def_clear_notes(row.native.kind(), row.native.data());
	const CatalogKindRow &rules = catalog_kind_row(row.kind);
	// The rows of its kind beside it (its original among them), whose ids and names it keeps apart from.
	std::vector<const void *> others;
	std::vector<std::string> names;
	for (const auto &other : rows) {
		if (other->kind != row.kind) continue;
		const auto &held = static_cast<const CatalogRow &>(*other);
		others.push_back(held.native.data());
		names.push_back(strutil::to_upper(held.name()));
	}
	if (rules.duplicated) rules.duplicated(row.native.data(), others, item_ids_elsewhere_);
	// A name of its own: the game's lookups by name find the first row of a name (an item, an ammo), and a
	// weapon block of a name the table has replaces that weapon (itemdef-re.md, "Repeated names and ids").
	const std::string name = catalog_copy_name(row.kind, row.name(), names, row.native.data());
	if (name.empty()) return;
	const TableKind &own = *catalog_table().kind(row.kind);
	const size_t place = own.find(catalog_name_field(row.kind));
	std::string refused;
	// A name that cannot be set (a character the game's code page lacks) refuses the copy, saying why: a
	// copy under its original's name would be the one no lookup finds.
	if (!own.value(place).set(row.record(), name, refused))
		duplicate_refusal_ = "The copy's name \"" + name + "\" cannot be set: " + refused;
}

bool DefCatalogDocument::set_file_value(std::shared_ptr<const FileState> &state, const Edit &edit, Diagnostic &error) {
	const CatalogFamily *own = family();
	std::string message;
	if (!own || !own->set_file_value || !own->set_file_value(state, edit, message)) {
		error = make_finding(CoreFinding::DocumentValue, DiagnosticSeverity::Error,
		                     message.empty() ? "This document has no file-wide values." : message, path());
		return false;
	}
	return true;
}

std::shared_ptr<const FileState> DefCatalogDocument::state_after_remove(const std::shared_ptr<const FileState> &state,
                                                                        size_t remaining) const {
	const CatalogFamily *own = family();
	return own && own->after_remove ? own->after_remove(state, remaining) : state;
}

bool DefCatalogDocument::same_file_state(const FileState *a, const FileState *b) const {
	const CatalogFamily *own = family();
	return own && own->same_state ? own->same_state(a, b) : a == b;
}

void DefCatalogDocument::after_edit(Node &node) {
	auto &row = static_cast<CatalogRow &>(node);
	if (const auto derive = catalog_kind_row(row.kind).after_edit) derive(row.native.data());
}

void DefCatalogDocument::refine_field(const NodeAddress &address, FieldUse &use) const {
	// What the table's labelled field decides on its record (none of a catalog's decides: the rows say
	// it all), then the catalog's own rules.
	TableDocument::refine_field(address, use);
	// An item's shadow line's TGA name, stored and never loaded: the game names no file by it [orig:
	// ItemDef_ParseProperty @ 0x49f3a5..0x49f44c stores it at +0xA0 and loads nothing; the blob decal it
	// would draw, RenderSlot_DrawAuthoredBlobDecal @ 0x5d59d0, is dead in JO, its ItemDef+0x114 gate never
	// assigned (render-lighting-re)]. Its width and length the shadow slot reads (@ 0x5d572a..0x5d5754).
	if (use.schema->id == "shadow_texture" && def_kind(address.kind) == DefRecordKind::Item) {
		use.applies = Applicability::Ignored;
		return;
	}
	// A texture through its use's loader, by its role (def_texture_role_arg).
	if (use.reference == ReferenceKind::Texture) {
		use.loader_arg = def_texture_role_arg(use.schema->id);
		return;
	}
	// A bit per id of the file-wide registry (record_choices).
	if (use.schema->id == "vehicle_spawn_mask" && def_kind(address.kind) == DefRecordKind::Item) {
		use.own_choices = true;
		return;
	}
	if (use.reference != ReferenceKind::UserPoint && use.reference != ReferenceKind::AnimationKey) return;
	// What the record's row names (an item's graphic, a weapon's models and map: a nested record's lookups read
	// its owner's).
	const Node *record = row(address.row);
	const auto owner_names = [&](const char *field) {
		Value value;
		if (!record || !get({address.row, record->kind, 0}, field, value) || !std::holds_alternative<std::string>(value))
			return std::string();
		return std::get<std::string>(value);
	};
	const DefRecordKind kind = def_kind(address.kind);
	const std::string &id = use.schema->id;
	if (use.reference == ReferenceKind::AnimationKey) {
		// An action's slot is found in its weapon's map [orig: Anim_InitActions @ 0x5421BD, the weapon's map at
		// +0x174]; a weapon naming none has none ("Error, need to define a anim adm" @ 0x54218D).
		use.scope = animation_map_scope(owner_names("animadm"));
		if (use.scope.empty()) use.reference = ReferenceKind::None;
		return;
	}
	// The model whose user points the lookup reads; with none nothing is looked up.
	std::string model;
	bool first_16 = false;
	if (kind == DefRecordKind::Item && id.rfind("particlefx", 0) == 0) {
		// An item's particle slot: the graphic's first 16 [orig: Game_ResolveItemMaterialsAndSpawnBoneTrails @
		// 0x522ee0 over ItemDef_GetBoneMaskByName @ 0x49ea40].
		model = owner_names("graphic");
		first_16 = true;
	} else if (kind == DefRecordKind::Item && id == "virtual_display_userpoint") {
		// The driver's eye in the cockpit model [orig: EntityDef_LoadModelsAndCallbacks @ 0x43A5DD..0x43A632].
		model = owner_names("virtual_display");
	} else if (kind == DefRecordKind::Item || kind == DefRecordKind::Attachment) {
		// The weapon points, the launch points, a mounted gun's point: the item's graphic [orig:
		// Entity_InitBoneReferences @ 0x441470; Entity_InitOrganicAI @ 0x4BFE8F; Entity_InitFromModel @ 0x40DE30].
		model = owner_names("graphic");
	} else if (kind == DefRecordKind::Weapon || kind == DefRecordKind::Action) {
		// A weapon's launch point and an action's effect point on its third-person model [orig:
		// WeaponDef_ResolveAllReferences @ 0x5402EF, @ 0x5403E7]; an action's on its first-person model too, an
		// edge of its own (catalog_references), and there alone where the weapon has no third-person model.
		model = owner_names("gfx3");
		if (model.empty() && kind == DefRecordKind::Action) model = owner_names("gfx1");
	}
	use.scope = user_point_scope(model, first_16);
	if (use.scope.empty()) use.reference = ReferenceKind::None;
}

void catalog_references(const Document &document, Extracted &out) {
	const auto *catalog = dynamic_cast<const DefCatalogDocument *>(&document);
	if (!catalog) return;
	if (catalog->kind() == AssetKind::ItemDefs) {
		// A person's face: the game opens the face animation its model's name makes for an entity of a person's
		// item, where the shadow quality is above 0 [orig: Entity_InitFromModel @ 0x40E211..0x40E22E, the type 3
		// @ 0x40E1C3 / 0x40E21A -> sub_57FCE0 @ 0x57FCE0]; an entity's item is the first row of its id [orig:
		// ItemList_FindIndexByTypeId @ 0x49E100], so a later row of an id makes none. The name is derived, never
		// written, so no rename rewrites it.
		std::set<int> seen;
		for (const auto &row : document.rows()) {
			if (!row || def_kind(row->kind) != DefRecordKind::Item) continue;
			const auto &item = static_cast<const CatalogRow &>(*row).native.as<DefItemDef>();
			if (!seen.insert(item.id).second || item.type != DEF_ITEM_TYPE_PERSON || !item.graphic[0]) continue;
			const NodeAddress address{row->id, row->kind, 0};
			GraphEdge edge;
			edge.source = document.path();
			edge.record = document.record_path(address);
			edge.locator = document.locator(address);
			edge.address = address;
			edge.field = "graphic";
			edge.kind = ReferenceKind::FaceAnimation;
			edge.value = grm::face_file_name(item.graphic);
			edge.rewritable = false;
			edge.optional = true; // the game shows no face for a person whose face it lacks, and says nothing
			out.edges.push_back(std::move(edge));
		}
		return;
	}
	if (catalog->kind() != AssetKind::WeaponDefs) return;
	// An action's effect point is looked up on its weapon's first-person model as well as its third-person one
	// [orig: WeaponDef_ResolveAllReferences @ 0x540377 (+57) and @ 0x5403E7 (+56), each over
	// modelgpm_FindUserpointByName @ 0x5B2170]: the field's reference is the third-person model's
	// (refine_field), this the first-person one's, where the two are different files.
	const NodeKind action_kind = node_kind(CatalogKind::Action);
	for (const auto &row : document.rows()) {
		if (!row || def_kind(row->kind) != DefRecordKind::Weapon) continue;
		const NodeAddress weapon{row->id, row->kind, 0};
		Value gfx1, gfx3;
		if (!document.get(weapon, "gfx1", gfx1) || !document.get(weapon, "gfx3", gfx3)) continue;
		const auto *first = std::get_if<std::string>(&gfx1);
		const auto *third = std::get_if<std::string>(&gfx3);
		if (!first || !third || first->empty() || third->empty()) continue;
		const std::string scope = user_point_scope(*first, false);
		if (scope == user_point_scope(*third, false)) continue;
		document.walk_records(*row, [&](const NodeAddress &nested, const Document::Placement &) {
			Value point;
			if (nested.kind != action_kind || !document.get(nested, "particleuserpoint", point)) return true;
			const auto *name = std::get_if<std::string>(&point);
			// A name its reader reads as none is no reference, as the field's (reference_target).
			if (!name || name->empty() || strutil::iequals(*name, "none") || strutil::iequals(*name, "null")) return true;
			GraphEdge edge;
			edge.source = document.path();
			edge.record = document.record_path(nested);
			edge.locator = document.locator(nested);
			edge.address = nested;
			edge.field = "particleuserpoint";
			edge.kind = ReferenceKind::UserPoint;
			edge.value = *name;
			edge.scope = scope;
			edge.rewritable = true;
			out.edges.push_back(std::move(edge));
			return true;
		});
	}
}

void DefCatalogDocument::refine_symbol(const NodeAddress &address, SymbolFacts &facts) const {
	if (address.child) return;
	const void *native = record(address);
	if (!native) return;
	if (def_kind(address.kind) == DefRecordKind::Item) facts.value = std::to_string(static_cast<const DefItemDef *>(native)->type);
	// A weapon's loadout name's key, which its words read where the HUD's has no text (definition_words).
	if (def_kind(address.kind) == DefRecordKind::Weapon) {
		facts.value = static_cast<const DefWeaponDef *>(native)->loadout_menu_textid;
		// A later block of the name reopens this one's row, reset to its defaults, so the game keeps nothing of
		// this block; "null" names no row [orig: WeaponDefs_ParseLineCallback @ 0x5436d7 ->
		// AvatarDef_FindIndexByName @ 0x53fd80 (stricmp, "null" none), AdmDef_InitEntryDefaults @ 0x543722].
		const Node *node = row(address.row);
		if (!node || node->name().empty() || strutil::iequals(node->name(), "null")) return;
		bool after = false;
		for (const auto &other : rows()) {
			if (!other) continue;
			if (other.get() == node) {
				after = true;
				continue;
			}
			if (after && other->kind == node->kind && strutil::iequals(other->name(), node->name())) {
				facts.inert = true;
				facts.inert_reason = "a later block of the name reopens its row, and the game keeps nothing of this one";
				return;
			}
		}
	}
}

// --- a name another file names, added (DI-15) -------------------------------------------------------------

namespace {

// The row kind a reference kind names, and what a new row of it holds before its lines are read, in words: the
// record the game's reader opens for it (def_init_record), stamped before any key is parsed: an item's physics
// block [orig: ItemDef_AllocateWithDefaults @ 0x49E3B0, called by the `begin` arm of ItemDef_ParseProperty @
// 0x49EB00], a weapon's field of view, scope floor and stability [orig: AdmDef_InitEntryDefaults @ 0x53ff31,
// @ 0x53FF61..0x53FF73], an ammo's velocity and age (taken from the table's first ammo at its `end`), drag,
// recoil and turn rates [orig: AmmoDef_AllocateSlot @ 0x409A20, the defaults @ 0x409A56..0x409AF6;
// AmmoDef_InheritDefaults @ 0x409EB0], a powerup's block zeroed [orig: the `end` arm's memset of
// PowerUpDef_ParseProperty @ 0x442F5D..0x443017].
bool catalog_kind_of(ReferenceKind kind, CatalogKind &out, const char *&what, const char *&defaults) {
	switch (kind) {
	case ReferenceKind::Weapon:
		out = CatalogKind::Weapon;
		what = "a weapon";
		defaults = "the values the game gives a new weapon before its lines are read (its field of view 80, its scope "
		           "magnification floor 2, full stability), no action and no ammo yet";
		return true;
	case ReferenceKind::Ammo:
		out = CatalogKind::Ammo;
		what = "an ammo";
		defaults = "the values the game gives a new ammo before its lines are read (its velocity and age the table's "
		           "first ammo's, drag 1, recoil 24), no effect rows yet";
		return true;
	case ReferenceKind::Item:
		out = CatalogKind::Item;
		what = "an item";
		defaults = "the physics values the game gives every item before its lines are read";
		return true;
	case ReferenceKind::Powerup:
		out = CatalogKind::Powerup;
		what = "a powerup";
		defaults = "a powerup's block as the game opens it, empty";
		return true;
	default: return false;
	}
}

} // namespace

bool define_catalog_symbol(const DocumentBase &document, const ReferenceSubject &missing, PlannedFix &out) {
	const auto *catalog = dynamic_cast<const DefCatalogDocument *>(&document);
	CatalogKind kind = CatalogKind::Item;
	const char *what = "", *defaults = "";
	if (!catalog || !catalog->family() || missing.target.empty() || !catalog_kind_of(missing.kind, kind, what, defaults) ||
	    !catalog->family()->holds(node_kind(kind)))
		return false;
	const std::string file = basename_of(document.path());
	out = PlannedFix();
	if (kind != CatalogKind::Item) {
		// A row named as the reference names it: the first row of a name is what a lookup finds (an ammo's, a
		// powerup's), and a weapon block of a name no block has adds that weapon.
		Edit add;
		add.operation = EditOperation::Add;
		add.address.kind = node_kind(kind);
		add.field = catalog_name_field(node_kind(kind));
		add.value = missing.target;
		out.edits.push_back(std::move(add));
		out.label = "Add " + missing.target + " to " + file;
		out.detail = "Adds " + std::string(what) + " named " + missing.target + " at the end of " + file + ", with " +
		             defaults + ", and selects it to fill in: the game's lookup then finds it.";
		return true;
	}
	// An item is named by its id: the id the reference names, by the reserved-id rule. An id the engine keeps for
	// a place, an objective or a model takes the item the engine looks for there, of its kind and named in the
	// editor's words for it (reserved_item_add_edits, Add engine item...'s row); any other id a new item, a
	// marker as an Add makes one, on that id.
	const std::optional<int> id = strutil::parse_int(missing.target);
	if (!id || *id < DEF_ITEM_ID_BASE) return false;
	if (const ReservedItem *reserved = reserved_item_by_id(*id)) {
		out.edits = reserved_item_add_edits(*reserved);
		out.label = "Add " + std::string(reserved->label) + " (" + missing.target + ") to " + file;
		out.detail = "Adds " + reserved_item_words(*reserved) + " on id " + missing.target + ", the id the engine keeps for it (" +
		             reserved->use + "), at the end of " + file + ", with " + defaults +
		             ", and selects it to fill in: a lookup by the id then finds it.";
		return true;
	}
	Edit add;
	add.operation = EditOperation::Add;
	add.address.kind = node_kind(kind);
	out.edits.push_back(std::move(add));
	Edit set;
	set.address = {batch_made(0), node_kind(kind), 0};
	set.field = "id";
	set.value = int64_t(*id);
	out.edits.push_back(std::move(set));
	out.label = "Add item " + missing.target + " to " + file;
	out.detail = "Adds an item on id " + missing.target + " at the end of " + file + ", a marker as a new item is, with " +
	             defaults + ", and selects it to fill in (its name, its type, its model): a lookup by the id then finds "
	             "it.";
	return true;
}

bool DefCatalogDocument::record_choices(const NodeAddress &address, const FieldUse &use,
		std::vector<FieldChoice> &out) const {
	if (use.schema->id != "vehicle_spawn_mask" || def_kind(address.kind) != DefRecordKind::Item)
		return false;
	// A bit per id of the file-wide registry, in its slot's order [orig: @0x4A0253; @0x49DFC0].
	const std::vector<int> &ids = spawn_ids();
	for (size_t i = 0; i < ids.size() && i < size_t(DEF_VEHICLE_SPAWN_SLOTS); ++i)
		out.push_back({std::to_string(ids[i]), int64_t(uint32_t(1) << i), ""});
	return true;
}

} // namespace opennova::editor
