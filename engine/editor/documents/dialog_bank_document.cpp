// The dialog bank document (dialog_bank_document.h, ADR 0046 DI-32): the bank's table over its native
// records, its parse through the engine's reader (formats/dbf), its save through the engine's writer from
// the rows alone, and its findings. The fields' words say what the game does with each value, each
// witnessed in docs/audio/lwf-dbf-sound-re.md "The dialog banks".
#include "dialog_bank_document.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <unordered_map>

#include <base/io/strutil.h>
#include <editor/assets/asset_kinds.h>
#include <editor/documents/source_issue_findings.h>
#include <editor/project/project_files.h>
#include <runtime/audio/dialog_queue.h>
#include <runtime/mission/mission_sidecars.h>

namespace opennova::editor {
namespace {

constexpr NodeKind kDialog = node_kind(DialogBankKind::Dialog);
constexpr NodeKind kLine = node_kind(DialogBankKind::Line);

const BankDialog &dialog_of(const RecordHandle &r) { return r.as<BankDialog>(); }
const BankLine &line_of(const RecordHandle &r) { return r.as<BankLine>(); }

bool whole_of(const Value &value, int64_t &out) {
	if (const auto *whole = std::get_if<int64_t>(&value)) {
		out = *whole;
		return true;
	}
	if (const auto *real = std::get_if<double>(&value); real && std::isfinite(*real) && *real == std::floor(*real)) {
		out = int64_t(*real);
		return true;
	}
	return false;
}

// A name the game compares: plain printable ASCII within the bytes its field keeps.
bool set_name(std::string &field, const char *what, const Value &value, std::string &error) {
	const auto *text = std::get_if<std::string>(&value);
	if (!text) {
		error = std::string(what) + " is a text.";
		return false;
	}
	for (const unsigned char c : *text)
		if (c < 0x20 || c > 0x7E) {
			error = std::string(what) + " is plain ASCII: the game compares it byte for byte.";
			return false;
		}
	if (text->size() >= dbf::kNameBytes) {
		error = std::string(what) + " holds at most " + std::to_string(dbf::kNameBytes - 1) + " characters: the file keeps " +
		        std::to_string(dbf::kNameBytes) + " bytes for it, its terminator among them.";
		return false;
	}
	field = *text;
	return true;
}

FieldSchema schema_of(const char *id, FieldType type, const char *label, const char *description) {
	FieldSchema schema;
	schema.id = id;
	schema.type = type;
	schema.label = label;
	schema.description = description;
	return schema;
}

RecordTable make_table() {
	using RF = LabelledField;
	// --- a dialog ---------------------------------------------------------------------------------
	TableKind dialog(RecordKindRow{kDialog, "dialog", "Dialog", "Add dialog", true});
	{
		FieldSchema name = schema_of("name", FieldType::Text, "Name",
				"What the game finds the dialog by, matched exactly and the first of the name in the bank [orig: "
				"Dialog_PlayByName @ 0x44d9f0]. A mission's Play dialog action and its Dialog triggers name it by the "
				"number that forms dlg%03i (dlg012 is dialog 12) [orig: Dialog_PlayByIndex @ 0x527ae0; "
				"Dialog_ExistsByIndex @ 0x44e170], so a name no number forms is never played.");
		name.width = dbf::kNameBytes;
		name.defines = ReferenceKind::Dialog;
		dialog.field(RF{name,
		                {[](const RecordHandle &r, Value &out) { return out = dialog_of(r).name, true; },
		                 [](const RecordHandle &r, const Value &v, std::string &e) {
			                 return set_name(r.as<BankDialog>().name, "A dialog's name", v, e);
		                 }}});
		FieldSchema number = schema_of("number", FieldType::Integer, "Played as",
				"The number a mission's Play dialog action and its Dialog triggers give to play this dialog: the "
				"number its name's dlg%03i forms; -1 where no number forms it.");
		number.read_only = true;
		dialog.field(RF{number, {[](const RecordHandle &r, Value &out) {
			                return out = audio::dialog_index_of(dialog_of(r).name), true;
		                }}});
		TableList lines;
		lines.spec = Document::CollectionSpec{kLine, "Lines", "wave", false, Applicability::Reads, 0};
		lines.ops = vector_list<BankDialog, BankLine>(kLine, [](BankDialog &d) -> std::vector<BankLine> & { return d.lines; });
		dialog.list(std::move(lines));
	}
	// --- a line -----------------------------------------------------------------------------------
	TableKind line(RecordKindRow{kLine, "line", "Line", "", false});
	{
		FieldSchema wave = schema_of("wave", FieldType::Text, "Wave",
				"The wave of the dialog bank's sounds (the .lwf of this bank's name, else its .pwf) the line plays, "
				"found by its name without case and played at that wave's dialog volume [orig: Dialog_LoadAudioClip "
				"@ 0x44dcf7..0x44dd15 -> SoundBank_FindEntryByName @ 0x75bba0]. A name the sounds lack shows \"EX "
				"Cannot load audio\" in the chat and plays nothing. The mission text's [Mission Dialog] entry of this "
				"name is the line's subtitle [orig: @ 0x44ddd6].");
		wave.width = dbf::kNameBytes;
		wave.reference = ReferenceKind::BankWave;
		line.field(RF{wave,
		              {[](const RecordHandle &r, Value &out) { return out = line_of(r).wave, true; },
		               [](const RecordHandle &r, const Value &v, std::string &e) {
			               return set_name(r.as<BankLine>().wave, "A line's wave", v, e);
		               }}});
		FieldSchema sequence = schema_of("sequence", FieldType::Text, "Subtitle entry",
				"Where the mission text has no [Mission Dialog] entry of the wave's name, the subtitle is the text's "
				"entry whose number follows the last '_' here, counting every entry of the table from 0 (_00003 the "
				"fourth); none without a '_' (##) [orig: Dialog_LoadAudioClip @ 0x44ddec..0x44de3c; "
				"IniSection_GetEntryByIndex @ 0x75d130].");
		sequence.width = dbf::kNameBytes;
		line.field(RF{sequence,
		              {[](const RecordHandle &r, Value &out) { return out = line_of(r).sequence, true; },
		               [](const RecordHandle &r, const Value &v, std::string &e) {
			               return set_name(r.as<BankLine>().sequence, "A subtitle entry", v, e);
		               }}});
		FieldSchema delay = schema_of("delay", FieldType::Byte, "Wait before",
				"A line after the first waits this long once the line before it has ended [orig: Dialog_UpdatePlayback "
				"@ 0x44e585: 62 * delay / 10 ticks].");
		delay.unit = "tenths of a second";
		delay.ranged = true;
		delay.min = 0;
		delay.max = 255;
		delay.step = 1;
		line.field(RF{delay,
		              {[](const RecordHandle &r, Value &out) { return out = int64_t(line_of(r).delay), true; },
		               [](const RecordHandle &r, const Value &v, std::string &e) {
			               int64_t number = 0;
			               if (!whole_of(v, number) || number < 0 || number > 255) {
				               e = "A wait is a whole number of tenths of a second from 0 to 255.";
				               return false;
			               }
			               r.as<BankLine>().delay = uint8_t(number);
			               return true;
		               }}});
	}
	return RecordTable({std::move(dialog), std::move(line)});
}

const DialogBankRow &bank_row(const Node &node) { return static_cast<const DialogBankRow &>(node); }

// The first dlg%03i name from `from` no dialog among `rows` but `self` has.
std::string free_dialog_name(const std::vector<std::shared_ptr<const Node>> &rows, const Node *self = nullptr) {
	const auto taken = [&](const std::string &name) {
		for (const auto &other : rows)
			if (other && other.get() != self && other->kind == kDialog && other->name() == name) return true;
		return false;
	};
	for (int n = 1; n < 1000000; ++n) {
		const std::string name = audio::dialog_name_of(n);
		if (!taken(name)) return name;
	}
	return "dlg001";
}

} // namespace

const RecordTable &dialog_bank_table() {
	static const RecordTable table = make_table();
	return table;
}

bool is_dialog_bank_kind(AssetKind kind) { return asset_kind_row(kind).document == DocumentTypeId::DialogBank; }

std::string dialog_bank_scope(const std::string &path) { return strutil::to_upper(basename_of(path)); }

std::string dialog_sounds_scope(const std::string &path) {
	return strutil::to_upper(mission::dialog_sounds_name(basename_of(path)));
}

std::string dialog_sounds_alternate(const std::string &path) {
	return strutil::to_upper(mission::dialog_sounds_name(basename_of(path), true));
}

size_t DialogBankRow::footprint() const {
	size_t bytes = sizeof(DialogBankRow) + footprint_of(dialog.name) + footprint_of(dialog.def_id_indices) +
	               footprint_of(dialog.lines) + ids_footprint();
	for (const BankLine &line : dialog.lines) bytes += footprint_of(line.wave) + footprint_of(line.sequence);
	return bytes;
}

// --- the document ----------------------------------------------------------------------------------

std::vector<const DialogBankRow *> DialogBankDocument::dialogs() const {
	std::vector<const DialogBankRow *> out;
	for (const auto &node : rows())
		if (node && node->kind == kDialog) out.push_back(&bank_row(*node));
	return out;
}

const DialogBankRow *DialogBankDocument::find_dialog(const std::string &name) const {
	// The game's lookup over the bank's dialogs in their order: the first of the name, matched exactly
	// (audio::find_dialog [orig: Dialog_PlayByName @ 0x44d9f0]).
	const std::vector<const DialogBankRow *> rows = dialogs();
	dbf::File names;
	names.groups.resize(rows.size());
	for (size_t i = 0; i < rows.size(); ++i) names.groups[i].group_name = rows[i]->dialog.name;
	const dbf::Group *found = audio::find_dialog(names, name);
	return found ? rows[size_t(found - names.groups.data())] : nullptr;
}

std::string DialogBankDocument::record_title(const NodeAddress &address) const {
	const Node *node = row(address.row);
	if (!node) return std::string();
	if (!address.child) {
		const BankDialog &d = bank_row(*node).dialog;
		std::string waves;
		for (size_t i = 0; i < d.lines.size() && i < 3; ++i) waves += (i ? ", " : "") + d.lines[i].wave;
		if (d.lines.size() > 3) waves += ", ...";
		return waves.empty() ? d.name + " (no line)" : d.name + ": " + waves;
	}
	const RecordHandle handle = record_in(*node, address);
	if (!handle || address.kind != kLine) return record_name(address);
	const BankLine &l = line_of(handle);
	const std::string wave = l.wave.empty() ? std::string("(no wave)") : l.wave;
	if (!l.delay) return wave;
	char wait[48];
	std::snprintf(wait, sizeof(wait), " after %.1f s", double(l.delay) / 10.0);
	return wave + wait;
}

bool DialogBankDocument::parse(const std::vector<uint8_t> &bytes, std::vector<std::shared_ptr<Node>> &rows,
                               std::shared_ptr<const FileState> &, std::vector<SourceIssue> &issues,
                               Diagnostic &error) {
	if (!is_dialog_bank_kind(kind())) {
		error = make_finding(CoreFinding::DocumentKind, DiagnosticSeverity::Error, "This file is not a dialog bank.", path());
		return false;
	}
	dbf::File file;
	std::string message;
	if (!dbf::parse_dbf_memory(bytes.data(), bytes.size(), file, message)) {
		error = make_finding(CoreFinding::DocumentParse, DiagnosticSeverity::Error,
		                     "The dialog bank could not be read: " + message + ".", path());
		return false;
	}
	if (file.header.version != 0x100)
		issues.push_back({false, 0, std::string(), std::string(),
		                  "The bank's version word is " + std::to_string(file.header.version) +
		                          ", which the game never reads [orig: DialogManager_LoadFromFile @ 0x44e6f7 checks the "
		                          "magic alone]: a save writes 256, as every shipped bank holds."});
	for (const dbf::Group &group : file.groups) {
		auto row = std::make_shared<DialogBankRow>();
		row->kind = kDialog;
		BankDialog &d = row->dialog;
		d.name = group.group_name;
		d.idlist_count = group.idlist_count;
		d.def_id_indices = group.def_id_indices;
		for (const dbf::Line &line : group.lines) {
			BankLine l;
			l.wave = line.def_id_name;
			l.sequence = line.sequence;
			l.delay = line.delay;
			l.flags = line.line_flags;
			l.def_id_index = line.def_id_index;
			l.param = line.param;
			l.resd0 = line.resd0;
			l.resd1 = line.resd1;
			d.lines.push_back(std::move(l));
		}
		shape(*row);
		rows.push_back(std::move(row));
	}
	return true;
}

dbf::File DialogBankDocument::bank() const {
	dbf::File file;
	file.header.magic = dbf::kMagic;
	file.header.version = 0x100;
	file.header.header_size = 28;
	for (const DialogBankRow *row : dialogs()) {
		dbf::Group group;
		group.group_name = row->dialog.name;
		group.idlist_count = row->dialog.idlist_count;
		group.def_id_indices = row->dialog.def_id_indices;
		for (const BankLine &l : row->dialog.lines) {
			dbf::Line line;
			line.def_id_name = l.wave;
			line.sequence = l.sequence;
			line.delay = l.delay;
			line.line_flags = l.flags;
			line.def_id_index = l.def_id_index;
			line.param = l.param;
			line.resd0 = l.resd0;
			line.resd1 = l.resd1;
			group.lines.push_back(std::move(line));
		}
		file.groups.push_back(std::move(group));
	}
	return file;
}

SerializeResult DialogBankDocument::serialize() const {
	SerializeResult result;
	std::vector<uint8_t> bytes;
	std::string error;
	if (!dbf::encode_dbf(bank(), bytes, error)) {
		result.issues.push_back({true, 0, std::string(), std::string(), "The dialog bank could not be written: " + error + "."});
		return result;
	}
	result.text.assign(bytes.begin(), bytes.end());
	return result;
}

std::string DialogBankDocument::save_words() const {
	return "A save writes the bank from its dialogs and their lines, each as the game reads it.";
}

std::shared_ptr<Node> DialogBankDocument::make_node(NodeKind kind, NodeId, const std::vector<std::shared_ptr<const Node>> &rows,
                                                    std::string &error) {
	if (kind != kDialog) {
		error = "A dialog bank's rows are its dialogs; a line goes into a dialog.";
		return nullptr;
	}
	auto row = std::make_shared<DialogBankRow>();
	row->kind = kDialog;
	row->dialog.name = free_dialog_name(rows);
	shape(*row);
	return row;
}

void DialogBankDocument::prepare_duplicate(Node &copy, const Node &, const std::vector<std::shared_ptr<const Node>> &rows) const {
	static_cast<DialogBankRow &>(copy).dialog.name = free_dialog_name(rows, &copy);
}

void DialogBankDocument::refine_field(const NodeAddress &address, FieldUse &use) const {
	TableDocument::refine_field(address, use);
	if (use.defines == ReferenceKind::Dialog) use.scope = dialog_bank_scope(path());
	if (use.reference == ReferenceKind::BankWave) {
		use.scope = dialog_sounds_scope(path());
		use.scope_alternate = dialog_sounds_alternate(path());
	}
}

void DialogBankDocument::refine_symbol(const NodeAddress &address, SymbolFacts &facts) const {
	const Node *node = row(address.row);
	if (!node || address.child || node->kind != kDialog) return;
	const DialogBankRow *first = find_dialog(bank_row(*node).dialog.name);
	if (first && first != node) {
		facts.inert = true;
		facts.inert_reason = "an earlier dialog of the bank has this name, and the game plays the first of a name";
	}
}

// --- the findings ------------------------------------------------------------------------------------

namespace {

constexpr FindingCodeEntry<DialogBankFinding> kFindingEntries[] = {
	{ DialogBankFinding::InvalidInput, { "dialog_bank.invalid_input", FindingFix::None, nullptr, true } },
	{ DialogBankFinding::IgnoredInput, { "dialog_bank.ignored_input", FindingFix::Rewrite, kRewriteDropsIgnoredInput } },
	// The game plays the first dialog of a name [orig: Dialog_PlayByName @ 0x44da8b]; no refusal is witnessed.
	{ DialogBankFinding::NameRepeated, listed_code("dialog_bank.name_repeated") },
	{ DialogBankFinding::NameUnplayed, listed_code("dialog_bank.name_unplayed") },
	{ DialogBankFinding::Silent, listed_code("dialog_bank.silent") },
	{ DialogBankFinding::LineNoWave, listed_code("dialog_bank.line_no_wave") },
};
static_assert(std::size(kFindingEntries) == static_cast<size_t>(DialogBankFinding::kCount),
		"every DialogBankFinding has exactly one row");
static_assert(finding_entries_well_formed(kFindingEntries),
		"the dialog bank's rows follow DialogBankFinding's order, each token its own");
constexpr auto kFindingRows = finding_rows(kFindingEntries, FindingGroup::DialogBanks);
static_assert(finding_rows_well_formed(kFindingRows), "every row of the table takes its group");

} // namespace

const FindingCodeRow &finding_code(DialogBankFinding code) { return kFindingRows[static_cast<size_t>(code)]; }

FindingTable dialog_bank_finding_codes() { return { kFindingRows.data(), kFindingRows.size() }; }

// A dialog a mission names that the bank lacks (DI-15): a dialog of the name, as Add dialog makes one, no line
// yet, so it plays nothing until one is given, as nothing plays for the name now.
bool define_dialog(const DocumentBase &document, const ReferenceSubject &missing, PlannedFix &out) {
	const auto *bank = dynamic_cast<const DialogBankDocument *>(&document);
	if (!bank || missing.kind != ReferenceKind::Dialog || missing.target.empty() || missing.target.size() >= dbf::kNameBytes)
		return false;
	// The dialog goes in the bank its scope names alone.
	const std::string file = basename_of(document.path());
	const std::string scoped = missing.scope.substr(0, missing.scope.find('/'));
	if (!scoped.empty() && !strutil::iequals(scoped, file)) return false;
	Edit add;
	add.operation = EditOperation::Add;
	add.address.kind = kDialog;
	add.field = "name";
	add.value = missing.target;
	out = PlannedFix();
	out.edits.push_back(std::move(add));
	out.label = "Add " + missing.target + " to " + file;
	out.detail = "Adds the dialog " + missing.target + " to " + file + ", as Add dialog makes one, and selects it to give it "
	             "its lines: until then it plays nothing, as nothing plays for the name now.";
	return true;
}

std::vector<Diagnostic> validate_dialog_bank_file(const DocumentBase &document) {
	std::vector<Diagnostic> findings;
	const auto *bank = dynamic_cast<const DialogBankDocument *>(&document);
	if (!bank) return findings;
	source_issue_findings(*bank, finding_code(DialogBankFinding::InvalidInput),
	                      finding_code(DialogBankFinding::IgnoredInput), findings);
	if (document.blocked()) return findings;
	const auto add = [&](const NodeAddress &address, const std::string &record, DiagnosticSeverity severity,
	                     DialogBankFinding code, const char *field, const std::string &message) {
		Diagnostic d = make_finding(code, severity, message, document.path(), field);
		d.row_id = address.row;
		d.child_id = address.child;
		d.record_kind = address.kind;
		d.record = record;
		findings.push_back(std::move(d));
	};
	std::unordered_map<std::string, bool> seen;
	for (const DialogBankRow *row : bank->dialogs()) {
		const NodeAddress at{row->id, kDialog, 0};
		const BankDialog &d = row->dialog;
		if (!seen.emplace(d.name, true).second)
			add(at, d.name, DiagnosticSeverity::Warning, DialogBankFinding::NameRepeated, "name",
			    "An earlier dialog of the bank is named '" + d.name + "': the game plays the first of a name, never this one.");
		else if (audio::dialog_index_of(d.name) < 1)
			add(at, d.name, DiagnosticSeverity::Warning, DialogBankFinding::NameUnplayed, "name",
			    "'" + d.name + "' is no name a Play dialog action plays: a mission names a dialog by the number that forms "
			    "dlg%03i from 1 (dlg001 is dialog 1), matched exactly.");
		if (d.lines.empty())
			add(at, d.name, DiagnosticSeverity::Info, DialogBankFinding::Silent, "",
			    d.name + " has no line: playing it plays nothing and it ends at once.");
		static const std::vector<RecordIds> kNone;
		const std::vector<RecordIds> &line_ids = row->ids.lists.empty() ? kNone : row->ids.lists[0];
		for (size_t i = 0; i < d.lines.size(); ++i) {
			if (!d.lines[i].wave.empty()) continue;
			add(NodeAddress{row->id, kLine, i < line_ids.size() ? line_ids[i].id : 0}, d.name, DiagnosticSeverity::Warning,
			    DialogBankFinding::LineNoWave, "wave",
			    d.name + ", line " + std::to_string(i + 1) + " names no wave: the game says \"EX Cannot load audio\" and plays "
			    "nothing for it.");
		}
	}
	return findings;
}

} // namespace opennova::editor
