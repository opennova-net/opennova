// The sound profile document (sound_profile_document.h): SndProf.def's profiles over the game's own
// reading of the file (audio::SoundProfileTable) and written back through its from-scratch writer
// (audio::write_sound_profiles), its slots in the engine's order, worded by what the game plays them
// for (docs/audio/lwf-dbf-sound-re.md and the consumers it cites).
#include "sound_profile_document.h"

#include <climits>
#include <cmath>
#include <cstring>
#include <unordered_map>

#include <base/io/strutil.h>
#include <editor/assets/asset_kinds.h>
#include <editor/project/project_files.h>

namespace opennova::editor {
namespace {

constexpr NodeKind kProfile = node_kind(SoundProfileKind::Profile);
constexpr NodeKind kSlot = node_kind(SoundProfileKind::Slot);

// The bytes the game keeps a name in, its terminator among them: a profile's 64 [orig: the strlen
// >= 0x40 cut @ 0x527043], a slot's set 24 [orig: the 24-byte name24 rows at +928].
constexpr size_t kProfileNameBytes = 64;
constexpr size_t kSetNameBytes = 24;

struct SlotWords {
	const char *words;
	const char *family;
};
// Each slot by what the game plays it for, in the engine's slot order [orig: the keyword table @
// 0x82F3B0]; the infantry consumers witnessed in world-wac-ai-re §17.4b (Entity_GetProfileSlotSound @
// 0x528300), the vehicle ones in vehicle-client-movers-re.
constexpr SlotWords kSlotWords[audio::kSoundProfileSlotCount] = {
	{"loop 1", "Loops"}, {"loop 2", "Loops"}, {"loop 3", "Loops"}, {"loop 4", "Loops"},
	{"loop 5", "Loops"}, {"loop 6", "Loops"}, {"loop 7", "Loops"},
	{"death scream", "Death"}, {"death scream at night", "Death"},
	{"door opening", "Doors"}, {"door closing", "Doors"},
	{"one-shot at dawn", "Time of day"}, {"one-shot by day", "Time of day"},
	{"one-shot at dusk", "Time of day"}, {"one-shot at night", "Time of day"},
	{"landing dead", "Landing"}, {"landing", "Landing"},
	{"left foot on the ground", "Footsteps"}, {"right foot on the ground", "Footsteps"},
	{"left foot on snow", "Footsteps"}, {"right foot on snow", "Footsteps"},
	{"left foot on an object", "Footsteps"}, {"right foot on an object", "Footsteps"},
	{"a foot in water", "Footsteps"},
	{"animation sound 1", "Animation sounds"}, {"animation sound 2", "Animation sounds"},
	{"animation sound 3", "Animation sounds"}, {"animation sound 4", "Animation sounds"},
	{"animation sound 5", "Animation sounds"}, {"animation sound 6", "Animation sounds"},
	{"engine start", "Engine"}, {"engine stop", "Engine"}, {"engine in reverse", "Engine"},
	{"engine revving high", "Engine"},
	{"warning", "Vehicle"}, {"impact", "Vehicle"}, {"landing (vehicle)", "Vehicle"}, {"rolling over", "Vehicle"},
	{"hitting someone", "Vehicle"}, {"hitting water", "Vehicle"}, {"rotor strike", "Vehicle"},
	{"parachute opening", "Parachute"}, {"parachute closing", "Parachute"}, {"parachute flapping", "Parachute"},
	{"free fall", "Parachute"},
	{"drive, repeating", "Vehicle motion"}, {"turret swivel", "Vehicle motion"},
	{"tumbling, hard hit", "Tumbling"}, {"tumbling, medium hit", "Tumbling"}, {"tumbling, soft hit", "Tumbling"},
	{"tumbling skid", "Tumbling"},
};

// The twelve percents in words, the keyword chain's order [orig: @ 0x5270a9-0x527481].
constexpr const char *kLoopWords[audio::kSoundProfileLoopParamCount] = {
	"Medium loop fade-in start", "Medium loop fade-in end", "Medium loop fade-out start",
	"Medium loop fade-out end", "Medium loop pitch start", "Medium loop pitch end",
	"Medium loop pitch start (p)", "Medium loop pitch end (p)", "Cruise loop fade-in start",
	"Cruise loop fade-in end", "Cruise loop pitch start (p)", "Cruise loop pitch end (p)",
};

const ProfileRecord &profile_of(const RecordHandle &r) { return r.as<ProfileRecord>(); }
const ProfileSlot &slot_of(const RecordHandle &r) { return r.as<ProfileSlot>(); }

bool number_of(const Value &value, double &out) {
	if (const auto *whole = std::get_if<int64_t>(&value)) return out = double(*whole), true;
	if (const auto *real = std::get_if<double>(&value)) return out = *real, std::isfinite(out);
	return false;
}

// A name the file's walk carries as one token, within the bytes the game keeps for it.
bool set_text(std::string &field, size_t bytes, const char *what, const Value &value, std::string &error) {
	const auto *text = std::get_if<std::string>(&value);
	if (!text) return error = std::string(what) + " is a text.", false;
	if (text->find_first_of("\"\r\n") != std::string::npos)
		return error = std::string(what) + " holds no quote and no line break: the file's reader ends it there.", false;
	for (const unsigned char c : *text)
		if (c < 0x20 || c > 0x7E) return error = std::string(what) + " is plain ASCII: the game compares it byte for byte.", false;
	if (text->size() >= bytes)
		return error = std::string(what) + " holds at most " + std::to_string(bytes - 1) + " characters: the game keeps " +
		               std::to_string(bytes) + " bytes for it, its terminator among them.",
		       false;
	field = *text;
	return true;
}

// A column-2 or column-3 number: the file's decimal, stored x 65536 [orig: @ 0x527122..0x527169].
bool set_q16(int32_t &field, const Value &value, std::string &error) {
	double number = 0.0;
	if (!number_of(value, number) || std::fabs(number) * 65536.0 > double(INT32_MAX))
		return error = "The number is from -32767 to 32767.", false;
	field = int32_t(std::lround(number * 65536.0));
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
	TableKind profile(RecordKindRow{kProfile, "profile", "Sound profile", "Add sound profile", true});
	FieldSchema name = schema_of("name", FieldType::Text, "Name",
			"What an item's sound_profile binds by: the first profile of the name, without case; a name no "
			"profile has binds the file's first profile [orig: SoundProfile_FindSlotByName @ 0x526e30].");
	name.width = kProfileNameBytes;
	name.defines = ReferenceKind::SoundProfile;
	profile.field(RF{name,
	                 {[](const RecordHandle &r, Value &out) { return out = profile_of(r).name, true; },
	                  [](const RecordHandle &r, const Value &v, std::string &e) {
		                  return set_text(r.as<ProfileRecord>().name, kProfileNameBytes, "A profile's name", v, e);
	                  }}});
	for (int i = 0; i < audio::kSoundProfileLoopParamCount; ++i) {
		FieldSchema percent = schema_of(audio::sound_profile_loop_keyword(i), FieldType::Integer, kLoopWords[i],
				"A whole percent the game stores x 655 [orig: SoundProfile_ParseLineCallback, the keyword chain @ "
				"0x5270a9-0x527481]; read by the vehicle sound's medium and cruise loops.");
		percent.unit = "%";
		percent.section = "Loop fades";
		percent.ranged = true;
		percent.min = -3278000;
		percent.max = 3278000;
		profile.field(RF{percent,
		                 {[i](const RecordHandle &r, Value &out) { return out = int64_t(profile_of(r).loop_params[size_t(i)] / 655), true; },
		                  [i](const RecordHandle &r, const Value &v, std::string &e) {
			                  const auto *whole = std::get_if<int64_t>(&v);
			                  if (!whole || *whole < -3278000 || *whole > 3278000) return e = "A percent is a whole number.", false;
			                  r.as<ProfileRecord>().loop_params[size_t(i)] = int32_t(*whole * 655);
			                  return true;
		                  }}});
	}
	TableList slots;
	slots.spec = Document::CollectionSpec{kSlot, "Slots", "keyword", true, Applicability::Reads,
	                                      size_t(audio::kSoundProfileSlotCount)};
	slots.spec.first_number = 0;
	slots.ops = vector_list<ProfileRecord, ProfileSlot>(kSlot,
			[](ProfileRecord &p) -> std::vector<ProfileSlot> & { return p.slots; });
	profile.list(std::move(slots));

	TableKind slot(RecordKindRow{kSlot, "slot", "Slot", "", false});
	FieldSchema keyword = schema_of("keyword", FieldType::Text, "Keyword",
			"The slot's keyword in the file, and what the game plays it for [orig: the keyword table @ 0x82F3B0].");
	keyword.read_only = true;
	slot.field(RF{keyword, {[](const RecordHandle &r, Value &out) {
		              return out = std::string(audio::sound_profile_slot_keyword(slot_of(r).slot)), true;
	              }}});
	FieldSchema set = schema_of("set", FieldType::Text, "Sound set",
			"The set the game plays for the slot, found by name across the loaded banks at mission start; "
			"empty plays nothing [orig: SoundProfile_ResolveAllTriggers @ 0x528210].");
	set.width = kSetNameBytes;
	set.reference = ReferenceKind::Sound;
	slot.field(RF{set,
	              {[](const RecordHandle &r, Value &out) { return out = slot_of(r).set, true; },
	               [](const RecordHandle &r, const Value &v, std::string &e) {
		               return set_text(r.as<ProfileSlot>().set, kSetNameBytes, "A set's name", v, e);
	               }}});
	slot.field(RF{schema_of("param2", FieldType::Real, "Number 1",
	                        "The line's third column, stored x 65536 [orig: @ 0x527122]; a loop's low pitch, a "
	                        "time-of-day shot's first value. Read only after a set."),
	              {[](const RecordHandle &r, Value &out) { return out = slot_of(r).param2_q16 / 65536.0, true; },
	               [](const RecordHandle &r, const Value &v, std::string &e) {
		               return set_q16(r.as<ProfileSlot>().param2_q16, v, e);
	               }}});
	slot.field(RF{schema_of("param3", FieldType::Real, "Number 2",
	                        "The line's fourth column, stored x 65536 [orig: @ 0x527169]; a loop's high pitch."),
	              {[](const RecordHandle &r, Value &out) { return out = slot_of(r).param3_q16 / 65536.0, true; },
	               [](const RecordHandle &r, const Value &v, std::string &e) {
		               return set_q16(r.as<ProfileSlot>().param3_q16, v, e);
	               }}});
	slot.field(RF{schema_of("param4", FieldType::Integer, "Whole number",
	                        "The line's fifth column, a whole number [orig: atol @ 0x52718b]; a vehicle loop's "
	                        "gear count."),
	              {[](const RecordHandle &r, Value &out) { return out = int64_t(slot_of(r).param4), true; },
	               [](const RecordHandle &r, const Value &v, std::string &e) {
		               const auto *whole = std::get_if<int64_t>(&v);
		               if (!whole || *whole < INT32_MIN || *whole > INT32_MAX) return e = "The number is whole.", false;
		               r.as<ProfileSlot>().param4 = int32_t(*whole);
		               return true;
	               }}});
	return RecordTable({std::move(profile), std::move(slot)});
}

const SoundProfileRow &profile_row(const Node &node) { return static_cast<const SoundProfileRow &>(node); }

std::string free_name(const std::vector<std::shared_ptr<const Node>> &rows, const std::string &stem, const Node *self) {
	const auto taken = [&](const std::string &name) {
		for (const auto &other : rows)
			if (other && other.get() != self && strutil::iequals(other->name(), name)) return true;
		return false;
	};
	if (!taken(stem)) return stem;
	for (int n = 2; n < 100000; ++n) {
		const std::string suffix = "_" + std::to_string(n);
		const std::string name = stem.substr(0, std::min(stem.size(), kProfileNameBytes - 1 - suffix.size())) + suffix;
		if (!taken(name)) return name;
	}
	return stem;
}

ProfileRecord record_of(const audio::SoundProfile &profile) {
	ProfileRecord out;
	out.name = profile.name;
	out.loop_params = profile.loop_params;
	out.slots.resize(size_t(audio::kSoundProfileSlotCount));
	for (int i = 0; i < audio::kSoundProfileSlotCount; ++i) {
		ProfileSlot &slot = out.slots[size_t(i)];
		slot.slot = i;
		slot.set = profile.set_names[size_t(i)];
		slot.param2_q16 = profile.param2_q16[size_t(i)];
		slot.param3_q16 = profile.param3_q16[size_t(i)];
		slot.param4 = profile.param4[size_t(i)];
	}
	return out;
}

} // namespace

const RecordTable &sound_profile_table() {
	static const RecordTable table = make_table();
	return table;
}

bool is_sound_profile_kind(AssetKind kind) {
	return asset_kind_row(kind).document == DocumentTypeId::SoundProfiles;
}

const char *sound_profile_slot_words(int slot) {
	return slot >= 0 && slot < audio::kSoundProfileSlotCount ? kSlotWords[slot].words : "";
}

const char *sound_profile_slot_family(int slot) {
	return slot >= 0 && slot < audio::kSoundProfileSlotCount ? kSlotWords[slot].family : "";
}

int sound_profile_slot_of(const std::string &keyword) {
	for (int i = 0; i < audio::kSoundProfileSlotCount; ++i)
		if (strutil::iequals(keyword, audio::sound_profile_slot_keyword(i))) return i;
	return -1;
}

SoundProfileRow::SoundProfileRow() {
	kind = kProfile;
	profile = record_of(audio::SoundProfile());
}

RecordHandle SoundProfileRow::record() const { return {kind, const_cast<ProfileRecord *>(&profile)}; }

size_t SoundProfileRow::footprint() const {
	size_t bytes = sizeof(SoundProfileRow) + footprint_of(profile.name) + footprint_of(profile.slots) + ids_footprint();
	for (const ProfileSlot &slot : profile.slots) bytes += footprint_of(slot.set);
	return bytes;
}

std::string SoundProfileDocument::record_title(const NodeAddress &address) const {
	const Node *node = row(address.row);
	if (!node) return std::string();
	if (!address.child) return profile_row(*node).profile.name;
	const RecordHandle handle = record_in(*node, address);
	if (!handle || address.kind != kSlot) return record_name(address);
	const ProfileSlot &slot = slot_of(handle);
	const std::string words = std::string(audio::sound_profile_slot_keyword(slot.slot)) + " (" +
	                          sound_profile_slot_words(slot.slot) + ")";
	return slot.set.empty() ? words : words + ": " + slot.set;
}

bool SoundProfileDocument::parse(const std::vector<uint8_t> &bytes, std::vector<std::shared_ptr<Node>> &rows,
                                 std::shared_ptr<const FileState> &, std::vector<SourceIssue> &, Diagnostic &error) {
	if (!is_sound_profile_kind(kind())) {
		error = make_finding(CoreFinding::DocumentKind, DiagnosticSeverity::Error, "This file is not SndProf.def.", path());
		return false;
	}
	// The game's own walk of the file, every line it reads as it reads it [orig: SoundProfile_LoadAll @
	// 0x527490 -> File_ParseASCIIFile @ 0x5274DD].
	audio::SoundProfileTable table;
	table.parse(reinterpret_cast<const char *>(bytes.data()), bytes.size());
	for (const audio::SoundProfile &profile : table.entries()) {
		auto row = std::make_shared<SoundProfileRow>();
		row->profile = record_of(profile);
		shape(*row);
		rows.push_back(std::move(row));
	}
	return true;
}

std::vector<audio::SoundProfile> SoundProfileDocument::profiles() const {
	std::vector<audio::SoundProfile> out;
	for (const auto &node : rows()) {
		if (!node || node->kind != kProfile) continue;
		const ProfileRecord &record = profile_row(*node).profile;
		audio::SoundProfile profile;
		profile.name = record.name;
		profile.loop_params = record.loop_params;
		for (const ProfileSlot &slot : record.slots) {
			if (slot.slot < 0 || slot.slot >= audio::kSoundProfileSlotCount) continue;
			const size_t i = size_t(slot.slot);
			profile.set_names[i] = slot.set;
			profile.param2_q16[i] = slot.param2_q16;
			profile.param3_q16[i] = slot.param3_q16;
			profile.param4[i] = slot.param4;
		}
		out.push_back(std::move(profile));
	}
	return out;
}

const SoundProfileRow *SoundProfileDocument::find_profile(const std::string &name) const {
	const SoundProfileRow *first = nullptr;
	for (const auto &node : rows()) {
		if (!node || node->kind != kProfile) continue;
		const SoundProfileRow &row = profile_row(*node);
		if (!first) first = &row;
		if (strutil::iequals(row.profile.name, name)) return &row;
	}
	return first;
}

SerializeResult SoundProfileDocument::serialize() const {
	SerializeResult result;
	std::string error;
	if (!audio::write_sound_profiles(profiles(), result.text, error)) {
		result.text.clear();
		result.issues.push_back({true, 0, std::string(), std::string(), error});
	}
	return result;
}

std::string SoundProfileDocument::save_words() const {
	return "A save writes every profile in the file's form: each slot with a set on one line of five columns, "
	       "the percents after, comments and the lines the game skips left out.";
}

std::shared_ptr<Node> SoundProfileDocument::make_node(NodeKind kind, NodeId, const std::vector<std::shared_ptr<const Node>> &rows,
                                                      std::string &error) {
	if (kind != kProfile) {
		error = "SndProf.def's rows are its profiles.";
		return nullptr;
	}
	auto row = std::make_shared<SoundProfileRow>();
	row->profile.name = free_name(rows, "default", nullptr);
	if (!strutil::iequals(row->profile.name, "default")) row->profile.name = free_name(rows, "NEW_PROFILE", nullptr);
	shape(*row);
	return row;
}

void SoundProfileDocument::prepare_duplicate(Node &copy, const Node &original,
                                             const std::vector<std::shared_ptr<const Node>> &rows) const {
	static_cast<SoundProfileRow &>(copy).profile.name = free_name(rows, profile_row(original).profile.name, &copy);
}

// --- the findings ------------------------------------------------------------------------------------

namespace {

constexpr FindingCodeEntry<SoundProfileFinding> kFindingEntries[] = {
	// The game binds the first of a name [orig: SoundProfile_FindSlotByName @ 0x526e30]; no refusal.
	{ SoundProfileFinding::NameRepeated, listed_code("sound_profiles.name_repeated") },
	{ SoundProfileFinding::NoDefault, listed_code("sound_profiles.no_default") },
	{ SoundProfileFinding::Unserializable, { "sound_profiles.unserializable", FindingFix::None, nullptr, true } },
};
static_assert(std::size(kFindingEntries) == static_cast<size_t>(SoundProfileFinding::kCount),
		"every SoundProfileFinding has exactly one row");
static_assert(finding_entries_well_formed(kFindingEntries),
		"the sound profiles' rows follow SoundProfileFinding's order, each token its own");
constexpr auto kFindingRows = finding_rows(kFindingEntries, FindingGroup::SoundProfiles);
static_assert(finding_rows_well_formed(kFindingRows), "every row of the table takes its group");

} // namespace

const FindingCodeRow &finding_code(SoundProfileFinding code) { return kFindingRows[static_cast<size_t>(code)]; }

FindingTable sound_profile_finding_codes() { return { kFindingRows.data(), kFindingRows.size() }; }

// A sound profile an item binds by a name no profile has (DI-15): a profile of that name as Add sound profile makes
// one, the profile the game's reader opens at a `begin` line, every slot empty and every loop number 0
// (audio::SoundProfile's) [orig: SoundProfile_ParseLineCallback @ 0x526fc0]. A name of 64 characters or more is
// cut by the reader [orig: @ 0x52703F..0x527043]: none is offered.
bool define_sound_profile(const DocumentBase &document, const ReferenceSubject &missing, PlannedFix &out) {
	const auto *profiles = dynamic_cast<const SoundProfileDocument *>(&document);
	if (!profiles || missing.kind != ReferenceKind::SoundProfile || missing.target.empty() ||
	    missing.target.size() >= kProfileNameBytes)
		return false;
	const SoundProfileRow *bound = profiles->find_profile(missing.target);
	Edit add;
	add.operation = EditOperation::Add;
	add.address.kind = kProfile;
	add.field = "name";
	add.value = missing.target;
	out = PlannedFix();
	out.edits.push_back(std::move(add));
	const std::string file = basename_of(document.path());
	out.label = "Add " + missing.target + " to " + file;
	out.detail = "Adds the sound profile " + missing.target + " at the end of " + file + ", as Add sound profile makes one, "
	             "every slot empty, and selects it to give its slots their sets: the game then binds it" +
	             (bound ? " in place of the first profile, " + bound->profile.name + ", which it binds now." : ".");
	return true;
}

std::vector<Diagnostic> validate_sound_profiles_file(const DocumentBase &document) {
	std::vector<Diagnostic> findings;
	const auto *profiles = dynamic_cast<const SoundProfileDocument *>(&document);
	if (!profiles) return findings;
	const auto add = [&](const Node *node, DiagnosticSeverity severity, SoundProfileFinding code, const char *field,
	                     const std::string &message) {
		Diagnostic d = make_finding(code, severity, message, document.path(), field);
		if (node) {
			d.row_id = node->id;
			d.record_kind = kProfile;
			d.record = node->name();
		}
		findings.push_back(std::move(d));
	};
	std::unordered_map<std::string, bool> seen;
	bool has_default = false;
	const Node *first = nullptr;
	for (const auto &node : profiles->rows()) {
		if (!node || node->kind != kProfile) continue;
		if (!first) first = node.get();
		const std::string &name = profile_row(*node).profile.name;
		has_default = has_default || strutil::iequals(name, "default");
		if (!seen.emplace(strutil::to_lower(name), true).second)
			add(node.get(), DiagnosticSeverity::Warning, SoundProfileFinding::NameRepeated, "name",
			    "An earlier profile is named '" + name + "': the game binds the first of a name, so no item gets this one.");
	}
	if (first && !has_default)
		add(first, DiagnosticSeverity::Info, SoundProfileFinding::NoDefault, "name",
		    "No profile is named default: every item without a sound_profile key binds the file's first profile, " +
		            first->name() + " [orig: ItemDef_AllocateWithDefaults @ 0x49e3e5 -> SoundProfile_FindSlotByName @ "
		                            "0x526e30].");
	const SerializeResult written = profiles->serialize();
	for (const SourceIssue &issue : written.issues)
		add(nullptr, DiagnosticSeverity::Error, SoundProfileFinding::Unserializable, "", issue.message + " The file cannot be saved until it can.");
	return findings;
}

} // namespace opennova::editor
