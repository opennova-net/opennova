// The bank as a model (formats/sbf/sbf.h: SbfFile, sbf_read_bank, sbf_write_bank): the minted
// synthetic bank (fixtures/sbf/synth_gamemus.sbf, tests/fixtures/minimal_sbf_gen.cpp) read into
// the model (its tails 0x80, its reserved pair gamemus's) and written back byte for byte; a bank
// built with no source bytes written as the shipped banks are laid out (each chunk a whole block,
// its tail the previous chunk's bytes or zeros, one reserved pair); an edit (a stream renamed, one
// dropped, one encoded anew) written from the fields alone and read back; each stream's decode the
// raw entry's, a stream stopping at a chunk whose shift is past 7; what the reader refuses (entries
// sharing bytes among them) and what it notes of a layout the writer lays out otherwise; what the
// writer refuses. The retail banks' byte sweep is sbf_jo_install_sweep.
#include <formats/sbf/sbf.h>

#include "common/file_io.h"
#include "common/test_expect.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace opennova::sbf;

namespace {

void put_u32(std::vector<uint8_t> &bytes, size_t at, uint32_t value) {
	for (int b = 0; b < 4; ++b) bytes[at + size_t(b)] = uint8_t(value >> (8 * b));
}

uint32_t get_u32(const std::vector<uint8_t> &bytes, size_t at) {
	uint32_t value = 0;
	std::memcpy(&value, &bytes[at], 4);
	return value;
}

// A bank built from fields alone, as a shipped bank is laid out: a stream of two chunks (one full, one
// of 100 bytes) and a stream of one 10-byte chunk, under menumus.sbf's reserved pair.
int test_from_scratch() {
	SbfFile bank;
	bank.chunk_reserved_a = 0x1A;
	bank.chunk_reserved_b = 0x01;
	SbfStream two;
	two.name = "TWO";
	SbfChunk full;
	full.scale_a = 3;
	full.scale_b = 4;
	for (int i = 0; i < SBF_CHUNK_AUDIO; ++i) full.audio.push_back(uint8_t(i * 7));
	SbfChunk part;
	part.audio.assign(100, 0x55);
	two.chunks = {full, part};
	SbfStream one;
	one.name = "ONE";
	SbfChunk small;
	small.audio.assign(10, 0x90);
	one.chunks = {small};
	bank.streams = {two, one};
	std::vector<uint8_t> written;
	std::string error;
	TEST_EXPECT(sbf_write_bank(bank, written, error));
	const size_t index_end = size_t(SBF_HEADER_SIZE) + 2 * size_t(SBF_ENTRY_SIZE);
	TEST_EXPECT(written.size() == index_end + 3 * size_t(SBF_CHUNK_TOTAL));
	if (written.size() != index_end + 3 * size_t(SBF_CHUNK_TOTAL)) return 1;
	// The index: each stream after the one before, whole blocks.
	TEST_EXPECT(get_u32(written, SBF_HEADER_SIZE + 16) == index_end &&
	            get_u32(written, SBF_HEADER_SIZE + 20) == 2u * SBF_CHUNK_TOTAL &&
	            get_u32(written, SBF_HEADER_SIZE + SBF_ENTRY_SIZE + 16) == index_end + 2 * SBF_CHUNK_TOTAL);
	// Every chunk header: its valid count, its shifts, the bank's reserved pair.
	const size_t first = index_end, second = index_end + SBF_CHUNK_TOTAL, third = index_end + 2 * SBF_CHUNK_TOTAL;
	TEST_EXPECT(get_u32(written, first) == uint32_t(SBF_CHUNK_AUDIO) && written[first + 4] == 3 && written[first + 5] == 4);
	TEST_EXPECT(get_u32(written, second) == 100 && get_u32(written, third) == 10);
	for (const size_t at : {first, second, third}) TEST_EXPECT(written[at + 6] == 0x1A && written[at + 7] == 0x01);
	// The tails, by the Residue rule: the previous chunk's bytes at the same offsets, zeros under a
	// stream's first chunk.
	bool residue = true;
	for (int i = 100; i < SBF_CHUNK_AUDIO; ++i) residue = residue && written[second + 8 + size_t(i)] == uint8_t(i * 7);
	bool zeros = true;
	for (int i = 10; i < SBF_CHUNK_AUDIO; ++i) zeros = zeros && written[third + 8 + size_t(i)] == 0;
	TEST_EXPECT(residue && zeros);
	// Read back: the same fields, the rule and the pair found, nothing laid out otherwise.
	SbfFile again;
	SbfFileLayout layout;
	TEST_EXPECT(sbf_read_bank(written.data(), written.size(), again, error, &layout));
	TEST_EXPECT(layout.packed && layout.trailing_bytes == 0 && layout.reserved_other == 0 && layout.tails_other == 0 &&
	            layout.short_chunks == 0);
	TEST_EXPECT(again.tail == SbfTail::Residue && again.chunk_reserved_a == 0x1A && again.chunk_reserved_b == 0x01);
	TEST_EXPECT(again.streams.size() == 2 && again.streams[0].chunks.size() == 2 &&
	            again.streams[0].chunks[0].audio == full.audio && again.streams[0].chunks[1].audio == part.audio &&
	            again.streams[1].chunks[0].audio == small.audio);
	// The same bank under the Silence rule: every tail 0x80.
	bank.tail = SbfTail::Silence;
	TEST_EXPECT(sbf_write_bank(bank, written, error));
	bool silent = true;
	for (int i = 100; i < SBF_CHUNK_AUDIO; ++i) silent = silent && written[second + 8 + size_t(i)] == 0x80;
	for (int i = 10; i < SBF_CHUNK_AUDIO; ++i) silent = silent && written[third + 8 + size_t(i)] == 0x80;
	TEST_EXPECT(silent);
	TEST_EXPECT(sbf_read_bank(written.data(), written.size(), again, error, &layout) && again.tail == SbfTail::Silence &&
	            layout.tails_other == 0);
	// A name of 16 bytes fills the index's with no terminator, and reads back whole.
	bank.streams[0].name = "SIXTEEN_CHARS_XX";
	TEST_EXPECT(sbf_write_bank(bank, written, error));
	TEST_EXPECT(sbf_read_bank(written.data(), written.size(), again, error, &layout) &&
	            again.streams[0].name == "SIXTEEN_CHARS_XX" && layout.names_with_tails == 0);
	// A stream stops at a chunk whose shift is past 7.
	SbfStream stopped = two;
	stopped.chunks[1].scale_a = 8;
	TEST_EXPECT(sbf_decode_stream(stopped).size() == size_t(SBF_CHUNK_AUDIO));
	return 0;
}

} // namespace

