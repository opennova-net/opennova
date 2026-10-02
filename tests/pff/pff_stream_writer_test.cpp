// Pins the resumable PFF writer (S13 A1, formats/pff/pff_stream_writer.h) against the archives the
// writer made before it existed: each fixture set written a budget of 1 byte, 7 bytes and the
// whole at a time, and through the single-call forms, is byte for byte the archive of the
// pre-stream writer, whose size and 64-bit FNV-1a digest are recorded below (taken from that
// writer's pff_write_archive over the same sets). Also: the payloads are asked for one entry at
// a time at increasing offsets, a writer dropped before finish() leaves no file, and finish()
// replaces an existing archive.
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

#include <formats/pff/pff.h>
#include <formats/pff/pff_stream_writer.h>

using namespace opennova::pff;

static int passed = 0;
static int failed = 0;

#define RUN_TEST(fn) do { \
	printf("Running %s... ", #fn); \
	if (fn()) { printf("PASS\n"); ++passed; } \
	else { printf("FAIL\n"); ++failed; } \
} while (0)

#define CHECK(cond, msg) do { \
	if (!(cond)) { fprintf(stderr, "  FAIL: %s (line %d)\n", msg, __LINE__); return 0; } \
} while (0)

namespace {

// A deterministic payload: `n` bytes of a 32-bit LCG seeded by `seed`.
std::vector<uint8_t> payload(uint32_t seed, size_t n) {
	std::vector<uint8_t> out(n);
	uint32_t x = seed * 2654435761u + 1u;
	for (size_t i = 0; i < n; ++i) {
		x = x * 1664525u + 1013904223u;
		out[i] = uint8_t(x >> 24);
	}
	return out;
}

struct Spec {
	const char *name;
	uint32_t seed;
	uint32_t size;
	uint32_t flags;
	uint32_t timestamp;
	uint32_t checksum;
};

// A fixture set and what the pre-stream writer made of it.
struct Fixture {
	const char *label;
	PffFormat format;
	std::vector<Spec> specs;
	size_t size;
	uint64_t digest;
};

std::vector<Fixture> fixtures() {
	return {
		{"mixed", PFF_FORMAT_PFF3,
		 {{"secret.bin", 3, 27, PFF_FLAG_ENCRYPTED, 0, 0},
		  {"alpha.txt", 1, 5, 0, 0x11223344u, 0x55667788u},
		  {"empty.bin", 0, 0, 0, 0, 0},
		  {"Bravo.dat", 2, 3, 0, 0, 0}},
		 199, 0x8345D1B73FB72573ull},
		{"sizes", PFF_FORMAT_PFF4,
		 {{"zz_last.bin", 10, 0, 0, 0, 0},
		  {"Mid.TGA", 11, 1, 0, 0, 0},
		  {"a0.pcx", 12, 7, 0, 7, 0},
		  {"sixteen_chars_ab", 13, 8, 0, 0, 9},
		  {"q.dat", 14, 13, 0, 0, 0},
		  {"MENU.MNU", 15, 64, 0, 0, 0},
		  {"b1.def", 16, 100, 0, 0, 0},
		  {"c2.bin", 17, 1000, 0, 0, 0},
		  {"d3.3di", 18, 4099, 0, 0, 0},
		  {"Big.tga", 19, 65537, 0, 0, 0},
		  {"bigger.pcx", 20, 70000, PFF_FLAG_ENCRYPTED, 0, 0},
		  {"e4.lwf", 21, 3, 0, 0, 0}},
		 141284, 0xEF7CAEA65A64E8C3ull},
		{"empty", PFF_FORMAT_BHD, {}, 20, 0x041C8451988F1062ull},
	};
}

uint64_t fnv(const std::vector<uint8_t> &bytes) {
	uint64_t h = 14695981039346656037ull;
	for (const uint8_t b : bytes) h = (h ^ b) * 1099511628211ull;
	return h;
}

bool read_all(const char *path, std::vector<uint8_t> &out) {
	FILE *f = fopen(path, "rb");
	if (!f) return false;
	fseek(f, 0, SEEK_END);
	const long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	out.resize(n > 0 ? size_t(n) : 0);
	const bool ok = n >= 0 && fread(out.data(), 1, out.size(), f) == out.size();
	fclose(f);
	return ok;
}

bool exists(const char *path) {
	FILE *f = fopen(path, "rb");
	if (!f) return false;
	fclose(f);
	return true;
}

// The payloads the stream writer reads, and whether it asked for them in order: one entry at a
// time, each from offset 0 up without a gap.
struct Source {
	std::vector<std::vector<uint8_t>> payloads;
	uint32_t entry = UINT32_MAX;
	uint32_t next = 0;
	bool in_order = true;
};

int read_chunk(void *ctx, uint32_t index, uint32_t offset, uint8_t *out, uint32_t size) {
	Source &source = *static_cast<Source *>(ctx);
	if (offset == 0) {
		source.entry = index;
		source.next = 0;
	}
	source.in_order = source.in_order && index == source.entry && offset == source.next;
	source.next = offset + size;
	if (index >= source.payloads.size() || offset + size > source.payloads[index].size()) return 1;
	memcpy(out, source.payloads[index].data() + offset, size);
	return 0;
}

int read_whole(void *ctx, uint32_t index, uint8_t *out, uint32_t size) {
	const Source &source = *static_cast<const Source *>(ctx);
	if (size) memcpy(out, source.payloads[index].data(), size);
	return 0;
}

struct Written {
	Source source;
	std::vector<PffWriteStreamEntry> entries;
};

Written prepare(const Fixture &fixture) {
	Written written;
	for (const Spec &spec : fixture.specs) {
		written.source.payloads.push_back(payload(spec.seed, spec.size));
		written.entries.push_back({spec.name, spec.size, spec.flags, spec.timestamp, spec.checksum});
	}
	return written;
}

// The fixture written through the stream writer `budget` bytes a write() at a time.
bool write_streamed(const Fixture &fixture, uint64_t budget, const char *path, size_t &writes, bool &in_order) {
	Written written = prepare(fixture);
	PffStreamWriter writer;
	if (writer.open(path, fixture.format, written.entries.empty() ? nullptr : written.entries.data(),
	            uint32_t(written.entries.size()), read_chunk, &written.source) != PFF_WRITE_OK)
		return false;
	writes = 0;
	uint64_t before = writer.payload_written();
	while (!writer.payloads_written()) {
		if (writer.write(budget) != PFF_WRITE_OK) return false;
		++writes;
		// No write goes past its budget.
		if (writer.payload_written() - before > budget) return false;
		before = writer.payload_written();
	}
	in_order = written.source.in_order;
	return writer.payload_written() == writer.payload_bytes() && writer.finish() == PFF_WRITE_OK;
}

} // namespace

