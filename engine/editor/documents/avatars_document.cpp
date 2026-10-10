// The avatars table document (avatars_document.h, ADR 0046 S23 B): Avatars.def's parts and nationalities (each
// holding its divisions, each its combinations) as rows over formats/avatars, read and written over the file's
// modeled layout; the fields' words say what the game does with each (docs/playerinfo/avatars-re.md).
#include "avatars_document.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <iterator>

#include <base/io/strutil.h>
#include <editor/assets/asset_kinds.h>
#include <editor/documents/noted_file_state.h>
#include <editor/documents/source_issue_findings.h>
#include <editor/project/project_files.h>

namespace opennova::editor {
namespace {

constexpr NodeKind kPart = node_kind(AvatarsKind::Part);
constexpr NodeKind kNationality = node_kind(AvatarsKind::Nationality);
constexpr NodeKind kDivision = node_kind(AvatarsKind::Division);
constexpr NodeKind kCombo = node_kind(AvatarsKind::Combo);

// The "Avatars" section of the menu shell's text table, Game.bin, which every display key resolves in [orig: the
// menu init @ 0x552510, TextResource_LoadFromArchive("game.bin"); TextResource_GetStringWithFallback @ 0x562ee0
// (resource, "Avatars", key)] (avatars-re "The Avatars section lives in Game.bin").
constexpr const char *kAvatarsText = "GAME.BIN/Avatars";

const AvatarPartRecord &part_of(const RecordHandle &r) { return r.as<AvatarPartRecord>(); }
const AvatarNationalityRecord &nationality_of(const RecordHandle &r) { return r.as<AvatarNationalityRecord>(); }
const AvatarDivisionRecord &division_of(const RecordHandle &r) { return r.as<AvatarDivisionRecord>(); }
const AvatarComboRecord &combo_of(const RecordHandle &r) { return r.as<AvatarComboRecord>(); }

FieldSchema schema_of(const char *id, FieldType type, const char *label, const char *description) {
	FieldSchema schema;
	schema.id = id;
	schema.type = type;
	schema.label = label;
	schema.description = description;
	return schema;
}

// A word of a line the walk cuts into one token: no blank, comma, quote, `;` or `//`, within the bytes its field
// keeps (an empty one ends the line's fields: the writer refuses one before a filled field).
bool set_word(std::string &field, size_t bytes, const Value &value, std::string &error) {
	const auto *text = std::get_if<std::string>(&value);
	if (!text) return error = "The value is a text.", false;
	if (text->find_first_of(" \t,\";\r\n") != std::string::npos || text->find("//") != std::string::npos)
		return error = "The value is one word of the walk: no blank, comma, quote, ';' or '//'.", false;
	if (text->size() >= bytes)
		return error = "The value holds at most " + std::to_string(bytes - 1) + " characters (the game keeps " +
		               std::to_string(bytes) + " bytes).", false;
	field = *text;
	return true;
}

bool set_whole(int &field, int64_t low, int64_t high, const char *what, const Value &value, std::string &error) {
	const auto *whole = std::get_if<int64_t>(&value);
	if (!whole || *whole < low || *whole > high)
		return error = std::string(what) + " is a whole number from " + std::to_string(low) + " to " + std::to_string(high) + ".", false;
	field = int(*whole);
	return true;
}

// The id the reader takes from a nationality's or division's id word: a first byte above '9' (signed) skipped, then
// atol (D-PLAYERINFO-6) [orig: CAvatarDefs_ParseConfigLine @ 0x57A628 / @ 0x57A74E].
int lenient_id(const std::string &word) {
	const char *t = word.c_str();
	if (static_cast<signed char>(t[0]) > '9') ++t;
	return int(std::strtol(t, nullptr, 10));
}

using RF = LabelledField;

LabelledField text_field(const char *id, const char *label, const char *description, size_t width, ReferenceKind reference,
                         std::string AvatarPartRecord::*member) {
	FieldSchema schema = schema_of(id, FieldType::Text, label, description);
	schema.width = width;
	schema.reference = reference;
	if (reference == ReferenceKind::TextId) schema.scope = kAvatarsText;
	return RF{schema,
	          {[member](const RecordHandle &r, Value &out) { return out = part_of(r).*member, true; },
	           [member, width](const RecordHandle &r, const Value &v, std::string &e) {
		           return set_word(r.as<AvatarPartRecord>().*member, width, v, e);
	           }}};
}

RecordTable make_table() {
	// --- a part ------------------------------------------------------------------------------------------
	TableKind part(RecordKindRow{kPart, "part", "Part", "Add part", true});
	{
		FieldSchema kind = schema_of("kind", FieldType::Integer, "Kind",
				"A head, a body or arms: the `define` line's word [orig: CAvatarDefs_ParseConfigLine @ 0x57A49F].");
		kind.choices = {{"head", avatars::AVATAR_PART_HEAD, ""}, {"body", avatars::AVATAR_PART_BODY, ""}, {"arms", avatars::AVATAR_PART_ARMS, ""}};
		part.field(RF{kind,
		              {[](const RecordHandle &r, Value &out) { return out = int64_t(part_of(r).kind), true; },
		               [](const RecordHandle &r, const Value &v, std::string &e) {
			               return set_whole(r.as<AvatarPartRecord>().kind, 0, 2, "A part's kind", v, e);
		               }}});
		LabelledField name = text_field("name", "Name",
				"What a combination names the part by, of its kind: the last part of the kind and the name defined before "
				"the combination, without case [orig: CAvatarDefs_ParseConfigLine @ 0x57A4ED, the lookup @ "
				"0x57A830..0x57A854].",
				64, ReferenceKind::None, &AvatarPartRecord::name);
		name.schema.defines = ReferenceKind::AvatarPart;
		part.field(std::move(name));
		part.field(text_field("display_name", "Shown as",
				"A key of Game.bin's Avatars section, the name the player-info screen shows; a key the section lacks shows "
				"as itself [orig: @ 0x57AB09; TextResource_GetStringWithFallback @ 0x562EE0].",
				64, ReferenceKind::TextId, &AvatarPartRecord::display_name));
		part.field(text_field("graphic", "Model",
				"The part's model (graphic_d reads the same) [orig: @ 0x57AB47; D-PLAYERINFO-3].", 128, ReferenceKind::Model,
				&AvatarPartRecord::graphic));
		part.field(text_field("graphic_j", "Jungle model", "The part's jungle model [orig: @ 0x57ABC3].", 128,
				ReferenceKind::Model, &AvatarPartRecord::graphic_j));
		part.field(text_field("graphic_s", "Snow model", "The part's snow model [orig: @ 0x57AC03].", 128,
				ReferenceKind::Model, &AvatarPartRecord::graphic_s));
		static const char *const kCamo[3] = {"camo_r", "camo_g", "camo_b"};
		for (int i = 0; i < 3; ++i) {
			FieldSchema camo = schema_of(kCamo[i], FieldType::Byte, i == 0 ? "Camouflage" : i == 1 ? "Camouflage 2" : "Camouflage 3",
					"The camo line's value, a byte, the per-part TEX_CAMO control [orig: @ 0x57AC50; avatars-re \"Per-part "
					"TEX_CAMO control stores\"].");
			camo.group = "camo";
			camo.ranged = true;
			camo.min = 0;
			camo.max = 255;
			part.field(RF{camo,
			              {[i](const RecordHandle &r, Value &out) { return out = int64_t(part_of(r).camo[size_t(i)]), true; },
			               [i](const RecordHandle &r, const Value &v, std::string &e) {
				               return set_whole(r.as<AvatarPartRecord>().camo[size_t(i)], 0, 255, "A camouflage value", v, e);
			               }}});
		}
		FieldSchema voice = schema_of("voice", FieldType::Byte, "Voice",
				"The voice set a head speaks with, a byte [orig: @ 0x57ACC0; avatars-re PLAYERVOICE].");
		voice.ranged = true;
		voice.min = 0;
		voice.max = 255;
		part.field(RF{voice,
		              {[](const RecordHandle &r, Value &out) { return out = int64_t(part_of(r).voice), true; },
		               [](const RecordHandle &r, const Value &v, std::string &e) {
			               return set_whole(r.as<AvatarPartRecord>().voice, 0, 255, "A voice", v, e);
		               }}});
		FieldSchema sex = schema_of("sex", FieldType::Integer, "Sex",
				"f female, any other word male [orig: @ 0x57ACEC].");
		sex.choices = {{"m", avatars::AVATAR_SEX_MALE, "male"}, {"f", avatars::AVATAR_SEX_FEMALE, "female"}};
		part.field(RF{sex,
		              {[](const RecordHandle &r, Value &out) { return out = int64_t(part_of(r).sex), true; },
		               [](const RecordHandle &r, const Value &v, std::string &e) {
			               return set_whole(r.as<AvatarPartRecord>().sex, 0, 1, "A sex", v, e);
		               }}});
	}
	// --- a nationality -----------------------------------------------------------------------------------
	TableKind nationality(RecordKindRow{kNationality, "nationality", "Nationality", "Add nationality", true});
	{
		FieldSchema id = schema_of("id", FieldType::Text, "Id",
				"The nationality's id word: a first character above '9' skipped, then its number, 0 to 31; a repeated or "
				"larger one refuses the block [orig: CAvatarDefs_ParseConfigLine @ 0x57A615..0x57A631].");
		id.width = 32;
		nationality.field(RF{id,
		                     {[](const RecordHandle &r, Value &out) { return out = nationality_of(r).raw_id, true; },
		                      [](const RecordHandle &r, const Value &v, std::string &e) {
			                      auto &n = r.as<AvatarNationalityRecord>();
			                      if (!set_word(n.raw_id, 32, v, e)) return false;
			                      n.id = lenient_id(n.raw_id);
			                      return true;
		                      }}});
		FieldSchema key = schema_of("name_key", FieldType::Text, "Shown as",
				"A key of Game.bin's Avatars section [orig: @ 0x57A681].");
		key.width = 64;
		key.reference = ReferenceKind::TextId;
		key.scope = kAvatarsText;
		nationality.field(RF{key,
		                     {[](const RecordHandle &r, Value &out) { return out = nationality_of(r).name_key, true; },
		                      [](const RecordHandle &r, const Value &v, std::string &e) {
			                      return set_word(r.as<AvatarNationalityRecord>().name_key, 64, v, e);
		                      }}});
		FieldSchema flags = schema_of("flags", FieldType::Text, "Words after",
				"The words after the key (skipdemo); the game reads none of them.");
		flags.width = 128;
		nationality.field(RF{flags,
		                     {[](const RecordHandle &r, Value &out) { return out = nationality_of(r).flags, true; },
		                      [](const RecordHandle &r, const Value &v, std::string &e) {
			                      const auto *text = std::get_if<std::string>(&v);
			                      if (!text || text->size() >= 128 || text->find_first_of("\",;\r\n") != std::string::npos)
				                      return e = "The words are plain words separated by blanks.", false;
			                      r.as<AvatarNationalityRecord>().flags = *text;
			                      return true;
		                      }}});
		FieldSchema alignment = schema_of("alignment", FieldType::Integer, "Alignment",
				"good or evil; any other word changes nothing [orig: @ 0x57A6BC].");
		alignment.choices = {{"good", avatars::AVATAR_ALIGN_GOOD, ""}, {"evil", avatars::AVATAR_ALIGN_EVIL, ""}};
		alignment.optional = true;
		nationality.field(RF{alignment,
		                     {[](const RecordHandle &r, Value &out) { return out = int64_t(nationality_of(r).alignment), true; },
		                      [](const RecordHandle &r, const Value &v, std::string &e) {
			                      auto &n = r.as<AvatarNationalityRecord>();
			                      const int held = n.alignment;
			                      if (!set_whole(n.alignment, 0, 1, "An alignment", v, e)) return false;
			                      // A Set of another value writes the line; one of the value it holds changes nothing.
			                      if (n.alignment != held) n.has_alignment = true;
			                      return true;
		                      },
		                      [](const RecordHandle &r) { return nationality_of(r).has_alignment; },
		                      [](const RecordHandle &r, bool present, std::string &) {
			                      r.as<AvatarNationalityRecord>().has_alignment = present;
			                      return true;
		                      }}});
		TableList divisions;
		divisions.spec = Document::CollectionSpec{kDivision, "Divisions", "id", false, Applicability::Reads, 16};
		divisions.ops = vector_list<AvatarNationalityRecord, AvatarDivisionRecord>(
				kDivision, [](AvatarNationalityRecord &n) -> std::vector<AvatarDivisionRecord> & { return n.divisions; });
		nationality.list(std::move(divisions));
	}
	// --- a division --------------------------------------------------------------------------------------
	TableKind division(RecordKindRow{kDivision, "division", "Division", "", false});
	{
		FieldSchema id = schema_of("id", FieldType::Text, "Id",
				"The division's id word: a first character above '9' skipped, then its number, 0 to 15 [orig: @ "
				"0x57A73B..0x57A757].");
		id.width = 32;
		division.field(RF{id,
		                  {[](const RecordHandle &r, Value &out) { return out = division_of(r).raw_id, true; },
		                   [](const RecordHandle &r, const Value &v, std::string &e) {
			                   auto &d = r.as<AvatarDivisionRecord>();
			                   if (!set_word(d.raw_id, 32, v, e)) return false;
			                   d.id = lenient_id(d.raw_id);
			                   return true;
		                   }}});
		FieldSchema key = schema_of("name_key", FieldType::Text, "Shown as", "A key of Game.bin's Avatars section [orig: @ 0x57A7AB].");
		key.width = 64;
		key.reference = ReferenceKind::TextId;
		key.scope = kAvatarsText;
		division.field(RF{key,
		                  {[](const RecordHandle &r, Value &out) { return out = division_of(r).name_key, true; },
		                   [](const RecordHandle &r, const Value &v, std::string &e) {
			                   return set_word(r.as<AvatarDivisionRecord>().name_key, 64, v, e);
		                   }}});
		FieldSchema flags = schema_of("flags", FieldType::Text, "Words after", "The words after the key (skipdemo).");
		flags.width = 128;
		division.field(RF{flags,
		                  {[](const RecordHandle &r, Value &out) { return out = division_of(r).flags, true; },
		                   [](const RecordHandle &r, const Value &v, std::string &e) {
			                   const auto *text = std::get_if<std::string>(&v);
			                   if (!text || text->size() >= 128 || text->find_first_of("\",;\r\n") != std::string::npos)
				                   return e = "The words are plain words separated by blanks.", false;
			                   r.as<AvatarDivisionRecord>().flags = *text;
			                   return true;
		                   }}});
		TableList combos;
		combos.spec = Document::CollectionSpec{kCombo, "Combinations", "id", false, Applicability::Reads, 0};
		combos.ops = vector_list<AvatarDivisionRecord, AvatarComboRecord>(
				kCombo, [](AvatarDivisionRecord &d) -> std::vector<AvatarComboRecord> & { return d.combos; });
		division.list(std::move(combos));
	}
	// --- a combination -----------------------------------------------------------------------------------
	TableKind combo(RecordKindRow{kCombo, "combo", "Combination", "", false});
	{
		FieldSchema id = schema_of("id", FieldType::Text, "Id", "The combination's id word, read by atol [orig: @ 0x57A90D].");
		id.width = 32;
		combo.field(RF{id,
		               {[](const RecordHandle &r, Value &out) { return out = combo_of(r).raw_id, true; },
		                [](const RecordHandle &r, const Value &v, std::string &e) {
			                auto &c = r.as<AvatarComboRecord>();
			                if (!set_word(c.raw_id, 32, v, e)) return false;
			                c.id = int(std::strtol(c.raw_id.c_str(), nullptr, 10));
			                return true;
		                }}});
		const auto part_ref = [&](const char *id, const char *label, std::string AvatarComboRecord::*member, const char *desc) {
			FieldSchema schema = schema_of(id, FieldType::Text, label, desc);
			schema.width = 64;
			schema.reference = ReferenceKind::AvatarPart;
			combo.field(RF{schema,
			               {[member](const RecordHandle &r, Value &out) { return out = combo_of(r).*member, true; },
			                [member](const RecordHandle &r, const Value &v, std::string &e) {
				                return set_word(r.as<AvatarComboRecord>().*member, 64, v, e);
			                }}});
		};
		part_ref("head", "Head", &AvatarComboRecord::head,
		         "A head defined before the combination, the last of the name; none drops the combination [orig: @ 0x57A804].");
		part_ref("body", "Body", &AvatarComboRecord::body,
		         "A body defined before the combination, the last of the name; none drops the combination.");
		part_ref("arms", "Arms", &AvatarComboRecord::arms,
		         "Arms defined before the combination, the last of the name; none (or no word) leaves it without arms, and "
		         "first person then draws none.");
	}
	return RecordTable({std::move(part), std::move(nationality), std::move(division), std::move(combo)});
}

void copy_to(const std::string &from, char *to, size_t size) { std::snprintf(to, size, "%s", from.c_str()); }

constexpr FindingCodeEntry<AvatarsFinding> kFindingEntries[] = {
	{ AvatarsFinding::InvalidInput, { "avatars.invalid_input", FindingFix::None, nullptr, true } },
	{ AvatarsFinding::IgnoredInput, listed_code("avatars.ignored_input") },
};
static_assert(std::size(kFindingEntries) == static_cast<size_t>(AvatarsFinding::kCount),
		"every AvatarsFinding has exactly one row");
static_assert(finding_entries_well_formed(kFindingEntries),
		"the avatars table's rows follow AvatarsFinding's order, each token its own");
constexpr auto kFindingRows = finding_rows(kFindingEntries, FindingGroup::AvatarTables);
static_assert(finding_rows_well_formed(kFindingRows), "every row of the table takes its group");

} // namespace

const RecordTable &avatars_table() {
	static const RecordTable table = make_table();
	return table;
}

bool is_avatars_kind(AssetKind kind) { return asset_kind_row(kind).document == DocumentTypeId::Avatars; }

std::string avatar_part_scope(const std::string &path, int kind) {
	static const char *const kKinds[] = {"HEAD", "BODY", "ARMS"};
	if (kind < 0 || kind > 2) return std::string();
	return strutil::to_upper(basename_of(path)) + "/" + kKinds[kind];
}

size_t AvatarPartRow::footprint() const {
	size_t bytes = sizeof(AvatarPartRow) + footprint_of(part.name) + footprint_of(part.display_name) + footprint_of(part.graphic) +
	               footprint_of(part.graphic_j) + footprint_of(part.graphic_s) + ids_footprint();
	return bytes;
}

size_t AvatarNationalityRow::footprint() const {
	const AvatarNationalityRecord &n = nationality;
	size_t bytes = sizeof(AvatarNationalityRow) + footprint_of(n.raw_id) + footprint_of(n.name_key) + footprint_of(n.flags) +
	               footprint_of(n.divisions) + ids_footprint();
	for (const AvatarDivisionRecord &d : n.divisions) {
		bytes += footprint_of(d.raw_id) + footprint_of(d.name_key) + footprint_of(d.flags) + footprint_of(d.combos);
		for (const AvatarComboRecord &c : d.combos)
			bytes += footprint_of(c.raw_id) + footprint_of(c.head) + footprint_of(c.body) + footprint_of(c.arms);
	}
	return bytes;
}

avatars::AvatarsFile AvatarsDocument::file() const {
	avatars::AvatarsFile out{};
	if (const auto *noted = noted_layout(file_state())) out.note = noted->root();
	std::vector<const AvatarPartRecord *> parts;
	std::vector<const AvatarNationalityRecord *> nationalities;
	for (const auto &node : rows()) {
		if (!node) continue;
		if (node->kind == kPart) parts.push_back(&static_cast<const AvatarPartRow &>(*node).part);
		if (node->kind == kNationality) nationalities.push_back(&static_cast<const AvatarNationalityRow &>(*node).nationality);
	}
	out.parts_count = parts.size();
	out.parts = parts.empty() ? nullptr : static_cast<avatars::AvatarPart *>(std::calloc(parts.size(), sizeof(avatars::AvatarPart)));
	for (size_t i = 0; i < parts.size(); ++i) {
		const AvatarPartRecord &p = *parts[i];
		avatars::AvatarPart &q = out.parts[i];
		q.kind = p.kind;
		copy_to(p.name, q.name, sizeof(q.name));
		copy_to(p.display_name, q.display_name, sizeof(q.display_name));
		copy_to(p.graphic, q.graphic, sizeof(q.graphic));
		copy_to(p.graphic_j, q.graphic_j, sizeof(q.graphic_j));
		copy_to(p.graphic_s, q.graphic_s, sizeof(q.graphic_s));
		for (int c = 0; c < 3; ++c) q.camo[c] = p.camo[size_t(c)];
		q.voice = p.voice;
		q.sex = p.sex;
		q.note = p.note;
	}
	out.nationalities_count = nationalities.size();
	out.nationalities = nationalities.empty() ? nullptr
	        : static_cast<avatars::AvatarNationality *>(std::calloc(nationalities.size(), sizeof(avatars::AvatarNationality)));
	for (size_t i = 0; i < nationalities.size(); ++i) {
		const AvatarNationalityRecord &n = *nationalities[i];
		avatars::AvatarNationality &m = out.nationalities[i];
		copy_to(n.raw_id, m.raw_id, sizeof(m.raw_id));
		m.id = n.id;
		copy_to(n.name_key, m.name_key, sizeof(m.name_key));
		copy_to(n.flags, m.flags, sizeof(m.flags));
		m.alignment = n.alignment;
		m.has_alignment = n.has_alignment ? 1 : 0;
		m.note = n.note;
		m.divisions_count = n.divisions.size();
		m.divisions = n.divisions.empty() ? nullptr
		        : static_cast<avatars::AvatarDivision *>(std::calloc(n.divisions.size(), sizeof(avatars::AvatarDivision)));
		for (size_t j = 0; j < n.divisions.size(); ++j) {
			const AvatarDivisionRecord &d = n.divisions[j];
			avatars::AvatarDivision &e = m.divisions[j];
			copy_to(d.raw_id, e.raw_id, sizeof(e.raw_id));
			e.id = d.id;
			copy_to(d.name_key, e.name_key, sizeof(e.name_key));
			copy_to(d.flags, e.flags, sizeof(e.flags));
			e.note = d.note;
			e.combos_count = d.combos.size();
			e.combos = d.combos.empty() ? nullptr
			        : static_cast<avatars::AvatarCombo *>(std::calloc(d.combos.size(), sizeof(avatars::AvatarCombo)));
			for (size_t k = 0; k < d.combos.size(); ++k) {
				const AvatarComboRecord &c = d.combos[k];
				avatars::AvatarCombo &f = e.combos[k];
				copy_to(c.raw_id, f.raw_id, sizeof(f.raw_id));
				f.id = c.id;
				copy_to(c.head, f.head_name, sizeof(f.head_name));
				copy_to(c.body, f.body_name, sizeof(f.body_name));
				copy_to(c.arms, f.arms_name, sizeof(f.arms_name));
				f.has_arms = c.arms.empty() ? 0 : 1;
				f.note = c.note;
			}
		}
	}
	return out;
}

std::string AvatarsDocument::record_title(const NodeAddress &address) const {
	static const char *const kKinds[] = {"Head", "Body", "Arms"};
	const Node *node = row(address.row);
	if (!node) return std::string();
	if (!address.child) {
		if (node->kind == kPart) {
			const AvatarPartRecord &p = static_cast<const AvatarPartRow &>(*node).part;
			return std::string(p.kind >= 0 && p.kind <= 2 ? kKinds[p.kind] : "Part") + " " + p.name;
		}
		const AvatarNationalityRecord &n = static_cast<const AvatarNationalityRow &>(*node).nationality;
		return "Nationality " + n.raw_id + " " + n.name_key;
	}
	const RecordHandle handle = record_in(*node, address);
	if (!handle) return record_name(address);
	if (address.kind == kDivision) return "Division " + division_of(handle).raw_id + " " + division_of(handle).name_key;
	const AvatarComboRecord &c = combo_of(handle);
	return "Combination " + c.raw_id + ": " + c.head + ", " + c.body + (c.arms.empty() ? std::string() : ", " + c.arms);
}

bool AvatarsDocument::parse(const std::vector<uint8_t> &bytes, std::vector<std::shared_ptr<Node>> &rows,
                            std::shared_ptr<const FileState> &state, std::vector<SourceIssue> &issues, Diagnostic &error) {
	if (!is_avatars_kind(kind())) {
		error = make_finding(CoreFinding::DocumentKind, DiagnosticSeverity::Error, "This file is not an avatars table.", path());
		return false;
	}
	auto notes = std::make_shared<textlayout::Notes>();
	avatars::AvatarsFile read{};
	if (avatars::avatars_parse_memory(bytes.data(), bytes.size(), &read, *notes) != 0) {
		// The walk ends where the part table is full (D-PLAYERINFO-2) or a 129th combination writes through no row:
		// the game's own failure, which the document cannot carry.
		error = make_finding(CoreFinding::DocumentParse, DiagnosticSeverity::Error,
		                     "The avatar table's reader stops: 512 parts or 129 combinations (the game's walk ends there, "
		                     "ComboObj Parse Error) [orig: CAvatarDefs_ParseConfigLine @ 0x57A456; sub_579E10 @ 0x579E10].",
		                     path());
		return false;
	}
	for (size_t i = 0; i < read.diagnostics_count; ++i) {
		const avatars::AvatarDiagnostic &note = read.diagnostics[i];
		issues.push_back({false, note.line, std::string(), std::string(), std::string("The game's avatar reader: ") + note.message + "."});
	}
	// Parts first, then nationalities: each kind keeps its order; the layout keeps where each stood.
	for (size_t i = 0; i < read.parts_count; ++i) {
		const avatars::AvatarPart &p = read.parts[i];
		auto row = std::make_shared<AvatarPartRow>();
		AvatarPartRecord &q = row->part;
		q.kind = p.kind;
		q.name = p.name;
		q.display_name = p.display_name;
		q.graphic = p.graphic;
		q.graphic_j = p.graphic_j;
		q.graphic_s = p.graphic_s;
		for (int c = 0; c < 3; ++c) q.camo[size_t(c)] = p.camo[c];
		q.voice = p.voice;
		q.sex = p.sex;
		q.note = p.note;
		shape(*row);
		rows.push_back(std::move(row));
	}
	for (size_t i = 0; i < read.nationalities_count; ++i) {
		const avatars::AvatarNationality &n = read.nationalities[i];
		auto row = std::make_shared<AvatarNationalityRow>();
		AvatarNationalityRecord &m = row->nationality;
		m.raw_id = n.raw_id;
		m.id = n.id;
		m.name_key = n.name_key;
		m.flags = n.flags;
		m.alignment = n.alignment;
		m.has_alignment = n.has_alignment != 0;
		m.note = n.note;
		for (size_t j = 0; j < n.divisions_count; ++j) {
			const avatars::AvatarDivision &d = n.divisions[j];
			AvatarDivisionRecord e;
			e.raw_id = d.raw_id;
			e.id = d.id;
			e.name_key = d.name_key;
			e.flags = d.flags;
			e.note = d.note;
			for (size_t k = 0; k < d.combos_count; ++k) {
				const avatars::AvatarCombo &c = d.combos[k];
				AvatarComboRecord f;
				f.raw_id = c.raw_id;
				f.id = c.id;
				f.head = c.head_name;
				f.body = c.body_name;
				f.arms = c.has_arms ? c.arms_name : "";
				f.note = c.note;
				e.combos.push_back(std::move(f));
			}
			m.divisions.push_back(std::move(e));
		}
		shape(*row);
		rows.push_back(std::move(row));
	}
	avatars::avatars_free(&read);
	auto noted = std::make_shared<NotedFileState>();
	noted->notes = std::move(notes);
	state = std::move(noted);
	return true;
}

SerializeResult AvatarsDocument::serialize() const {
	SerializeResult result;
	avatars::AvatarsFile table = file();
	char *data = nullptr;
	size_t size = 0;
	bool rewritten = false;
	const int rc = avatars::avatars_write(&table, noted_layout(file_state()), &data, &size, &rewritten);
	avatars::avatars_free(&table);
	if (rc != 0 || !data) {
		result.issues.push_back({true, 0, std::string(), std::string(),
		                         rc == 2 ? "A line would hold an empty word ahead of a filled one, which the game's reader "
		                                   "reads one place early: give each id, key and part a word."
		                                 : "The avatar table could not be written."});
		avatars::avatars_free_buffer(data);
		return result;
	}
	result.text.assign(data, size);
	avatars::avatars_free_buffer(data);
	if (rewritten) result.notes.push_back("The table is written in the editor's form: the file's lines would not read back as it is.");
	return result;
}

std::string AvatarsDocument::save_words() const {
	return "Saving writes the file in the form it was read in: its comments, its spacing and the lines the game reads "
	       "nothing of; a changed value changes its own line, a new record goes after the last of its kind.";
}

std::shared_ptr<Node> AvatarsDocument::make_node(NodeKind kind, NodeId, const std::vector<std::shared_ptr<const Node>> &rows,
                                                 std::string &error) {
	if (kind == kPart) {
		auto row = std::make_shared<AvatarPartRow>();
		// A head named apart from the file's heads (NEW_HEAD, NEW_HEAD_2, ...).
		const auto taken = [&](const std::string &name) {
			return std::any_of(rows.begin(), rows.end(), [&](const std::shared_ptr<const Node> &other) {
				return other && other->kind == kPart && strutil::iequals(other->name(), name);
			});
		};
		std::string name = "NEW_HEAD";
		for (int n = 2; taken(name); ++n) name = "NEW_HEAD_" + std::to_string(n);
		row->part.name = name;
		shape(*row);
		return row;
	}
	if (kind == kNationality) {
		auto row = std::make_shared<AvatarNationalityRow>();
		// The first id 0..31 no nationality has (the reader refuses a repeated one).
		for (int id = 0; id < 32 && row->nationality.raw_id.empty(); ++id) {
			const bool used = std::any_of(rows.begin(), rows.end(), [&](const std::shared_ptr<const Node> &other) {
				return other && other->kind == kNationality &&
				       static_cast<const AvatarNationalityRow &>(*other).nationality.id == id;
			});
			if (used) continue;
			char word[8];
			std::snprintf(word, sizeof(word), "N%02d", id);
			row->nationality.raw_id = word;
			row->nationality.id = id;
		}
		if (row->nationality.raw_id.empty()) {
			error = "The table has a nationality of every id 0 to 31, all the game holds.";
			return nullptr;
		}
		row->nationality.name_key = "AV_NAT_NEW";
		shape(*row);
		return row;
	}
	error = "An avatars table's rows are its parts and nationalities; a division goes into a nationality.";
	return nullptr;
}

void AvatarsDocument::prepare_duplicate(Node &copy, const Node &, const std::vector<std::shared_ptr<const Node>> &) const {
	if (copy.kind == kPart) static_cast<AvatarPartRow &>(copy).part.note = 0;
	if (copy.kind == kNationality) {
		AvatarNationalityRecord &n = static_cast<AvatarNationalityRow &>(copy).nationality;
		n.note = 0;
		for (AvatarDivisionRecord &d : n.divisions) {
			d.note = 0;
			for (AvatarComboRecord &c : d.combos) c.note = 0;
		}
	}
}

void AvatarsDocument::prepare_record(const Node &, const ListChange &change, DetachedRecord &record) const {
	if (change.operation != EditOperation::Duplicate || !record.data) return;
	if (record.kind == kDivision) {
		auto &d = *static_cast<AvatarDivisionRecord *>(record.data.get());
		d.note = 0;
		for (AvatarComboRecord &c : d.combos) c.note = 0;
	}
	if (record.kind == kCombo) static_cast<AvatarComboRecord *>(record.data.get())->note = 0;
}

// The first part of a kind the file defines, by name ("" for none).
std::string AvatarsDocument::first_part(int kind) const {
	for (const auto &node : rows())
		if (node && node->kind == kPart && static_cast<const AvatarPartRow &>(*node).part.kind == kind)
			return static_cast<const AvatarPartRow &>(*node).part.name;
	return std::string();
}

bool AvatarsDocument::accept_list_edit(const Node &row, const ListChange &change, std::string &error) const {
	if (change.operation != EditOperation::Add) return TableDocument::accept_list_edit(row, change, error);
	if (change.owner && change.owner->record.kind == kDivision &&
	    (first_part(avatars::AVATAR_PART_HEAD).empty() || first_part(avatars::AVATAR_PART_BODY).empty())) {
		// The reader drops a combination whose head or body it does not find [orig: CAvatarDefs_ParseConfigLine @
		// 0x57A804].
		error = "A combination names a head and a body the file defines before it, and the file defines none of one.";
		return false;
	}
	return TableDocument::accept_list_edit(row, change, error);
}

void AvatarsDocument::after_add(Node &row, const ListChange &change, const RecordHandle &made) {
	if (made.kind == kDivision && row.kind == kNationality) {
		// The first id 0..15 the nationality has no division of (the reader refuses a repeated one).
		const AvatarNationalityRecord &n = static_cast<const AvatarNationalityRow &>(row).nationality;
		AvatarDivisionRecord &d = made.as<AvatarDivisionRecord>();
		for (int id = 0; id < 16; ++id) {
			const bool used = std::any_of(n.divisions.begin(), n.divisions.end(), [&](const AvatarDivisionRecord &other) {
				return &other != &d && !other.raw_id.empty() && other.id == id;
			});
			if (used) continue;
			char word[8];
			std::snprintf(word, sizeof(word), "D%02d", id);
			d.raw_id = word;
			d.id = id;
			break;
		}
		d.name_key = "AV_DIV_NEW";
	}
	if (made.kind == kCombo && change.owner) {
		// The next number of its division, the file's first head, body and arms.
		const AvatarDivisionRecord &d = change.owner->record.as<AvatarDivisionRecord>();
		AvatarComboRecord &c = made.as<AvatarComboRecord>();
		int next = 1;
		for (const AvatarComboRecord &other : d.combos)
			if (&other != &c && !other.raw_id.empty()) next = std::max(next, other.id + 1);
		c.raw_id = std::to_string(next);
		c.id = next;
		c.head = first_part(avatars::AVATAR_PART_HEAD);
		c.body = first_part(avatars::AVATAR_PART_BODY);
		c.arms = first_part(avatars::AVATAR_PART_ARMS);
	}
}

void AvatarsDocument::refine_field(const NodeAddress &address, FieldUse &use) const {
	TableDocument::refine_field(address, use);
	const Node *node = row(address.row);
	if (!node) return;
	if (use.defines == ReferenceKind::AvatarPart && node->kind == kPart)
		use.scope = avatar_part_scope(path(), static_cast<const AvatarPartRow &>(*node).part.kind);
	if (use.reference == ReferenceKind::AvatarPart && address.kind == kCombo) {
		const std::string &field = use.schema->id;
		use.scope = avatar_part_scope(path(), field == "head" ? avatars::AVATAR_PART_HEAD
		                                      : field == "body" ? avatars::AVATAR_PART_BODY : avatars::AVATAR_PART_ARMS);
	}
}

void AvatarsDocument::refine_symbol(const NodeAddress &address, SymbolFacts &facts) const {
	const Node *node = row(address.row);
	if (!node || address.child || node->kind != kPart) return;
	const AvatarPartRecord &p = static_cast<const AvatarPartRow &>(*node).part;
	bool after = false;
	for (const auto &other : rows()) {
		if (!other) continue;
		if (other.get() == node) {
			after = true;
			continue;
		}
		if (!after || other->kind != kPart) continue;
		const AvatarPartRecord &q = static_cast<const AvatarPartRow &>(*other).part;
		if (q.kind == p.kind && strutil::iequals(q.name, p.name)) {
			facts.inert = true;
			facts.inert_reason = "a later part of the name replaces it for the combinations after both";
			return;
		}
	}
}

const FindingCodeRow &finding_code(AvatarsFinding code) { return kFindingRows[static_cast<size_t>(code)]; }

FindingTable avatars_finding_codes() { return { kFindingRows.data(), kFindingRows.size() }; }

std::vector<Diagnostic> validate_avatars_file(const DocumentBase &document) {
	std::vector<Diagnostic> findings;
	const auto *table = dynamic_cast<const AvatarsDocument *>(&document);
	if (!table) return findings;
	source_issue_findings(*table, finding_code(AvatarsFinding::InvalidInput), finding_code(AvatarsFinding::IgnoredInput), findings);
	return findings;
}

} // namespace opennova::editor
