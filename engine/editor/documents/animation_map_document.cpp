#include "animation_map_document.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <unordered_map>

#include <base/io/strutil.h>
#include <editor/assets/asset_kinds.h>
#include <editor/documents/animation_slots.h>
#include <editor/documents/source_issue_findings.h>
#include <editor/graph/reference_kinds.h>
#include <editor/model/id_list.h>
#include <editor/project/project_files.h>
#include <runtime/anim/adm_clip_index.h>
#include <runtime/anim/anim_slot_names.h>

namespace opennova::editor {
namespace {

constexpr NodeKind kRow = node_kind(AnimationMapKind::Row);
constexpr NodeKind kClip = node_kind(AnimationMapKind::Clip);
constexpr size_t kTextWidth = sizeof(adm::AdmEntry::key); // 64, the terminator included

// The slot keys the engine looks rows up by: "anim_" and each of its 252 slot names
// [orig: AnimMap_FindSlotByName @ 0x40CFA0 over g_AnimStateNameTable @ 0x8135F0].
const std::vector<std::string> &slot_keys() {
	static const std::vector<std::string> keys = [] {
		std::vector<std::string> out;
		for (int i = 0; i < anim::kAnimSlotCount; ++i) out.push_back(std::string("anim_") + anim::kAnimSlotNames[i]);
		return out;
	}();
	return keys;
}

// The slots as the editor names them: the slot's words apart ("anim_walk_forward": "walk
// forward"); the key, the token, stays the value the file writes.
const std::vector<std::string> &slot_labels() {
	static const std::vector<std::string> labels = [] {
		std::vector<std::string> out;
		for (int i = 0; i < anim::kAnimSlotCount; ++i) {
			std::string label = anim::kAnimSlotNames[i];
			std::replace(label.begin(), label.end(), '_', ' ');
			out.push_back(std::move(label));
		}
		return out;
	}();
	return labels;
}

bool has_reset(const std::vector<std::shared_ptr<const Node>> &rows) {
	for (const auto &r : rows)
		if (r && adm::adm_key_names_slot(static_cast<const AnimationMapRow &>(*r).key, "reset")) return true;
	return false;
}

AnimationMapRow &row_of(Node &node) { return static_cast<AnimationMapRow &>(node); }
const AnimationMapRow &row_of(const Node &node) { return static_cast<const AnimationMapRow &>(node); }

size_t clip_index(const AnimationMapRow &row, NodeId id) {
	const auto &ids = row.collections[0];
	const auto found = std::find(ids.begin(), ids.end(), id);
	return found == ids.end() ? SIZE_MAX : size_t(found - ids.begin());
}

bool set_text(std::string &target, const Value &value, std::string &error) {
	const std::string *text = std::get_if<std::string>(&value);
	if (!text) {
		error = "This field takes text.";
		return false;
	}
	if (text->size() + 1 > kTextWidth || text->find('\0') != std::string::npos) {
		error = "At most 63 characters.";
		return false;
	}
	target = *text;
	return true;
}

adm::AdmEntry entry_of(const AnimationMapRow &row) {
	adm::AdmEntry e{};
	std::snprintf(e.key, sizeof(e.key), "%s", row.key.c_str());
	e.variant_count = std::min<size_t>(row.clips.size(), size_t(adm::ADM_MAX_VARIANTS) + 1);
	for (size_t v = 0; v < row.clips.size() && v < size_t(adm::ADM_MAX_VARIANTS); ++v)
		std::snprintf(e.variants[v], sizeof(e.variants[v]), "%s", row.clips[v].c_str());
	return e;
}

} // namespace

size_t AnimationMapRow::footprint() const {
	size_t bytes = sizeof(AnimationMapRow) + collections_footprint() + footprint_of(key) +
	               footprint_of(clips);
	for (const std::string &clip : clips) bytes += footprint_of(clip);
	return bytes;
}

AnimationMapRow::AnimationMapRow() {
	kind = kRow;
	collections.resize(1);
}

bool is_animation_map_kind(AssetKind kind) {
	return asset_kind_row(kind).document == DocumentTypeId::AnimationMap;
}

std::string animation_key_title(const std::string &key) {
	// The slot keys by their lower case, as the key's choices name them.
	static const std::unordered_map<std::string, size_t> slots = [] {
		std::unordered_map<std::string, size_t> out;
		for (size_t i = 0; i < slot_keys().size(); ++i) out.emplace(strutil::to_lower(slot_keys()[i]), i);
		return out;
	}();
	const auto found = slots.find(strutil::to_lower(key));
	return found == slots.end() ? key : slot_labels()[found->second];
}

std::string AnimationMapDocument::record_title(const NodeAddress &address) const {
	const std::string name = record_name(address);
	if (address.child) return name;
	const Node *node = row(address.row);
	if (!node || node->kind != kRow) return animation_key_title(name);
	const AnimationMapRow &r = row_of(*node);
	// The slot the game reads the key as (past its first five characters, any case), else the key
	// as written: the game skips its row.
	const int slot = animation_key_slot(r.key);
	std::string title = slot >= 0 ? animation_slot_words(slot) : r.key + " (no slot)";
	if (r.clips.empty()) return title + ": no clip";
	title += ": " + r.clips[0];
	if (r.clips.size() == 2) title += ", " + r.clips[1];
	else if (r.clips.size() > 2) title += ", " + r.clips[1] + " and " + std::to_string(r.clips.size() - 2) + " more";
	return title;
}

void animation_map_row_headings(const Document &document, const NameSource *,
		std::vector<std::vector<RowHeading>> &out) {
	out.clear();
	for (const auto &node : document.rows()) {
		std::vector<RowHeading> headings;
		if (node && node->kind == kRow) {
			const int family = animation_slot_family(animation_key_slot(row_of(*node).key));
			const AnimSlotFamily &heading =
					family >= 0 ? animation_slot_family_row(family) : animation_slot_unknown_family();
			headings.push_back({heading.key, heading.heading});
		}
		out.push_back(std::move(headings));
	}
}

const std::vector<RecordKindRow> &AnimationMapDocument::kinds() const {
	static const std::vector<RecordKindRow> table = {
	        {kRow, "row", "Row", "Add row", true},
	        {kClip, "clip", "Clip"},
	};
	return table;
}

std::vector<Document::Collection> AnimationMapDocument::collections(const Node &row, const NodeAddress &owner) const {
	if (row.kind != kRow || owner.child != 0) return {};
	CollectionSpec clips{kClip, "Clips (served last to first)", "clip"};
	clips.max = adm::ADM_MAX_VARIANTS;
	return {{clips, row.collections[0]}};
}

const std::vector<FieldSchema> &AnimationMapDocument::schema(NodeKind kind) {
	static const std::vector<FieldSchema> row_fields = [] {
		FieldSchema key;
		key.id = "key";
		key.type = FieldType::Text;
		key.width = kTextWidth;
		key.label = "Slot";
		key.description = "The slot the game plays this row's clips in: the key past its first five characters, "
		                  "in any case. A key naming none of the 252 slots registers nothing.";
		for (size_t i = 0; i < slot_keys().size(); ++i)
			key.choices.push_back({slot_keys()[i].c_str(), int64_t(i), slot_labels()[i].c_str()});
		// Any key is written as typed: the game looks the slot up by it [orig: AnimMap_FindSlotByName
		// @ 0x40CFA0], and the validator names one that finds none.
		key.open_choices = true;
		// The slot it plays in this map, which a weapon action names by the same key [orig: Anim_InitActions @
		// 0x54219E]: scoped to the map's file (refine_field).
		key.defines = ReferenceKind::AnimationKey;
		return std::vector<FieldSchema>{key};
	}();
	static const std::vector<FieldSchema> clip_fields = [] {
		FieldSchema clip;
		clip.id = "clip";
		clip.type = FieldType::Text;
		clip.width = kTextWidth;
		clip.reference = ReferenceKind::Animation;
		clip.label = "Clip";
		return std::vector<FieldSchema>{clip};
	}();
	static const std::vector<FieldSchema> none;
	return kind == kRow ? row_fields : kind == kClip ? clip_fields : none;
}

void AnimationMapDocument::refine_field(const NodeAddress &, FieldUse &use) const {
	if (use.defines == ReferenceKind::AnimationKey) use.scope = animation_map_scope(path());
}

void AnimationMapDocument::refine_symbol(const NodeAddress &address, SymbolFacts &facts) const {
	// A key naming none of the 252 slots registers nothing: the game skips the row [orig: AnimMap_ParseConfigLine
	// @ 0x40CB60, the test @ 0x40CBA4].
	Value key;
	if (address.child != 0 || !get(address, "key", key) || !std::holds_alternative<std::string>(key)) return;
	if (animation_key_slot(std::get<std::string>(key)) >= 0) return;
	facts.inert = true;
	facts.inert_reason = "its key names none of the engine's 252 slots, so the game skips the row";
}

bool AnimationMapDocument::read(const Node &node, const NodeAddress &address, const std::string &field, Value &out) const {
	if (node.kind != kRow) return false;
	const AnimationMapRow &r = row_of(node);
	if (address.child == 0) {
		if (field != "key") return false;
		out = r.key;
		return true;
	}
	const size_t i = clip_index(r, address.child);
	if (i == SIZE_MAX || field != "clip") return false;
	out = r.clips[i];
	return true;
}

std::vector<adm::AdmEntry> AnimationMapDocument::entries() const {
	std::vector<adm::AdmEntry> out;
	for (const auto &node : rows())
		if (node) out.push_back(entry_of(row_of(*node)));
	return out;
}

SerializeResult AnimationMapDocument::serialize() const {
	SerializeResult result;
	if (blocked()) {
		for (const auto &issue : issues()) if (issue.blocks) result.issues.push_back(issue);
		return result;
	}
	const std::vector<adm::AdmEntry> table = entries();
	for (size_t i = 0; i < table.size(); ++i)
		if (const char *problem = adm::adm_row_problem(table[i]))
			result.issues.push_back({true, 0, table[i].key, "key", problem, locator({rows()[i]->id, kRow, 0})});
	if (!has_reset(rows()))
		result.issues.push_back({true, 0, "", "key", "The table has no anim_reset row: the game cannot load it."});
	if (!result.issues.empty()) return result;
	adm::AdmFile file{};
	file.entries = const_cast<adm::AdmEntry *>(table.data());
	file.count = table.size();
	if (adm::adm_write_buffer(&file, result.text) != 0)
		result.issues.push_back({true, 0, "", "", "The table could not be written."});
	return result;
}

bool AnimationMapDocument::parse(const std::vector<uint8_t> &bytes, std::vector<std::shared_ptr<Node>> &rows,
                                 std::shared_ptr<const FileState> &, std::vector<SourceIssue> &issues, Diagnostic &error) {
	if (!is_animation_map_kind(kind())) {
		error = make_finding(CoreFinding::DocumentKind, DiagnosticSeverity::Error, "This file is not an animation map.", path());
		return false;
	}
	adm::AdmFile file{};
	std::vector<adm::AdmDroppedLine> dropped;
	if (adm::adm_parse_buffer(reinterpret_cast<const char *>(bytes.data()), bytes.size(), &file, &dropped) != 0) {
		error = make_finding(CoreFinding::DocumentParse, DiagnosticSeverity::Error, "The animation map could not be read.", path());
		return false;
	}
	// Each line whose input the table leaves out: what the game ignores is dropped on save; a
	// row the table cannot hold blocks the file. On the row the line keeps, else the file.
	for (const adm::AdmDroppedLine &line : dropped)
		issues.push_back({line.blocks, line.line, line.key, line.row == SIZE_MAX ? "" : "key", line.what,
		                  line.row == SIZE_MAX ? std::string() : std::to_string(line.row)});
	for (size_t i = 0; i < file.count; ++i) {
		auto row = std::make_shared<AnimationMapRow>();
		row->key = file.entries[i].key;
		for (size_t v = 0; v < file.entries[i].variant_count; ++v) row->clips.push_back(file.entries[i].variants[v]);
		row->collections[0].resize(row->clips.size());
		rows.push_back(row);
	}
	adm::adm_free(&file);
	return true;
}

std::shared_ptr<Node> AnimationMapDocument::make_node(
		NodeKind kind, NodeId, const std::vector<std::shared_ptr<const Node>> &rows,
		std::string &error) {
	if (kind != kRow) {
		error = "A table adds rows at the top level.";
		return nullptr;
	}
	auto row = std::make_shared<AnimationMapRow>();
	row->key = has_reset(rows) ? "anim_idle" : "anim_reset";
	return row;
}

bool AnimationMapDocument::set_field(Node &node, const NodeAddress &address, const std::string &field, const Value &value,
                                     std::string &error) {
	AnimationMapRow &r = row_of(node);
	if (address.child == 0) {
		if (field != "key") {
			error = "Unknown field.";
			return false;
		}
		return set_text(r.key, value, error);
	}
	const size_t i = clip_index(r, address.child);
	if (i == SIZE_MAX || field != "clip") {
		error = "The clip no longer exists.";
		return false;
	}
	return set_text(r.clips[i], value, error);
}

bool AnimationMapDocument::edit_collection(Node &node, const Edit &edit, const IdAllocator &allocate, NodeId &added,
                                           std::string &error) {
	AnimationMapRow &r = row_of(node);
	const bool grows = edit.operation == EditOperation::Add || edit.operation == EditOperation::Duplicate;
	if (grows && r.clips.size() >= size_t(adm::ADM_MAX_VARIANTS)) {
		error = "A row names at most 8 clips.";
		return false;
	}
	return edit_id_list(r.clips, r.collections[0], edit, std::string(), allocate, added, error);
}

namespace {

constexpr FindingCodeEntry<AnimationMapFinding> kFindingEntries[] = {
	// A row of more than 8 clips: the game registers every one (unwritable_code: a closed table packs as
	// stored, its Save refused).
	{ AnimationMapFinding::InvalidInput, unwritable_code("animation_map.invalid_input") },
	{ AnimationMapFinding::IgnoredInput, { "animation_map.ignored_input", FindingFix::Rewrite, kRewriteDropsIgnoredInput } },
	// The load returns no map [orig: AnimMap_LoadAdmFile @ 0x40cc40, the reset row's test @ 0x40ce03]:
	// that map is not loaded, and no refusal of the game's load or run past it is witnessed: listed
	// (the gate follows retail, ADR 0046 S14).
	{ AnimationMapFinding::NoReset, listed_code("animation_map.no_reset", FindingFix::ResetRow) },
	// A row the writer cannot write as it reads back: from a file, a key no slot has (registered for nothing) or
	// a clip name registered as written, which the game reads on; from an edit, a Save refused
	// (document.unserializable). Listed.
	{ AnimationMapFinding::Row, listed_code("animation_map.row") },
	{ AnimationMapFinding::KeyUnknown, { "animation_map.key_unknown" } },
	{ AnimationMapFinding::SlotRepeated, { "animation_map.slot_repeated" } },
};
static_assert(std::size(kFindingEntries) == static_cast<size_t>(AnimationMapFinding::kCount),
		"every AnimationMapFinding has exactly one row");
static_assert(finding_entries_well_formed(kFindingEntries),
		"the animation map's rows follow AnimationMapFinding's order, each token its own");
constexpr auto kFindingRows = finding_rows(kFindingEntries, FindingGroup::AnimationMaps);
static_assert(finding_rows_well_formed(kFindingRows), "every row of the table takes its group");

} // namespace

const FindingCodeRow &finding_code(AnimationMapFinding code) {
	return kFindingRows[static_cast<size_t>(code)];
}

FindingTable animation_map_finding_codes() { return { kFindingRows.data(), kFindingRows.size() }; }

std::vector<Diagnostic> validate_animation_map_file(const DocumentBase &document) {
	std::vector<Diagnostic> findings;
	const auto *table = dynamic_cast<const AnimationMapDocument *>(&document);
	if (!table) return findings;
	// The lines the table leaves out, each on its line (Problems shows it) and on the row it
	// was read into, wherever that row is now (source_address; gone: the file): input the
	// game ignores is dropped on save (a warning, Rewrite drops it); a row the table cannot
	// hold blocks the file (an error).
	source_issue_findings(
			*table, finding_code(AnimationMapFinding::InvalidInput),
			finding_code(AnimationMapFinding::IgnoredInput), findings, nullptr,
			"A build packs the table as it stands; a save, which would drop the clips past the eighth, is refused.");
	if (document.blocked()) return findings;
	if (!has_reset(table->rows())) {
		// On the first row's key, where a row takes the name (its Add anim_reset row fix
		// adds one instead).
		Diagnostic d = make_finding(AnimationMapFinding::NoReset, DiagnosticSeverity::Error,
		                            "The table has no anim_reset row: the game cannot load it.", document.path(), "key");
		if (!table->rows().empty()) {
			const Node &first = *table->rows().front();
			d.row_id = first.id;
			d.record_kind = kRow;
			d.record = row_of(first).key;
		}
		findings.push_back(std::move(d));
	}
	// The first row naming each slot, by the slot's index.
	std::unordered_map<int, size_t> first_of_slot;
	for (size_t i = 0; i < table->rows().size(); ++i) {
		const Node *node = table->rows()[i].get();
		const AnimationMapRow &r = row_of(*node);
		const auto add = [&](DiagnosticSeverity severity, AnimationMapFinding code, const std::string &message) {
			Diagnostic d = make_finding(code, severity, message, document.path(), "key");
			d.row_id = node->id;
			d.record_kind = kRow;
			d.record = r.key;
			findings.push_back(std::move(d));
		};
		const adm::AdmEntry e = entry_of(r);
		const int slot = anim::adm_slot_index(r.key);
		if (const char *problem = adm::adm_row_problem(e))
			add(DiagnosticSeverity::Error, AnimationMapFinding::Row,
			    std::string(problem) + " The game reads the row on, as written (a key no slot has registers nothing [orig: "
			                           "AnimMap_ParseConfigLine @ 0x40cba4]); a build packs the table as it stands, and a "
			                           "save cannot write the row.");
		else if (slot < 0)
			add(DiagnosticSeverity::Warning, AnimationMapFinding::KeyUnknown,
			    "'" + r.key + "' names none of the engine's animation slots: the game skips the row.");
		if (slot < 0) continue;
		const auto first = first_of_slot.emplace(slot, i);
		if (first.second) continue;
		// A later row of a slot registers onto the same head: each clip token of a slot
		// joins its ring ahead of the head, so the rows serve as one ring, last token first;
		// slot 0's head is replaced by each reset token instead, so the last one loaded
		// serves [orig: AnimMap_ParseConfigLine @ 0x40CB60, the slot lookup per row @0x40CB97;
		// AnimMap_RegisterBoneNode @ 0x40C2D0, the ring insert @0x40C37F..0x40C385, slot 0's
		// replace @0x40C38B..0x40C38F].
		const std::string earlier = "row " + std::to_string(first.first->second + 1);
		add(DiagnosticSeverity::Info, AnimationMapFinding::SlotRepeated,
		    slot == 0 ? "'" + r.key + "' names the reset slot as " + earlier +
		                        " does: each reset clip replaces the one before, so the skeleton is the last reset "
		                        "clip it loads, and the slots the map leaves out serve the first."
		              : "'" + r.key + "' names the slot " + earlier +
		                        " names: the game joins their clips into one ring, served last to first.");
	}
	return findings;
}

bool define_animation_slot(const DocumentBase &document, const ReferenceSubject &missing, PlannedFix &out) {
	const auto *map = dynamic_cast<const AnimationMapDocument *>(&document);
	if (!map || missing.kind != ReferenceKind::AnimationKey || missing.target.empty()) return false;
	if (!missing.scope.empty() && !strutil::iequals(missing.scope, animation_map_scope(document.path()))) return false;
	const int slot = animation_key_slot(missing.target);
	if (slot < 0) return false;
	for (const auto &node : map->rows())
		if (node && animation_key_slot(row_of(*node).key) == slot) return false;
	out = PlannedFix();
	Edit add;
	add.operation = EditOperation::Add;
	add.address.kind = kRow;
	add.field = "key";
	add.value = missing.target;
	out.edits.push_back(std::move(add));
	const std::string file = basename_of(document.path());
	out.label = "Add the slot " + missing.target + " to " + file;
	out.detail = "Adds a row for the slot " + missing.target + " at the end of " + file + " and selects it: name its "
	             "clips there (the file saves once the row names one, as the game reads no row without). The "
	             "weapon's action then plays and times those clips.";
	return true;
}

} // namespace opennova::editor
