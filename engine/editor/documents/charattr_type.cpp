#include <editor/documents/charattr_type.h>

#include <iterator>
#include <string>
#include <utility>

#include <base/io/strutil.h>
#include <editor/documents/text_types.h>
#include <formats/configfile/config_file.h>
#include <formats/def/reserved_items.h>

namespace opennova::editor {

namespace {

constexpr FindingCodeEntry<CharAttrFinding> kFindingEntries[] = {
	{ CharAttrFinding::Unread, listed_code("charattr.unread_section") },
	{ CharAttrFinding::AttributeWord, listed_code("charattr.attribute_word") },
	{ CharAttrFinding::NotANumber, listed_code("charattr.not_a_number") },
	{ CharAttrFinding::NoCammo, listed_code("charattr.no_cammo") },
};
static_assert(std::size(kFindingEntries) == static_cast<size_t>(CharAttrFinding::kCount),
		"every CharAttrFinding has exactly one row");
static_assert(finding_entries_well_formed(kFindingEntries),
		"the charattr type's rows follow CharAttrFinding's order, each token its own");
constexpr auto kFindingRows = finding_rows(kFindingEntries, FindingGroup::CharAttrs);
static_assert(finding_rows_well_formed(kFindingRows), "every row of the table takes its group");

Diagnostic warning(CharAttrFinding code, std::string message, const TextDocument &document, size_t offset) {
	return text_finding(finding_code(code), DiagnosticSeverity::Warning, std::move(message), document, offset);
}

// The camouflage keys, by the mission's camouflage they serve [orig: Entity_SpawnFromAnimSlotProperty
// @ 0x43c399..0x43c3be: the BMS camouflage selector 1 JUNGLE_CAMMO (10), 2 ARCTIC_CAMMO (12), any other
// DESERT_CAMMO (11)].
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

int32_t cammo_of(const charattr::ClassRow &row, charattr::Property property) {
	return property == charattr::kJungleCammo   ? row.jungle_cammo
	       : property == charattr::kDesertCammo ? row.desert_cammo
	                                            : row.arctic_cammo;
}

std::string class_label(size_t index) { return "CHARACTER" + std::to_string(index + 1); }

} // namespace

const FindingCodeRow &finding_code(CharAttrFinding code) {
	return kFindingRows[static_cast<size_t>(code)];
}

FindingTable charattr_finding_codes() {
	return { kFindingRows.data(), kFindingRows.size() };
}

std::unique_ptr<DocumentBase> make_charattr_document() {
	// The file is its text; the ConfigFile reader ends a line at CR LF alone [orig: ConfigFile_ParseText
	// @ 0x7608a0].
	return std::make_unique<TextDocument>(nullptr, TextLineEnds::CrLf);
}

bool read_charattr_text(const TextDocument &document, charattr::Table &table, charattr::Reading &reading) {
	const std::string &text = document.text();
	return charattr::read_table(reinterpret_cast<const uint8_t *>(text.data()), text.size(), table, &reading);
}

std::vector<Diagnostic> validate_charattr_file(const DocumentBase &document) {
	std::vector<Diagnostic> findings;
	const TextDocument *text = text_of(document);
	if (!text) return findings;
	charattr::Table table;
	charattr::Reading reading;
	if (!read_charattr_text(*text, table, reading)) return findings; // an empty file: the game's own error, a required file's
	// A section the loader never reaches [orig: CharAttr_LoadFromDef @ 0x4121b5..0x4121bf, the classes in order to
	// the first one the file has no section of; ConfigFile_FindSection @ 0x75eeb0, the first of a label].
	const std::string stop = class_label(reading.classes);
	for (const charattr::UnreadSection &unread : reading.unread) {
		std::string why;
		switch (unread.why) {
			case charattr::UnreadSection::Why::AfterMissing:
				why = "the game reads the classes in order from CHARACTER1 and stops at " + stop +
				      ", which the file has no section of";
				break;
			case charattr::UnreadSection::Why::Repeated:
				why = "the game reads the file's first [" + unread.label + "]";
				break;
			case charattr::UnreadSection::Why::NotAClass:
				why = "the game reads [CHARACTER1] to [CHARACTER16] alone";
				break;
		}
		findings.push_back(warning(CharAttrFinding::Unread,
				"[" + unread.label + "] is never read: " + why + ". Nothing in it reaches the game.", *text,
				unread.offset));
	}
	for (size_t index = 0; index < reading.classes; ++index) {
		const charattr::ClassSource &source = reading.sources[index];
		const std::string label = class_label(index);
		// Each ATTRIBUTES word through the attribute table [orig: g_CharAttrAttributeNames @ 0x813F18].
		for (const charattr::ValueSource &word : source.attribute_words) {
			if (charattr::attribute_flag(word.written) != 0) continue;
			const std::string key = strutil::to_upper(word.written);
			if (key == "NULL" || key == "NONE") continue;
			findings.push_back(warning(CharAttrFinding::AttributeWord,
					label + "'s ATTRIBUTES word '" + word.written +
							"' is no attribute (AutoScope, SpreadBonus, KnifeBonus, Medic, WaterGirl): the class gets "
							"nothing for it.",
					*text, word.offset));
		}
		// A number key's value the reader classifies as text reads 0 [orig: effect_get_param_value_0 @ 0x75fb3d].
		for (const charattr::KeySpec &spec : charattr::kScalarKeys) {
			const charattr::ValueSource &value = source.values[spec.property];
			if (!value.read || configfile::classify_numeric(value.written) != 0) continue;
			findings.push_back(warning(CharAttrFinding::NotANumber,
					label + "'s " + spec.key + " '" + value.written + "' is no number: the game reads 0.", *text,
					value.offset));
		}
		// A camouflage with no item: type 0, which the item lookup finds as the item whose id is 100000, else the
		// first items.def row [orig: ItemList_FindIndexByTypeId @ 0x49e100, its 0 for no match @ 0x49e12f].
		for (const Cammo &cammo : kCammo) {
			if (cammo_of(table.rows[index], cammo.property) != 0) continue;
			const charattr::ValueSource &value = source.values[cammo.property];
			findings.push_back(warning(CharAttrFinding::NoCammo,
					label + (value.read ? "'s " + std::string(cammo.key) + " is 0" : " has no " + std::string(cammo.key)) +
							": in " + cammo.missions + " a player of the class spawns as items.def's first row (or "
							"the item whose id is 100000).",
					*text, value.read ? value.offset : source.section_offset));
		}
	}
	return findings;
}

void charattr_references(const TextDocument &document, std::vector<TextReference> &out) {
	charattr::Table table;
	charattr::Reading reading;
	if (!read_charattr_text(document, table, reading)) return;
	for (size_t index = 0; index < reading.classes; ++index) {
		for (const Cammo &cammo : kCammo) {
			const charattr::ValueSource &value = reading.sources[index].values[cammo.property];
			const int32_t type = cammo_of(table.rows[index], cammo.property);
			// 0 names none (NoCammo says what the game makes of it); a token that is no plain integer (a float
			// the reader truncates) is no name a rename could write back.
			if (!value.read || type == 0) continue;
			TextReference reference;
			reference.kind = ReferenceKind::Item;
			// The item's id: the type id plus 100000 [orig: ItemDef_ParseProperty's id arm less 100000 @ 0x49EC54].
			reference.value = std::to_string(int64_t(type) + def::DEF_ITEM_ID_BASE);
			reference.name_offset = def::DEF_ITEM_ID_BASE;
			reference.record = class_label(index);
			reference.field = cammo.key;
			reference.span = document.span_at(value.offset, value.length);
			reference.rewritable = configfile::classify_numeric(value.written) == 1;
			out.push_back(std::move(reference));
		}
	}
}

} // namespace opennova::editor