static int test_budgets_match_the_recorded_archives() {
	for (const Fixture &fixture : fixtures()) {
		for (const uint64_t budget : {uint64_t(1), uint64_t(7), UINT64_MAX}) {
			const std::string path = std::string("pff_stream_") + fixture.label + ".pff";
			size_t writes = 0;
			bool in_order = false;
			CHECK(write_streamed(fixture, budget, path.c_str(), writes, in_order), "stream write");
			std::vector<uint8_t> bytes;
			CHECK(read_all(path.c_str(), bytes), "read back");
			CHECK(bytes.size() == fixture.size, "the recorded size");
			CHECK(fnv(bytes) == fixture.digest, "the recorded bytes");
			CHECK(in_order, "each payload asked for in order");
			if (budget == 1) {
				uint64_t payload_bytes = 0;
				for (const Spec &spec : fixture.specs) payload_bytes += spec.size;
				CHECK(writes == payload_bytes || (payload_bytes == 0 && writes == 0), "a byte a write");
			}
			CHECK(!exists((path + ".tmp").c_str()) && !exists((path + ".bak").c_str()), "no temp or backup left");
			remove(path.c_str());
		}
	}
	return 1;
}

static int test_single_call_forms_match() {
	for (const Fixture &fixture : fixtures()) {
		Written written = prepare(fixture);
		std::vector<PffWriteEntry> flat;
		for (size_t i = 0; i < written.entries.size(); ++i) {
			const PffWriteStreamEntry &e = written.entries[i];
			flat.push_back({e.name, e.size ? written.source.payloads[i].data() : nullptr, e.size, e.flags, e.timestamp,
			                e.checksum});
		}
		const std::string path = std::string("pff_single_") + fixture.label + ".pff";
		std::vector<uint8_t> bytes;
		CHECK(pff_write_archive(path.c_str(), fixture.format, flat.empty() ? nullptr : flat.data(),
		                        uint32_t(flat.size())) == PFF_WRITE_OK, "pff_write_archive");
		CHECK(read_all(path.c_str(), bytes) && bytes.size() == fixture.size && fnv(bytes) == fixture.digest,
		      "pff_write_archive: the recorded bytes");
		CHECK(pff_write_archive_streamed(path.c_str(), fixture.format,
		                                 written.entries.empty() ? nullptr : written.entries.data(),
		                                 uint32_t(written.entries.size()), read_whole, &written.source) == PFF_WRITE_OK,
		      "pff_write_archive_streamed over an existing file");
		CHECK(read_all(path.c_str(), bytes) && bytes.size() == fixture.size && fnv(bytes) == fixture.digest,
		      "pff_write_archive_streamed: the recorded bytes");
		CHECK(!exists((path + ".bak").c_str()), "the replaced archive's backup gone");
		remove(path.c_str());
	}
	return 1;
}

