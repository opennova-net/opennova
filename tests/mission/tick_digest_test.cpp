// The tick digest (ADR 0043): one hash over the simulation's per-tick state,
// chained across N ticks, so a STRUCTURAL slice (a free function becoming a
// method, a field regrouped, a tick leg moved between owners) proves it changed
// nothing observable: the digest before equals the digest after.
//
// What is hashed is common/tick_digest.h's chain (shared with tests/null_backend,
// which runs the same mission through the platform seams and pins the same
// value). Any reorder of a tick leg that consumes an RNG draw, moves a body, or
// changes an AI decision changes the chain.
//
// Two legs:
//   default          the SYNTHETIC mission (ungated, every CI run): boots the
//                    same in-memory mission twice and asserts (a) both boots
//                    produce the same chain (determinism) and (b) the chain
//                    equals the committed kSyntheticDigest. A slice whose
//                    change is genuinely behavior-neutral leaves the constant
//                    alone; `--print` reports the current value for the rare
//                    ledgered follow-up that is allowed to move it.
//   --install DIR --mission NAME
//                    a mission of the packed retail install DIR through the
//                    ctest rig (a local tool, never a ctest row), printing
//                    `digest <mission> <ticks> <hex>` for scripts/parity/
//                    tick_digest.py --record / --check. Float codegen differs
//                    across compilers, so a retail digest is compared on one
//                    machine and never committed.
#include "common/boot_file_source.h"
#include "common/file_io.h"
#include "common/mission_rig.h"
#include "common/test_paths.h"
#include "common/tick_digest.h"

#include <base/io/strutil.h>
#include <base/resource_index/resource_index.h>
#include <base/vfs/file_source.h>
#include <runtime/assets/asset_store.h>
#include <runtime/mission/mission_kernel.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace opennova;
namespace ms = opennova::mission;
using test_digest::Digest;
using test_digest::hash_world;
using test_digest::kSyntheticDigest;
using test_digest::kSyntheticTicks;

namespace {

// --- the synthetic mission (tests/common/synthetic_mission.h) ------------------

using test_boot::source_over;
using test_boot::tick_no_net;

struct PeopleClips final : FileSource {
	std::map<std::string, std::vector<uint8_t>> files; // by lower-case name
	bool read(const std::string &name, std::vector<uint8_t> &out) const override {
		const auto found = files.find(strutil::to_lower(name));
		if (found == files.end()) return false;
		out = found->second;
		return true;
	}
	uint64_t stamp(const std::string &name) const override {
		return files.count(strutil::to_lower(name)) != 0 ? 1 : 0;
	}
};

bool synthetic_chain(int ticks, uint64_t &out, std::string &error) {
	std::map<std::string, std::string> files;
	files["synth.wac"] = test_digest::kSyntheticWac;
	files["items.def"] = test_digest::kPeopleItems;
	// The people's table, the one their item names: every body holds its
	// animation channel and runs the org1 update (one without runs nothing,
	// world::infantry_has_anim_channel). No configured default stands in.
	auto clips = std::make_shared<PeopleClips>();
	for (const char *name : test_digest::kPeopleClipFiles) {
		clips->files[name] = test_io::read_file(test_paths_fixture(std::string("anim/") + name));
		if (clips->files[name].empty()) {
			error = std::string("no fixture anim/") + name;
			return false;
		}
	}
	ResourceIndex index;
	if (!index.mount_source(clips)) {
		error = "mount: " + index.last_error();
		return false;
	}
	const assets::AssetStore store(&index);
	ms::MissionKernel kernel;
	kernel.set_assets(&store);
	kernel.open_document(test_digest::people_mission(), "synth", source_over(&files));
	ms::KernelBootOptions options;
	if (!kernel.boot(options, error)) return false;
	if (kernel.root_motion.clip_count(0) == 0) {
		error = "the people's table registered no clips";
		return false;
	}
	Digest d;
	hash_world(d, kernel.world);
	for (int i = 0; i < ticks; ++i) {
		tick_no_net(kernel);
		hash_world(d, kernel.world);
	}
	out = d.h;
	return true;
}

// --- a retail install's mission (local tool) ---------------------------------

int install_chain(const std::string &root, const std::string &mission_name, int ticks, bool listen) {
	if (root.empty()) {
		std::fprintf(stderr, "FAIL: --mission needs --install <packed retail install>\n");
		return 1;
	}
	testrig::MissionRig rig;
	std::string error;
	if (!rig.open(root, mission_name, error)) {
		std::printf("FAIL: open %s: %s\n", mission_name.c_str(), error.c_str());
		return 1;
	}
	testrig::BootOptions options;
	options.listen_server = listen;
	// The listen host mints its host key, start tick and session seed from the
	// clock (the seed also seeds the world's CRT rand stream), so pin all three
	// or two runs of the same mission never agree.
	rig.host.host_key = 0x00C0FFEEu;
	rig.host.host_start_tick = 1000u;
	rig.host.session_seed_id = 123456u;
	if (!rig.boot(options, error)) {
		std::printf("FAIL: boot %s: %s\n", mission_name.c_str(), error.c_str());
		return 1;
	}
	Digest d;
	hash_world(d, rig.world);
	for (int i = 0; i < ticks; ++i) {
		rig.tick();
		hash_world(d, rig.world);
	}
	std::printf("digest %s %d %016llx\n", mission_name.c_str(), ticks,
			static_cast<unsigned long long>(d.h));
	return 0;
}

} // namespace

int main(int argc, char **argv) {
	std::string install;
	std::string mission;
	int ticks = kSyntheticTicks;
	bool listen = true;
	bool print = false;
	for (int i = 1; i < argc; ++i) {
		if (std::strcmp(argv[i], "--install") == 0 && i + 1 < argc) install = argv[++i];
		else if (std::strcmp(argv[i], "--mission") == 0 && i + 1 < argc) mission = argv[++i];
		else if (std::strcmp(argv[i], "--ticks") == 0 && i + 1 < argc) ticks = std::atoi(argv[++i]);
		else if (std::strcmp(argv[i], "--no-listen") == 0) listen = false;
		else if (std::strcmp(argv[i], "--print") == 0) print = true;
	}
	if (!mission.empty()) return install_chain(install, mission, ticks, listen);

	std::string error;
	uint64_t first = 0, second = 0;
	if (!synthetic_chain(ticks, first, error)) {
		std::printf("FAIL: synthetic boot: %s\n", error.c_str());
		return 1;
	}
	if (!synthetic_chain(ticks, second, error)) {
		std::printf("FAIL: synthetic boot (second): %s\n", error.c_str());
		return 1;
	}
	std::printf("digest synthetic %d %016llx\n", ticks, static_cast<unsigned long long>(first));
	if (first != second) {
		std::printf("FAIL: two identical boots diverged (%016llx vs %016llx): the tick is not "
					"deterministic\n",
				static_cast<unsigned long long>(first), static_cast<unsigned long long>(second));
		return 1;
	}
	if (print) return 0;
	if (ticks == kSyntheticTicks && first != kSyntheticDigest) {
		std::printf("FAIL: synthetic digest %016llx != committed %016llx. A structural slice must "
					"not move it; a ledgered behavior change re-pins kSyntheticDigest and says why.\n",
				static_cast<unsigned long long>(first),
				static_cast<unsigned long long>(kSyntheticDigest));
		return 1;
	}
	std::printf("tick_digest: OK\n");
	return 0;
}
