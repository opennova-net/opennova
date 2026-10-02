#include "def_catalog_document.h"

#include <base/io/strutil.h>
#include <editor/assets/asset_kinds.h>
#include <editor/model/diagnostic.h>
#include <editor/project/project_files.h>

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
	return result;
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
		rules.made(row->native.data(), others);
	}
	shape(*row);
	return row;
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
	if (std::filesystem::path(model).extension().empty()) model += ".3di";
	use.scope = strutil::to_upper(model);
}

void DefCatalogDocument::refine_symbol(const NodeAddress &address, SymbolFacts &facts) const {
	if (address.child || def_kind(address.kind) != DefRecordKind::Item) return;
	if (const void *item = record(address)) facts.value = std::to_string(static_cast<const DefItemDef *>(item)->type);
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