int main() {
	if (test_from_scratch() != 0) return 1;
	std::vector<uint8_t> bytes;
	TEST_EXPECT(test_io::read_file(std::string(SBF_FIXTURE_DIR) + "/synth_gamemus.sbf", bytes));
	if (test_io::is_lfs_pointer(bytes)) {
		std::printf("[skip] synth_gamemus.sbf is an unpulled LFS pointer\n");
		return 0;
	}

	// The minted bank through the model and back, byte for byte: its tails 0x80, its reserved pair
	// gamemus's.
	SbfFile bank;
	SbfFileLayout layout;
	std::string error;
	TEST_EXPECT(sbf_read_bank(bytes.data(), bytes.size(), bank, error, &layout));
	TEST_EXPECT(layout.packed && layout.trailing_bytes == 0 && layout.names_with_tails == 0 && layout.reserved_other == 0 &&
	            layout.tails_other == 0 && layout.short_chunks == 0);
	TEST_EXPECT(bank.version == SBF_VERSION_DEFAULT && bank.flags == SBF_FLAGS_BYTE_PAIRED_STEREO);
	TEST_EXPECT(bank.tail == SbfTail::Silence && bank.chunk_reserved_a == 0xFA && bank.chunk_reserved_b == 0x00);
	TEST_EXPECT(bank.streams.size() == 13 && bank.streams[0].name == "SILENCE" && bank.streams[1].name == "TONE01");
	TEST_EXPECT(bank.streams[0].chunks.size() == 1 && bank.streams[0].chunks[0].audio.size() == 2216);
	TEST_EXPECT(bank.streams[1].chunks.size() == 3 && bank.streams[1].chunks[2].audio.size() == 1000);
	TEST_EXPECT(bank.streams[1].chunks[0].audio.size() == size_t(SBF_CHUNK_AUDIO));
	std::vector<uint8_t> written;
	TEST_EXPECT(sbf_write_bank(bank, written, error));
	TEST_EXPECT(written == bytes);

	// Each stream decodes as its raw entry does.
	SbfArchive arc;
	TEST_EXPECT(sbf_open_memory(&arc, bytes.data(), bytes.size()) == 0);
	for (uint32_t i = 0; i < arc.header.entry_count; ++i) {
		const SbfRawEntry &e = arc.entries[i];
		std::vector<int16_t> raw(e.total_size);
		const int n = sbf_decode_all(bytes.data() + e.data_offset, e.total_size, raw.data(), raw.size());
		TEST_EXPECT(n >= 0);
		raw.resize(size_t(n));
		TEST_EXPECT(sbf_decode_stream(bank.streams[i]) == raw);
	}
	sbf_close(&arc);

	// An edit written from the fields alone: a stream renamed, one dropped, one encoded anew (its chunks
	// under the bank's pair and rule); read back with the offsets the writer worked out.
	SbfFile edited = bank;
	edited.streams[2].name = "RENAMED";
	edited.streams.erase(edited.streams.begin() + 3);
	std::vector<int16_t> pcm(5000);
	for (size_t i = 0; i < pcm.size(); ++i) pcm[i] = int16_t((i % 100) * 200 - 10000);
	edited.streams.push_back(sbf_encode_stream("NEWTRACK", pcm.data(), pcm.size()));
	TEST_EXPECT(edited.streams.back().chunks.size() == 2 && edited.streams.back().chunks[1].audio.size() == 904);
	TEST_EXPECT(sbf_write_bank(edited, written, error));
	SbfFile again;
	TEST_EXPECT(sbf_read_bank(written.data(), written.size(), again, error, &layout) && layout.packed &&
	            layout.reserved_other == 0 && layout.tails_other == 0);
	TEST_EXPECT(again.streams.size() == 13 && again.streams[2].name == "RENAMED" && again.streams[12].name == "NEWTRACK");
	TEST_EXPECT(sbf_decode_stream(again.streams[12]).size() == pcm.size());
	TEST_EXPECT(sbf_decode_stream(again.streams[4]) == sbf_decode_stream(bank.streams[5]));
	TEST_EXPECT(sbf_open_memory(&arc, written.data(), written.size()) == 0);
	TEST_EXPECT(sbf_find_by_name(&arc, "NEWTRACK") != nullptr &&
	            sbf_find_by_name(&arc, "NEWTRACK")->data_offset + sbf_find_by_name(&arc, "NEWTRACK")->total_size ==
	                    written.size());
	sbf_close(&arc);

	// An empty bank: the 24-byte header alone, as sbf_encode_file writes one.
	SbfFile empty;
	TEST_EXPECT(sbf_write_bank(empty, written, error) && written.size() == size_t(SBF_HEADER_SIZE));
	TEST_EXPECT(sbf_read_bank(written.data(), written.size(), again, error) && again.streams.empty());

	// What the writer refuses: a name past 16 characters, a name holding a NUL, a block under its
	// header, a chunk playing more than its block holds, a flags word the engine refuses.
	SbfFile bad = bank;
	bad.streams[0].name = "SEVENTEEN_CHARS_X";
	TEST_EXPECT(!sbf_write_bank(bad, written, error) && !error.empty());
	bad = bank;
	bad.streams[0].name = std::string("A\0B", 3);
	TEST_EXPECT(!sbf_write_bank(bad, written, error));
	bad = bank;
	bad.streams[0].block_size = 4;
	TEST_EXPECT(!sbf_write_bank(bad, written, error));
	bad = bank;
	bad.streams[1].chunks[2].audio.resize(SBF_CHUNK_AUDIO + 1);
	TEST_EXPECT(!sbf_write_bank(bad, written, error));
	bad = bank;
	bad.flags = 3;
	TEST_EXPECT(!sbf_write_bank(bad, written, error));

	// What the reader refuses: another magic, an entry past the file's end, a block under its header,
	// a last chunk shorter than its header, entries whose sizes add up past the file (sharing bytes).
	std::vector<uint8_t> broken = bytes;
	broken[0] = 'X';
	TEST_EXPECT(!sbf_read_bank(broken.data(), broken.size(), again, error));
	broken = bytes;
	put_u32(broken, SBF_HEADER_SIZE + 16 + 4, uint32_t(bytes.size())); // entry 0's total_size
	TEST_EXPECT(!sbf_read_bank(broken.data(), broken.size(), again, error));
	broken = bytes;
	put_u32(broken, SBF_HEADER_SIZE + 16 + 8, 4); // entry 0's block_size
	TEST_EXPECT(!sbf_read_bank(broken.data(), broken.size(), again, error));
	broken = bytes;
	const size_t last_entry = SBF_HEADER_SIZE + 12 * size_t(SBF_ENTRY_SIZE);
	put_u32(broken, last_entry + 16 + 4, get_u32(bytes, last_entry + 16 + 4) + 4); // a 4-byte last chunk
	broken.insert(broken.end(), 4, 0);
	TEST_EXPECT(!sbf_read_bank(broken.data(), broken.size(), again, error));
	broken = bytes;
	for (uint32_t i = 0; i < 13; ++i) { // every entry over the whole audio
		put_u32(broken, SBF_HEADER_SIZE + i * SBF_ENTRY_SIZE + 16, SBF_HEADER_SIZE + 13 * SBF_ENTRY_SIZE);
		put_u32(broken, SBF_HEADER_SIZE + i * SBF_ENTRY_SIZE + 20,
		        uint32_t(bytes.size()) - (SBF_HEADER_SIZE + 13 * SBF_ENTRY_SIZE));
	}
	TEST_EXPECT(!sbf_read_bank(broken.data(), broken.size(), again, error) && error.find("share") != std::string::npos);

	// What it notes of a layout the writer lays out otherwise: bytes past the last stream, a name with
	// bytes after its terminator, streams out of their index's order, a chunk's reserved pair not the
	// bank's, a tail the bank's rule does not make, a valid count past its block.
	broken = bytes;
	broken.push_back(0);
	TEST_EXPECT(sbf_read_bank(broken.data(), broken.size(), again, error, &layout) && layout.trailing_bytes == 1);
	broken = bytes;
	broken[SBF_HEADER_SIZE + 10] = 'Z';
	TEST_EXPECT(sbf_read_bank(broken.data(), broken.size(), again, error, &layout) && layout.names_with_tails == 1 &&
	            again.streams[0].name == "SILENCE");
	broken = bytes;
	const uint32_t second = get_u32(bytes, SBF_HEADER_SIZE + SBF_ENTRY_SIZE + 16);
	put_u32(broken, SBF_HEADER_SIZE + 16, second);
	TEST_EXPECT(sbf_read_bank(broken.data(), broken.size(), again, error, &layout) && !layout.packed);
	const size_t chunk0 = get_u32(bytes, SBF_HEADER_SIZE + SBF_ENTRY_SIZE + 16); // TONE01's first chunk
	broken = bytes;
	broken[chunk0 + 6] = 0x1A;
	TEST_EXPECT(sbf_read_bank(broken.data(), broken.size(), again, error, &layout) && layout.reserved_other == 1 &&
	            again.chunk_reserved_a == 0xFA);
	broken = bytes;
	broken[chunk0 + 2 * SBF_CHUNK_TOTAL + 8 + 2000] = 0x11; // in TONE01's last chunk's tail
	TEST_EXPECT(sbf_read_bank(broken.data(), broken.size(), again, error, &layout) && layout.tails_other == 1 &&
	            again.tail == SbfTail::Silence);
	TEST_EXPECT(sbf_write_bank(again, written, error) && written == bytes);
	broken = bytes;
	put_u32(broken, chunk0 + 2 * SBF_CHUNK_TOTAL, SBF_CHUNK_AUDIO + 50); // a valid count past the block
	TEST_EXPECT(sbf_read_bank(broken.data(), broken.size(), again, error, &layout) && layout.short_chunks == 1 &&
	            again.streams[1].chunks[2].audio.size() == size_t(SBF_CHUNK_AUDIO));

	std::printf("sbf_bank: OK\n");
	return 0;
}
