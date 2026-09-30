#include "strings_document.h"

#include <base/io/cp1252.h>
#include <base/io/strutil.h>
#include <editor/assets/asset_kinds.h>
#include <editor/documents/source_issue_findings.h>
#include <editor/project/project_files.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <set>

namespace opennova::editor {
namespace {

constexpr NodeKind kSection = node_kind(StringsKind::Section);
constexpr NodeKind kString = node_kind(StringsKind::String);
constexpr size_t kNameWidth = 128;
constexpr size_t kTextWidth = 4096;

StringsSection &section_of(Node &node) { return static_cast<StringsSection &>(node); }
const StringsSection &section_of(const Node &node) { return static_cast<const StringsSection &>(node); }

// The entry an address names inside a section, or null: the one at `hint` (its place in the
// document's index, Document::placement) when the identity there is the address's, else the
// one a scan finds (a row an edit or the saved baseline holds in another order).
rtxt::Entry *entry_of(StringsSection &section, const NodeAddress &address, size_t hint) {
	if (address.kind != kString || address.child == 0 || section.collections.empty()) return nullptr;
	const auto &ids = section.collections[0];
	size_t index = hint;
	if (index >= ids.size() || ids[index] != address.child) {
		const auto found = std::find(ids.begin(), ids.end(), address.child);
		if (found == ids.end()) return nullptr;
		index = size_t(found - ids.begin());
	}
	return index < section.entries.size() ? &section.entries[index] : nullptr;
}

FieldSchema field(const char *id, FieldType type, size_t width, const char *label, const char *description) {
	FieldSchema schema;
	schema.id = id;
	schema.type = type;
	schema.width = width;
	schema.label = label;
	schema.description = description;
	return schema;
}

// What docs/interface/rtxt-strings-re.md says of each field: a section is found by its name,
// the first of a name; a string by its key in its section, the first of a key; the text is
// stored in cp1252; the position is a pair of 16-bit numbers no witnessed reader uses.
const std::vector<FieldSchema> &section_fields() {
	static const std::vector<FieldSchema> fields = [] {
		std::vector<FieldSchema> out = {
			field("name", FieldType::Text, kNameWidth, "Name",
			      "A lookup finds the section by this name, in any case: the first section of a name."),
		};
		out[0].code_page = true; // stored in cp1252 (set_text)
		return out;
	}();
	return fields;
}

// A string's key is the string id the game and the other files look it up by, in its table
// and section (field_on).
const std::vector<FieldSchema> &string_fields() {
	static const std::vector<FieldSchema> fields = [] {
		std::vector<FieldSchema> out = {
			field("key", FieldType::Text, kNameWidth, "Key",
			      "The string id a lookup finds in the section, in any case: the first string of a key."),
			field("text", FieldType::Text, kTextWidth, "Text",
			      "Stored in Windows-1252, the game's text encoding: a character it has no byte for is refused."),
			field("x", FieldType::Integer, 0, "Position X", "A 16-bit layout hint the table stores; no reader of it in the game is witnessed."),
			field("y", FieldType::Integer, 0, "Position Y", "A 16-bit layout hint the table stores; no reader of it in the game is witnessed."),
		};
		out[0].defines = ReferenceKind::TextId;
		out[1].multiline = true;
		// Both stored in cp1252 (set_text): their widths count characters.
		out[0].code_page = true;
		out[1].code_page = true;
		for (size_t coordinate : {size_t(2), size_t(3)}) {
			out[coordinate].group = "Position";
			out[coordinate].applies = Applicability::Unverified;
		}
		return out;
	}();
	return fields;
}

// The characters a message names, each as itself and its code point (a check mark and
// " (U+2713)"), five at most.
std::string named_characters(const std::u32string &characters) {
	std::string out;
	for (size_t i = 0; i < characters.size() && i < 5; ++i) {
		if (i) out += ", ";
		utf8_append(out, characters[i]);
		char code[16];
		std::snprintf(code, sizeof(code), " (U+%04X)", unsigned(characters[i]));
		out += code;
	}
	if (characters.size() > 5) out += ", ...";
	return out;
}

// A text stored as the game reads it, in cp1252: a character cp1252 has no byte for is
// refused, never written as UTF-8 bytes the game would show as other characters.
bool set_text(std::string &target, const Value &value, size_t width, std::string &error) {
	const auto *text = std::get_if<std::string>(&value);
	if (!text) { error = "This field takes text."; return false; }
	if (text->find('\0') != std::string::npos) { error = "Text cannot contain a NUL byte."; return false; }
	std::string stored;
	std::u32string unstorable;
	if (!utf8_to_cp1252(*text, stored, &unstorable)) {
		error = "The game's text encoding (Windows-1252) has no " + named_characters(unstorable) + ".";
		return false;
	}
	if (stored.size() >= width) { error = "The text is too long."; return false; }
	target = stored;
	return true;
}

bool set_coordinate(int16_t &target, const Value &value, std::string &error) {
	const auto *number = std::get_if<int64_t>(&value);
	if (!number || *number < INT16_MIN || *number > INT16_MAX) { error = "A position is a whole number within the table's 16-bit range."; return false; }
	target = static_cast<int16_t>(*number);
	return true;
}

} // namespace

StringsSection::StringsSection() {
	kind = kSection;
	collections.resize(1);
}

bool is_strings_kind(AssetKind kind) {
	return asset_kind_row(kind).document == DocumentTypeId::Strings;
}

const std::vector<RecordKindRow> &StringsDocument::kinds() const {
	static const std::vector<RecordKindRow> table = {
	        {kSection, "section", "Section", "Add section", true},
	        {kString, "string", "String"},
	};
	return table;
}

// A section holds its strings; a string holds nothing.
std::vector<Document::Collection> StringsDocument::collections(const Node &row, const NodeAddress &owner) const {
	if (row.kind != kSection || owner.child || row.collections.empty()) return {};
	return {{{kString, "Strings", "key"}, row.collections[0]}};
}

const std::vector<FieldSchema> &StringsDocument::schema(NodeKind kind) {
	static const std::vector<FieldSchema> none;
	return kind == kSection ? section_fields() : kind == kString ? string_fields() : none;
}

size_t StringsDocument::place_of(const NodeAddress &address) const {
	Placement at;
	return placement(address, at) ? at.index : SIZE_MAX;
}

void StringsDocument::refine_field(const NodeAddress &address, FieldUse &use) const {
	const Node *section = row(address.row);
	if (use.defines == ReferenceKind::TextId && section)
		use.scope = strutil::to_upper(basename_of(path())) + "/" + section->name();
}

void StringsDocument::refine_symbol(const NodeAddress &address, SymbolFacts &facts) const {
	// A lookup reads the first section of a name [orig: TextResource_FindEntryBySectionAndKey @
	// 0x75d250]: the ids of a later one are defined but never read.
	const Node *section = row(address.row);
	if (!section) return;
	for (const auto &node : rows()) {
		if (node->id == section->id) return;
		if (strutil::iequals(node->name(), section->name())) {
			facts.inert = true;
			facts.inert_reason = "an earlier section " + node->name() + " of the table shadows its section, and a lookup reads the first";
			return;
		}
	}
}

bool StringsDocument::read(const Node &row, const NodeAddress &address, const std::string &field, Value &out) const {
	const StringsSection &section = section_of(row);
	if (address.kind == kSection && !address.child) {
		if (field != "name") return false;
		out = retail_text_to_utf8(section.section_name);
		return true;
	}
	const rtxt::Entry *entry = entry_of(const_cast<StringsSection &>(section), address, place_of(address));
	if (!entry) return false;
	if (field == "key") out = retail_text_to_utf8(entry->key);
	else if (field == "text") out = retail_text_to_utf8(entry->text);
	else if (field == "x") out = int64_t(entry->position.x);
	else if (field == "y") out = int64_t(entry->position.y);
	else return false;
	return true;
}

rtxt::File StringsDocument::table() const {
	rtxt::File file;
	for (const auto &node : rows()) {
		const StringsSection &section = section_of(*node);
		const auto index = static_cast<uint32_t>(file.sections.size());
		file.sections.push_back({section.section_name, static_cast<uint32_t>(section.entries.size())});
		for (const rtxt::Entry &entry : section.entries) {
			rtxt::Entry stored = entry;
			stored.section_index = index;
			file.entries.push_back(stored);
		}
	}
	return file;
}

SerializeResult StringsDocument::serialize() const {
	SerializeResult result;
	if (blocked()) {
		for (const auto &issue : issues()) if (issue.blocks) result.issues.push_back(issue);
		return result;
	}
	std::vector<uint8_t> bytes;
	std::string error;
	if (!rtxt::write(table(), bytes, error)) {
		result.issues.push_back({true, 0, "", "", error});
		return result;
	}
	result.text.assign(bytes.begin(), bytes.end());
	return result;
}

bool StringsDocument::parse(const std::vector<uint8_t> &bytes, std::vector<std::shared_ptr<Node>> &rows,
                            std::shared_ptr<const FileState> &, std::vector<SourceIssue> &issues, Diagnostic &error) {
	if (!is_strings_kind(kind())) {
		error = make_finding(CoreFinding::DocumentKind, DiagnosticSeverity::Error, "This file is not a string table.", path());
		return false;
	}
	rtxt::File file;
	std::string message;
	// The empty-file guard of the loader hands a single NUL for an empty file; a
	// table with no bytes is an empty table.
	if (!(bytes.size() == 1 && bytes[0] == 0) && !rtxt::parse(bytes.data(), bytes.size(), file, message)) {
		error = make_finding(CoreFinding::DocumentParse, DiagnosticSeverity::Error, message, path());
		return false;
	}
	if (!file.is_grouped()) {
		// The engine reads a table by accumulated section counts, so an ungrouped
		// table already reads differently from how it was written; saving regroups it. The
		// finding points at the first string the game reads under another section than its
		// own, where regrouping puts it (the sort keeps each section's strings in order).
		SourceIssue issue{false, 0, "", "", "The strings are not grouped by section the way the game reads them; saving regroups them."};
		size_t section = 0, left = file.sections.empty() ? 0 : file.sections[0].string_count;
		for (size_t i = 0; i < file.entries.size(); ++i) {
			while (left == 0 && section < file.sections.size())
				left = ++section < file.sections.size() ? file.sections[section].string_count : 0;
			const uint32_t own = file.entries[i].section_index;
			if (section < file.sections.size() && own == section) {
				--left;
				continue;
			}
			if (own < file.sections.size()) {
				size_t index = 0;
				for (size_t j = 0; j < i; ++j) index += file.entries[j].section_index == own ? 1 : 0;
				issue.record = file.sections[own].name;
				issue.field = "key";
				issue.locator = std::to_string(own) + "/string:" + std::to_string(index);
			}
			break;
		}
		issues.push_back(std::move(issue));
		file.normalize_grouping();
	}
	std::vector<std::shared_ptr<StringsSection>> sections;
	for (const rtxt::Section &section : file.sections) {
		auto row = std::make_shared<StringsSection>();
		row->section_name = section.name;
		sections.push_back(row);
	}
	for (const rtxt::Entry &entry : file.entries) {
		if (entry.section_index >= sections.size()) {
			issues.push_back({true, 0, entry.key, "key", "A string names a section the table does not have."});
			continue;
		}
		sections[entry.section_index]->entries.push_back(entry);
	}
	for (auto &row : sections) {
		row->collections[0].resize(row->entries.size());
		rows.push_back(row);
	}
	return true;
}

// A copy under its original's name would never be read: a lookup finds the first section of a
// name [orig: TextResource_FindEntryBySectionAndKey @ 0x75D250, @0x75D2B0]. The copy takes the
// name's stem (its trailing digits dropped), cut where the number would not fit the field (the
// bytes are cp1252, one per character), and the first number whose name no section has.
void StringsDocument::prepare_duplicate(Node &copy) const {
	std::set<std::string> taken;
	for (const auto &row : rows()) taken.insert(strutil::to_upper(row->name()));
	std::string &name = section_of(copy).section_name;
	std::string stem = name;
	while (!stem.empty() && std::isdigit(static_cast<unsigned char>(stem.back()))) stem.pop_back();
	if (stem.empty()) stem = name;
	for (int n = 2;; ++n) {
		const std::string number = std::to_string(n);
		const std::string candidate = stem.substr(0, std::min(stem.size(), kNameWidth - 1 - number.size())) + number;
		if (!taken.count(strutil::to_upper(candidate))) {
			name = candidate;
			return;
		}
	}
}

std::shared_ptr<Node> StringsDocument::make_node(NodeKind kind, NodeId id, std::string &error) {
	if (kind != kSection) { error = "A string table adds sections at the top level."; return nullptr; }
	auto row = std::make_shared<StringsSection>();
	row->section_name = "Section" + std::to_string(id);
	return row;
}

bool StringsDocument::set_field(Node &node, const NodeAddress &address, const std::string &field, const Value &value,
                                std::string &error) {
	StringsSection &section = section_of(node);
	if (address.kind == kSection && !address.child) {
		if (field != "name") { error = "Unknown field."; return false; }
		return set_text(section.section_name, value, kNameWidth, error);
	}
	rtxt::Entry *entry = entry_of(section, address, place_of(address));
	if (!entry) { error = "The string no longer exists."; return false; }
	if (field == "key") return set_text(entry->key, value, kNameWidth, error);
	if (field == "text") return set_text(entry->text, value, kTextWidth, error);
	if (field == "x") return set_coordinate(entry->position.x, value, error);
	if (field == "y") return set_coordinate(entry->position.y, value, error);
	error = "Unknown field.";
	return false;
}

bool StringsDocument::edit_collection(Node &node, const Edit &edit, const IdAllocator &allocate, NodeId &added,
                                      std::string &error) {
	StringsSection &section = section_of(node);
	if (edit.address.kind != kString) { error = "A section holds strings only."; return false; }
	auto &ids = section.collections[0];
	auto &entries = section.entries;
	const size_t index = size_t(std::find(ids.begin(), ids.end(), edit.address.child) - ids.begin());
	if (edit.operation != EditOperation::Add && index == entries.size()) { error = "The string no longer exists."; return false; }
	switch (edit.operation) {
	case EditOperation::Add:
	case EditOperation::Duplicate: {
		rtxt::Entry entry;
		if (edit.operation == EditOperation::Duplicate) entry = entries[index];
		else entry.key = "NEW_STRING";
		const size_t position = std::min(edit.position, entries.size());
		entries.insert(entries.begin() + static_cast<std::ptrdiff_t>(position), entry);
		added = allocate();
		ids.insert(ids.begin() + static_cast<std::ptrdiff_t>(position), added);
		return true;
	}
	case EditOperation::Remove:
		entries.erase(entries.begin() + static_cast<std::ptrdiff_t>(index));
		ids.erase(ids.begin() + static_cast<std::ptrdiff_t>(index));
		return true;
	case EditOperation::Move: {
		const size_t to = std::min(edit.position, entries.size() - 1);
		const rtxt::Entry entry = entries[index];
		const NodeId id = ids[index];
		entries.erase(entries.begin() + static_cast<std::ptrdiff_t>(index));
		entries.insert(entries.begin() + static_cast<std::ptrdiff_t>(to), entry);
		ids.erase(ids.begin() + static_cast<std::ptrdiff_t>(index));
		ids.insert(ids.begin() + static_cast<std::ptrdiff_t>(to), id);
		return true;
	}
	default:
		error = "This collection cannot accept that edit.";
		return false;
	}
}

namespace {

constexpr FindingCodeEntry<StringsFinding> kFindingEntries[] = {
	{ StringsFinding::InvalidInput, { "strings.invalid_input", FindingFix::None, nullptr, true } },
	{ StringsFinding::Regrouped, { "strings.regrouped", FindingFix::Rewrite,
				"with its strings grouped by section the way the game reads them" } },
	{ StringsFinding::SectionEmpty, { "strings.section_empty" } },
	{ StringsFinding::SectionDuplicate, { "strings.section_duplicate" } },
	{ StringsFinding::KeyEmpty, { "strings.key_empty" } },
	{ StringsFinding::KeyDuplicate, { "strings.key_duplicate" } },
};
static_assert(std::size(kFindingEntries) == static_cast<size_t>(StringsFinding::kCount),
		"every StringsFinding has exactly one row");
static_assert(finding_entries_well_formed(kFindingEntries),
		"the string table's rows follow StringsFinding's order, each token its own");
constexpr auto kFindingRows = finding_rows(kFindingEntries, FindingGroup::StringTables);
static_assert(finding_rows_well_formed(kFindingRows), "every row of the table takes its group");

} // namespace

const FindingCodeRow &finding_code(StringsFinding code) {
	return kFindingRows[static_cast<size_t>(code)];
}

FindingTable strings_finding_codes() { return { kFindingRows.data(), kFindingRows.size() }; }

std::vector<Diagnostic> validate_strings_file(const DocumentBase &document) {
	std::vector<Diagnostic> findings;
	const auto *strings = dynamic_cast<const StringsDocument *>(&document);
	if (!strings) return findings;
	// On the string it names, when the table holds it, wherever it is now (source_address; a
	// string of a section the table does not have, or one removed since: the file alone).
	source_issue_findings(*strings, finding_code(StringsFinding::InvalidInput),
			finding_code(StringsFinding::Regrouped), findings);
	if (document.blocked()) return findings;
	// The sections by the reader's rule (rtxt::File::section_index): a lookup finds the first
	// section of a name, in any case, so a later one of the name is never read by section (the
	// flat key walk still reads its strings [orig: TextResource_FindEntryByKey @ 0x75D450]). An
	// empty name is the editor's rule, as an empty key is.
	rtxt::File names;
	for (const auto &node : strings->rows()) names.sections.push_back({section_of(*node).section_name, 0});
	for (size_t s = 0; s < names.sections.size(); ++s) {
		const Node &node = *strings->rows()[s];
		const std::string &name = names.sections[s].name;
		const size_t found = names.section_index(name);
		if (!name.empty() && found == s) continue;
		auto diagnostic = name.empty()
		        ? make_finding(StringsFinding::SectionEmpty, DiagnosticSeverity::Error,
		                       "Enter a name for this section: a lookup finds a section by its name.", document.path(), "name")
		        : make_finding(StringsFinding::SectionDuplicate, DiagnosticSeverity::Warning,
		                       "Section " + std::to_string(found + 1) + " is named '" + retail_text_to_utf8(names.sections[found].name) +
		                               "' too, and a lookup by section reads the first: this one's strings are reached only by a "
		                               "lookup of the key alone.",
		                       document.path(), "name");
		diagnostic.record = retail_text_to_utf8(name);
		diagnostic.row_id = node.id;
		diagnostic.record_kind = kSection;
		findings.push_back(std::move(diagnostic));
	}
	for (const auto &node : strings->rows()) {
		const StringsSection &section = section_of(*node);
		std::set<std::string> keys;
		for (size_t i = 0; i < section.entries.size(); ++i) {
			const rtxt::Entry &entry = section.entries[i];
			auto add = [&](DiagnosticSeverity severity, StringsFinding code, const std::string &message) {
				auto diagnostic = make_finding(code, severity, message, document.path(), "key");
				diagnostic.record = section.section_name;
				diagnostic.row_id = node->id;
				diagnostic.child_id = section.collections[0][i];
				diagnostic.record_kind = kString;
				findings.push_back(std::move(diagnostic));
			};
			if (entry.key.empty()) add(DiagnosticSeverity::Error, StringsFinding::KeyEmpty, "Enter a key for this string in section '" + section.section_name + "'.");
			else if (!keys.insert(strutil::to_upper(entry.key)).second)
				add(DiagnosticSeverity::Warning, StringsFinding::KeyDuplicate,
				    "Section '" + section.section_name + "' has more than one '" + entry.key + "'; the game reads the first.");
		}
	}
	return findings;
}

} // namespace opennova::editor