static int test_dropped_writer_leaves_nothing() {
	const Fixture fixture = fixtures()[1];
	Written written = prepare(fixture);
	const char *path = "pff_stream_dropped.pff";
	remove(path);
	{
		PffStreamWriter writer;
		CHECK(writer.open(path, fixture.format, written.entries.data(), uint32_t(written.entries.size()), read_chunk,
		                  &written.source) == PFF_WRITE_OK, "open");
		CHECK(writer.write(100) == PFF_WRITE_OK && !writer.payloads_written() && writer.payload_written() == 100, "a slice");
		CHECK(exists("pff_stream_dropped.pff.tmp"), "the temp file while it writes");
	}
	CHECK(!exists("pff_stream_dropped.pff.tmp") && !exists(path), "dropped: no temp, no archive");
	PffStreamWriter aborted;
	CHECK(aborted.open(path, fixture.format, written.entries.data(), uint32_t(written.entries.size()), read_chunk,
	                   &written.source) == PFF_WRITE_OK, "open again");
	aborted.abort();
	CHECK(!aborted.is_open() && !exists("pff_stream_dropped.pff.tmp") && !exists(path), "aborted: nothing");
	// A refused set opens nothing.
	PffWriteStreamEntry twice[2] = {{"dup.bin", 1, 0, 0, 0}, {"DUP.BIN", 1, 0, 0, 0}};
	PffStreamWriter refused;
	CHECK(refused.open(path, PFF_FORMAT_PFF3, twice, 2, read_chunk, &written.source) == PFF_WRITE_ERR_DUP_NAME &&
	      !refused.is_open() && !exists("pff_stream_dropped.pff.tmp"), "a repeated name refused before any write");
	return 1;
}

int main(void) {
	RUN_TEST(test_budgets_match_the_recorded_archives);
	RUN_TEST(test_single_call_forms_match);
	RUN_TEST(test_dropped_writer_leaves_nothing);
	printf("\n%d passed, %d failed\n", passed, failed);
	return failed > 0 ? 1 : 0;
}
