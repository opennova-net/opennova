// Retail SBF sweep: the two banks a JO install streams loose beside its
// archives (gamemus.sbf + menumus.sbf [orig: AudioVM_InitMenuMusicStreaming
// @ 0x56aa60]). Every header field the engine reads is in range, every
// chunk of every entry decodes, and the gamemus facts the synthetic bank
// mirrors hold on the real thing: 13 entries, NULLS first, NULLS one chunk
// (total_size 0x1008) of 0x08A8 valid samples.
//
// Reports Skipped without OPENNOVA_JO_DIR.

#include <formats/sbf/sbf.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "common/retail_paths.h"

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
