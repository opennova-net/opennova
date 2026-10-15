// pff_apply_edits (engine/formats/pff), the archive edit an update's COPY and REMOVE rows make
// in place of retail's pack.exe (D-UPD-1, ADR 0061 d8): adds stored plain with the row's time
// and the updater's file CRC as their checksum, removes dropped, every other entry kept as
// stored, nothing written when nothing changes. The retail leg checks the checksum convention
// over every entry of the JO:CA install's localres.pff and jox01L.pff (docs/update/updater-re.md
// §10: the 2005 build's entries carry the plain CRC-32/BZIP2, the one patched entry, localres's
// main.mnu of 2009-07-24, the file CRC with the unroll defect), and edits a copy of jox01L.pff.

#include "common/retail_paths.h"
#include "common/temp_dir.h"
#include "common/test_expect.h"

#include <formats/pff/pff.h>
#include <formats/update/update_crc.h>

#include <base/io/crc32_mpeg2.h>

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

using namespace opennova::pff;

namespace {

std::vector<uint8_t> bytes(std::string_view text) {
	return std::vector<uint8_t>(text.begin(), text.end());
}

std::vector<uint8_t> entry_bytes(const PffArchive &archive, const char *name) {
	const PffEntry *entry = pff_find(&archive, name);
	if (entry == nullptr)
		return {};
	std::vector<uint8_t> out(entry->size ? entry->size : 1);
	if (pff_extract(&archive, entry, out.data(), out.size()) != 0)
		return {};
	out.resize(entry->size);
	return out;
}

int test_edits() {
	test_temp::TempDir dir("pff_apply_edits");
	const std::string path = dir.file("edit.pff");

	// No archive yet: the adds create one.
	bool rewritten = false;
	TEST_EXPECT(pff_apply_edits(path.c_str(),
						{ { "Keep.def", bytes("keep"), 1100000000 },
								{ "Swap.mnu", bytes("old"), 1100000001 } },
						{}, &rewritten) == PFF_WRITE_OK);
	TEST_EXPECT(rewritten);

	// Replace one, remove one that is missing and one that is there, add one.
	TEST_EXPECT(pff_apply_edits(path.c_str(),
						{ { "SWAP.MNU", bytes("new bytes"), 1700000000 },
								{ "Fresh.txt", bytes("fresh"), 1700000001 } },
						{ "absent.x", "keep.def" }, &rewritten) == PFF_WRITE_OK);
	TEST_EXPECT(rewritten);
	PffArchive archive{};
	TEST_EXPECT(pff_open(&archive, path.c_str()) == 0);
	TEST_EXPECT(archive.entry_count == 2);
	TEST_EXPECT(pff_find(&archive, "keep.def") == nullptr);
	const PffEntry *swapped = pff_find(&archive, "swap.mnu");
	TEST_EXPECT(swapped != nullptr);
	if (swapped == nullptr)
		return 1;
	TEST_EXPECT(swapped->flags == 0 && swapped->timestamp == 1700000000u);
	const std::vector<uint8_t> swapped_bytes = bytes("new bytes");
	TEST_EXPECT(swapped->checksum ==
			opennova::update::file_crc(swapped_bytes.data(), swapped_bytes.size()));
	TEST_EXPECT(pff_entry_stored_name(*swapped) == "SWAP.MNU");
	TEST_EXPECT(entry_bytes(archive, "fresh.txt") == bytes("fresh"));
	pff_close(&archive);

	// The same edit again changes nothing and writes nothing.
	const auto before = std::filesystem::last_write_time(path);
	TEST_EXPECT(pff_apply_edits(path.c_str(), { { "SWAP.MNU", bytes("new bytes"), 1700000000 } },
						{ "absent.x" }, &rewritten) == PFF_WRITE_OK);
	TEST_EXPECT(!rewritten);
	TEST_EXPECT(std::filesystem::last_write_time(path) == before);
	// Only the time differs: an entry the precheck would fetch again is rewritten.
	TEST_EXPECT(pff_apply_edits(path.c_str(), { { "SWAP.MNU", bytes("new bytes"), 1700000999 } },
						{}, &rewritten) == PFF_WRITE_OK);
	TEST_EXPECT(rewritten);
	// A remove and an add of one name: the remove runs first, the add puts it back.
	TEST_EXPECT(pff_apply_edits(path.c_str(), { { "fresh.txt", bytes("again"), 1700000002 } },
						{ "FRESH.TXT" }, &rewritten) == PFF_WRITE_OK);
	TEST_EXPECT(pff_open(&archive, path.c_str()) == 0);
	TEST_EXPECT(entry_bytes(archive, "fresh.txt") == bytes("again"));
	pff_close(&archive);
	// A name past the field is refused and the archive left as it was.
	TEST_EXPECT(pff_apply_edits(path.c_str(), { { "seventeen_chars__", bytes("x"), 1 } }, {},
						&rewritten) == PFF_WRITE_ERR_NAME_LEN);
	TEST_EXPECT(!std::filesystem::exists(path + ".new"));
	return 0;
}

// Every entry's checksum word is the plain CRC-32/BZIP2 of its stored bytes (the 2005 build's),
// or, for an entry a patch wrote, the updater's file CRC; `patched` collects the latter's names.
int check_retail_checksums(
		const std::string &path, uint32_t &checked, std::vector<std::string> &patched) {
	PffArchive archive{};
	TEST_EXPECT(pff_open(&archive, path.c_str()) == 0);
	std::vector<uint8_t> stored;
	for (uint32_t i = 0; i < archive.entry_count; ++i) {
		const PffEntry &entry = archive.entries[i];
		stored.resize(entry.size ? entry.size : 1);
		TEST_EXPECT(pff_extract_raw(&archive, &entry, stored.data(), entry.size) == 0);
		const uint32_t plain = ~opennova::io::crc32_mpeg2(stored.data(), entry.size);
		const uint32_t defect = opennova::update::file_crc(stored.data(), entry.size);
		if (entry.checksum != plain && entry.checksum != defect) {
			std::fprintf(stderr, "%s: %s checksum %08x\n", path.c_str(),
					pff_entry_stored_name(entry).c_str(), entry.checksum);
			pff_close(&archive);
			return 1;
		}
		if (entry.checksum != plain)
			patched.push_back(pff_entry_stored_name(entry));
		++checked;
	}
	pff_close(&archive);
	return 0;
}

int retail_leg() {
	const std::string install = retail::install();
	if (install.empty())
		return retail::skip_leg("OPENNOVA_JO_DIR (the packed JO:CA install)");
	const std::string localres = retail::join(install, "localres.pff");
	const std::string jox01l =
			retail::join(retail::join(retail::join(install, "expansion"), "jox01"), "jox01L.pff");
	if (!retail::file_exists(localres) || !retail::file_exists(jox01l))
		return retail::skip_leg("localres.pff and expansion/jox01/jox01L.pff in OPENNOVA_JO_DIR");
	uint32_t checked = 0;
	std::vector<std::string> patched;
	if (check_retail_checksums(localres, checked, patched) != 0)
		return 1;
	if (check_retail_checksums(jox01l, checked, patched) != 0)
		return 1;
	// The one entry a patch wrote, localres.pff's main.mnu (2009-07-24).
	TEST_EXPECT(patched == std::vector<std::string>{ "main.mnu" });

	// An edit of a copy keeps every other retail entry's stored bytes, time and checksum.
	test_temp::TempDir dir("pff_apply_edits_retail");
	const std::string copy = dir.file("jox01L.pff");
	std::filesystem::copy_file(opennova::io::os_path(jox01l), opennova::io::os_path(copy));
	PffArchive source{};
	TEST_EXPECT(pff_open(&source, jox01l.c_str()) == 0);
	TEST_EXPECT(source.entry_count > 2);
	const std::string removed = pff_entry_stored_name(source.entries[0]);
	bool rewritten = false;
	TEST_EXPECT(pff_apply_edits(copy.c_str(), { { "opennova.txt", bytes("added"), 1700000000 } },
						{ removed }, &rewritten) == PFF_WRITE_OK &&
			rewritten);
	PffArchive edited{};
	TEST_EXPECT(pff_open(&edited, copy.c_str()) == 0);
	TEST_EXPECT(edited.entry_count == source.entry_count);
	for (uint32_t i = 1; i < source.entry_count; ++i) {
		const PffEntry &before = source.entries[i];
		const std::string name = pff_entry_stored_name(before);
		const PffEntry *after = pff_find(&edited, name.c_str());
		TEST_EXPECT(after != nullptr);
		if (after == nullptr)
			break;
		TEST_EXPECT(after->size == before.size && after->flags == before.flags &&
				after->timestamp == before.timestamp && after->checksum == before.checksum);
	}
	TEST_EXPECT(entry_bytes(edited, "opennova.txt") == bytes("added"));
	pff_close(&edited);
	pff_close(&source);
	std::printf("retail leg: %u entries checked (main.mnu the one patched); jox01L.pff edited\n",
			checked);
	return 0;
}

} // namespace

int main(int argc, char **argv) {
	retail::configure_mixed(argc, argv);
	if (int rc = test_edits())
		return rc;
	if (int rc = retail_leg())
		return rc;
	std::printf("update_pff: ok\n");
	return 0;
}
