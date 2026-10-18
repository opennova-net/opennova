// Retail SBF sweep: the two banks a JO install streams loose beside its
// archives (gamemus.sbf + menumus.sbf [orig: AudioVM_InitMenuMusicStreaming
// @ 0x56aa60]) and each installed expansion's pair (expansion\<n>\M<n>.sbf,
// G<n>.sbf). Every header field the engine reads is in range, every chunk of
// every entry decodes, and the gamemus facts the synthetic bank mirrors hold
// on the real thing: 13 entries, NULLS first, NULLS one chunk (total_size
// 0x1008) of 0x08A8 valid samples. Every bank reads into the bank model
// (sbf_read_bank), packed as the writer packs one; its write (sbf_write_bank,
// the header's version and reserved words and the chunks' reserved pair and
// tails the writer's constants, never the bank's) reads back as the same
// fields, the same size, and differs from the file only in those never-read
// bytes; each stream's decode is the raw entry's.
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
#include "sbf/sbf_streaming_reference.h"

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

// The bank through the model and back: the same fields and size, differing
// only in the chunks' never-read bytes, each stream decoding as its raw entry
// does.
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
	CHECK(layout.packed && layout.trailing_bytes == 0 && layout.names_with_tails == 0 && layout.short_chunks == 0,
	      "%s is packed as the writer packs a bank (%zu short chunks otherwise)\n", label, layout.short_chunks);
	std::vector<uint8_t> written;
	CHECK(sbf_write_bank(bank, written, error), "%s writes: %s\n", label, error.c_str());
	CHECK(written.size() == bytes.size(), "%s writes its size (%zu of %zu bytes)\n", label, written.size(), bytes.size());
	SbfFile again;
	SbfFileLayout again_layout;
	bool same = sbf_read_bank(written.data(), written.size(), again, error, &again_layout) &&
	            again_layout.reserved_other == 0 && again_layout.tails_other == 0 && again_layout.header_words_other == 0 &&
	            again.streams.size() == bank.streams.size() && again.flags == bank.flags;
	for (size_t i = 0; same && i < bank.streams.size(); ++i) {
		const SbfStream &x = bank.streams[i], &y = again.streams[i];
		same = x.name == y.name && x.block_size == y.block_size && x.sample_length_hint == y.sample_length_hint &&
		       x.chunks.size() == y.chunks.size();
		for (size_t c = 0; same && c < x.chunks.size(); ++c)
			same = x.chunks[c].scale_a == y.chunks[c].scale_a && x.chunks[c].scale_b == y.chunks[c].scale_b &&
			       x.chunks[c].audio == y.chunks[c].audio;
	}
	CHECK(same, "%s's write reads back as the same fields\n", label);
	// The write differs from the file only in the header's version and reserved words and the chunks'
	// reserved pairs and tails, which no reader reads.
	size_t other = 0;
	if (written.size() == bytes.size() && bank.streams.size() == again.streams.size()) {
		std::vector<bool> never_read(bytes.size(), false);
		for (size_t k = 4; k < 8; ++k) never_read[k] = true;
		for (size_t k = 12; k < 16; ++k) never_read[k] = true;
		size_t at = size_t(SBF_HEADER_SIZE) + bank.streams.size() * size_t(SBF_ENTRY_SIZE);
		for (const SbfStream &stream : bank.streams)
			for (const SbfChunk &chunk : stream.chunks) {
				never_read[at + 6] = never_read[at + 7] = true;
				for (size_t k = SBF_CHUNK_HEADER + chunk.audio.size(); k < stream.block_size; ++k) never_read[at + k] = true;
				at += stream.block_size;
			}
		for (size_t i = 0; i < bytes.size(); ++i) other += written[i] != bytes[i] && !never_read[i] ? 1 : 0;
	}
	CHECK(other == 0, "%s's write differs only in never-read bytes (%zu other bytes)\n", label, other);
	SbfArchive arc;
	if (sbf_open_memory(&arc, bytes.data(), bytes.size()) != 0) return;
	size_t streams_decoding = 0;
	size_t streams_as_streamed = 0;
	for (uint32_t i = 0; i < arc.header.entry_count && i < bank.streams.size(); ++i) {
		const SbfRawEntry &e = arc.entries[i];
		std::vector<int16_t> raw(e.total_size);
		const int n = sbf_decode_all(bytes.data() + e.data_offset, e.total_size, raw.data(), raw.size());
		raw.resize(n < 0 ? 0 : static_cast<size_t>(n));
		if (sbf_decode_stream(bank.streams[i]) == raw) ++streams_decoding;
		// The Godot player's PCM (C-164) is the deleted streaming playback's, sample for sample.
		const std::vector<int16_t> entry = sbf_decode_entry(bytes.data() + e.data_offset, e.total_size, e.block_size);
		if (!entry.empty() && entry == sbf_streaming_reference(bytes.data() + e.data_offset, e.total_size, e.block_size))
			++streams_as_streamed;
	}
	CHECK(streams_decoding == arc.header.entry_count, "%s: every stream decodes as its raw entry (%zu of %u)\n", label,
	      streams_decoding, arc.header.entry_count);
	CHECK(streams_as_streamed == arc.header.entry_count,
	      "%s: every entry's player PCM is the streaming playback's (%zu of %u)\n", label, streams_as_streamed,
	      arc.header.entry_count);
	sbf_close(&arc);
	std::printf("%s: %zu streams through the bank model and back (%zu header words, %zu chunk reserved pairs and %zu "
	            "tails not the writer's)\n", label, bank.streams.size(), layout.header_words_other, layout.reserved_other,
	            layout.tails_other);
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
