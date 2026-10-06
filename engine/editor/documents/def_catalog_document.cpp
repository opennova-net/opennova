#include "def_catalog_document.h"

#include <base/io/strutil.h>
#include <editor/assets/asset_kinds.h>
#include <editor/documents/catalog_validation.h>
#include <editor/graph/reference_kinds.h>
#include <editor/documents/texture_roles.h>
#include <editor/model/diagnostic.h>
#include <editor/project/project_files.h>
#include <runtime/renderer/texture_load_rules.h>

#include <array>
#include <filesystem>

namespace opennova::editor {
using namespace def;
namespace {

std::vector<SourceIssue> source_issues(const DefParseReport &report) {
	std::vector<SourceIssue> issues;
	for (const DefIssue &issue : report)
		issues.push_back({issue.blocks(), issue.line, issue.record, issue.field, issue.message});
	return issues;
}

const ItemsFileState *items_state(const FileState *state) { return dynamic_cast<const ItemsFileState *>(state); }

// A def's texture field's role (ADR 0046 S18, documents/texture_roles.h), by the field: HUD art alpha
// only for a weapon's HUD icon, its clip and round graphics, its crosshair and an item's HUD image
// [orig: HUD_LoadAllTextures @ 0x59E248..0x59E26A, the ItemDef's +0xA74 in mode 1]; a menu image for a
// weapon's loadout icon [orig: CTextureManager_LoadOrFindTexture @ 0x654980]; a sight card for a
// sight's texture (render-material-re.md "The game's texture loaders", the role table). None (-1) for a
// field whose loader the game is not witnessed using (a weapon's crosshair_secondary): the name as
// written. (An item's shadow_texture the game never loads: refine_field marks it ignored.)
int32_t def_texture_role_arg(const std::string &field) {
	if (field == "hudicon" || field == "hudclipgfx_texture" || field == "hudrndgfx_texture" || field == "crosshair" ||
	    field == "hud_image")
		return texture_role_arg(TextureRoleId::HudAlphaOnly);
	if (field == "loadout_menu_icon") return texture_role_arg(TextureRoleId::MenuImage);
	if (field == "texture") return texture_role_arg(TextureRoleId::SightCard);
	return -1;
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
	const std::string name = catalog_copy_name(row.kind, row.name(), names);
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
	if (use.reference != ReferenceKind::UserPoint) return;
	// The user point is looked up on the item's graphic model [orig:
	// Game_ResolveItemMaterialsAndSpawnBoneTrails @ 0x522ee0]; with no graphic nothing is.
	const Node *record = row(address.row);
	Value graphic;
	if (!record || !get({address.row, record->kind, 0}, "graphic", graphic) || !std::holds_alternative<std::string>(graphic) ||
	    std::get<std::string>(graphic).empty()) {
		use.reference = ReferenceKind::None;
		return;
	}
	std::string model = basename_of(std::get<std::string>(graphic));
	if (path_of(model).extension().empty()) model += ".3di";
	use.scope = strutil::to_upper(model);
}

void DefCatalogDocument::refine_symbol(const NodeAddress &address, SymbolFacts &facts) const {
	if (address.child) return;
	const void *native = record(address);
	if (!native) return;
	if (def_kind(address.kind) == DefRecordKind::Item) facts.value = std::to_string(static_cast<const DefItemDef *>(native)->type);
	// A weapon's loadout name's key, which its words read where the HUD's has no text (definition_words).
	if (def_kind(address.kind) == DefRecordKind::Weapon) facts.value = static_cast<const DefWeaponDef *>(native)->loadout_menu_textid;
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
