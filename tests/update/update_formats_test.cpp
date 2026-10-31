// The updater's formats (engine/formats/update): the checksums, the INI dialect, the wizard and
// cc.bin's territory, the command file, SERVER.LST, 1ZLN and 1SFS, each read as UPDATE.EXE 2.1.4.2
// reads it and written back in canonical form (docs/update/updater-re.md §2, §5, §6, §9, §10).
// Synthetic inputs only: the wizard below has the shape JO:CA ships (CMD, URL, DIR, PRD,
// PRODUCT_LOGIN), which the CI install does not carry.

#include "common/test_expect.h"

#include <formats/update/command_file.h>
#include <formats/update/nlz.h>
#include <formats/update/server_list.h>
#include <formats/update/sfs.h>
#include <formats/update/update_crc.h>
#include <formats/update/update_ini.h>
#include <formats/update/update_wizard.h>

#include <base/io/crc32_mpeg2.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace opennova::update;

namespace {

std::vector<uint8_t> bytes(std::string_view text) {
	return std::vector<uint8_t>(text.begin(), text.end());
}

// The plain complemented CRC (CRC-32/BZIP2), the 2005 archive build's entry checksum.
uint32_t bzip2_crc(const std::vector<uint8_t> &data) {
	return ~opennova::io::crc32_mpeg2(data.data(), data.size());
}

int test_crcs() {
	const std::vector<uint8_t> check = bytes("123456789");
	// CRC-32/MPEG-2 and CRC-32/BZIP2 check values.
	TEST_EXPECT(command_file_crc(check.data(), check.size()) == 0x0376E6E7u);
	TEST_EXPECT(bzip2_crc(check) == 0xFC891918u);
	// The retail unroll feeds "1111" "2222" then the tail "3": the BZIP2 CRC of those bytes.
	TEST_EXPECT(file_crc(check.data(), check.size()) == bzip2_crc(bytes("111122223")));
	TEST_EXPECT(file_crc(check.data(), check.size()) == 0x7DD1E6FDu);
	// Under four bytes the defect does not show.
	const std::vector<uint8_t> one = bytes("a");
	TEST_EXPECT(file_crc(one.data(), one.size()) == bzip2_crc(one));
	// An empty command file reads as 0.
	TEST_EXPECT(command_file_crc(nullptr, 0) == 0);
	TEST_EXPECT(crc_as_signed(0xFC891918u) == -58124008);
	return 0;
}

int test_ini() {
	const UpdateIni ini("junk\r\n[HEADER]\r\nCMD = A.CMD ;comment\r\nlower=x\r\n[NEXT]\r\nCMD=B\r\n"
						"[Header2]\r\nZ=1");
	TEST_EXPECT(ini.line_count() == 8);
	TEST_EXPECT(ini.find_section("header") == 2u);
	TEST_EXPECT(ini.find_section("NEXT") == 5u);
	// "[Header2]" reads as the run "H", never HEADER2.
	TEST_EXPECT(!ini.find_section("HEADER2"));
	TEST_EXPECT(ini.find_section("H") == 7u);
	// The key's trailing and the value's leading spaces go, ';' ends the value.
	TEST_EXPECT(ini.value("HEADER", "cmd", 260) == std::string("A.CMD "));
	// A file's key is compared case-sensitively against the upper-cased query.
	TEST_EXPECT(!ini.value("HEADER", "LOWER", 260));
	// The search stops at the next section.
	TEST_EXPECT(ini.value("NEXT", "CMD", 260) == std::string("B"));
	TEST_EXPECT(!ini.value("HEADER", "Z", 260));
	// A last line without CR LF still reads; the buffer bounds what is kept.
	TEST_EXPECT(ini.value("H", "Z", 260) == std::string("1"));
	TEST_EXPECT(ini.value("HEADER", "CMD", 3) == std::string("A."));
	return 0;
}

int test_wizard() {
	const std::string text = "[HEADER]\r\nCMD=JOPTR.CMD\r\nURL=ftp.novaworld.com\r\n"
							 "DIR=/pub/updates/JOTR/qq\r\nPRD=1\r\nPRODUCT_LOGIN=JOTRqq\r\n\r\n";
	const UpdateWizard wizard = read_update_wizard(text);
	TEST_EXPECT(wizard.cmd == "JOPTR.CMD");
	TEST_EXPECT(wizard.url == "ftp.novaworld.com");
	TEST_EXPECT(wizard.url2.empty());
	TEST_EXPECT(wizard.product_login == "JOTRqq");
	TEST_EXPECT(wizard.dir == "/pub/updates/JOTR/qq");
	TEST_EXPECT(wizard.game.empty());

	// cc.bin "usqq0409us": territory "us" over placeholder "qq".
	const std::vector<uint8_t> cc = bytes("usqq0409us");
	const Territory territory = territory_from_cc_bin(cc.data(), cc.size());
	const UpdateWizard local = with_territory(wizard, territory);
	TEST_EXPECT(local.product_login == "JOTRus");
	TEST_EXPECT(local.dir == "/pub/updates/JOTR/us");
	// Only the first placeholder, lower case searched first, the case kept.
	TEST_EXPECT(substitute_territory("aQQbqqcqq", territory) == "aQQbuscqq");
	TEST_EXPECT(substitute_territory("XQQ", territory) == "XUS");
	// No cc.bin: "US" over "QQ".
	TEST_EXPECT(substitute_territory("JOEqq", territory_from_cc_bin(nullptr, 0)) == "JOEus");
	// A short cc.bin replaces only what it holds.
	const std::vector<uint8_t> short_cc = bytes("de");
	TEST_EXPECT(
			substitute_territory("JOEqq", territory_from_cc_bin(short_cc.data(), 2)) == "JOEde");

	// Written in canonical form and read back.
	UpdateWizard ours;
	ours.cmd = "ONJOPTR.CMD";
	ours.url = "update.example.net";
	ours.product_login = "ONJOqq";
	ours.drive = "C";
	const std::string written = write_update_wizard(ours);
	TEST_EXPECT(written ==
			"[HEADER]\r\nCMD=ONJOPTR.CMD\r\nURL=update.example.net\r\n"
			"PRODUCT_LOGIN=ONJOqq\r\nDRIVE=C\r\n");
	const UpdateWizard back = read_update_wizard(written);
	TEST_EXPECT(back.cmd == ours.cmd && back.url == ours.url && back.drive == "C");
	// GAME, ours, goes last, after every key retail reads, and reads back.
	ours.game = "jo";
	const std::string with_game = write_update_wizard(ours);
	TEST_EXPECT(with_game ==
			"[HEADER]\r\nCMD=ONJOPTR.CMD\r\nURL=update.example.net\r\n"
			"PRODUCT_LOGIN=ONJOqq\r\nDRIVE=C\r\nGAME=jo\r\n");
	TEST_EXPECT(read_update_wizard(with_game).game == "jo");
	ours.game.clear();
	ours.dlg = "a;b";
	TEST_EXPECT(write_update_wizard(ours).empty());
	TEST_EXPECT(kUpdateInfText == "[UPDATE]\r\nDAT=0\r\n");
	return 0;
}

int test_command_file() {
	const std::string text = "[HEADER]\r\nEXE=Jointops.exe\r\nPRODUCT=Joint Operations\r\n"
							 "VERSION=0.2.0\r\n\r\n[COMMAND]\r\n"
							 "UPD=version.txt,1700000000,52,-123\r\n"
							 "bogus=thing\r\n"
							 "WIZ=update.wiz,1700000001\r\n" // no size: skipped
							 "TARGET=resource.pff\r\n"
							 "copy=wpn.def 1700000002 , 900,  77\r\n"
							 "REMOVE=old.def\r\n"
							 "[LATER]\r\n"
							 "RESEXE=late.exe,5,6\r\n"; // later sections are just lines
	const std::optional<CommandFile> file = read_command_file(text);
	TEST_EXPECT(file.has_value());
	TEST_EXPECT(file->exe == "Jointops.exe" && file->product == "Joint Operations");
	TEST_EXPECT(file->version == "0.2.0");
	TEST_EXPECT(file->rows.size() == 5);
	TEST_EXPECT(file->rows[0].type == CommandType::Loose && file->rows[0].name == "version.txt");
	TEST_EXPECT(file->rows[0].time == 1700000000 && file->rows[0].size == 52);
	TEST_EXPECT(file->rows[0].crc == -123);
	TEST_EXPECT(file->rows[1].type == CommandType::Target && file->rows[1].name == "resource.pff");
	TEST_EXPECT(file->rows[2].type == CommandType::Copy && file->rows[2].name == "wpn.def");
	TEST_EXPECT(file->rows[2].time == 1700000002 && file->rows[2].size == 900);
	TEST_EXPECT(file->rows[2].crc == 77);
	TEST_EXPECT(file->rows[3].type == CommandType::Remove && file->rows[3].name == "old.def");
	TEST_EXPECT(!file->rows[3].crc);
	TEST_EXPECT(file->rows[4].key == "RESEXE" && file->rows[4].size == 6);
	TEST_EXPECT(!read_command_file("[COMMAND]\r\nUPD=a,1,2\r\n"));

	// The writer's canonical form reads back to the same model.
	CommandFile ours;
	ours.exe = "opennova.exe";
	ours.product = "OpenNova";
	ours.version = "0.0.20";
	ours.rows.push_back({ "UPD", CommandType::Loose, "version.txt", 1700000000, 20,
			crc_as_signed(0xFFFFFFFFu) });
	ours.rows.push_back({ "TARGET", CommandType::Target, "resource.pff", 0, 0, std::nullopt });
	ours.rows.push_back({ "COPY", CommandType::Copy, "menu.mnu", 1700000005, 300, 12345 });
	ours.rows.push_back({ "REMOVE", CommandType::Remove, "gone.mnu", 0, 0, std::nullopt });
	const std::string written = write_command_file(ours);
	TEST_EXPECT(written ==
			"[HEADER]\r\nEXE=opennova.exe\r\nPRODUCT=OpenNova\r\nVERSION=0.0.20\r\n"
			"\r\n[COMMAND]\r\nUPD=version.txt,1700000000,20,-1\r\nTARGET=resource.pff\r\n"
			"COPY=menu.mnu,1700000005,300,12345\r\nREMOVE=gone.mnu\r\n");
	const std::optional<CommandFile> back = read_command_file(written);
	TEST_EXPECT(back && back->rows.size() == 4 && back->rows[0].crc == -1);
	TEST_EXPECT(write_command_file(*back) == written);
	ours.rows.push_back({ "COPY", CommandType::Copy, "has space", 1, 2, std::nullopt });
	TEST_EXPECT(write_command_file(ours).empty());
	return 0;
}

// Each refusal the writer makes, named with what UPDATE.EXE would read, and none it would read back:
// a row's line split at " ,\r\n" after its key's '=' and kept to 255 characters [orig:
// CmdFile_ReadCommandRow @ 0x404f60 (UPDATE.EXE)], a header value ending at ';' on a line kept to
// 127 [orig: IniFile_GetValue @ 0x403b90 (UPDATE.EXE)].
int test_command_file_refusals() {
	using R = CommandRefusal;
	const auto row = [](const char *key, CommandType type, std::string name) {
		return CommandRow{ key, type, std::move(name), 1700000000, 15132, -42 };
	};
	const auto refused_by = [](const CommandFile &file, R refusal) {
		return command_file_problem(file).refusal == refusal && write_command_file(file).empty();
	};
	CommandFile file;
	file.product = "Joint Operations: Tactical Assault Coalition";
	file.version = "0.0.1";
	file.rows.push_back({ "TARGET", CommandType::Target, "RevX02.pff", 0, 0, std::nullopt });
	TEST_EXPECT(command_file_problem(file).ok() && !write_command_file(file).empty());

	// A space or a comma: the name ends there, the rest read as its fields.
	TEST_EXPECT(command_name_refusal("Audio Track.wav") == R::NameSeparator);
	TEST_EXPECT(command_name_refusal("a,b.wav") == R::NameSeparator);
	CommandFile spaced = file;
	spaced.rows.push_back(row("COPY", CommandType::Copy, "Audio Track.wav"));
	TEST_EXPECT(refused_by(spaced, R::NameSeparator));
	const CommandFileProblem problem = command_file_problem(spaced);
	TEST_EXPECT(!problem.header && problem.key == "COPY" && problem.text == "Audio Track.wav" &&
			problem.target == "RevX02.pff");
	const std::string words = command_file_problem_words(problem);
	TEST_EXPECT(words.rfind("the COPY row \"Audio Track.wav\" (in RevX02.pff) has a space: ", 0) == 0 &&
			words.find("reads the name as \"Audio\"") != std::string::npos &&
			words.find("[orig: CmdFile_ReadCommandRow @ 0x404f60 (UPDATE.EXE)") != std::string::npos);
	// As retail reads it: the name its first field, the rest its time and size.
	const std::optional<CommandFile> misread = read_command_file(
			"[HEADER]\r\n\r\n[COMMAND]\r\nCOPY=Audio Track.wav,1700000000,15132\r\n");
	TEST_EXPECT(misread && misread->rows.size() == 1 && misread->rows[0].name == "Audio" &&
			misread->rows[0].time == 0 && misread->rows[0].size == 1700000000);
	TEST_EXPECT(command_refusal_words(R::NameSeparator, "a,b.wav").rfind("has a comma: ", 0) == 0);
	CommandFile loose;
	loose.rows.push_back(row("UPD", CommandType::Loose, "my,file.txt"));
	TEST_EXPECT(refused_by(loose, R::NameSeparator) &&
			command_file_problem_words(command_file_problem(loose))
							.rfind("the UPD row \"my,file.txt\" has a comma: ", 0) == 0);

	// None: the reader skips a row with no name.
	TEST_EXPECT(command_name_refusal("") == R::NameEmpty);
	CommandFile unnamed = file;
	unnamed.rows.push_back(row("REMOVE", CommandType::Remove, ""));
	TEST_EXPECT(refused_by(unnamed, R::NameEmpty));

	// A CR or an LF ends the name.
	TEST_EXPECT(command_name_refusal("a\rb") == R::NameLineBreak &&
			command_name_refusal("a\nb") == R::NameLineBreak);
	CommandFile broken = file;
	broken.rows.push_back(row("COPY", CommandType::Copy, "a\nb"));
	TEST_EXPECT(refused_by(broken, R::NameLineBreak));

	// A line of 255 characters is read whole, one of 256 cut: "REMOVE=" and a name of 248, then 249.
	CommandFile longest = file;
	longest.rows.push_back(row("REMOVE", CommandType::Remove, std::string(248, 'n')));
	TEST_EXPECT(command_file_problem(longest).ok());
	const std::optional<CommandFile> kept = read_command_file(write_command_file(longest));
	TEST_EXPECT(kept && kept->rows.size() == 2 && kept->rows[1].name == std::string(248, 'n'));
	CommandFile too_long = file;
	too_long.rows.push_back(row("REMOVE", CommandType::Remove, std::string(249, 'n')));
	TEST_EXPECT(refused_by(too_long, R::RowTooLong));
	CommandFile long_name = file; // past UpdateCommand::name[260] its line is past 255 first
	long_name.rows.push_back(row("TARGET", CommandType::Target, std::string(kCommandNameBytes, 'n')));
	TEST_EXPECT(refused_by(long_name, R::RowTooLong));

	// A key the table lacks, or one of another kind than the row's.
	CommandFile unknown = file;
	unknown.rows.push_back(row("PATCH", CommandType::Copy, "a.def"));
	TEST_EXPECT(refused_by(unknown, R::RowKey));
	CommandFile mismatched = file;
	mismatched.rows.push_back(row("COPY", CommandType::Loose, "a.def"));
	TEST_EXPECT(refused_by(mismatched, R::RowKey));

	// A '=' inside a name is kept: the fields' strtok does not split at it.
	CommandFile equals = file;
	equals.rows.push_back(row("COPY", CommandType::Copy, "a=b.def"));
	equals.rows.push_back(row("COPY", CommandType::Copy, "=lead.def"));
	TEST_EXPECT(command_file_problem(equals).ok());
	const std::optional<CommandFile> equals_back = read_command_file(write_command_file(equals));
	TEST_EXPECT(equals_back && equals_back->rows.size() == 3 && equals_back->rows[1].name == "a=b.def" &&
			equals_back->rows[2].name == "=lead.def" && equals_back->rows[2].size == 15132);

	// A header value: a CR or an LF, a ';' (the value ends there), a line past 127.
	CommandFile header = file;
	header.product = "A\r\nB";
	TEST_EXPECT(refused_by(header, R::HeaderLineBreak));
	header.product = "Joint Operations; TAC";
	TEST_EXPECT(refused_by(header, R::HeaderSemicolon));
	const CommandFileProblem semicolon = command_file_problem(header);
	TEST_EXPECT(semicolon.header && semicolon.key == "PRODUCT" &&
			command_file_problem_words(semicolon)
							.rfind("the PRODUCT value \"Joint Operations; TAC\" has a ';', where "
								   "UPDATE.EXE's header reader ends the value, so it reads "
								   "\"Joint Operations\"",
									0) == 0);
	header.product = std::string(kIniKeyLineBytes - 1 - 8, 'P'); // PRODUCT= and 119: 127, read whole
	TEST_EXPECT(command_file_problem(header).ok());
	const std::optional<CommandFile> header_back = read_command_file(write_command_file(header));
	TEST_EXPECT(header_back && header_back->product == header.product);
	header.product += "P";
	TEST_EXPECT(refused_by(header, R::HeaderTooLong));
	TEST_EXPECT(command_header_refusal("EXE", "") == R::None);
	CommandFile version = file;
	version.version = "1;2";
	TEST_EXPECT(refused_by(version, R::HeaderSemicolon) && command_file_problem(version).key == "VERSION");
	return 0;
}

int test_server_list() {
	const std::vector<std::string> hosts =
			read_server_list("  ftp.novaworld.com  extra\r\n\r\n"
							 "\tupdate2.example.net\r\nlast.example.net");
	TEST_EXPECT(hosts.size() == 3);
	TEST_EXPECT(hosts[0] == "ftp.novaworld.com" && hosts[1] == "update2.example.net");
	TEST_EXPECT(hosts[2] == "last.example.net");
	TEST_EXPECT(write_server_list({ "a.example", "b.example" }) == "a.example\r\nb.example\r\n");
	TEST_EXPECT(write_server_list({ "bad host" }).empty());
	return 0;
}

int test_nlz() {
	std::vector<uint8_t> content;
	for (int i = 0; i < 5000; ++i)
		content.push_back(uint8_t((i * 7) % 13));
	const std::vector<uint8_t> packed = nlz_encode(content.data(), content.size());
	NlzHeader header;
	TEST_EXPECT(nlz_read_header(packed.data(), packed.size(), header));
	TEST_EXPECT(header.data_offset == 16 && header.raw_size == content.size());
	TEST_EXPECT(header.packed_size + kNlzHeaderBytes == packed.size());
	TEST_EXPECT(std::memcmp(packed.data(), "1ZLN", 4) == 0);
	std::vector<uint8_t> out;
	TEST_EXPECT(nlz_decode(packed.data(), packed.size(), out) == NlzDecode::Inflated);
	TEST_EXPECT(out == content);
	// A file without the magic is its own content.
	const std::vector<uint8_t> raw = bytes("plain text file");
	TEST_EXPECT(nlz_decode(raw.data(), raw.size(), out) == NlzDecode::Raw && out == raw);
	// Under four bytes, a stream past the end, a bad stream.
	TEST_EXPECT(nlz_decode(raw.data(), 3, out) == NlzDecode::Corrupt);
	std::vector<uint8_t> cut(packed.begin(), packed.end() - 5);
	TEST_EXPECT(nlz_decode(cut.data(), cut.size(), out) == NlzDecode::Corrupt);
	std::vector<uint8_t> broken = packed;
	broken[20] ^= 0xFF;
	broken[21] ^= 0xFF;
	TEST_EXPECT(nlz_decode(broken.data(), broken.size(), out) == NlzDecode::Corrupt);
	// An empty file packs and unpacks.
	const std::vector<uint8_t> empty_packed = nlz_encode(nullptr, 0);
	TEST_EXPECT(nlz_decode(empty_packed.data(), empty_packed.size(), out) == NlzDecode::Inflated);
	TEST_EXPECT(out.empty());
	return 0;
}

int test_sfs() {
	SfsPackage package;
	package.product = "Joint Operations Typhoon Rising";
	package.sections.push_back(
			{ "", { { "JOPTR.CMD", bytes("[HEADER]\r\n") }, { "version.txt", bytes("v") } } });
	package.sections.push_back({ "jox01", { { "JOEPTR.CMD", bytes("[HEADER]\r\n") } } });
	const std::vector<uint8_t> written = sfs_write(package);
	TEST_EXPECT(!written.empty());
	TEST_EXPECT(sfs_has_trailer(written.data(), written.size()));
	SfsPackage back;
	std::string error;
	TEST_EXPECT(sfs_read(written.data(), written.size(), back, error));
	TEST_EXPECT(back.product == package.product && back.sections.size() == 2);
	TEST_EXPECT(back.sections[0].entries.size() == 2 &&
			back.sections[0].entries[1].name == "version.txt");
	TEST_EXPECT(back.sections[1].expansion == "jox01");
	TEST_EXPECT(back.sections[1].entries[0].data == bytes("[HEADER]\r\n"));
	// An executable before the package changes nothing: the trailer is read from the end.
	std::vector<uint8_t> exe = bytes("MZ fake updater executable bytes");
	exe.insert(exe.end(), written.begin(), written.end());
	SfsPackage from_exe;
	TEST_EXPECT(sfs_read(exe.data(), exe.size(), from_exe, error));
	TEST_EXPECT(sfs_write(from_exe) == written);
	// The layout: the trailer's offset names the first entry.
	const int32_t offset = int32_t(uint32_t(written[written.size() - 8]) |
			uint32_t(written[written.size() - 7]) << 8 |
			uint32_t(written[written.size() - 6]) << 16 |
			uint32_t(written[written.size() - 5]) << 24);
	TEST_EXPECT(offset == -int32_t(written.size()));
	TEST_EXPECT(std::memcmp(written.data(), "JOPTR.CMD", 9) == 0);
	// Names that do not fit, a break name, a base section after the first.
	SfsPackage bad = package;
	bad.sections[0].entries[0].name = "sixteen_chars___";
	TEST_EXPECT(sfs_write(bad).empty());
	bad = package;
	bad.sections[0].entries[0].name = "PACKAGEBREAK";
	TEST_EXPECT(sfs_write(bad).empty());
	bad = package;
	bad.sections.push_back({ "", {} });
	TEST_EXPECT(sfs_write(bad).empty());
	// Not a package; an entry running into the trailer.
	TEST_EXPECT(!sfs_read(exe.data(), 10, back, error));
	std::vector<uint8_t> overrun = written;
	overrun[16] = 0xFF; // the first entry's size
	overrun[17] = 0xFF;
	TEST_EXPECT(!sfs_read(overrun.data(), overrun.size(), back, error));
	// Located in a file, never read whole (a whole game's package): the sections and entries
	// sfs_read finds, each entry's bytes at its span, the executable before them included.
	std::FILE *file = std::tmpfile();
	TEST_EXPECT(file != nullptr && std::fwrite(exe.data(), 1, exe.size(), file) == exe.size());
	SfsLayout layout;
	TEST_EXPECT(sfs_locate(file, layout, error));
	TEST_EXPECT(layout.product == package.product && layout.sections.size() == 2 &&
			layout.sections[0].entries.size() == 2 && layout.sections[1].expansion == "jox01" &&
			layout.sections[1].entries.size() == 1);
	for (size_t s = 0; s < layout.sections.size(); ++s)
		for (size_t e = 0; e < layout.sections[s].entries.size(); ++e) {
			const SfsEntrySpan &span = layout.sections[s].entries[e];
			const SfsEntry &entry = from_exe.sections[s].entries[e];
			TEST_EXPECT(span.name == entry.name && span.size == entry.data.size() &&
					span.offset + span.size <= exe.size() &&
					std::equal(entry.data.begin(), entry.data.end(),
							exe.begin() + static_cast<std::ptrdiff_t>(span.offset)));
		}
	std::fclose(file);
	file = std::tmpfile();
	TEST_EXPECT(file != nullptr && std::fwrite(overrun.data(), 1, overrun.size(), file) == overrun.size());
	TEST_EXPECT(!sfs_locate(file, layout, error));
	std::fclose(file);
	return 0;
}

} // namespace

int main() {
	if (int rc = test_crcs())
		return rc;
	if (int rc = test_ini())
		return rc;
	if (int rc = test_wizard())
		return rc;
	if (int rc = test_command_file())
		return rc;
	if (int rc = test_command_file_refusals())
		return rc;
	if (int rc = test_server_list())
		return rc;
	if (int rc = test_nlz())
		return rc;
	if (int rc = test_sfs())
		return rc;
	std::printf("update_formats: ok\n");
	return 0;
}
