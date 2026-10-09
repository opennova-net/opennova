// The files the game finds by a mission's name (runtime/mission/mission_sidecars.h): the six
// witnessed rows, each with its reader's extension, its alternate, its fallback and the row it
// waits on, and the names they build for a mission, the mission's case kept.

#include <set>
#include <string>

#include "common/test_expect.h"
#include <formats/pff/pff.h>
#include <runtime/mission/mission_sidecars.h>

int main() {
	using namespace opennova::mission;

	const std::vector<Sidecar> &rows = sidecars();
	TEST_EXPECT(rows.size() == 6u);

	// Each role and each extension once; every extension a dot and three letters.
	std::set<std::string> roles, extensions;
	for (const Sidecar &row : rows) {
		TEST_EXPECT(row.role != nullptr && row.extension != nullptr);
		TEST_EXPECT(roles.insert(row.role).second);
		TEST_EXPECT(extensions.insert(row.extension).second);
		TEST_EXPECT(std::string(row.extension).size() == 4u && row.extension[0] == '.');
		if (row.alternate) TEST_EXPECT(extensions.insert(row.alternate).second);
		if (row.needs) TEST_EXPECT(sidecar_for_role(row.needs) != nullptr);
		TEST_EXPECT(sidecar_for_role(row.role) == &row);
	}
	TEST_EXPECT(sidecar_for_role("briefing") == nullptr && sidecar_for_role("") == nullptr);

	// The rows as witnessed (docs/required-resources.md, "Mission start"; docs/audio/
	// lwf-dbf-sound-re.md for the dialog bank's sounds).
	const Sidecar *text = sidecar_for_role("text");
	TEST_EXPECT(text && std::string(text->extension) == ".bin" && std::string(text->fallback) == "medmssn.bin");
	TEST_EXPECT(text->alternate == nullptr && text->needs == nullptr);
	const Sidecar *script = sidecar_for_role("script");
	TEST_EXPECT(script && std::string(script->extension) == ".wac" && !script->fallback && !script->alternate);
	const Sidecar *image = sidecar_for_role("loading_image");
	TEST_EXPECT(image && std::string(image->extension) == ".pcx" && std::string(image->fallback) == "loadscrn.pcx");
	const Sidecar *tiles = sidecar_for_role("tiles");
	TEST_EXPECT(tiles && std::string(tiles->extension) == ".til" && !tiles->fallback);
	const Sidecar *dialog = sidecar_for_role("dialog");
	TEST_EXPECT(dialog && std::string(dialog->extension) == ".dbf" && !dialog->fallback && !dialog->needs);
	const Sidecar *sounds = sidecar_for_role("dialog_sounds");
	TEST_EXPECT(sounds && std::string(sounds->extension) == ".lwf" && std::string(sounds->alternate) == ".pwf");
	TEST_EXPECT(!sounds->fallback && std::string(sounds->needs) == "dialog");

	// The names: the mission's base name, case kept, whatever folder it was spelled with.
	TEST_EXPECT(mission_base_name("00TRa.bms") == "00TRa");
	TEST_EXPECT(mission_base_name("missions/00TRa.bms") == "00TRa");
	TEST_EXPECT(mission_base_name("missions\\ASP_G7.BMS") == "ASP_G7");
	TEST_EXPECT(mission_base_name("noextension") == "noextension");
	// The first dot, as the game's extension swap (Path_ReplaceOrAppendExtension) cuts it: a
	// mission named with an inner dot finds its files by the name before it (review F8).
	TEST_EXPECT(mission_base_name("two.dots.bms") == "two");
	TEST_EXPECT(mission_base_name("maps.v2/op.v2.bms") == "op");
	TEST_EXPECT(script && sidecar_name("op.v2.bms", *script) == "op.wac");
	TEST_EXPECT(sidecar_name("00TRa.bms", *script) == "00TRa.wac");
	TEST_EXPECT(sidecar_name("missions/00TRa.bms", *text) == "00TRa.bin");
	TEST_EXPECT(sidecar_name("ASP_G7.BMS", *image) == "ASP_G7.pcx");
	TEST_EXPECT(sidecar_name("00TRa.bms", *sounds) == "00TRa.lwf");
	TEST_EXPECT(sidecar_alternate_name("00TRa.bms", *sounds) == "00TRa.pwf");
	TEST_EXPECT(sidecar_alternate_name("00TRa.bms", *text).empty());

	// The names each reader opens, the dialog rows by the bank the header picks, and the file a
	// row needs beside it.
	{
		const SidecarNames own = sidecar_names("maps/00TRa.bms", *sounds);
		TEST_EXPECT(own.name == "00TRa.lwf" && own.alternate == "00TRa.pwf" && own.needs == "00TRa.dbf");
		const SidecarNames slotted = sidecar_names("00TRa.bms", *sounds, "talk.cnv");
		TEST_EXPECT(slotted.name == "talk.lwf" && slotted.alternate == "talk.pwf" && slotted.needs == "talk.dbf");
		TEST_EXPECT(sidecar_names("00TRa.bms", *dialog, "talk.cnv").name == "talk.dbf");
		const SidecarNames bin = sidecar_names("00TRa.bms", *text);
		TEST_EXPECT(bin.name == "00TRa.bin" && bin.alternate.empty() && bin.needs.empty());
	}
	// A row that needs another's file reads only where that file is [orig: DialogSystem_Init
	// @ 0x527648]; the row a file is to a mission, its name compared as the archives compare.
	{
		const std::set<std::string> project = {"00TRA.DBF", "00TRA.PWF", "00TRA.WAC"};
		const auto has = [&](const std::string &name) { return project.count(opennova::pff::normalized_logical_name(name)) != 0; };
		const auto none = [](const std::string &) { return false; };
		TEST_EXPECT(sidecar_reads(sidecar_names("00TRa.bms", *sounds), has));
		TEST_EXPECT(!sidecar_reads(sidecar_names("00TRa.bms", *sounds), none));
		TEST_EXPECT(sidecar_reads(sidecar_names("00TRa.bms", *script), none));
		TEST_EXPECT(sidecar_naming("00TRa.bms", "00tra.wac", has) == script);
		TEST_EXPECT(sidecar_naming("00TRa.bms", "00TRA.PWF", has) == sounds);
		TEST_EXPECT(sidecar_naming("00TRa.bms", "00TRa.pwf", none) == nullptr);
		TEST_EXPECT(sidecar_naming("00TRa.bms", "00TRa.dbf", none) == dialog);
		TEST_EXPECT(sidecar_naming("op.v2.bms", "op.wac", none) == script);
		TEST_EXPECT(sidecar_naming("00TRa.bms", "medmssn.bin", has) == nullptr);
		TEST_EXPECT(sidecar_naming("00TRa.bms", "01TR.wac", has) == nullptr);
	}
	return 0;
}
