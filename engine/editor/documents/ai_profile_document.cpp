// The AI profile document (ai_profile_document.h, ADR 0046 S23 B): the profile's table over aip::Profile, its
// fields the reader's keys (aip::key_rows) in the units the file writes them, its parse through the game's reader,
// its save through the profile's writer from the profile alone (S26).
#include "ai_profile_document.h"

#include <cmath>
#include <cstdio>
#include <iterator>

#include <base/io/crt_ftol.h>
#include <base/io/strutil.h>
#include <editor/assets/asset_kinds.h>
#include <editor/documents/source_issue_findings.h>

namespace opennova::editor {
namespace {

constexpr NodeKind kProfile = node_kind(AiProfileKind::Profile);

// What the editor shows of a key: its label, the section it stands in, the unit its number is in, and what the
// game does with it (each the reader's arm, AIProfile_ParseProperty @ 0x45DE70; world-wac-ai-re §17.9c, §17.9d,
// §23.3).
struct KeyWords {
	const char *key;
	const char *label;
	const char *section;
	const char *unit;
	const char *description;
};
constexpr KeyWords kWords[] = {
	{"type", "Type", "Profile", "",
	 "Which key set the reader takes: HELO (1) the helicopter's and the plane's, GROUND (2) the vehicle's, ORGANIC "
	 "(3) none. Keys before the type line are read for nothing [orig: AIProfile_ParseProperty @ 0x45DE8C..0x45DF07]."},
	{"subtype", "Subtype", "Profile", "",
	 "STD, or a GROUND profile's BOAT or TRAIN, a HELO profile's PLANE; another word keeps the value [orig: @ "
	 "0x45E887..0x45E8F5, HELO @ 0x45FA63..0x45FAA0]."},
	{"default_state", "Starting state", "Profile", "",
	 "The AI state a unit of the profile starts in, by its name in the state table [orig: AIState_LookupByName @ "
	 "0x457530, the table @ 0x815198]; a name the table lacks reads 0."},
	{"rank", "Rank", "Profile", "", "A whole number, stored at +60 [orig: atol]."},
	{"view_fov", "View cone", "Senses", "degrees",
	 "The cone the unit sees targets in, stored as a binary angle [orig: atof x 11930464.0 @ 0x45DF85..0x45DFB6]."},
	{"view_dist", "View distance", "Senses", "m", "How far the unit sees, stored x 65536 [orig: atol << 16]."},
	{"radar_fov", "Radar cone", "Senses", "degrees", "The radar's cone, a binary angle [orig: atof x 11930464.0]."},
	{"radar_dist", "Radar distance", "Senses", "m", "The radar's reach, stored x 65536 [orig: atol << 16]."},
	{"priority_air", "Air targets", "Targets", "",
	 "The weight the target walk gives an aircraft; 0 takes none [orig: atol; world-wac-ai-re §16.2]."},
	{"priority_ground", "Ground targets", "Targets", "", "The weight the target walk gives a vehicle; 0 takes none."},
	{"priority_organics", "People", "Targets", "", "The weight the target walk gives a person; 0 takes none."},
	{"priority_decorations", "Decorations", "Targets", "", "The weight the target walk gives a decoration; 0 takes none."},
	{"evade_flags", "Evade modes", "Modes", "", "Each word's bit ORed in [orig: the EVADE_FLAGS loop into +96]."},
	{"combat_flags", "Combat modes", "Modes", "", "Each word's bit ORed in [orig: the COMBAT_FLAGS loop into +100]."},
	{"react_time", "Reaction time", "Targets", "seconds",
	 "Stored as logic ticks, chopped [orig: atof x 62.5 through _ftol2_sse @ 0x45EE24]."},
	{"aim_skill", "Aim skill", "Targets", "", "0 to 4, clamped [orig: atol @ 0x45EE43..0x45EE81]."},
	{"check_six_rate", "Check-six rate", "Targets", "", "Stored x 655.36, chopped [orig: atof x 655.36]."},
	{"target_eval_rate", "Target rate", "Targets", "", "Stored x 655.36, chopped [orig: atof x 655.36]."},
	{"tether_dist", "Tether", "Targets", "m", "Stored x 65536 [orig: atol << 16]."},
	{"primary_weap", "Weapon", "Primary weapon", "",
	 "The ammo the weapon fires, by its name in the ammo table [orig: AmmoDef_LookupByName @ 0x45EF6B]."},
	{"primary_ammo", "Rounds", "Primary weapon", "", "The shots it holds; -1 for no end [orig: atol]."},
	{"primary_rate", "Fire interval", "Primary weapon", "seconds",
	 "Stored as logic ticks, chopped [orig: atof x 62.5]."},
	{"primary_fov", "Aim cone", "Primary weapon", "degrees", "A binary angle [orig: atof x 11930464.0]."},
	{"primary_range", "Range", "Primary weapon", "m", "Stored x 65536 [orig: atol << 16]."},
	{"primary_facing", "Facing", "Primary weapon", "degrees", "A binary angle [orig: atof x 11930464.0]."},
	{"primary_pitch", "Pitch", "Primary weapon", "degrees", "A binary angle [orig: atof x 11930464.0]."},
	{"primary_flags", "Flags", "Primary weapon", "", "Each word's bit ORed in [orig: the primary_flags loop into +136]."},
	{"secondary_weap", "Weapon", "Secondary weapon", "",
	 "The ammo the weapon fires, by its name in the ammo table [orig: AmmoDef_LookupByName @ 0x45F269]."},
	{"secondary_ammo", "Rounds", "Secondary weapon", "", "The shots it holds; -1 for no end [orig: atol]."},
	{"secondary_rate", "Fire interval", "Secondary weapon", "seconds", "Stored as logic ticks, chopped [orig: atof x 62.5]."},
	{"secondary_fov", "Aim cone", "Secondary weapon", "degrees", "A binary angle [orig: atof x 11930464.0]."},
	{"secondary_range", "Range", "Secondary weapon", "m", "Stored x 65536 [orig: atol << 16]."},
	{"secondary_facing", "Facing", "Secondary weapon", "degrees", "A binary angle [orig: atof x 11930464.0]."},
	{"secondary_pitch", "Pitch", "Secondary weapon", "degrees", "A binary angle [orig: atof x 11930464.0]."},
	{"secondary_flags", "Flags", "Secondary weapon", "", "Each word's bit ORed in [orig: the secondary_flags loop into +168]."},
	{"hunt_flags", "Hunt flags", "Flight", "", "MAINTAIN_SPEED; the line clears the mask first [orig: @ 0x45F5F6]."},
	{"hunt_limit", "Hunt limit", "Flight", "seconds", "Stored as logic ticks [orig: atof x 62.5]."},
	{"patrol_speed", "Patrol speed", "Movement", "km/h",
	 "Stored as 16.16 units a tick [orig: atof x 1000 x 4.444444444444444e-06 x 65536, chopped; GROUND +0xC0, HELO "
	 "+200 @ 0x45F6CF..0x45F70D]."},
	{"patrol_altitude", "Patrol altitude", "Flight", "m", "Stored x 65536 [orig: atol << 16 @ 0x45F733..0x45F747]."},
	{"patrol_climb", "Patrol climb", "Flight", "", "Stored x 0.016 x 65536 [orig: @ 0x45F687..0x45F6BF]."},
	{"combat_speed", "Combat speed", "Movement", "km/h",
	 "Stored as 16.16 units a tick [orig: GROUND +0xC4, HELO +212 @ 0x45F79F..0x45F7DD]."},
	{"combat_altitude", "Combat altitude", "Flight", "m", "Stored x 65536 [orig: atol << 16 @ 0x45F803..0x45F817]."},
	{"combat_climb", "Combat climb", "Flight", "", "Stored x 0.016 x 65536 [orig: @ 0x45F757..0x45F78F]."},
	{"min_agl", "Lowest height", "Flight", "m", "Stored x 65536, chopped [orig: @ 0x45F975..0x45F991]."},
	{"min_speed", "Lowest speed", "Flight", "km/h", "Stored as 16.16 units a tick [orig: @ 0x45F9B7..0x45F9DF]."},
	{"min_chase_dist", "Chase from", "Targets", "m", "Stored x 65536, chopped [orig: atof x 65536]."},
	{"max_chase_dist", "Chase to", "Targets", "m", "Stored x 65536, chopped [orig: atof x 65536]."},
	{"drive_skill", "Driving skill", "Movement", "",
	 "0 to 4, clamped; a HELO profile writes it flight_skill, either word the same arm [orig: +32]."},
	{"turn_rate", "Turn rate", "Movement", "degrees a second",
	 "Stored as a binary angle a tick: 11930464 x the number / 62 in 32 bits [orig: @ 0x45E788..0x45E7C4]."},
	{"accel_time", "Acceleration time", "Movement", "seconds", "Stored as 62 x the number [orig: @ 0x45E7DA..0x45E803]."},
	{"radio_distance", "Radio distance", "Radio", "", "A whole number [orig: atol @ 0x45E819]."},
	{"radio_delay", "Radio delay", "Radio", "", "A whole number [orig: atol @ 0x45E846]."},
	{"alert", "Alert", "Profile", "", "GREEN 0, YELLOW 1, RED 2; any other word 0 [orig: @ 0x45E91F..0x45E961]."},
	{"use_waypoint_z", "Use waypoint height", "Flight", "", "A whole number [orig: atol @ 0x45F941..0x45F952]."},
};

const KeyWords *words_of(const char *key) {
	for (const KeyWords &words : kWords)
		if (std::string(words.key) == key) return &words;
	return nullptr;
}

const aip::Profile &profile_of(const RecordHandle &r) { return r.as<aip::Profile>(); }

bool number_of(const Value &value, double &out) {
	if (const auto *whole = std::get_if<int64_t>(&value)) return out = double(*whole), true;
	if (const auto *real = std::get_if<double>(&value)) return out = *real, std::isfinite(*real);
	return false;
}

bool real_unit(aip::Unit unit) {
	switch (unit) {
	case aip::Unit::Degrees:
	case aip::Unit::Seconds:
	case aip::Unit::Rate:
	case aip::Unit::Fixed:
	case aip::Unit::Speed:
	case aip::Unit::Climb: return true;
	default: return false;
	}
}

bool choice_unit(aip::Unit unit) {
	return unit == aip::Unit::Type || unit == aip::Unit::Subtype || unit == aip::Unit::State || unit == aip::Unit::Alert;
}

// A key's value as the file writes it: the writer's word for the stored value (the decimal its arm reads back to
// it), a choice's or a mask's number as stored.
Value value_of(const aip::KeyRow &row, const aip::Profile &p) {
	if (row.unit == aip::Unit::Weapon) return (p.*(row.block)).*(row.name);
	const int32_t stored = aip::key_value(row, p);
	if (choice_unit(row.unit) || row.unit == aip::Unit::Flags || row.unit == aip::Unit::HuntFlags) return int64_t(uint32_t(stored));
	if (stored == 0) return real_unit(row.unit) ? Value(0.0) : Value(int64_t(0));
	const std::vector<std::string> words = aip::value_words(row, p.type, stored);
	const std::string word = words.empty() ? std::string() : words[0];
	if (real_unit(row.unit)) return io::retail_atof(word.c_str());
	return int64_t(io::retail_atol(word.c_str()));
}

// A value set: the number written as the file writes it and read through the key's arm, as the game reads it (so
// the field then shows what the game holds: 0.25 seconds read as 15 ticks shows 0.24).
bool set_key(const aip::KeyRow &row, aip::Profile &p, const Value &value, std::string &error) {
	// A key the profile's type does not read stays as the reader left it, which no line of the writer's puts down
	// (aip::write_profile writes a key of the type alone): only HELO reads the helicopter keys, GROUND its own, and
	// ORGANIC none but the type [orig: AIProfile_ParseProperty @ 0x45DF15..0x45DF2F, the type dispatch]. Setting
	// it back to the zeroed record's value is the one edit it takes.
	const bool unread = row.unit != aip::Unit::Type && !aip::reads(row, p.type);
	const auto refuse_unread = [&]() {
		error = std::string("The game reads ") + row.key + " only in a profile of another type, so this one keeps none.";
		return false;
	};
	if (row.unit == aip::Unit::Weapon) {
		const auto *text = std::get_if<std::string>(&value);
		if (!text) return error = "A weapon is an ammo's name.", false;
		if (unread && !text->empty()) return refuse_unread();
		if (text->find_first_of(" \t,\";\r\n") != std::string::npos || text->find("//") != std::string::npos)
			return error = "A weapon's name is one word of the walk: no blank, comma, quote, ';' or '//'.", false;
		(p.*(row.block)).*(row.name) = *text;
		return true;
	}
	double number = 0.0;
	if (!number_of(value, number)) return error = "The value is a number.", false;
	if (unread && number != 0.0) return refuse_unread();
	int32_t stored = 0;
	if (choice_unit(row.unit) || row.unit == aip::Unit::Flags || row.unit == aip::Unit::HuntFlags) {
		if (number != std::floor(number) || number < 0 || number > 0xFFFFFFFF) return error = "The value is a choice.", false;
		stored = int32_t(uint32_t(number));
		// What the writer cannot put down is no value of the file's (a subtype of the other type).
		if (stored != 0 && aip::value_words(row, row.unit == aip::Unit::Type ? stored : p.type, stored).empty())
			return error = std::string("The game reads no word as that ") + row.key + " for this profile's type.", false;
	} else {
		char word[64];
		if (real_unit(row.unit)) std::snprintf(word, sizeof(word), "%.9g", number);
		else {
			if (number != std::floor(number) || std::fabs(number) > 2147483647.0) return error = "The value is a whole number.", false;
			std::snprintf(word, sizeof(word), "%lld", (long long)number);
		}
		if (row.unit == aip::Unit::Metres && std::fabs(number) > 32767) return error = "A distance is from -32767 to 32767 metres.", false;
		stored = aip::read_value(row, p.type, {word}, row.get ? row.get(p) : 0);
		if (stored != 0 && aip::value_words(row, p.type, stored).empty())
			return error = "No number the file can hold reads back as that.", false;
	}
	// A new type's reader reads none of another type's keys, and keeps none of a subtype word of the other type
	// [orig: AIProfile_ParseProperty @ 0x45DF15..0x45DF2F; the subtype words @ 0x45E887 (GROUND), @ 0x45FA63 (HELO)]:
	// the profile becomes what the file then holds as the game reads it, which the writer puts down. The loader's
	// zeroed record [orig: AIProfile_LoadOrFind @ 0x45FE09] of the new type reads each line the old profile's
	// writer puts down (its words, value_words) through the new type's arm, a key the new type does not read none
	// (a speed both read moving to the new type's member as its words read there; given back, the old type reads
	// the same words again).
	if (row.unit == aip::Unit::Type && stored != p.type) {
		const aip::Profile was = p;
		aip::Profile made;
		row.set(made, stored);
		for (const aip::KeyRow &other : aip::key_rows()) {
			if (other.unit == aip::Unit::Type || !aip::reads(other, stored)) continue;
			if (other.unit == aip::Unit::Weapon) {
				(made.*(other.block)).*(other.name) = (was.*(other.block)).*(other.name);
				continue;
			}
			const int32_t held = other.get ? other.get(was) : 0;
			if (held == 0 || !other.set) continue;
			const std::vector<std::string> words = aip::value_words(other, was.type, held);
			if (!words.empty()) other.set(made, aip::read_value(other, stored, words, 0));
		}
		p = made;
		return true;
	}
	row.set(p, stored);
	return true;
}

RecordTable make_table() {
	TableKind profile(RecordKindRow{kProfile, "profile", "AI profile", "", true});
	for (const aip::KeyRow &row : aip::key_rows()) {
		const KeyWords *words = words_of(row.key);
		FieldSchema schema;
		schema.id = row.key;
		schema.label = words ? words->label : row.key;
		schema.section = words ? words->section : "";
		schema.unit = words ? words->unit : "";
		schema.description = words ? words->description : "";
		schema.type = row.unit == aip::Unit::Weapon ? FieldType::Text : real_unit(row.unit) ? FieldType::Real : FieldType::Integer;
		if (row.unit == aip::Unit::Weapon) schema.reference = ReferenceKind::Ammo;
		if (row.unit == aip::Unit::Skill) {
			schema.ranged = true;
			schema.min = 0;
			schema.max = 4;
		}
		if (row.unit == aip::Unit::Type)
			schema.choices = {{"", 0, "none: the game reads no key"}, {"HELO", aip::kTypeHelo, "HELO"},
			                  {"GROUND", aip::kTypeGround, "GROUND"}, {"ORGANIC", aip::kTypeOrganic, "ORGANIC"}};
		if (row.unit == aip::Unit::Subtype)
			schema.choices = {{"STD", 0, "STD"}, {"BOAT", 1, "BOAT (GROUND)"}, {"PLANE", 2, "PLANE (HELO)"}, {"TRAIN", 3, "TRAIN (GROUND)"}};
		if (row.unit == aip::Unit::Alert) schema.choices = {{"GREEN", 0, ""}, {"YELLOW", 1, ""}, {"RED", 2, ""}};
		if (row.unit == aip::Unit::State)
			for (const aip::StateName &state : aip::state_names()) schema.choices.push_back({state.word, state.id, ""});
		if (row.unit == aip::Unit::Flags || row.unit == aip::Unit::HuntFlags) {
			schema.flags = true;
			for (const aip::FlagWord &flag : row.flags()) schema.choices.push_back({flag.word, int64_t(flag.bit), ""});
		}
		const aip::KeyRow *key = &row;
		LabelledField field{schema,
		                    {[key](const RecordHandle &r, Value &out) { return out = value_of(*key, profile_of(r)), true; },
		                     [key](const RecordHandle &r, const Value &v, std::string &e) {
			                     return set_key(*key, r.as<aip::Profile>(), v, e);
		                     }}};
		// A key the profile's type does not read is read for nothing [orig: AIProfile_ParseProperty's type gate].
		if (row.unit != aip::Unit::Type)
			field.applies = [key](const RecordHandle &r, const RecordOwners &) {
				return aip::reads(*key, profile_of(r).type) ? Applicability::Reads : Applicability::Ignored;
			};
		profile.field(std::move(field));
	}
	return RecordTable({std::move(profile)});
}

const AiProfileRow &profile_row(const Node &node) { return static_cast<const AiProfileRow &>(node); }

constexpr FindingCodeEntry<AiProfileFinding> kFindingEntries[] = {
	{ AiProfileFinding::InvalidInput, { "ai_profile.invalid_input", FindingFix::None, nullptr, true } },
	// The words the reader reads nothing of: a save writes the profile alone (formats/aip), without them.
	{ AiProfileFinding::IgnoredInput, listed_code("ai_profile.ignored_input", FindingFix::Rewrite, kRewriteDropsIgnoredInput) },
	{ AiProfileFinding::NoType, listed_code("ai_profile.no_type") },
};
static_assert(std::size(kFindingEntries) == static_cast<size_t>(AiProfileFinding::kCount),
		"every AiProfileFinding has exactly one row");
static_assert(finding_entries_well_formed(kFindingEntries),
		"the AI profile's rows follow AiProfileFinding's order, each token its own");
constexpr auto kFindingRows = finding_rows(kFindingEntries, FindingGroup::AiProfiles);
static_assert(finding_rows_well_formed(kFindingRows), "every row of the table takes its group");

} // namespace

const RecordTable &ai_profile_table() {
	static const RecordTable table = make_table();
	return table;
}

size_t AiProfileRow::footprint() const {
	return sizeof(AiProfileRow) + footprint_of(profile.primary.weapon) + footprint_of(profile.secondary.weapon) +
	       ids_footprint();
}

const aip::Profile *AiProfileDocument::profile() const {
	for (const auto &node : rows())
		if (node && node->kind == kProfile) return &profile_row(*node).profile;
	return nullptr;
}

std::string AiProfileDocument::record_title(const NodeAddress &address) const {
	const Node *node = row(address.row);
	if (!node) return std::string();
	const aip::Profile &p = profile_row(*node).profile;
	const char *type = p.type == aip::kTypeHelo ? "HELO" : p.type == aip::kTypeGround ? "GROUND"
	                   : p.type == aip::kTypeOrganic ? "ORGANIC" : "no type";
	return std::string("Profile (") + type + ")";
}

bool AiProfileDocument::parse(const std::vector<uint8_t> &bytes, std::vector<std::shared_ptr<Node>> &rows,
                              std::shared_ptr<const FileState> &, std::vector<SourceIssue> &issues,
                              Diagnostic &error) {
	std::vector<aip::UnreadLine> unread;
	auto row = std::make_shared<AiProfileRow>();
	// The loader hands a single NUL for an empty file (Document's load): an empty profile, of no type and no line, as
	// a profile of no type saves.
	const size_t size = bytes.size() == 1 && bytes[0] == 0 ? 0 : bytes.size();
	row->profile = aip::parse_profile(bytes.data(), size, &unread);
	const std::string text(bytes.begin(), bytes.end());
	// The 1-based line of a byte offset (CR LF and LF each end a line).
	const auto line_of_offset = [&text](size_t offset) {
		size_t line = 1;
		for (size_t i = 0; i < offset && i < text.size(); ++i)
			if (text[i] == '\n') ++line;
		return line;
	};
	for (const aip::UnreadLine &line : unread) {
		std::string why;
		switch (line.why) {
		case aip::UnreadLine::Why::NoType: why = "it stands before the profile's type line, and a profile of no type takes no key"; break;
		case aip::UnreadLine::Why::Organic: why = "an ORGANIC profile takes no key"; break;
		case aip::UnreadLine::Why::OtherType: why = "only the other type's key set holds it"; break;
		case aip::UnreadLine::Why::Unknown: why = "no key of the reader's is it"; break;
		}
		issues.push_back({false, line_of_offset(line.offset), "Profile", line.key,
		                  "The game reads nothing of '" + line.key + "': " + why + " [orig: AIProfile_ParseProperty @ 0x45DE70]."});
	}
	shape(*row);
	rows.push_back(std::move(row));
	return true;
}

SerializeResult AiProfileDocument::serialize() const {
	SerializeResult result;
	const aip::Profile *p = profile();
	if (!p) return result;
	std::string text, error;
	if (!aip::write_profile(*p, text, error)) {
		result.issues.push_back({true, 0, "Profile", std::string(), "The profile could not be written: " + error});
		return result;
	}
	result.text = std::move(text);
	return result;
}

std::string AiProfileDocument::save_words() const {
	return "Saving writes the profile in the editor's form, from what the game reads of it: its type, then each key "
	       "its type reads. Comments and the lines the game reads nothing of are not kept.";
}

std::shared_ptr<Node> AiProfileDocument::make_node(NodeKind, NodeId, const std::vector<std::shared_ptr<const Node>> &,
                                                   std::string &error) {
	error = "An AI profile's file holds one profile.";
	return nullptr;
}

const FindingCodeRow &finding_code(AiProfileFinding code) { return kFindingRows[static_cast<size_t>(code)]; }

FindingTable ai_profile_finding_codes() { return { kFindingRows.data(), kFindingRows.size() }; }

std::vector<Diagnostic> validate_ai_profile_file(const DocumentBase &document) {
	std::vector<Diagnostic> findings;
	const auto *profile = dynamic_cast<const AiProfileDocument *>(&document);
	if (!profile) return findings;
	source_issue_findings(*profile, finding_code(AiProfileFinding::InvalidInput),
	                      finding_code(AiProfileFinding::IgnoredInput), findings);
	const aip::Profile *p = profile->profile();
	if (p && p->type == 0) {
		Diagnostic d = make_finding(AiProfileFinding::NoType, DiagnosticSeverity::Info,
				"The profile has no type line: the game reads none of its keys, and a unit given it runs the cleared "
				"profile (no targets taken, no speeds) [orig: AIProfile_ParseProperty @ 0x45DE70's type gate; "
				"AIProfile_LoadOrFind @ 0x45FE09].",
				document.path(), "type");
		d.record_kind = kProfile;
		d.record = "Profile";
		for (const auto &node : document.as_records()->rows())
			if (node) d.row_id = node->id;
		findings.push_back(std::move(d));
	}
	return findings;
}

} // namespace opennova::editor
