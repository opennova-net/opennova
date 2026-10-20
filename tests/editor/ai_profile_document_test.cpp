// The AI profile document (ADR 0046 S23 B; editor/documents/ai_profile_document.h): an .aip's one profile as a
// record document over the game's reader (formats/aip), a save writing the profile alone in the format's writer's
// form (the S26 ruling): its keys as fields in the units the file writes them (a degree, a second, a km/h; a choice,
// a mask, an ammo's name), each read by its type's key set alone (a HELO key ignored on a GROUND profile); a value
// set read back as the game reads it (0.25 seconds holds 15 ticks, shown 0.24), its own line; the lines the reader
// reads nothing of and a profile of no type as findings; no row added. The shipped profiles' two shapes (authored
// here) and the format's variants (its fixtures) through the document, unchanged, saved as the format's writer
// writes what its reader reads, a fixed point.
#include <editor/documents/ai_profile_document.h>
#include <editor/documents/document_types.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include "common/test_expect.h"
#include "editor/editor_test_support.h"
#include "editor/variant_files.h"

using namespace opennova;
using namespace opennova::editor;

namespace {

constexpr NodeKind kProfile = node_kind(AiProfileKind::Profile);

std::vector<uint8_t> bytes_of(const std::string &text) { return std::vector<uint8_t>(text.begin(), text.end()); }

FieldUse use_of(const AiProfileDocument &document, const NodeAddress &at, const char *id) {
	for (const FieldSchema &field : document.fields(kProfile))
		if (field.id == id) return document.field_on(at, field);
	return FieldUse();
}

using editor_test::has_code;

// The text a save of `text` writes: the format's writer's of the profile its reader reads.
std::string written(const std::string &text) {
	const aip::Profile profile = aip::parse_profile(reinterpret_cast<const uint8_t *>(text.data()), text.size());
	std::string out, error;
	return aip::write_profile(profile, out, error) ? out : std::string();
}

std::string replaced(std::string text, const std::string &from, const std::string &to) {
	text.replace(text.find(from), from.size(), to);
	return text;
}

const std::string kFile =
		"description     \"Grnd - test\"\r\n"
		"type\t\t\tGROUND\r\n"
		"default_state\tGROUND_FOLLOWWP\r\n"
		"view_fov\t\t360\t\r\n"
		"primary_weap\t\tAI_LAW\t\r\n"
		"primary_rate\t\t10\t\r\n"
		"primary_flags\t\tWEAPON_TURRET WEAPON_FAST\r\n"
		"patrol_speed\t\t40\t\r\n"
		"min_speed\t\t\t0\t\r\n"
		"turn_rate\t\t\t60\t\r\n";

int test_fields() {
	AiProfileDocument document;
	Diagnostic error;
	TEST_EXPECT(document.load_bytes(bytes_of(kFile), "ai/g_test.aip", AssetKind::AiProfile, "jo", error));
	TEST_EXPECT(document.rows().size() == 1 && !document.blocked());
	const NodeId id = document.rows()[0]->id;
	const NodeAddress at{id, kProfile, 0};
	Value value;
	TEST_EXPECT(document.get(at, "type", value) && value == Value(int64_t(aip::kTypeGround)));
	TEST_EXPECT(document.get(at, "view_fov", value) && value == Value(360.0));
	TEST_EXPECT(document.get(at, "primary_rate", value) && value == Value(10.0));
	TEST_EXPECT(document.get(at, "patrol_speed", value) && value == Value(40.0));
	TEST_EXPECT(document.get(at, "turn_rate", value) && value == Value(int64_t(60)));
	TEST_EXPECT(document.get(at, "primary_flags", value) && value == Value(int64_t(aip::kWeaponTurret | aip::kWeaponFast)));
	TEST_EXPECT(document.get(at, "primary_weap", value) && value == Value(std::string("AI_LAW")));
	// A HELO key on a GROUND profile is read for nothing; a GROUND key is read.
	TEST_EXPECT(use_of(document, at, "patrol_altitude").applies == Applicability::Ignored);
	TEST_EXPECT(use_of(document, at, "view_dist").applies == Applicability::Reads);
	TEST_EXPECT(use_of(document, at, "primary_weap").reference == ReferenceKind::Ammo);
	// Unchanged, the save is the writer's text of the profile the game reads.
	TEST_EXPECT(!written(kFile).empty() && document.serialize().text == written(kFile));
	// A value set reads back as the game reads it: 0.25 seconds is 15 ticks, which the file writes 0.24.
	Edit edit;
	edit.address = at;
	edit.field = "primary_rate";
	edit.value = 0.25;
	TEST_EXPECT(document.apply(edit, error));
	TEST_EXPECT(document.get(at, "primary_rate", value) && value == Value(0.24));
	std::string saved = document.serialize().text;
	TEST_EXPECT(saved == written(replaced(kFile, "primary_rate\t\t10", "primary_rate\t\t0.25")) &&
	            saved.find("0.24") != std::string::npos);
	// A key set anew.
	edit.field = "view_dist";
	edit.value = int64_t(500);
	TEST_EXPECT(document.apply(edit, error));
	saved = document.serialize().text;
	TEST_EXPECT(saved == written(replaced(kFile, "primary_rate\t\t10", "primary_rate\t\t0.25") + "view_dist\t500\r\n"));
	// A subtype of the other type's is refused; a choice set; no row is added.
	edit.field = "subtype";
	edit.value = int64_t(2);
	TEST_EXPECT(!document.apply(edit, error));
	edit.value = int64_t(1);
	TEST_EXPECT(document.apply(edit, error));
	Edit add;
	add.operation = EditOperation::Add;
	add.address.kind = kProfile;
	TEST_EXPECT(!document.apply(add, error));
	std::printf("fields: the keys in the file's units, a type's set, a value read back as the game reads it, a key "
	            "set anew, the save the writer's text of the profile\n");
	return 0;
}

// A key the profile's type does not read is refused (S26 A11): the game's reader dispatches every key but `type` on
// the type [orig: AIProfile_ParseProperty @ 0x45DF15..0x45DF2F], so a GROUND profile holds no HELO key; setting it to
// none is taken. A type change clears every key the new type does not read (ORGANIC reads none but the type), and
// the profile saves and reads back as it stands.
int test_type_keys() {
	AiProfileDocument document;
	Diagnostic error;
	TEST_EXPECT(document.load_bytes(bytes_of(kFile), "ai/g_test.aip", AssetKind::AiProfile, "jo", error));
	const NodeAddress at{document.rows()[0]->id, kProfile, 0};
	Edit edit;
	edit.address = at;
	edit.field = "patrol_altitude";
	edit.value = 100.0;
	TEST_EXPECT(!document.apply(edit, error) && error.message.find("only in a profile of another type") != std::string::npos);
	edit.value = 0.0;
	TEST_EXPECT(document.apply(edit, error));
	edit.field = "view_dist";
	edit.value = int64_t(500);
	TEST_EXPECT(document.apply(edit, error));
	edit.field = "type";
	edit.value = int64_t(aip::kTypeOrganic);
	TEST_EXPECT(document.apply(edit, error));
	Value value;
	size_t cleared = 0;
	for (const aip::KeyRow &row : aip::key_rows()) {
		if (row.unit == aip::Unit::Type || aip::reads(row, aip::kTypeOrganic)) continue;
		TEST_EXPECT(document.get(at, row.key, value));
		const bool none = value == Value(std::string()) || value == Value(0.0) || value == Value(int64_t(0));
		if (!none) std::printf("  %s still holds a value on an ORGANIC profile\n", row.key);
		TEST_EXPECT(none);
		++cleared;
	}
	TEST_EXPECT(cleared > 0);
	const SerializeResult saved = document.serialize();
	for (const SourceIssue &issue : saved.issues) std::printf("  the save: %s\n", issue.message.c_str());
	TEST_EXPECT(saved.ok());
	AiProfileDocument back;
	TEST_EXPECT(back.load_bytes(bytes_of(saved.text), "ai/g_test.aip", AssetKind::AiProfile, "jo", error));
	const NodeAddress back_at{back.rows()[0]->id, kProfile, 0};
	for (const aip::KeyRow &row : aip::key_rows()) {
		Value mine, read;
		TEST_EXPECT(document.get(at, row.key, mine) && back.get(back_at, row.key, read) && mine == read);
	}
	// Every type change saves a file that reads back as it was saved: the record is the file's as the game reads it.
	const std::string ground = "type\tGROUND\r\nview_dist\t500\r\npatrol_speed\t3\r\nprimary_weap\tAI_LAW\r\n";
	for (const int32_t type : {0, aip::kTypeHelo, aip::kTypeOrganic}) {
		AiProfileDocument changed;
		TEST_EXPECT(changed.load_bytes(bytes_of(ground), "ai/g.aip", AssetKind::AiProfile, "jo", error));
		Edit retype;
		retype.address = {changed.rows()[0]->id, kProfile, 0};
		retype.field = "type";
		retype.value = int64_t(type);
		TEST_EXPECT(changed.apply(retype, error));
		const std::string once = changed.serialize().text;
		AiProfileDocument again;
		TEST_EXPECT(again.load_bytes(bytes_of(once), "ai/g.aip", AssetKind::AiProfile, "jo", error));
		const std::string twice = again.serialize().text;
		if (once != twice)
			std::printf("  type %d: saved %zu bytes [%s], read and saved again %zu bytes [%s]\n", int(type), once.size(),
			            once.c_str(), twice.size(), twice.c_str());
		TEST_EXPECT(once == twice);
	}
	std::printf("type keys: another type's key refused, a type change clearing the keys the new type does not read\n");
	return 0;
}

int test_findings() {
	AiProfileDocument document;
	Diagnostic error;
	TEST_EXPECT(document.load_bytes(bytes_of(kFile), "g_test.aip", AssetKind::AiProfile, "jo", error));
	std::vector<Diagnostic> findings = validate_ai_profile_file(document);
	// The description line and the GROUND file's min_speed: read for nothing, listed.
	const size_t ignored = size_t(std::count_if(findings.begin(), findings.end(),
	                                            [](const Diagnostic &d) { return d.code() == "ai_profile.ignored_input"; }));
	TEST_EXPECT(ignored == 2 && !has_code(findings, "ai_profile.no_type"));
	AiProfileDocument none;
	TEST_EXPECT(none.load_bytes(bytes_of("// no type\r\nrank 2\r\n"), "none.aip", AssetKind::AiProfile, "jo", error));
	findings = validate_ai_profile_file(none);
	TEST_EXPECT(has_code(findings, "ai_profile.no_type") && has_code(findings, "ai_profile.ignored_input"));
	std::printf("findings: the lines read for nothing, a profile of no type\n");
	return 0;
}

// The shipped profiles' two shapes, authored: a GROUND and a HELO, each its description before its type, tabs and
// trailing blanks, a line of a tab alone, a flag mask of two words, a decimal, a number after blanks, a key of the
// other type, and the file ending in a NUL after its last line end.
const std::string kGroundShape =
		std::string("description     \"Grnd - truck\"\r\ntype\t\t\tGROUND\r\ndefault_state\tGROUND_FOLLOWWP\t\t\r\n\r\n"
		            "rank\t\t\t\t1\r\nview_fov\t\t\t200\t\r\nview_dist\t\t\t350\t\r\n\t\r\n"
		            "evade_flags\t\t\tFOLLOW_WP FLEE\r\ncombat_flags\t\tFOLLOW_WP FLEE\r\n\r\n"
		            "primary_weap\t\tAI_LAW\t\r\nprimary_rate\t\t0.5\t\r\nprimary_fov\t\t    90\t\t\t\r\n"
		            "secondary_weap\t\t20MM\t\r\n\r\npatrol_speed\t\t60\t\r\nmin_speed\t\t\t0\t\r\n"
		            "max_chase_dist\t\t250\t\r\ndrive_skill\t\t\t2\r\nturn_rate\t\t\t40\t\r\n\r\naccel_time\t\t\t30\r\n"
		            "radio_delay\t\t\t0\r\n") +
		std::string(1, '\0');
const std::string kHeloShape =
		std::string("description     \"Helo - test\"\r\ntype\t\t\t\tHELO\r\ndefault_state\t\tHELO_FOLLOWWP\t\t\r\n\r\n"
		            "rank\t\t\t\t2\r\nview_fov\t\t\t150\t\r\n\t\r\nevade_flags\t\t\tNO_ACTION COUNTER\r\n"
		            "combat_flags\t\tNO_ACTION COUNTER\r\n\r\nprimary_weap\t\t50CAL\t\t\r\nprimary_rate\t\t0.01\t\t\r\n"
		            "primary_fov\t\t    100\t\t\t\r\nprimary_facing\t\tWEAPON_TURRET\r\nprimary_flags\t\t0\r\n"
		            "secondary_flags\t\tWEAPON_TURRET\r\nhunt_flags\t\t0\r\n\r\npatrol_speed\t\t90\t\r\n"
		            "patrol_altitude\t\t30\t\r\npatrol_climb\t\t8\t\r\nmin_agl\t\t\t\t4\t\r\nmin_speed\t\t\t0\t\r\n"
		            "flight_skill\t\t3\r\nturn_rate\t\t\t60\t\r\n\r\naccel_time\t\t\t25\r\nuse_waypoint_z\t\t1\r\n") +
		std::string(1, '\0');

// The format's variants: the two shapes above and each profile the format's fixtures hold (its rows' shapes of the
// shipped profiles among them: a ground vehicle, a helicopter, a boat, a turret) through the document, unchanged and
// unblocked, saved as the format's writer writes what its reader reads, and that save read again the same text.
const char *const kShapes[] = {"fixtures/aip/ground_vehicle.aip", "fixtures/aip/helo.aip", "fixtures/aip/boat.aip",
                               "fixtures/aip/turret.aip"};

int test_variants() {
	const editor_test::VariantFiles variants = editor_test::variant_files(
			{"fixtures/aip"}, {".aip"}, std::vector<std::string>(std::begin(kShapes), std::end(kShapes)));
	int failures = editor_test::report_missing(variants);
	std::vector<std::pair<std::string, std::vector<uint8_t>>> inputs{{"the GROUND shape", bytes_of(kGroundShape)},
	                                                                 {"the HELO shape", bytes_of(kHeloShape)}};
	for (const std::string &path : variants.files) inputs.emplace_back(path, editor_test::repo_bytes(path));
	for (const auto &[name, bytes] : inputs) {
		AiProfileDocument document;
		Diagnostic error;
		TEST_EXPECT(!bytes.empty() && document.load_bytes(bytes, "ai/profile.aip", AssetKind::AiProfile, "jo", error));
		TEST_EXPECT(!document.blocked());
		const SerializeResult saved = document.serialize();
		const std::string expected = written(std::string(bytes.begin(), bytes.end()));
		AiProfileDocument again;
		if (!saved.ok() || expected.empty() || saved.text != expected ||
		    !again.load_bytes(bytes_of(saved.text), "ai/profile.aip", AssetKind::AiProfile, "jo", error) ||
		    again.serialize().text != saved.text) {
			std::fprintf(stderr, "FAIL %s is not saved as the format's writer writes it\n", name.c_str());
			++failures;
			continue;
		}
		std::printf("variant: %s saved as the format's writer writes it, a fixed point\n", name.c_str());
	}
	TEST_EXPECT(variants.files.size() >= 4);
	return failures;
}

} // namespace

int main() {
	if (test_fields() || test_type_keys() || test_findings() || test_variants()) return 1;
	return 0;
}
