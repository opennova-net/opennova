// The HUD effects document (hudfx_document.h, ADR 0046 S23 B): hudfx.def's tagged lines as rows over
// formats/def/def_hudfx, read through its reader and written by its writer from the rows alone (S26); the fields'
// words and the findings say what the game makes of the file (docs/interface/hud-re.md "hudfx.def").
#include "hudfx_document.h"

#include <iterator>

#include <editor/assets/asset_kinds.h>
#include <editor/model/staged_rows.h>

namespace opennova::editor {
namespace {

constexpr NodeKind kLine = node_kind(HudFxKind::Line);

const def::HudFxLine &line_of(const RecordHandle &r) { return r.as<def::HudFxLine>(); }

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
	TableKind line(RecordKindRow{kLine, "line", "Model line", "Add model line", true});
	FieldSchema tag = schema_of("tag", FieldType::Integer, "Slot",
			"The slot the model goes in: the HUD model, drawn over the first-person view, or a power slot, drawn in a "
			"tenth of the screen while its ammo pool (3 to 10) holds any [orig: HUD_CacheModelNameByTag @ 0x58F970; "
			"HUD_RenderAllOverlays @ 0x5A8221..0x5A8459]. The tag compares without case.");
	for (size_t slot = 0; slot < def::kHudFxSlots; ++slot)
		tag.choices.push_back({def::hudfx_tag(slot), int64_t(slot), slot ? "power slot " + std::to_string(slot) : "the HUD model"});
	line.field(RF{tag,
	              {[](const RecordHandle &r, Value &out) { return out = int64_t(line_of(r).slot), true; },
	               [](const RecordHandle &r, const Value &v, std::string &e) {
		               const auto *whole = std::get_if<int64_t>(&v);
		               if (!whole || *whole < 0 || *whole >= int64_t(def::kHudFxSlots)) return e = "A slot is one of the nine tags.", false;
		               r.as<def::HudFxLine>().slot = uint8_t(*whole);
		               return true;
	               }}});
	FieldSchema model = schema_of("model", FieldType::Text, "Model",
			"The model loaded into the slot, by its file name [orig: ThreediGp_LoadModel @ 0x5A465F..0x5A4797]. The "
			"name is copied whole into the slot's 16 bytes and on: a name of 16 or more characters runs into the next "
			"slots [orig: @ 0x58F992..0x58FB42].");
	model.reference = ReferenceKind::Model;
	line.field(RF{model,
	              {[](const RecordHandle &r, Value &out) { return out = line_of(r).model, true; },
	               [](const RecordHandle &r, const Value &v, std::string &e) {
		               const auto *text = std::get_if<std::string>(&v);
		               if (!text) return e = "A model is a file name.", false;
		               if (text->find_first_of(" \t,\";\r\n") != std::string::npos || text->find("//") != std::string::npos)
			               return e = "A model's name is one word of the walk: no blank, comma, quote, ';' or '//'.", false;
		               r.as<def::HudFxLine>().model = *text;
		               return true;
	               }}});
	return RecordTable({std::move(line)});
}

const HudFxRow &fx_row(const Node &node) { return static_cast<const HudFxRow &>(node); }

constexpr FindingCodeEntry<HudFxFinding> kFindingEntries[] = {
	{ HudFxFinding::NoModel, listed_code("hudfx.no_model") },
	{ HudFxFinding::NameRuns, listed_code("hudfx.name_runs") },
	{ HudFxFinding::PowerUnseen, listed_code("hudfx.power_unseen") },
	{ HudFxFinding::PowerSlotsEmpty, { "hudfx.power_slots_empty", FindingFix::None, nullptr, false, FindingPlace::Content,
	                                   FindingGroup::None, FindingSource::Own, FindingProblem::None, true,
	                                   "with the HUD model the file's one line, the HUD's draw reads each power slot's model "
	                                   "with no test while its ammo pool holds any, and those slots hold none: the game "
	                                   "fails at a null read [orig: HUD_RenderAllOverlays @ 0x5A8403..0x5A840A]" } },
};
static_assert(std::size(kFindingEntries) == static_cast<size_t>(HudFxFinding::kCount),
		"every HudFxFinding has exactly one row");
static_assert(finding_entries_well_formed(kFindingEntries),
		"the HUD effects' rows follow HudFxFinding's order, each token its own");
constexpr auto kFindingRows = finding_rows(kFindingEntries, FindingGroup::HudEffects);
static_assert(finding_rows_well_formed(kFindingRows), "every row of the table takes its group");

} // namespace

const RecordTable &hudfx_table() {
	static const RecordTable table = make_table();
	return table;
}

std::string HudFxRow::name() const {
	const char *tag = def::hudfx_tag(line.slot);
	return tag ? tag : "";
}

def::HudFxFile HudFxDocument::file() const {
	def::HudFxFile out;
	for (const auto &node : rows())
		if (node && node->kind == kLine) {
			out.line = fx_row(*node).line;
			break;
		}
	return out;
}

std::string HudFxDocument::record_title(const NodeAddress &address) const {
	const Node *node = row(address.row);
	if (!node) return std::string();
	const def::HudFxLine &line = fx_row(*node).line;
	return std::string(def::hudfx_tag(line.slot) ? def::hudfx_tag(line.slot) : "?") + ": " +
	       (line.model.empty() ? std::string("(no model)") : line.model);
}

