// Generator + guard for the minimal set's custom map — the .env (time-of-day)
// and .bms (mission: header + team spawn markers) legs. Authored from scratch
// by libs/env + libs/mission (no retail asset); the .trn terrain leg rides its
// own slice. Emits with OPENNOVA_WRITE_MINIMAL_FIXTURES=1; otherwise guards
// each file round-trips (env: save->load; bms: write->parse) and carries the
// minimal content a host+join needs (a named mission on the minimal terrain,
// two team spawns). See fixtures/minimal/README.md.
#include <env/env.h>
#include <mission/mission.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

int fail = 0;
#define CHECK(c, m)                                                                                 \
	do {                                                                                            \
		if (!(c)) {                                                                                 \
			std::fprintf(stderr, "FAIL: %s\n", m);                                                  \
			++fail;                                                                                 \
		}                                                                                           \
	} while (0)

const char kMapBase[] = "mnml"; // the minimal map's base name (mnml.bms/.trn/.env)

std::string path(const std::string &name) { return std::string(MINIMAL_FIXTURE_DIR) + "/resources/" + name; }

bool read_file(const std::string &p, std::vector<uint8_t> &out) {
	std::ifstream f(p, std::ios::binary | std::ios::ate);
	if (!f) return false;
	const std::streamoff sz = f.tellg();
	if (sz < 0) return false;
	f.seekg(0);
	out.resize(static_cast<size_t>(sz));
	if (!out.empty()) f.read(reinterpret_cast<char *>(out.data()), static_cast<std::streamsize>(out.size()));
	return true;
}

// The authored .env: one default time-of-day. The map header points at it.
std::string build_env_text() {
	opennova::env::Config cfg = opennova::env::make_default_config();
	cfg.name = "mnml";
	std::ostringstream os;
	std::string err;
	if (!opennova::env::save_env(os, cfg, err)) {
		std::fprintf(stderr, "FAIL: save_env: %s\n", err.c_str());
		++fail;
	}
	return os.str();
}

// The authored .bms: a named mission on the minimal terrain with two team
// spawn markers (item 100001 "Marker Alpha" from the minimal items.def).
void build_bms(opennova::mission::MissionDocument &doc) {
	using namespace opennova::mission;
	doc.create_default();
	doc.set_header_string("mission_name", "Minimal");
	doc.set_header_string("terrain", kMapBase);
	doc.set_header_string("environment", kMapBase);
	EntityTransform blue{-64.0f, 0.0f, 0.0f, 0, 0, 0};
	EntityTransform red{64.0f, 0.0f, 0.0f, 0, 0x8000, 0};
	doc.add_entity(EntityKind::Marker, 100001, blue);
	doc.add_entity(EntityKind::Marker, 100001, red);
	doc.set_entity_property_int(EntityKind::Marker, 0, "team", 1);
	doc.set_entity_property_int(EntityKind::Marker, 1, "team", 2);
}

} // namespace

int main() {
	const bool write_mode = std::getenv("OPENNOVA_WRITE_MINIMAL_FIXTURES") != nullptr;

	// ---- .env ----
	{
		const std::string env_path = path(std::string(kMapBase) + ".env");
		const std::string text = build_env_text();
		// Round-trip: the emitted text loads back.
		opennova::env::Config reloaded;
		std::string err;
		std::istringstream is(text);
		CHECK(opennova::env::load_env(is, reloaded, err), err.c_str());

		if (write_mode) {
			std::ofstream o(env_path, std::ios::binary);
			o << text;
			std::printf("wrote %s (%zu bytes)\n", env_path.c_str(), text.size());
		} else {
			std::vector<uint8_t> committed;
			CHECK(read_file(env_path, committed),
			      "committed .env missing — run with OPENNOVA_WRITE_MINIMAL_FIXTURES=1");
			// .env is text (LF vs the committed EOL may differ) — assert it LOADS,
			// which is what the engine cares about, rather than byte-identity.
			opennova::env::Config c;
			std::string e2;
			std::string committed_text(committed.begin(), committed.end());
			std::istringstream cis(committed_text);
			CHECK(opennova::env::load_env(cis, c, e2), e2.c_str());
		}
	}

	// ---- .bms ----
	{
		const std::string bms_path = path(std::string(kMapBase) + ".bms");
		opennova::mission::MissionDocument doc;
		build_bms(doc);
		std::vector<uint8_t> bytes;
		CHECK(doc.write_bms_bytes(bytes), "write_bms_bytes");

		// Round-trip: the emitted bytes parse and carry the two spawn markers.
		opennova::mission::MissionDocument reparsed;
		CHECK(reparsed.load_bms_bytes(bytes.data(), bytes.size()), "bms parse");
		CHECK(reparsed.entity_count(opennova::mission::EntityKind::Marker) == 2,
		      "bms carries the two team spawn markers");
		CHECK(reparsed.info().terrain == kMapBase, "bms header points at the minimal terrain");

		if (write_mode) {
			std::ofstream o(bms_path, std::ios::binary);
			o.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
			std::printf("wrote %s (%zu bytes)\n", bms_path.c_str(), bytes.size());
		} else {
			std::vector<uint8_t> committed;
			CHECK(read_file(bms_path, committed),
			      "committed .bms missing — run with OPENNOVA_WRITE_MINIMAL_FIXTURES=1");
			static const char kLfsSentinel[] = "version https://git-lfs";
			const bool is_ptr = committed.size() >= sizeof(kLfsSentinel) - 1 &&
			                    std::equal(kLfsSentinel, kLfsSentinel + sizeof(kLfsSentinel) - 1,
			                               committed.begin());
			if (is_ptr) {
				std::printf("[skip] mnml.bms is an unpulled LFS pointer\n");
			} else {
				CHECK(committed == bytes, "committed .bms differs from the writer output — "
				                          "regenerate with OPENNOVA_WRITE_MINIMAL_FIXTURES=1");
			}
		}
	}

	if (fail == 0) std::printf("OK: minimal map .env + .bms authored + validated\n");
	return fail == 0 ? 0 : 1;
}
