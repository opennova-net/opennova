// Retail SBF sweep: the two banks a JO install streams loose beside its
// archives (gamemus.sbf + menumus.sbf [orig: AudioVM_InitMenuMusicStreaming
// @ 0x56aa60]) and each installed expansion's pair (expansion\<n>\M<n>.sbf,
// G<n>.sbf). Every header field the engine reads is in range, every chunk of
// every entry decodes, and the gamemus facts the synthetic bank mirrors hold
// on the real thing: 13 entries, NULLS first, NULLS one chunk (total_size
// 0x1008) of 0x08A8 valid samples. Every bank reads into the bank model
// (sbf_read_bank) laid out as the writer lays one out (every chunk's tail the
// Residue rule's, every chunk's reserved pair the bank's one pair), writes
// back through sbf_write_bank byte for byte, and each stream's decode is the
// raw entry's.
//
// Reports Skipped without OPENNOVA_JO_DIR.

#include <formats/sbf/sbf.h>

#include <cstdint>
#include <filesystem>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "common/retail_paths.h"

using namespace opennova::sbf;

namespace {

int g_failures = 0;

#define CHECK(cond, ...)                                          \
	do {                                                          \
		if (!(cond)) {                                            \
			std::printf("FAIL: " __VA_ARGS__);                    \
			std::printf("  (%s:%d: %s)\n", __FILE__, __LINE__, #cond); \
			++g_failures;                                         \
		}                                                         \
	} while (0)

// The bank through the model and back: byte for byte, laid out as the writer
// lays one out, each stream decoding as its raw entry does.
void round_trip(const std::string &path, const char *label) {
	std::vector<uint8_t> bytes;
	FILE *file = std::fopen(path.c_str(), "rb");
	CHECK(file != nullptr, "%s reads\n", label);
	if (!file) return;
	std::fseek(file, 0, SEEK_END);
	bytes.resize(static_cast<size_t>(std::ftell(file)));
	std::fseek(file, 0, SEEK_SET);
	const size_t got = std::fread(bytes.data(), 1, bytes.size(), file);
	std::fclose(file);
	CHECK(got == bytes.size(), "%s reads whole\n", label);
	SbfFile bank;
	SbfFileLayout layout;
	std::string error;
	CHECK(sbf_read_bank(bytes.data(), bytes.size(), bank, error, &layout), "%s reads into the bank model: %s\n", label,
	      error.c_str());
	CHECK(layout.packed && layout.trailing_bytes == 0 && layout.names_with_tails == 0 && layout.reserved_other == 0 &&
	              layout.tails_other == 0 && layout.short_chunks == 0,
	      "%s is laid out as the writer lays a bank out (%zu reserved pairs, %zu tails, %zu short chunks otherwise)\n",
	      label, layout.reserved_other, layout.tails_other, layout.short_chunks);
	CHECK(bank.tail == SbfTail::Residue, "%s's tails are the retail encoder's reused buffer\n", label);
	std::vector<uint8_t> written;
	CHECK(sbf_write_bank(bank, written, error), "%s writes: %s\n", label, error.c_str());
	CHECK(written == bytes, "%s writes back byte for byte (%zu of %zu bytes)\n", label, written.size(), bytes.size());
	SbfArchive arc;
	if (sbf_open_memory(&arc, bytes.data(), bytes.size()) != 0) return;
	size_t streams_decoding = 0;
	for (uint32_t i = 0; i < arc.header.entry_count && i < bank.streams.size(); ++i) {
		const SbfRawEntry &e = arc.entries[i];
		std::vector<int16_t> raw(e.total_size);
		const int n = sbf_decode_all(bytes.data() + e.data_offset, e.total_size, raw.data(), raw.size());
		raw.resize(n < 0 ? 0 : static_cast<size_t>(n));
		if (sbf_decode_stream(bank.streams[i]) == raw) ++streams_decoding;
	}
	CHECK(streams_decoding == arc.header.entry_count, "%s: every stream decodes as its raw entry (%zu of %u)\n", label,
	      streams_decoding, arc.header.entry_count);
	sbf_close(&arc);
	std::printf("%s: %zu streams through the bank model, byte for byte (reserved pair 0x%02X/0x%02X)\n", label,
	            bank.streams.size(), bank.chunk_reserved_a, bank.chunk_reserved_b);
}

void sweep(const std::string &path, const char *label) {
	SbfArchive arc;
	CHECK(sbf_open(&arc, path.c_str()) == 0, "%s opens\n", label);
	if (g_failures != 0) return;
	CHECK(arc.header.magic == SBF_MAGIC, "%s magic\n", label);
	CHECK(arc.header.flags <= 2, "%s flags %u within the engine's accepted range\n", label, arc.header.flags);
	CHECK(arc.header.index_offset == SBF_HEADER_SIZE, "%s index at 0x18\n", label);
	CHECK(arc.header.entry_count > 0, "%s has entries\n", label);
	std::vector<uint8_t> chunk(SBF_CHUNK_TOTAL);
	std::vector<int16_t> decoded(SBF_CHUNK_AUDIO);
	uint32_t chunks = 0;
	for (uint32_t i = 0; i < arc.header.entry_count; ++i) {
		const SbfRawEntry &e = arc.entries[i];
		CHECK(e.block_size == SBF_CHUNK_TOTAL, "%s entry %u block_size 0x1008\n", label, i);
		if (e.block_size != SBF_CHUNK_TOTAL) continue;
		const uint32_t count = e.total_size / e.block_size;
		CHECK(count * e.block_size == e.total_size, "%s entry %u total_size is whole chunks\n", label, i);
		for (uint32_t c = 0; c < count; ++c) {
			const int got = sbf_read_chunk(&arc, &e, c, chunk.data(), chunk.size());
			CHECK(got == static_cast<int>(e.block_size), "%s entry %u chunk %u reads\n", label, i, c);
			if (got != static_cast<int>(e.block_size)) break;
			CHECK(sbf_decode_chunk(chunk.data(), chunk.size(), decoded.data(), decoded.size()) >= 0,
			      "%s entry %u chunk %u decodes\n", label, i, c);
			++chunks;
		}
	}
	std::printf("%s: %u entries, %u chunks decode\n", label, arc.header.entry_count, chunks);
	sbf_close(&arc);
	round_trip(path, label);
}

} // namespace

int main() {
	const std::string install = retail::install();
	if (install.empty())
		return retail::skip("OPENNOVA_JO_DIR (a packed retail install: gamemus.sbf + menumus.sbf stream loose beside the archives)");
	const std::string gamemus = retail::join(install, "gamemus.sbf");
	const std::string menumus = retail::join(install, "menumus.sbf");
	CHECK(retail::file_exists(gamemus), "gamemus.sbf beside the archives\n");
	CHECK(retail::file_exists(menumus), "menumus.sbf beside the archives\n");
	if (g_failures != 0) return 1;

	sweep(gamemus, "gamemus.sbf");
	sweep(menumus, "menumus.sbf");
	// Each installed expansion's banks, streamed loose from its folder (Expansion_LoadAssets @ 0x4a4906 / 0x4a4936,
	// docs/audio/mus-sbf-re.md): the round trip over every bank of the install.
	std::error_code ec;
	const std::filesystem::path expansions = std::filesystem::u8path(retail::join(install, "expansion"));
	if (std::filesystem::is_directory(expansions, ec))
		for (const auto &folder : std::filesystem::directory_iterator(expansions, ec))
			for (const auto &entry : std::filesystem::directory_iterator(folder.path(), ec)) {
				std::string ext = entry.path().extension().u8string();
				for (char &c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
				if (ext != ".sbf") continue;
				const std::string label = entry.path().filename().u8string();
				sweep(entry.path().u8string(), label.c_str());
			}

	// The JO gamemus facts the synthetic bank mirrors.
	SbfArchive arc;
	if (sbf_open(&arc, gamemus.c_str()) == 0) {
		CHECK(arc.header.entry_count == 13, "JO gamemus has 13 entries\n");
		CHECK(std::strncmp(arc.entries[0].name, "NULLS", 5) == 0, "JO gamemus entry 0 is NULLS\n");
		const SbfRawEntry *nulls = sbf_find_by_name(&arc, "NULLS");
		CHECK(nulls != nullptr && nulls->total_size == 0x1008, "NULLS is one chunk\n");
		if (nulls != nullptr) {
			uint8_t chunk[SBF_CHUNK_TOTAL];
			if (sbf_read_chunk(&arc, nulls, 0, chunk, sizeof(chunk)) == SBF_CHUNK_TOTAL) {
				uint32_t valid = 0;
				std::memcpy(&valid, chunk, sizeof(valid));
				CHECK(valid == 0x08A8, "NULLS carries 0x08A8 valid samples\n");
			}
		}
		sbf_close(&arc);
	}
	std::printf("sbf_jo_install_sweep: failures=%d\n", g_failures);
	return g_failures == 0 ? 0 : 1;
}