bool HudFxDocument::parse(const std::vector<uint8_t> &bytes, std::vector<std::shared_ptr<Node>> &rows,
                          std::shared_ptr<const FileState> &, std::vector<SourceIssue> &, Diagnostic &error) {
	const def::HudFxFile read = def::hudfx_parse(bytes.data(), bytes.size());
	if (read.line) {
		auto row = std::make_shared<HudFxRow>();
		row->line = *read.line;
		shape(*row);
		rows.push_back(std::move(row));
	}
	return true;
}

SerializeResult HudFxDocument::serialize() const {
	SerializeResult result;
	std::string text, error;
	if (!def::hudfx_write(file(), text, error)) {
		result.issues.push_back({true, 0, std::string(), std::string(), "The file could not be written: " + error});
		return result;
	}
	result.text = std::move(text);
	return result;
}

std::string HudFxDocument::save_words() const {
	return "Saving writes the one line the game reads, its tag and its model, in the editor's form; what followed it "
	       "in the file, which the game never read, is not kept.";
}

std::shared_ptr<Node> HudFxDocument::make_node(NodeKind kind, NodeId, const std::vector<std::shared_ptr<const Node>> &,
                                               std::string &error) {
	if (kind != kLine) {
		error = "hudfx.def's rows are its model lines.";
		return nullptr;
	}
	auto row = std::make_shared<HudFxRow>();
	shape(*row);
	return row;
}

bool HudFxDocument::accept_step(const EditStep &step, const StagedRows &rows, StepRefusal &refusal) const {
	if (!TableDocument::accept_step(step, rows, refusal)) return false;
	// The walk ends at the first tagged line [orig: File_ParseASCIIFile @ 0x53D942]: a second is one the game never
	// reads, which the file does not hold.
	size_t lines = 0;
	for (const auto &node : rows.rows())
		if (node && node->kind == kLine) ++lines;
	if (lines > 1) {
		refusal.message = "hudfx.def holds the one line the game reads (its walk ends there): change that line's tag "
		                  "or model instead.";
		return false;
	}
	return true;
}

const FindingCodeRow &finding_code(HudFxFinding code) { return kFindingRows[static_cast<size_t>(code)]; }

FindingTable hudfx_finding_codes() { return { kFindingRows.data(), kFindingRows.size() }; }

std::vector<Diagnostic> validate_hudfx_file(const DocumentBase &document) {
	std::vector<Diagnostic> findings;
	const auto *fx = dynamic_cast<const HudFxDocument *>(&document);
	if (!fx) return findings;
	const auto add = [&](const Node &node, DiagnosticSeverity severity, HudFxFinding code, const char *field,
	                     const std::string &message) {
		Diagnostic d = make_finding(code, severity, message, document.path(), field);
		d.row_id = node.id;
		d.record_kind = kLine;
		d.record = node.name();
		findings.push_back(std::move(d));
	};
	const auto &all = fx->rows();
	for (size_t i = 0; i < all.size(); ++i) {
		const Node &node = *all[i];
		const def::HudFxLine &line = fx_row(node).line;
		if (line.model.empty()) {
			add(node, DiagnosticSeverity::Warning, HudFxFinding::NoModel, "model",
			    "The line names no model: its slot holds none and nothing is loaded [orig: HUD_InitOverlaySystem @ "
			    "0x5A465F, a slot loaded only where its name is set].");
			continue;
		}
		// A name of 16 characters copies its NUL alone into the next slot's first byte, a slot no other line fills
		// (the walk reads one line), so nothing changes; from 17 the copy runs into the next slot's name [orig:
		// HUD_CacheModelNameByTag @ 0x58F970, its byte loop @ 0x58F992..0x58FB42].
		if (line.model.size() > def::kHudFxNameBytes) {
			const auto names = def::hudfx_slot_names(fx->file());
			std::string runs;
			for (size_t slot = size_t(line.slot) + 1; slot < def::kHudFxSlots; ++slot)
				if (!names[slot].empty()) runs += (runs.empty() ? "" : ", ") + std::string(def::hudfx_tag(slot)) + " '" + names[slot] + "'";
			add(node, DiagnosticSeverity::Warning, HudFxFinding::NameRuns, "model",
			    "The name is " + std::to_string(line.model.size()) + " characters and its slot keeps 16: the copy runs on into "
			    "the next slots [orig: @ 0x58F992..0x58FB42]" + (runs.empty() ? std::string(".") : ", which then load " + runs + "."));
		}
		if (line.slot == 0)
			add(node, DiagnosticSeverity::Error, HudFxFinding::PowerSlotsEmpty, "tag",
			    "The HUD model is the file's one line read, so the eight power slots hold no model; the HUD's draw reads a "
			    "power slot's model with no test while its ammo pool (3 to 10) holds any in the first-person view, and the "
			    "game fails there [orig: HUD_RenderAllOverlays @ 0x5A83D1..0x5A840A].");
		else
			add(node, DiagnosticSeverity::Info, HudFxFinding::PowerUnseen, "tag",
			    "A power slot's model is loaded but never drawn: the power slots are drawn only with a HUD model, and the "
			    "file's one line read is this one [orig: HUD_RenderAllOverlays @ 0x5A8221].");
	}
	return findings;
}

} // namespace opennova::editor
