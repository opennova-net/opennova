// The character attributes document (charattr_document.h, ADR 0046 S23 B): charattr.def's classes as rows over
// formats/charattr, read through the loader and written by the table's writer from the rows alone (S26); the fields'
// words say what the game does with each (docs/net/novaworld-net-re.md, "charattr.def: the table and its readers").
#include "charattr_document.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <utility>

#include <base/io/strutil.h>
#include <editor/assets/asset_kinds.h>
#include <editor/documents/config_overrun.h>
#include <editor/model/core_finding.h>
#include <editor/model/staged_rows.h>
#include <editor/project/project_files.h>
#include <formats/configfile/config_file.h>
#include <formats/def/reserved_items.h>

namespace opennova::editor {
namespace {

constexpr NodeKind kClass = node_kind(CharAttrKind::Class);

const charattr::ClassRow &class_of(const RecordHandle &r) { return r.as<charattr::ClassRow>(); }

// The camouflage keys, by the mission's camouflage they serve (charattr::cammo_property_for_camouflage: the BMS
// camouflage selector 1 JUNGLE_CAMMO (10), 2 ARCTIC_CAMMO (12), any other DESERT_CAMMO (11) [orig:
// Entity_SpawnFromAnimSlotProperty @ 0x43c399..0x43c3be]).
struct Cammo {
	charattr::Property property;
	const char *key;
	const char *missions;
};
constexpr Cammo kCammo[] = {
	{ charattr::kJungleCammo, "JUNGLE_CAMMO", "a jungle mission" },
	{ charattr::kDesertCammo, "DESERT_CAMMO", "a desert mission (any camouflage but jungle and arctic)" },
	{ charattr::kArcticCammo, "ARCTIC_CAMMO", "an arctic mission" },
};

// Each key the loader reads: its field's id (the key in lower case), its words.
struct KeyWords {
	charattr::Property property;
	const char *id;
	const char *label;
	const char *description;
};
// Read by no game code but the anti-cheat challenge's hash of the row (S2C 0x39 -> C2S 0x1C) and a debug page
// [orig: CharAttr_GetClassChecksum @ 0x412AA0] (novaworld-net-re "charattr.def: the table and its readers").
#define CHARATTR_UNREAD " Read by no game code: the anti-cheat challenge hashes it with the row [orig: CharAttr_GetClassChecksum @ 0x412AA0]."
constexpr KeyWords kKeyWords[] = {
	{ charattr::kStealth, "stealth", "Stealth", "STEALTH, a float." CHARATTR_UNREAD },
	{ charattr::kHpBonus, "hpbonus", "Health bonus", "HPBONUS, a float." CHARATTR_UNREAD },
	{ charattr::kManaBonus, "manabonus", "Mana bonus", "MANABONUS, a float." CHARATTR_UNREAD },
	{ charattr::kRecoilMute, "recoil_mute", "Recoil scale", "RECOIL_MUTE, a float." CHARATTR_UNREAD },
	{ charattr::kXhairMute, "xhair_mute", "Crosshair scale", "XHAIR_MUTE, a float." CHARATTR_UNREAD },
	{ charattr::kXhairDxMute, "xhairdx_mute", "Crosshair spread scale", "XHAIRDX_MUTE, a float." CHARATTR_UNREAD },
	{ charattr::kScopeMute, "scope_mute", "Scope scale", "SCOPE_MUTE, a float." CHARATTR_UNREAD },
	{ charattr::kReloadMute, "reload_mute", "Reload scale", "RELOAD_MUTE, a float." CHARATTR_UNREAD },
	{ charattr::kJungleCammo, "jungle_cammo", "Jungle camouflage",
	  "JUNGLE_CAMMO: the item a player of the class spawns as in a jungle mission, an items.def item by its id less "
	  "100000 [orig: Entity_SpawnFromAnimSlotProperty @ 0x43C390 -> CharAttr_GetCammoTypeId @ 0x4127B0 -> "
	  "ItemList_FindIndexByTypeId @ 0x49E100]; 0, or a type no item has, the first items.def row." },
	{ charattr::kDesertCammo, "desert_cammo", "Desert camouflage",
	  "DESERT_CAMMO: the item a player of the class spawns as in a desert mission (any camouflage but jungle and "
	  "arctic), an items.def item by its id less 100000 [orig: CharAttr_GetCammoTypeId @ 0x4127B0]." },
	{ charattr::kArcticCammo, "arctic_cammo", "Arctic camouflage",
	  "ARCTIC_CAMMO: the item a player of the class spawns as in an arctic mission, an items.def item by its id "
	  "less 100000 [orig: CharAttr_GetCammoTypeId @ 0x4127B0]." },
	{ charattr::kRunModifier, "run_modifier", "Run modifier", "RUN_MODIFIER, a whole number." CHARATTR_UNREAD },
};
#undef CHARATTR_UNREAD

float *real_of(charattr::ClassRow &row, charattr::Property property) {
	switch (property) {
	case charattr::kStealth: return &row.stealth;
	case charattr::kHpBonus: return &row.hp_bonus;
	case charattr::kManaBonus: return &row.mana_bonus;
	case charattr::kRecoilMute: return &row.recoil_mute;
	case charattr::kXhairMute: return &row.xhair_mute;
	case charattr::kXhairDxMute: return &row.xhairdx_mute;
	case charattr::kScopeMute: return &row.scope_mute;
	case charattr::kReloadMute: return &row.reload_mute;
	default: return nullptr;
	}
}

int32_t *integer_of(charattr::ClassRow &row, charattr::Property property) {
	switch (property) {
	case charattr::kJungleCammo: return &row.jungle_cammo;
	case charattr::kDesertCammo: return &row.desert_cammo;
	case charattr::kArcticCammo: return &row.arctic_cammo;
	case charattr::kRunModifier: return &row.run_modifier;
	default: return nullptr;
	}
}

using RF = LabelledField;

RecordTable make_table() {
	TableKind kind(RecordKindRow{kClass, "class", "Class", "Add class", true});
	for (const KeyWords &key : kKeyWords) {
		const charattr::Property property = key.property;
		charattr::ClassRow probe;
		if (real_of(probe, property)) {
			kind.field(RF{schema_of(key.id, FieldType::Real, key.label, key.description),
			              {[property](const RecordHandle &r, Value &out) {
				               return out = double(*real_of(const_cast<charattr::ClassRow &>(class_of(r)), property)), true;
			               },
			               [property](const RecordHandle &r, const Value &v, std::string &e) {
				               double real = 0.0;
				               if (const auto *d = std::get_if<double>(&v)) real = *d;
				               else if (const auto *i = std::get_if<int64_t>(&v)) real = double(*i);
				               else return e = "The value is a number.", false;
				               if (!std::isfinite(real) || std::fabs(real) > 3.4e38)
					               return e = "The value is a number a float holds.", false;
				               *real_of(r.as<charattr::ClassRow>(), property) = float(real);
				               return true;
			               }}});
			continue;
		}
		FieldSchema schema = schema_of(key.id, FieldType::Integer, key.label, key.description);
		if (property != charattr::kRunModifier) {
			schema.reference = ReferenceKind::Item;
			schema.name_offset = def::DEF_ITEM_ID_BASE;
		}
		kind.field(RF{schema,
		              {[property](const RecordHandle &r, Value &out) {
			               return out = int64_t(*integer_of(const_cast<charattr::ClassRow &>(class_of(r)), property)), true;
		               },
		               [property](const RecordHandle &r, const Value &v, std::string &e) {
			               const auto *whole = std::get_if<int64_t>(&v);
			               if (!whole || *whole < INT32_MIN || *whole > INT32_MAX)
				               return e = "The value is a whole number the game's 32-bit reader holds.", false;
			               *integer_of(r.as<charattr::ClassRow>(), property) = int32_t(*whole);
			               return true;
		               }}});
	}
	FieldSchema attributes = schema_of("attributes", FieldType::Unsigned, "Attributes",
			"ATTRIBUTES, its words' flags ORed [orig: CharAttr_LoadFromDef @ 0x4123B0, g_CharAttrAttributeNames @ "
			"0x813F18]: Medic heals and shows the medic markers, KnifeBonus the knife's reach; AutoScope, SpreadBonus "
			"and WaterGirl are read by no game code.");
	attributes.flags = true;
	for (const charattr::AttributeName &name : charattr::kAttributeNames)
		attributes.choices.push_back({name.name, int64_t(name.flag), ""});
	kind.field(RF{attributes,
	              {[](const RecordHandle &r, Value &out) { return out = int64_t(class_of(r).attributes), true; },
	               [](const RecordHandle &r, const Value &v, std::string &e) {
		               uint32_t named = 0;
		               for (const charattr::AttributeName &name : charattr::kAttributeNames) named |= name.flag;
		               const auto *whole = std::get_if<int64_t>(&v);
		               if (!whole || *whole < 0 || (uint64_t(*whole) & ~uint64_t(named)) != 0)
			               return e = "The attributes are AutoScope, SpreadBonus, KnifeBonus, Medic and WaterGirl.", false;
		               r.as<charattr::ClassRow>().attributes = uint32_t(*whole);
		               return true;
	               }}});
	return RecordTable({std::move(kind)});
}

constexpr FindingCodeEntry<CharAttrFinding> kFindingEntries[] = {
	{ CharAttrFinding::Unread, listed_code("charattr.unread_section") },
	{ CharAttrFinding::AttributeWord, listed_code("charattr.attribute_word") },
	{ CharAttrFinding::NotANumber, listed_code("charattr.not_a_number") },
	{ CharAttrFinding::NoCammo, listed_code("charattr.no_cammo") },
	{ CharAttrFinding::BinaryForm, listed_code("charattr.binary_form") },
};
static_assert(std::size(kFindingEntries) == static_cast<size_t>(CharAttrFinding::kCount),
		"every CharAttrFinding has exactly one row");
static_assert(finding_entries_well_formed(kFindingEntries),
		"the charattr type's rows follow CharAttrFinding's order, each token its own");
constexpr auto kFindingRows = finding_rows(kFindingEntries, FindingGroup::CharAttrs);
static_assert(finding_rows_well_formed(kFindingRows), "every row of the table takes its group");

std::string class_label(size_t index) { return "CHARACTER" + std::to_string(index + 1); }

// The 1-based line of `offset` in `text`.
size_t line_of(const std::string &text, size_t offset) {
	offset = std::min(offset, text.size());
	return 1 + size_t(std::count(text.begin(), text.begin() + std::ptrdiff_t(offset), '\n'));
}

uint8_t largest_class(const std::vector<std::shared_ptr<const Node>> &rows) {
	uint8_t largest = 0;
	for (const auto &node : rows)
		if (node && node->kind == kClass) largest = std::max(largest, static_cast<const CharAttrRow &>(*node).row.class_id);
	return largest;
}

} // namespace

const RecordTable &charattr_table() {
	static const RecordTable table = make_table();
	return table;
}

const FindingCodeRow &finding_code(CharAttrFinding code) { return kFindingRows[static_cast<size_t>(code)]; }

FindingTable charattr_finding_codes() { return { kFindingRows.data(), kFindingRows.size() }; }

charattr::Table CharAttrDocument::file() const {
	charattr::Table out;
	for (const auto &node : rows()) {
		if (!node || node->kind != kClass) continue;
		const charattr::ClassRow &row = static_cast<const CharAttrRow &>(*node).row;
		if (row.class_id < 1 || row.class_id > charattr::kClassCount) continue;
		out.rows[row.class_id - 1] = row;
		out.rows[row.class_id - 1].active = true;
	}
	return out;
}

bool CharAttrDocument::composed(std::string &text, std::string &error) const {
	return charattr::compose_table(file(), text, error);
}

std::string CharAttrDocument::record_title(const NodeAddress &address) const {
	const Node *node = row(address.row);
	if (!node || node->kind != kClass) return std::string();
	const charattr::ClassRow &row = static_cast<const CharAttrRow &>(*node).row;
	std::string words;
	for (const charattr::AttributeName &name : charattr::kAttributeNames)
		if (row.attributes & name.flag) words += (words.empty() ? "" : ", ") + std::string(name.name);
	return node->name() + (words.empty() ? std::string() : " (" + words + ")");
}

bool CharAttrDocument::parse(const std::vector<uint8_t> &bytes, std::vector<std::shared_ptr<Node>> &rows,
                             std::shared_ptr<const FileState> &, std::vector<SourceIssue> &issues, Diagnostic &error) {
	charattr::Table table;
	charattr::Reading reading;
	// A CBIN-form file the ConfigFile loader reads through its binary reader [orig: ConfigFile_LoadFromFile @
	// 0x760aa3, on the "CBIN" magic], which the format's reader takes as its text twin (D-CBIN-4).
	// The document holds those classes read only (charattr.binary_form): it writes the text form alone, and no
	// shipped charattr.def is a CBIN one. A file of no bytes reads as no class (the game's own error, a required
	// file's).
	binary_form_ = configfile::data_strings_pool(bytes.data(), bytes.size()).binary;
	const bool read = charattr::read_table(bytes.data(), bytes.size(), table, &reading);
	if (binary_form_) {
		if (!read) reading = charattr::Reading();
		issues.push_back({true, 0, std::string(), std::string(),
		                  read ? "This charattr.def is in the ConfigFile's binary (CBIN) form: the editor shows the "
		                         "classes the game reads of it, read only."
		                       : "This charattr.def is in the ConfigFile's binary (CBIN) form, which the editor cannot "
		                         "read; it holds the file read only."});
	}
	for (size_t index = 0; index < reading.classes && index < charattr::kClassCount; ++index) {
		auto row = std::make_shared<CharAttrRow>();
		row->row = table.rows[index];
		shape(*row);
		rows.push_back(std::move(row));
	}
	// The loader hands a single NUL for an empty file (Document's load): a file of no bytes.
	stored_text_ = bytes.size() == 1 && bytes[0] == 0 ? std::string() : std::string(bytes.begin(), bytes.end());
	return true;
}

SerializeResult CharAttrDocument::serialize() const {
	SerializeResult result;
	std::string text, error;
	// The text whatever the ConfigFile pool makes of it (charattr::write_table refuses one past the pool): a file past
	// it is document.config_overrun's, which refuses a build (the game's failure), not the save of the work.
	if (!charattr::compose_table(file(), text, error)) {
		result.issues.push_back({true, 0, std::string(), std::string(), "The character attributes could not be written: " + error});
		return result;
	}
	result.text = std::move(text);
	return result;
}

std::string CharAttrDocument::save_words() const {
	return "Saving writes the table in the editor's form, from what the game reads of it: each class's section with "
	       "its keys. Comments and the lines the game reads nothing of are not kept.";
}

std::shared_ptr<Node> CharAttrDocument::make_node(NodeKind kind, NodeId, const std::vector<std::shared_ptr<const Node>> &rows,
                                                  std::string &error) {
	if (kind != kClass) {
		error = "The character attributes' rows are its classes.";
		return nullptr;
	}
	const uint8_t next = uint8_t(largest_class(rows) + 1);
	if (next > charattr::kClassCount) {
		error = "The table holds sixteen classes, all the game reads [orig: CharAttr_LoadFromDef @ 0x4121BF].";
		return nullptr;
	}
	auto row = std::make_shared<CharAttrRow>();
	row->row.active = true;
	row->row.class_id = next;
	shape(*row);
	return row;
}

void CharAttrDocument::prepare_duplicate(Node &copy, const Node &, const std::vector<std::shared_ptr<const Node>> &rows) const {
	if (copy.kind != kClass) return;
	charattr::ClassRow &row = static_cast<CharAttrRow &>(copy).row;
	row.class_id = uint8_t(largest_class(rows) + 1);
}

bool CharAttrDocument::accept_step(const EditStep &step, const StagedRows &rows, StepRefusal &refusal) const {
	std::vector<int> ids;
	for (const auto &node : rows.rows())
		if (node && node->kind == kClass) ids.push_back(static_cast<const CharAttrRow &>(*node).row.class_id);
	std::sort(ids.begin(), ids.end());
	for (size_t i = 0; i < ids.size(); ++i)
		if (ids[i] != int(i + 1) || ids.size() > charattr::kClassCount) {
			// The loader reads CHARACTER1, CHARACTER2 and on, stopping at the first class the file has no section
			// of [orig: CharAttr_LoadFromDef @ 0x4121B5..0x4121BF].
			refusal.message = "The game reads CHARACTER1, CHARACTER2 and on to the first class the file lacks, sixteen at "
			                  "most: the classes stay 1 to " +
			                  std::to_string(std::min(ids.size(), charattr::kClassCount)) +
			                  ", each once (remove the last class first).";
			return false;
		}
	return TableDocument::accept_step(step, rows, refusal);
}

std::vector<Diagnostic> validate_charattr_file(const DocumentBase &document) {
	std::vector<Diagnostic> findings;
	const auto *doc = dynamic_cast<const CharAttrDocument *>(&document);
	if (doc && doc->binary_form()) {
		// The CBIN form: the game reads it whole through its binary reader; the editor shows it, read only.
		findings.push_back(make_finding(finding_code(CharAttrFinding::BinaryForm), DiagnosticSeverity::Warning,
		                                "This charattr.def is in the ConfigFile's binary (CBIN) form, which the game reads "
		                                "through its binary reader: the editor holds its classes read only, writing "
		                                "charattr.def's text form alone. Edit it as text elsewhere, or replace it with "
		                                "a text form to edit it here.",
		                                document.path()));
		return findings;
	}
	if (!doc || doc->blocked()) return findings;
	// What the game reads: the file as it sits. A save writes the table in the editor's form, from what the game
	// reads of it: none of the input below (a section never read, a word no attribute is, a value read as 0) is in
	// it, and its classes are these.
	const std::string &text = doc->stored_text();
	charattr::Table table;
	charattr::Reading reading;
	if (!charattr::read_table(reinterpret_cast<const uint8_t *>(text.data()), text.size(), table, &reading))
		return findings; // an empty file: the game's own error, a required file's
	const auto class_node = [&](size_t index) -> const Node * {
		for (const auto &node : doc->rows())
			if (node && node->kind == kClass && static_cast<const CharAttrRow &>(*node).row.class_id == index + 1) return node.get();
		return nullptr;
	};
	const auto add = [&](CharAttrFinding code, size_t index, const char *field, std::string message, size_t offset) {
		Diagnostic d = make_finding(finding_code(code), DiagnosticSeverity::Warning, std::move(message), document.path(), field);
		d.line = line_of(text, offset);
		if (const Node *node = index < charattr::kClassCount ? class_node(index) : nullptr) {
			d.row_id = node->id;
			d.record_kind = node->kind;
			d.record = node->name();
		}
		findings.push_back(std::move(d));
	};
	// A section the loader never reaches [orig: CharAttr_LoadFromDef @ 0x4121b5..0x4121bf, the classes in order to
	// the first one the file has no section of; ConfigFile_FindSection @ 0x75eeb0, the first of a label].
	const std::string stop = class_label(reading.classes);
	for (const charattr::UnreadSection &unread : reading.unread) {
		std::string reason;
		switch (unread.why) {
			case charattr::UnreadSection::Why::AfterMissing:
				reason = "the game reads the classes in order from CHARACTER1 and stops at " + stop +
				         ", which the file has no section of";
				break;
			case charattr::UnreadSection::Why::Repeated: reason = "the game reads the file's first [" + unread.label + "]"; break;
			case charattr::UnreadSection::Why::NotAClass: reason = "the game reads [CHARACTER1] to [CHARACTER16] alone"; break;
		}
		add(CharAttrFinding::Unread, charattr::kClassCount, "",
		    "[" + unread.label + "] is never read: " + reason + ". Nothing in it reaches the game.", unread.offset);
	}
	for (size_t index = 0; index < reading.classes; ++index) {
		const charattr::ClassSource &source = reading.sources[index];
		const std::string label = class_label(index);
		// Each ATTRIBUTES word through the attribute table [orig: g_CharAttrAttributeNames @ 0x813F18].
		for (const charattr::ValueSource &word : source.attribute_words) {
			if (charattr::attribute_flag(word.written) != 0) continue;
			const std::string key = strutil::to_upper(word.written);
			if (key == "NULL" || key == "NONE") continue;
			add(CharAttrFinding::AttributeWord, index, "attributes",
			    label + "'s ATTRIBUTES word '" + word.written +
			            "' is no attribute (AutoScope, SpreadBonus, KnifeBonus, Medic, WaterGirl): the class gets nothing for it.",
			    word.offset);
		}
		// A number key's value the reader classifies as text reads 0 [orig: effect_get_param_value_0 @ 0x75fb3d].
		for (const KeyWords &key : kKeyWords) {
			const charattr::ValueSource &value = source.values[key.property];
			if (!value.read || configfile::classify_numeric(value.written) != 0) continue;
			add(CharAttrFinding::NotANumber, index, key.id,
			    label + "'s " + charattr::property_key(key.property) + " '" + value.written + "' is no number: the game reads 0.",
			    value.offset);
		}
		// A camouflage with no item: type 0, which the item lookup finds as the item whose id is 100000, else the
		// first items.def row [orig: ItemList_FindIndexByTypeId @ 0x49e100, its 0 for no match @ 0x49e12f].
		for (const Cammo &cammo : kCammo) {
			if (charattr::cammo_of(table.rows[index], cammo.property) != 0) continue;
			const charattr::ValueSource &value = source.values[cammo.property];
			std::string id = strutil::to_lower(cammo.key);
			add(CharAttrFinding::NoCammo, index, id.c_str(),
			    label + (value.read ? "'s " + std::string(cammo.key) + " is 0" : " has no " + std::string(cammo.key)) + ": in " +
			            cammo.missions + " a player of the class spawns as items.def's first row (or the item whose id is 100000).",
			    value.read ? value.offset : source.section_offset);
		}
	}
	// The ConfigFile pool rule (documents/config_overrun.h) over the file a build packs: the file as it sits while
	// nothing is changed, else the text a save writes (a build saves first).
	std::string saved, why;
	const bool composed = doc->composed(saved, why);
	if (doc->dirty() && !composed) return findings; // a table no file loads as: its save says why
	const std::string &packed = doc->dirty() ? saved : text;
	const auto pool_of = [](const std::string &bytes) {
		return configfile::data_strings_pool(reinterpret_cast<const uint8_t *>(bytes.data()), bytes.size());
	};
	const configfile::DataStringsPool pool = pool_of(packed);
	if (!pool.binary && pool.overrun() != 0) {
		const std::string file = basename_of(document.path());
		const std::vector<configfile::ConfigSection> sections =
				configfile::parse_config_text(reinterpret_cast<const uint8_t *>(packed.data()), packed.size());
		std::string words = config_overrun_words(file, pool);
		// The file as it sits past the line, a save's text under it (the keys at 0 and the lines the game reads
		// nothing of left out): Save writes it so.
		if (!doc->dirty() && composed && pool_of(saved).overrun() == 0)
			words += " Saving it writes the table in the editor's form, from what the game reads of it, which holds " +
			         std::to_string(pool_of(saved).values) + " values: under the line.";
		// Past the block's 36-byte slack it gates; within it, a warning (configfile::fastmem_overrun_reaches_header).
		Diagnostic d = make_finding(CoreFinding::DocumentConfigOverrun,
		                            configfile::fastmem_overrun_reaches_header(pool.overrun()) ? DiagnosticSeverity::Error
		                                                                                       : DiagnosticSeverity::Warning,
		                            std::move(words), document.path());
		d.line = line_of(packed, configfile::data_strings_overrun_offset(sections, pool));
		findings.push_back(std::move(d));
	}
	return findings;
}

} // namespace opennova::editor
