// The avatars table document (avatars_document.h, ADR 0046 S23 B): Avatars.def's parts and nationalities (each
// holding its divisions, each its combinations) as rows over formats/avatars, read through its reader and written
// by its writer from the rows alone (S26); the fields' words say what the game does with each
// (docs/playerinfo/avatars-re.md).
#include "avatars_document.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <set>

#include <base/io/strutil.h>
#include <editor/assets/asset_kinds.h>
#include <editor/documents/source_issue_findings.h>
#include <editor/model/staged_rows.h>
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
		FieldSchema id = schema_of("id", FieldType::Integer, "Id",
				"The nationality's id, the number its id word reads as (a first character above '9' skipped, then atol), 0 "
				"to 31, written as its decimal; a repeated or larger one refuses the block [orig: CAvatarDefs_ParseConfigLine "
				"@ 0x57A615..0x57A631].");
		id.ranged = true;
		id.min = 0;
		id.max = 31;
		nationality.field(RF{id,
		                     {[](const RecordHandle &r, Value &out) { return out = int64_t(nationality_of(r).id), true; },
		                      [](const RecordHandle &r, const Value &v, std::string &e) {
			                      return set_whole(r.as<AvatarNationalityRecord>().id, 0, 31, "A nationality's id", v, e);
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
		FieldSchema id = schema_of("id", FieldType::Integer, "Id",
				"The division's id, the number its id word reads as (a first character above '9' skipped, then atol), 0 to "
				"15, written as its decimal [orig: @ 0x57A73B..0x57A757].");
		id.ranged = true;
		id.min = 0;
		id.max = 15;
		division.field(RF{id,
		                  {[](const RecordHandle &r, Value &out) { return out = int64_t(division_of(r).id), true; },
		                   [](const RecordHandle &r, const Value &v, std::string &e) {
			                   return set_whole(r.as<AvatarDivisionRecord>().id, 0, 15, "A division's id", v, e);
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
		TableList combos;
		combos.spec = Document::CollectionSpec{kCombo, "Combinations", "id", false, Applicability::Reads, 0};
		combos.ops = vector_list<AvatarDivisionRecord, AvatarComboRecord>(
				kCombo, [](AvatarDivisionRecord &d) -> std::vector<AvatarComboRecord> & { return d.combos; });
		division.list(std::move(combos));
	}
	// --- a combination -----------------------------------------------------------------------------------
	TableKind combo(RecordKindRow{kCombo, "combo", "Combination", "", false});
	{
		FieldSchema id = schema_of("id", FieldType::Integer, "Id",
				"The combination's id, the number its id word reads as (atol), written as its decimal [orig: @ 0x57A90D].");
		combo.field(RF{id,
		               {[](const RecordHandle &r, Value &out) { return out = int64_t(combo_of(r).id), true; },
		                [](const RecordHandle &r, const Value &v, std::string &e) {
			                return set_whole(r.as<AvatarComboRecord>().id, INT32_MIN, INT32_MAX, "A combination's id", v, e);
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

using Rows = std::vector<std::shared_ptr<const Node>>;

const AvatarPartRecord *part_row(const std::shared_ptr<const Node> &node) {
	return node && node->kind == kPart ? &static_cast<const AvatarPartRow &>(*node).part : nullptr;
}

// The part a combination of the nationality at `nationality` (its place among the rows) names by `name`: the last
// part of the kind and the name, without case, above it in the file [orig: CAvatarDefs_ParseConfigLine @
// 0x57A830..0x57A8D3, each scan running the parts the walk has defined so far, g_AvatarPartCount, a match
// overwriting the one before]; null for none.
const Node *bound_part(const Rows &rows, size_t nationality, int kind, const std::string &name) {
	const Node *found = nullptr;
	for (size_t i = 0; i < nationality && i < rows.size(); ++i) {
		const AvatarPartRecord *p = part_row(rows[i]);
		if (p && p->kind == kind && strutil::iequals(p->name, name)) found = rows[i].get();
	}
	return found;
}

// Where the game's reader stops in a file it fails on: the first line its walk fails at, the shortest run of the
// file's whole lines it fails on (the walk goes line by line, so every longer run fails too), with the bytes before
// that line in `before`; 0 where it reads the whole file.
size_t reader_stop(const std::vector<uint8_t> &bytes, size_t &before) {
	std::vector<size_t> ends; // each line's end, past its '\n' (the last the file's end)
	for (size_t i = 0; i < bytes.size(); ++i)
		if (bytes[i] == '\n') ends.push_back(i + 1);
	if (ends.empty() || ends.back() != bytes.size()) ends.push_back(bytes.size());
	const auto fails = [&bytes, &ends](size_t lines) {
		avatars::AvatarsFile probe{};
		if (avatars::avatars_parse_memory(bytes.data(), ends[lines - 1], &probe) != 0) return true;
		avatars::avatars_free(&probe);
		return false;
	};
	size_t low = 1, high = ends.size();
	if (!fails(high)) return 0;
	while (low < high) {
		const size_t mid = low + (high - low) / 2;
		if (fails(mid)) high = mid;
		else low = mid + 1;
	}
	before = low > 1 ? ends[low - 2] : 0;
	return low;
}


constexpr FindingCodeEntry<AvatarsFinding> kFindingEntries[] = {
	{ AvatarsFinding::InvalidInput, { "avatars.invalid_input", FindingFix::None, nullptr, true } },
	// What the reader reads nothing of: a save writes the table alone (formats/avatars), without it.
	{ AvatarsFinding::IgnoredInput, listed_code("avatars.ignored_input", FindingFix::Rewrite, kRewriteDropsIgnoredInput) },
	// The reader stopping (the document's game_stops issue, or the text a save writes): it gates, the game's failure.
	{ AvatarsFinding::ReaderStops,
	  game_stops_code("avatars.reader_stops",
	                  "the avatar table's reader stops at a 513th part or a 129th combination and the game fails "
	                  "(ComboObj Parse Error) [orig: CAvatarDefs_ParseConfigLine @ 0x57A456; CAvatarDefs_AllocCombo @ 0x579E10; "
	                  "D-PLAYERINFO-2]") },
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
	size_t bytes = sizeof(AvatarNationalityRow) + footprint_of(n.name_key) + footprint_of(n.divisions) + ids_footprint();
	for (const AvatarDivisionRecord &d : n.divisions) {
		bytes += footprint_of(d.name_key) + footprint_of(d.combos);
		for (const AvatarComboRecord &c : d.combos) bytes += footprint_of(c.head) + footprint_of(c.body) + footprint_of(c.arms);
	}
	return bytes;
}

avatars::AvatarsFile AvatarsDocument::file() const {
	avatars::AvatarsFile out{};
	std::vector<const AvatarPartRecord *> parts;
	std::vector<const AvatarNationalityRecord *> nationalities;
	std::vector<size_t> parts_above; // each nationality's place among the parts: the parts above it
	for (const auto &node : rows()) {
		if (!node) continue;
		if (node->kind == kPart) parts.push_back(&static_cast<const AvatarPartRow &>(*node).part);
		if (node->kind == kNationality) {
			nationalities.push_back(&static_cast<const AvatarNationalityRow &>(*node).nationality);
			parts_above.push_back(parts.size());
		}
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
	}
	out.nationalities_count = nationalities.size();
	out.nationalities = nationalities.empty() ? nullptr
	        : static_cast<avatars::AvatarNationality *>(std::calloc(nationalities.size(), sizeof(avatars::AvatarNationality)));
	for (size_t i = 0; i < nationalities.size(); ++i) {
		const AvatarNationalityRecord &n = *nationalities[i];
		avatars::AvatarNationality &m = out.nationalities[i];
		m.id = n.id;
		copy_to(n.name_key, m.name_key, sizeof(m.name_key));
		m.alignment = n.alignment;
		m.has_alignment = n.has_alignment ? 1 : 0;
		m.parts_below = parts.size() - parts_above[i];
		m.divisions_count = n.divisions.size();
		m.divisions = n.divisions.empty() ? nullptr
		        : static_cast<avatars::AvatarDivision *>(std::calloc(n.divisions.size(), sizeof(avatars::AvatarDivision)));
		for (size_t j = 0; j < n.divisions.size(); ++j) {
			const AvatarDivisionRecord &d = n.divisions[j];
			avatars::AvatarDivision &e = m.divisions[j];
			e.id = d.id;
			copy_to(d.name_key, e.name_key, sizeof(e.name_key));
			e.combos_count = d.combos.size();
			e.combos = d.combos.empty() ? nullptr
			        : static_cast<avatars::AvatarCombo *>(std::calloc(d.combos.size(), sizeof(avatars::AvatarCombo)));
			for (size_t k = 0; k < d.combos.size(); ++k) {
				const AvatarComboRecord &c = d.combos[k];
				avatars::AvatarCombo &f = e.combos[k];
				f.id = c.id;
				copy_to(c.head, f.head_name, sizeof(f.head_name));
				copy_to(c.body, f.body_name, sizeof(f.body_name));
				copy_to(c.arms, f.arms_name, sizeof(f.arms_name));
				f.has_arms = c.arms.empty() ? 0 : 1;
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
		return "Nationality " + std::to_string(n.id) + " " + n.name_key;
	}
	const RecordHandle handle = record_in(*node, address);
	if (!handle) return record_name(address);
	if (address.kind == kDivision)
		return "Division " + std::to_string(division_of(handle).id) + " " + division_of(handle).name_key;
	const AvatarComboRecord &c = combo_of(handle);
	return "Combination " + std::to_string(c.id) + ": " + c.head + ", " + c.body + (c.arms.empty() ? std::string() : ", " + c.arms);
}

bool AvatarsDocument::parse(const std::vector<uint8_t> &bytes, std::vector<std::shared_ptr<Node>> &rows,
                            std::shared_ptr<const FileState> &, std::vector<SourceIssue> &issues, Diagnostic &error) {
	avatars::AvatarsFile read{};
	reader_stops_line_ = 0;
	if (avatars::avatars_parse_memory(bytes.data(), bytes.size(), &read) != 0) {
		// The walk ends where the part table is full (D-PLAYERINFO-2) or a 129th combination writes through no row:
		// the game's own failure (avatars.reader_stops, which refuses a build). The document holds what the reader
		// read before that line, read only (a save would lose the rest), so the file opens to be seen and fixed.
		size_t before = 0;
		const size_t stop = reader_stop(bytes, before);
		read = avatars::AvatarsFile{};
		if (!stop || avatars::avatars_parse_memory(bytes.data(), before, &read) != 0) {
			error = make_finding(CoreFinding::DocumentParse, DiagnosticSeverity::Error,
			                     "The avatar table's reader stops: 512 parts or 129 combinations (the game's walk ends "
			                     "there, ComboObj Parse Error) [orig: CAvatarDefs_ParseConfigLine @ 0x57A456; CAvatarDefs_AllocCombo "
			                     "@ 0x579E10].",
			                     path());
			return false;
		}
		reader_stops_line_ = stop;
		issues.push_back({true, stop, std::string(), std::string(),
		                  "The game's avatar reader stops at this line, a 513th part or a 129th combination, and the game "
		                  "fails (ComboObj Parse Error) [orig: CAvatarDefs_ParseConfigLine @ 0x57A456; CAvatarDefs_AllocCombo @ "
		                  "0x579E10; D-PLAYERINFO-2]. The editor holds what the reader reads before it, read only: fix the "
		                  "file outside the editor."});
		issues.back().game_stops = true;
	}
	for (size_t i = 0; i < read.diagnostics_count; ++i) {
		const avatars::AvatarDiagnostic &note = read.diagnostics[i];
		issues.push_back({false, note.line, std::string(), std::string(), std::string("The game's avatar reader: ") + note.message + "."});
	}
	// The file's order: the parts in order, each nationality after the parts above it.
	size_t parts = 0;
	const auto parts_to = [&](size_t end) {
		for (; parts < end; ++parts) {
			const avatars::AvatarPart &p = read.parts[parts];
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
			shape(*row);
			rows.push_back(std::move(row));
		}
	};
	for (size_t i = 0; i < read.nationalities_count; ++i) {
		const avatars::AvatarNationality &n = read.nationalities[i];
		parts_to(read.parts_count - std::min(n.parts_below, read.parts_count));
		auto row = std::make_shared<AvatarNationalityRow>();
		AvatarNationalityRecord &m = row->nationality;
		m.id = n.id;
		m.name_key = n.name_key;
		m.alignment = n.alignment;
		m.has_alignment = n.has_alignment != 0;
		for (size_t j = 0; j < n.divisions_count; ++j) {
			const avatars::AvatarDivision &d = n.divisions[j];
			AvatarDivisionRecord e;
			e.id = d.id;
			e.name_key = d.name_key;
			for (size_t k = 0; k < d.combos_count; ++k) {
				const avatars::AvatarCombo &c = d.combos[k];
				AvatarComboRecord f;
				f.id = c.id;
				f.head = c.head_name;
				f.body = c.body_name;
				f.arms = c.has_arms ? c.arms_name : "";
				e.combos.push_back(std::move(f));
			}
			m.divisions.push_back(std::move(e));
		}
		shape(*row);
		rows.push_back(std::move(row));
	}
	parts_to(read.parts_count);
	avatars::avatars_free(&read);
	return true;
}

SerializeResult AvatarsDocument::serialize() const {
	SerializeResult result;
	avatars::AvatarsFile table = file();
	char *data = nullptr;
	size_t size = 0;
	const int rc = avatars::avatars_write(&table, &data, &size);
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
	return result;
}

std::string AvatarsDocument::save_words() const {
	return "Saving writes the table in the editor's form, from what the game reads of it: the parts and the "
	       "nationalities (each with its divisions and combinations) in the table's order, which is what each "
	       "combination binds. Comments and the lines the game reads nothing of are not kept.";
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
		int free_id = -1;
		for (int id = 0; id < 32 && free_id < 0; ++id) {
			const bool used = std::any_of(rows.begin(), rows.end(), [&](const std::shared_ptr<const Node> &other) {
				return other && other->kind == kNationality &&
				       static_cast<const AvatarNationalityRow &>(*other).nationality.id == id;
			});
			if (!used) free_id = id;
		}
		row->nationality.id = free_id;
		if (free_id < 0) {
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

std::string AvatarsDocument::first_part(int kind, NodeId nationality) const {
	for (const auto &node : rows()) {
		if (node && node->id == nationality) break;
		const AvatarPartRecord *p = part_row(node);
		if (p && p->kind == kind) return p->name;
	}
	return std::string();
}

bool AvatarsDocument::accept_list_edit(const Node &row, const ListChange &change, std::string &error) const {
	if (change.operation != EditOperation::Add) return TableDocument::accept_list_edit(row, change, error);
	if (change.owner && change.owner->record.kind == kDivision &&
	    (first_part(avatars::AVATAR_PART_HEAD, row.id).empty() || first_part(avatars::AVATAR_PART_BODY, row.id).empty())) {
		// The reader drops a combination whose head or body it does not find among the parts above it [orig:
		// CAvatarDefs_ParseConfigLine @ 0x57A804..0x57A8E6].
		error = "A combination names a head and a body the file defines above its nationality, and none of one stands "
		        "there.";
		return false;
	}
	return TableDocument::accept_list_edit(row, change, error);
}

bool AvatarsDocument::accept_step(const EditStep &step, const StagedRows &rows, StepRefusal &refusal) const {
	// The parts above the row the walk stands at, by kind, each name upper case (the reader compares without case).
	std::set<std::string> above[3];
	static const char *const kWhat[] = {"head", "body", "arms"};
	for (const auto &node : rows.rows()) {
		if (const AvatarPartRecord *p = part_row(node)) {
			if (p->kind >= 0 && p->kind <= 2) above[p->kind].insert(strutil::to_upper(p->name));
			continue;
		}
		if (!node || node->kind != kNationality) continue;
		for (const AvatarDivisionRecord &d : static_cast<const AvatarNationalityRow &>(*node).nationality.divisions)
			for (const AvatarComboRecord &c : d.combos) {
				const std::string *named[] = {&c.head, &c.body, &c.arms};
				for (int kind = 0; kind < 3; ++kind) {
					if (kind == avatars::AVATAR_PART_ARMS && c.arms.empty()) continue;
					if (above[kind].count(strutil::to_upper(*named[kind]))) continue;
					// The reader drops a combination whose head or body it does not find among the parts above it, and
					// keeps one without arms it does not find [orig: CAvatarDefs_ParseConfigLine @ 0x57A804..0x57A8E6].
					refusal.message = std::string("Combination ") + std::to_string(c.id) + " would name the " + kWhat[kind] +
					                  " '" + *named[kind] + "', which no " + kWhat[kind] + " part above its nationality is: the game " +
					                  (kind == avatars::AVATAR_PART_ARMS ? "reads it without arms." : "drops the combination.");
					return false;
				}
			}
	}
	return TableDocument::accept_step(step, rows, refusal);
}

void AvatarsDocument::after_add(Node &row, const ListChange &change, const RecordHandle &made) {
	if (made.kind == kDivision && row.kind == kNationality) {
		// The first id 0..15 the nationality has no division of (the reader refuses a repeated one).
		const AvatarNationalityRecord &n = static_cast<const AvatarNationalityRow &>(row).nationality;
		AvatarDivisionRecord &d = made.as<AvatarDivisionRecord>();
		for (int id = 0; id < 16; ++id) {
			const bool used = std::any_of(n.divisions.begin(), n.divisions.end(), [&](const AvatarDivisionRecord &other) {
				return &other != &d && other.id == id;
			});
			if (used) continue;
			d.id = id;
			break;
		}
		d.name_key = "AV_DIV_NEW";
	}
	if (made.kind == kCombo && change.owner) {
		// The next number of its division, the first head, body and arms above its nationality.
		const AvatarDivisionRecord &d = change.owner->record.as<AvatarDivisionRecord>();
		AvatarComboRecord &c = made.as<AvatarComboRecord>();
		int next = 1;
		for (const AvatarComboRecord &other : d.combos)
			if (&other != &c) next = std::max(next, other.id + 1);
		c.id = next;
		c.head = first_part(avatars::AVATAR_PART_HEAD, row.id);
		c.body = first_part(avatars::AVATAR_PART_BODY, row.id);
		c.arms = first_part(avatars::AVATAR_PART_ARMS, row.id);
	}
}

const AvatarsDocument::Order &AvatarsDocument::order() const {
	if (order_.made && order_.load_generation == load_generation() && order_.revision == revision()) return order_;
	Order made;
	made.made = true;
	made.load_generation = load_generation();
	made.revision = revision();
	// Each part's rank among its kind's of the name in the rows' order, the last none; and whether a nationality
	// stands between it and the next part of its name (or below it, for the last), where a combination binds it.
	struct Held {
		NodeId id;
		size_t place;
		int kind;
		std::string name;
	};
	std::vector<Held> parts;
	std::vector<size_t> nationalities;
	for (size_t i = 0; i < rows().size(); ++i) {
		const auto &node = rows()[i];
		if (!node) continue;
		made.place[node->id] = i;
		if (const AvatarPartRecord *p = part_row(node)) parts.push_back({node->id, i, p->kind, strutil::to_upper(p->name)});
		if (node->kind == kNationality) nationalities.push_back(i);
	}
	for (size_t i = 0; i < parts.size(); ++i) {
		size_t rank = 1, next = rows().size();
		for (size_t j = 0; j < parts.size(); ++j) {
			if (j == i || parts[j].kind != parts[i].kind || parts[j].name != parts[i].name) continue;
			if (j < i) ++rank;
			else next = std::min(next, parts[j].place);
		}
		if (next < rows().size()) made.rank[parts[i].id] = rank;
		const bool bound = std::any_of(nationalities.begin(), nationalities.end(),
		                               [&](size_t place) { return place > parts[i].place && place < next; });
		if (!bound) made.unbound.insert(parts[i].id);
	}
	order_ = std::move(made);
	return order_;
}

void AvatarsDocument::refine_field(const NodeAddress &address, FieldUse &use) const {
	TableDocument::refine_field(address, use);
	const Node *node = row(address.row);
	if (!node) return;
	// The section of a part a later one of its name follows (its rank), the plain kind's otherwise.
	const auto section = [this](int kind, NodeId part) {
		std::string scope = avatar_part_scope(path(), kind);
		const auto ranked = order().rank.find(part);
		if (part && ranked != order().rank.end()) scope += "#" + std::to_string(ranked->second);
		return scope;
	};
	if (use.defines == ReferenceKind::AvatarPart && node->kind == kPart)
		use.scope = section(static_cast<const AvatarPartRow &>(*node).part.kind, node->id);
	if (use.reference == ReferenceKind::AvatarPart && address.kind == kCombo) {
		const std::string &field = use.schema->id;
		const int kind = field == "head" ? avatars::AVATAR_PART_HEAD
		                 : field == "body" ? avatars::AVATAR_PART_BODY : avatars::AVATAR_PART_ARMS;
		// The section of the part it binds: the last of the name above its nationality.
		const RecordHandle combo = record_in(*node, address);
		const auto place = order().place.find(node->id);
		const Node *bound = combo && place != order().place.end()
		                            ? bound_part(rows(), place->second, kind,
		                                         kind == avatars::AVATAR_PART_HEAD   ? combo_of(combo).head
		                                         : kind == avatars::AVATAR_PART_BODY ? combo_of(combo).body
		                                                                             : combo_of(combo).arms)
		                            : nullptr;
		use.scope = section(kind, bound ? bound->id : 0);
	}
}

void AvatarsDocument::refine_symbol(const NodeAddress &address, SymbolFacts &facts) const {
	const Node *node = row(address.row);
	if (!node || address.child || node->kind != kPart || !order().unbound.count(node->id)) return;
	facts.inert = true;
	facts.inert_reason = order().rank.count(node->id)
	                             ? "a later part of the name comes before any nationality below it does, so no "
	                               "combination binds it (a combination binds the last part of the name above it)"
	                             : "no nationality stands below it, so no combination binds it (a combination binds the "
	                               "last part of the name above it)";
}

void AvatarsDocument::saved_text_findings(std::vector<Diagnostic> &out) const {
	if (blocked()) return;
	const SerializeResult saved = serialize();
	if (!saved.ok()) return;
	avatars::AvatarsFile reread{};
	if (avatars::avatars_parse_memory(saved.text.data(), saved.text.size(), &reread) != 0) {
		out.push_back(make_finding(finding_code(AvatarsFinding::ReaderStops), DiagnosticSeverity::Error,
		                           "The table as a save writes it holds a 513th part or a 129th combination: the game's "
		                           "reader stops there and the game fails (ComboObj Parse Error) [orig: "
		                           "CAvatarDefs_ParseConfigLine @ 0x57A456; CAvatarDefs_AllocCombo @ 0x579E10; D-PLAYERINFO-2].",
		                           path()));
		return;
	}
	avatars::avatars_free(&reread);
}

const FindingCodeRow &finding_code(AvatarsFinding code) { return kFindingRows[static_cast<size_t>(code)]; }

FindingTable avatars_finding_codes() { return { kFindingRows.data(), kFindingRows.size() }; }

std::vector<Diagnostic> validate_avatars_file(const DocumentBase &document) {
	std::vector<Diagnostic> findings;
	const auto *table = dynamic_cast<const AvatarsDocument *>(&document);
	if (!table) return findings;
	// The file as read stopping the reader is its game_stops issue, at its line (the document holding what reads
	// before it, read only).
	source_issue_findings(*table, finding_code(AvatarsFinding::InvalidInput), finding_code(AvatarsFinding::IgnoredInput),
	                      findings, nullptr, nullptr, &finding_code(AvatarsFinding::ReaderStops));
	table->saved_text_findings(findings);
	return findings;
}

} // namespace opennova::editor
