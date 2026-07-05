// Generator + guard for the minimal mission's dialog companions: mnml.dbf and
// mnml.lwf. The witnessed retail mission family is <stem>.bms + .til + .pcx +
// .dbf + .lwf + .wac + .bin; the dialog chain is loaded per mission by
// DialogSystem_Init @ 0x5275e0 (mission base name + ".dbf") which co-loads
// "<base>.lwf" (fallback .pwf) as the dialog sound bank @ 0x44e7d4 — see
// docs/audio/lwf-dbf-sound-re.md. The minimal mission speaks no dialog, so
// both are authored empty-but-valid from scratch (libs/dbf + libs/lwf
// writers). Same contract as minimal_rtxt_gen: default guards byte-stability
// against the committed files; OPENNOVA_WRITE_MINIMAL_FIXTURES=1 rewrites.
#include <dbf/dbf.h>
#include <lwf/lwf.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

using namespace opennova;

namespace {

int fail = 0;
#define CHECK(c, m)                                                                                 \
	do {                                                                                            \
		if (!(c)) {                                                                                 \
			std::fprintf(stderr, "FAIL: %s\n", m);                                                  \
			++fail;                                                                                 \
		}                                                                                           \
	} while (0)

std::string path(const char *name) { return std::string(MINIMAL_FIXTURE_DIR) + "/resources/" + name; }

bool read_bytes(const std::string &p, std::vector<uint8_t> &b) {
	std::ifstream f(p, std::ios::binary | std::ios::ate);
	if (!f) return false;
	const std::streamoff sz = f.tellg();
	if (sz < 0) return false;
	f.seekg(0);
	b.resize(static_cast<size_t>(sz));
	if (!b.empty()) f.read(reinterpret_cast<char *>(b.data()), static_cast<std::streamsize>(b.size()));
	return true;
}

bool is_lfs_pointer(const std::vector<uint8_t> &b) {
	static const char k[] = "version https://git-lfs";
	return b.size() >= sizeof(k) - 1 && std::equal(k, k + sizeof(k) - 1, b.begin());
}

// Empty-but-valid dialog bank: DLG0 header, zero id-defs, zero groups.
std::vector<uint8_t> make_dbf() {
	dbf::File f;
	f.header.magic = dbf::kMagic;
	f.header.version = 0x100;
	f.header.header_size = 28;
	std::vector<uint8_t> out;
	std::string err;
	CHECK(dbf::encode_dbf(f, out, err), err.c_str());
	return out;
}

// Empty-but-valid sound bank: LWF1 header, no singles/multis/playlists.
std::vector<uint8_t> make_lwf() {
	lwf::File f;
	f.header.header_size = 28;
	f.header.magic = lwf::kMagic;
	std::vector<uint8_t> out;
	std::string err;
	CHECK(lwf::encode_lwf(f, out, err), err.c_str());
	return out;
}

void run(const char *name, const std::vector<uint8_t> &bytes, bool write_mode,
         bool (*reparse)(const std::vector<uint8_t> &, std::string &)) {
	std::string err;
	CHECK(!bytes.empty(), (std::string(name) + " encoded empty").c_str());
	CHECK(reparse(bytes, err), (std::string(name) + " re-parse: " + err).c_str());
	const std::string p = path(name);
	if (write_mode) {
		std::ofstream o(p, std::ios::binary);
		o.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
		std::printf("wrote %s (%zu bytes)\n", p.c_str(), bytes.size());
		return;
	}
	std::vector<uint8_t> committed;
	CHECK(read_bytes(p, committed),
	      (std::string("committed ") + name + " missing — run with OPENNOVA_WRITE_MINIMAL_FIXTURES=1").c_str());
	if (committed.empty()) return;
	if (is_lfs_pointer(committed)) {
		std::printf("[skip] %s is an unpulled LFS pointer\n", name);
		return;
	}
	CHECK(committed == bytes,
	      (std::string(name) + " committed bytes differ from the writer — regenerate").c_str());
}

} // namespace

int main() {
	const bool write_mode = std::getenv("OPENNOVA_WRITE_MINIMAL_FIXTURES") != nullptr;

	run("mnml.dbf", make_dbf(), write_mode, [](const std::vector<uint8_t> &b, std::string &err) {
		dbf::File f;
		return dbf::parse_dbf_memory(b.data(), b.size(), f, err) && f.groups.empty();
	});
	run("mnml.lwf", make_lwf(), write_mode, [](const std::vector<uint8_t> &b, std::string &err) {
		lwf::File f;
		return lwf::parse_lwf_buffer(b.data(), b.size(), f, err) && f.singles.empty();
	});

	if (fail == 0) std::printf("OK: minimal dialog companions (dbf+lwf) authored + valid\n");
	return fail == 0 ? 0 : 1;
}
