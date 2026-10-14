// The AI profile document (ADR 0046 S23 B; editor/documents/ai_profile_document.h): an .aip's one profile as a
// record document over the game's reader (formats/aip), a save writing the profile alone in the format's writer's
// form (the S26 ruling): its keys as fields in the units the file writes them (a degree, a second, a km/h; a choice,
// a mask, an ammo's name), each read by its type's key set alone (a HELO key ignored on a GROUND profile); a value
// set read back as the game reads it (0.25 seconds holds 15 ticks, shown 0.24), its own line; the lines the reader
// reads nothing of and a profile of no type as findings; no row added. The retail leg (OPENNOVA_JO_DIR): every
// shipped profile through the document, unchanged, saved as the format's writer writes what its reader reads, a
// fixed point.
#include <editor/documents/ai_profile_document.h>
#include <editor/documents/document_types.h>
#include <base/vfs/vfs.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "common/retail_paths.h"
#include "common/test_expect.h"
#include "editor/editor_test_support.h"

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

int test_retail() {
	const std::string install = retail::install();
	if (install.empty()) {
		retail::skip_leg("OPENNOVA_JO_DIR (every shipped AI profile through the document)");
		return 0;
	}
	Vfs vfs;
	TEST_EXPECT(vfs.mount_game(install, "", VfsMountMode::Packed));
	size_t files = 0;
	for (const auto &location : vfs.list_files()) {
		std::string lower = location.logical_name;
		std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return char(std::tolower(c)); });
		if (lower.size() < 4 || lower.substr(lower.size() - 4) != ".aip") continue;
		std::vector<uint8_t> bytes;
		TEST_EXPECT(vfs.read_file(location.logical_name, bytes));
		AiProfileDocument document;
		Diagnostic error;
		TEST_EXPECT(document.load_bytes(bytes, location.logical_name, AssetKind::AiProfile, "jo", error));
		TEST_EXPECT(!document.blocked());
		const SerializeResult saved = document.serialize();
		AiProfileDocument again;
		if (!saved.ok() || saved.text != written(std::string(bytes.begin(), bytes.end())) ||
		    !again.load_bytes(bytes_of(saved.text), location.logical_name, AssetKind::AiProfile, "jo", error) ||
		    again.serialize().text != saved.text) {
			std::fprintf(stderr, "FAIL %s is not saved as the format's writer writes it\n", location.logical_name.c_str());
			return 1;
		}
		++files;
	}
	std::printf("retail: %zu shipped profiles through the document, saved as the format's writer writes them, a fixed "
	            "point\n",
	            files);
	TEST_EXPECT(files >= 98);
	return 0;
}

} // namespace

int main(int argc, char **argv) {
	retail::configure_mixed(argc, argv);
	if (test_fields() || test_type_keys() || test_findings() || test_retail()) return 1;
	return 0;
}
