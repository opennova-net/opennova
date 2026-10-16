// opennova-hostd's tree store (apps/hostd/tree_store.h) on a fake fetcher and a
// hand-stepped clock: a tree the reply names fetched into its partial directory (one
// fetch at a time, the lowest id first), checked against the manifest (every file there
// with its size; a size off, or a file missing, fails it with the reason; any other file
// but the manifest removed), renamed and marked present with its bytes; a fetch that
// fails, or one that does not start, failed with the reason (held so at once, but
// standing as fetching for the planner while a try is left) and tried
// again a minute later, three times in all, then failed until no reply names it;
// what the store lets go of (a present tree no reply named for half an hour, a queued
// or failed one at once, the fetch under way cancelled); a restart's scan of the root
// (the marked directories present, an unmarked or partial one removed); the expansion
// mount point made under the base tree. Then the real fetcher: the file:// copy on a
// real directory, and `aws s3 sync` run by stand-in programs (ended, failed with its
// standard error's first line, cancelled). No AWS CLI runs.

#include "tree_store.h"

#include <base/io/log.h>

#include "common/temp_dir.h"
#include "common/test_expect.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <sys/stat.h>

namespace hostd = opennova::hostd;
namespace hosted = opennova::novaworld_server::hosted;
namespace fs = std::filesystem;
using hostd::TreeFetchState;

namespace {

std::string g_log;
void capture_log(opennova::io::LogLevel, const char *message) {
	g_log += message;
	g_log += '\n';
}

using Files = std::map<std::string, std::string>; // name -> bytes

void write_file(const std::string &path, const std::string &bytes) {
	std::ofstream(path, std::ios::binary) << bytes;
}

std::string read_text(const std::string &path) {
	std::ifstream in(path, std::ios::binary);
	std::ostringstream out;
	out << in.rdbuf();
	return out.str();
}

void write_tree(const std::string &dir, const Files &files) {
	fs::create_directories(dir);
	for (const auto &file : files)
		write_file(dir + "/" + file.first, file.second);
}

// The manifest of `files`, as the service's reply carries it.
hosted::TreeSpec spec_of(const Files &files, const std::string &url, const std::string &version) {
	hosted::TreeSpec spec;
	spec.url = url;
	spec.version = version;
	for (const auto &file : files) {
		hosted::TreeFileSpec f;
		f.name = file.first;
		f.size = file.second.size();
		spec.files.push_back(f);
	}
	return spec;
}

uint64_t bytes_of(const Files &files) {
	uint64_t n = 0;
	for (const auto &file : files)
		n += file.second.size();
	return n;
}

const Files kBase = { { "resource.pff", "RESOURCE" }, { "localres.pff", "LOCAL" },
	{ "language.pff", "LANG" }, { "version.txt", "1.7.5.3\r\n" } };
const Files kExpansion = { { "jox01.pff", "ESCALATION" }, { "jox01L.pff", "ESC-L" },
	{ "version.txt", "1.0.0.1\r\n" } };

// Records each start; the test ends the fetch under way with end().
class FakeFetcher : public hostd::TreeFetcher {
public:
	struct Fetch {
		std::string url, dir;
	};
	std::vector<Fetch> starts;
	bool refuse = false;
	bool running = false;
	int cancels = 0;
	int overlaps = 0; // starts made while one ran (never)

	bool start(const std::string &url, const std::string &dir, std::string &error) override {
		if (refuse) {
			error = "aws s3 sync did not start: no such file";
			return false;
		}
		overlaps += running;
		starts.push_back({ url, dir });
		running = true;
		outcome_.reset();
		return true;
	}
	hostd::FetchPoll poll() override {
		hostd::FetchPoll p;
		if (!running) {
			p.running = false;
			p.error = "no fetch is under way";
			return p;
		}
		if (!outcome_)
			return p;
		running = false;
		p = *outcome_;
		outcome_.reset();
		return p;
	}
	void cancel() override {
		++cancels;
		running = false;
		outcome_.reset();
	}
	void end(bool ok, const std::string &error = "") {
		hostd::FetchPoll p;
		p.running = false;
		p.ok = ok;
		p.error = error;
		outcome_ = p;
	}
	const std::string &dir() const { return starts.back().dir; }

private:
	std::optional<hostd::FetchPoll> outcome_;
};

struct Rig {
	test_temp::TempDir temp;
	std::string root;
	FakeFetcher fetcher;
	uint64_t clock_ms = 1000;
	std::unique_ptr<hostd::TreeStore> store;

	explicit Rig(const char *name) : temp(name) {
		root = temp.file("trees");
		fs::create_directories(root);
		make_store();
	}
	void make_store() {
		store = std::make_unique<hostd::TreeStore>(root, fetcher, [this] { return clock_ms; });
	}
	// Ends the fetch under way with `files` fetched, and lets the store see it.
	void fetched(const Files &files, const std::map<int64_t, hosted::TreeSpec> &want) {
		write_tree(fetcher.dir(), files);
		fetcher.end(true);
		store->want(want);
	}
	// Tree `id`'s state; Failed with "(none)" as its error for one the store holds nothing of.
	TreeFetchState state(int64_t id) const {
		const auto s = store->state_of(id);
		return s ? s->state : TreeFetchState::Failed;
	}
	~Rig() {
		if (fetcher.overlaps)
			std::fprintf(stderr, "the store started a fetch while one ran\n");
	}
};

int test_fetch_and_verify() {
	Rig rig("hostd_tree_fetch");
	const std::map<int64_t, hosted::TreeSpec> want = { { 1,
			spec_of(kBase, "s3://bucket/data/dep/trees/1/", "1.7.5.3") } };
	TEST_EXPECT(rig.store->standings().empty() && !rig.store->state_of(1));
	// Named: fetched into its partial directory, fetching.
	rig.store->want(want);
	TEST_EXPECT(rig.fetcher.starts.size() == 1 && rig.store->fetching() == 1);
	TEST_EXPECT(rig.fetcher.starts[0].url == "s3://bucket/data/dep/trees/1/");
	TEST_EXPECT(
			rig.fetcher.dir() == rig.root + "/1.partial" && fs::is_directory(rig.fetcher.dir()));
	TEST_EXPECT(rig.state(1) == TreeFetchState::Fetching && rig.store->state_of(1)->bytes == 0);
	TEST_EXPECT(rig.store->standings().size() == 1 && rig.store->standings().count(1) == 1);
	// Still running: no second fetch, the bytes so far counted.
	rig.store->want(want);
	TEST_EXPECT(rig.fetcher.starts.size() == 1);
	write_tree(rig.fetcher.dir(), { { "resource.pff", "RESOURCE" }, { "localres.pff", "LO" } });
	TEST_EXPECT(rig.store->state_of(1)->bytes == 10u);
	// Ended: checked, the extras removed, the manifest kept, renamed and marked.
	Files fetched = kBase;
	fetched["manifest.json"] = "{\"tree\": 1}";
	fetched["leftover.tmp"] = "xx";
	write_tree(rig.fetcher.dir() + "/sub", { { "deep", "y" } });
	rig.fetched(fetched, want);
	TEST_EXPECT(rig.state(1) == TreeFetchState::Present && !rig.store->fetching());
	TEST_EXPECT(rig.store->state_of(1)->bytes == bytes_of(kBase));
	TEST_EXPECT(rig.store->state_of(1)->error.empty());
	const std::string dir = rig.store->dir_of(1);
	TEST_EXPECT(dir == rig.root + "/1" && fs::is_regular_file(dir + "/.ready"));
	TEST_EXPECT(!fs::exists(rig.root + "/1.partial"));
	TEST_EXPECT(read_text(dir + "/resource.pff") == "RESOURCE" &&
			read_text(dir + "/manifest.json") == "{\"tree\": 1}");
	TEST_EXPECT(!fs::exists(dir + "/leftover.tmp") && !fs::exists(dir + "/sub"));
	// Present: named again, nothing fetches.
	rig.store->want(want);
	TEST_EXPECT(rig.fetcher.starts.size() == 1 && rig.state(1) == TreeFetchState::Present);
	TEST_EXPECT(rig.store->standings().size() == 1 &&
			rig.store->standings().at(1).state == TreeFetchState::Present);
	TEST_EXPECT(g_log.find("tree 1 is present (26 bytes, version 1.7.5.3)") != std::string::npos);
	return 0;
}

int test_verify_failures() {
	Rig rig("hostd_tree_verify");
	const std::map<int64_t, hosted::TreeSpec> want = { { 2,
			spec_of(kBase, "s3://bucket/data/dep/trees/2/", "1.7.5.3") } };
	// A size off.
	rig.store->want(want);
	Files bad = kBase;
	bad["resource.pff"] = "RES";
	rig.fetched(bad, want);
	TEST_EXPECT(rig.state(2) == TreeFetchState::Failed);
	TEST_EXPECT(rig.store->state_of(2)->error == "resource.pff is 3 bytes, not 8");
	TEST_EXPECT(!fs::exists(rig.root + "/2.partial") && !fs::exists(rig.root + "/2"));
	// A file missing.
	rig.clock_ms += hostd::TreeStore::kRetryAfterMs;
	rig.store->want(want);
	TEST_EXPECT(rig.fetcher.starts.size() == 2);
	bad = kBase;
	bad.erase("localres.pff");
	rig.fetched(bad, want);
	TEST_EXPECT(rig.store->state_of(2)->error == "localres.pff is missing");
	TEST_EXPECT(rig.state(2) == TreeFetchState::Failed);
	// Other bytes of the same size pass: the sizes alone are checked, the sync trusted.
	rig.clock_ms += hostd::TreeStore::kRetryAfterMs;
	rig.store->want(want);
	TEST_EXPECT(rig.fetcher.starts.size() == 3);
	bad = kBase;
	bad["resource.pff"] = "RESOURCF";
	rig.fetched(bad, want);
	TEST_EXPECT(rig.state(2) == TreeFetchState::Present && rig.store->state_of(2)->error.empty());

	// The fetch itself failing, with its reason.
	const std::map<int64_t, hosted::TreeSpec> other = { { 3,
			spec_of(kBase, "s3://bucket/data/dep/trees/3/", "1.7.5.3") } };
	rig.store->want(other);
	rig.fetcher.end(false, "aws s3 sync exited 1: fatal error: Unable to locate credentials");
	rig.store->want(other);
	TEST_EXPECT(rig.state(3) == TreeFetchState::Failed);
	TEST_EXPECT(rig.store->state_of(3)->error ==
			"aws s3 sync exited 1: fatal error: Unable to locate credentials");
	TEST_EXPECT(!fs::exists(rig.root + "/3.partial"));
	// One that does not start.
	const std::map<int64_t, hosted::TreeSpec> another = { { 4,
			spec_of(kBase, "s3://bucket/data/dep/trees/4/", "1.7.5.3") } };
	rig.fetcher.refuse = true;
	rig.store->want(another);
	TEST_EXPECT(rig.state(4) == TreeFetchState::Failed && !rig.store->fetching());
	TEST_EXPECT(rig.store->state_of(4)->error == "aws s3 sync did not start: no such file");
	TEST_EXPECT(!fs::exists(rig.root + "/4.partial"));
	TEST_EXPECT(g_log.find("tree 2 did not fetch (try 1 of 3): resource.pff is 3 bytes, not 8") !=
			std::string::npos);
	return 0;
}

int test_retries() {
	Rig rig("hostd_tree_retries");
	const std::map<int64_t, hosted::TreeSpec> want = { { 1,
			spec_of(kBase, "s3://bucket/data/dep/trees/1/", "1.7.5.3") } };
	rig.store->want(want);
	TEST_EXPECT(rig.store->standings().at(1).state == TreeFetchState::Fetching);
	rig.fetcher.end(false, "boom");
	rig.store->want(want);
	TEST_EXPECT(rig.state(1) == TreeFetchState::Failed && rig.fetcher.starts.size() == 1);
	// Reported failed with the reason; for the planner still a fetch to come.
	TEST_EXPECT(rig.store->state_of(1)->error == "boom");
	TEST_EXPECT(rig.store->standings().at(1).state == TreeFetchState::Fetching &&
			rig.store->standings().at(1).error.empty());
	// Not before the minute.
	TEST_EXPECT(hostd::TreeStore::kRetryAfterMs == 60 * 1000 && hostd::TreeStore::kMaxTries == 3);
	rig.clock_ms += hostd::TreeStore::kRetryAfterMs - 1;
	rig.store->want(want);
	TEST_EXPECT(rig.fetcher.starts.size() == 1 && rig.state(1) == TreeFetchState::Failed);
	rig.clock_ms += 1;
	rig.store->want(want);
	TEST_EXPECT(rig.fetcher.starts.size() == 2 && rig.state(1) == TreeFetchState::Fetching);
	TEST_EXPECT(rig.store->state_of(1)->error.empty());
	rig.fetcher.end(false, "boom again");
	rig.store->want(want);
	TEST_EXPECT(rig.store->state_of(1)->error == "boom again");
	TEST_EXPECT(rig.store->standings().at(1).state == TreeFetchState::Fetching);
	rig.clock_ms += hostd::TreeStore::kRetryAfterMs;
	rig.store->want(want);
	TEST_EXPECT(rig.fetcher.starts.size() == 3);
	rig.fetcher.end(false, "and again");
	rig.store->want(want);
	// The third failure is the last try: failed, for the planner too, while the
	// tree is named.
	TEST_EXPECT(rig.store->standings().at(1).state == TreeFetchState::Failed &&
			rig.store->standings().at(1).error == "and again");
	rig.clock_ms += hostd::TreeStore::kRetryAfterMs * 6;
	rig.store->want(want);
	TEST_EXPECT(rig.fetcher.starts.size() == 3 && rig.state(1) == TreeFetchState::Failed);
	TEST_EXPECT(rig.store->state_of(1)->error == "and again");
	TEST_EXPECT(g_log.find("(try 3 of 3): and again") != std::string::npos);
	// No reply naming it: forgotten, and the next naming starts over.
	rig.store->gc({});
	TEST_EXPECT(!rig.store->state_of(1) && !rig.store->standings().count(1));
	rig.store->want(want);
	TEST_EXPECT(rig.fetcher.starts.size() == 4 && rig.state(1) == TreeFetchState::Fetching);
	rig.fetched(kBase, want);
	TEST_EXPECT(rig.state(1) == TreeFetchState::Present);
	return 0;
}

int test_one_at_a_time() {
	Rig rig("hostd_tree_queue");
	std::map<int64_t, hosted::TreeSpec> want = {
		{ 2, spec_of(kExpansion, "s3://bucket/data/dep/trees/2/", "1.0.0.1") },
		{ 1, spec_of(kBase, "s3://bucket/data/dep/trees/1/", "1.7.5.3") },
	};
	// Both named: the lowest id fetches, the other waits its turn (fetching too).
	rig.store->want(want);
	TEST_EXPECT(rig.fetcher.starts.size() == 1 && rig.store->fetching() == 1);
	TEST_EXPECT(
			rig.state(1) == TreeFetchState::Fetching && rig.state(2) == TreeFetchState::Fetching);
	TEST_EXPECT(rig.store->standings().size() == 2);
	rig.fetched(kBase, want);
	TEST_EXPECT(rig.state(1) == TreeFetchState::Present && rig.fetcher.starts.size() == 2);
	TEST_EXPECT(rig.store->fetching() == 2 && rig.fetcher.dir() == rig.root + "/2.partial");
	rig.fetched(kExpansion, want);
	TEST_EXPECT(rig.state(2) == TreeFetchState::Present && !rig.store->fetching());
	TEST_EXPECT(rig.store->state_of(2)->bytes == bytes_of(kExpansion));
	TEST_EXPECT(rig.fetcher.overlaps == 0);
	// The fetch under way of a tree no reply names any more: ended, its partial gone.
	want = { { 3, spec_of(kBase, "s3://bucket/data/dep/trees/3/", "1.7.5.4") } };
	rig.store->want(want);
	TEST_EXPECT(rig.store->fetching() == 3 && fs::is_directory(rig.root + "/3.partial"));
	rig.store->gc({ 1, 2 });
	TEST_EXPECT(rig.fetcher.cancels == 1 && !rig.store->fetching() && !rig.store->state_of(3));
	TEST_EXPECT(!fs::exists(rig.root + "/3.partial"));
	TEST_EXPECT(rig.state(1) == TreeFetchState::Present && rig.state(2) == TreeFetchState::Present);
	// A tree queued behind another and dropped before its turn is forgotten.
	want = { { 4, spec_of(kBase, "s3://bucket/data/dep/trees/4/", "1.7.5.4") },
		{ 5, spec_of(kBase, "s3://bucket/data/dep/trees/5/", "1.7.5.4") } };
	rig.store->want(want);
	TEST_EXPECT(rig.store->fetching() == 4 && rig.state(5) == TreeFetchState::Fetching);
	rig.store->gc({ 4 });
	TEST_EXPECT(!rig.store->state_of(5) && rig.store->fetching() == 4);
	TEST_EXPECT(rig.fetcher.cancels == 1);
	return 0;
}

int test_gc() {
	Rig rig("hostd_tree_gc");
	std::map<int64_t, hosted::TreeSpec> want = {
		{ 1, spec_of(kBase, "s3://bucket/data/dep/trees/1/", "1.7.5.3") },
		{ 2, spec_of(kExpansion, "s3://bucket/data/dep/trees/2/", "1.0.0.1") },
	};
	rig.store->want(want);
	rig.fetched(kBase, want);
	rig.fetched(kExpansion, want);
	TEST_EXPECT(rig.state(1) == TreeFetchState::Present && rig.state(2) == TreeFetchState::Present);
	// Unnamed, a present tree stays for half an hour.
	rig.store->gc({});
	rig.clock_ms += hostd::TreeStore::kUnnamedGraceMs - 1;
	rig.store->gc({});
	TEST_EXPECT(rig.state(1) == TreeFetchState::Present && fs::is_directory(rig.root + "/1"));
	// Named again meanwhile, tree 1 survives; tree 2 goes with its directory.
	want.erase(2);
	rig.store->want(want);
	rig.clock_ms += 1;
	rig.store->gc({ 1 });
	TEST_EXPECT(rig.state(1) == TreeFetchState::Present && fs::is_directory(rig.root + "/1"));
	TEST_EXPECT(!rig.store->state_of(2) && !fs::exists(rig.root + "/2"));
	TEST_EXPECT(g_log.find("removing tree 2: no reply named it for 30 min") != std::string::npos);
	rig.clock_ms += hostd::TreeStore::kUnnamedGraceMs;
	rig.store->gc({});
	TEST_EXPECT(!rig.store->state_of(1) && !fs::exists(rig.root + "/1"));
	TEST_EXPECT(rig.store->standings().empty());
	return 0;
}

int test_restart() {
	Rig rig("hostd_tree_restart");
	// What a restarted agent finds: a marked tree (its expansion mount point in
	// it), one renamed but never marked, a partial, and what is not a tree's.
	write_tree(rig.root + "/5", kBase);
	write_file(rig.root + "/5/.ready", "");
	fs::create_directories(rig.root + "/5/expansion/jox01");
	write_tree(rig.root + "/6", kBase);
	write_tree(rig.root + "/7.partial", { { "resource.pff", "RESO" } });
	fs::create_directories(rig.root + "/lost+found");
	write_tree(rig.root + "/08", kBase);
	write_file(rig.root + "/9", "a file, not a tree");
	rig.make_store();
	TEST_EXPECT(rig.state(5) == TreeFetchState::Present);
	TEST_EXPECT(rig.store->state_of(5)->bytes == bytes_of(kBase));
	TEST_EXPECT(!rig.store->state_of(6) && !fs::exists(rig.root + "/6"));
	TEST_EXPECT(!rig.store->state_of(7) && !fs::exists(rig.root + "/7.partial"));
	TEST_EXPECT(fs::is_directory(rig.root + "/lost+found") && fs::is_directory(rig.root + "/08"));
	TEST_EXPECT(fs::is_regular_file(rig.root + "/9") && !rig.store->state_of(9));
	TEST_EXPECT(rig.store->standings().size() == 1);
	// Found at the start: it stays its grace, then goes unnamed.
	rig.store->gc({});
	TEST_EXPECT(rig.state(5) == TreeFetchState::Present);
	rig.clock_ms += hostd::TreeStore::kUnnamedGraceMs;
	rig.store->gc({});
	TEST_EXPECT(!rig.store->state_of(5) && !fs::exists(rig.root + "/5"));
	// A root that is missing holds nothing.
	hostd::TreeStore none(rig.temp.file("nowhere"), rig.fetcher, [&rig] { return rig.clock_ms; });
	TEST_EXPECT(none.standings().empty());
	return 0;
}

int test_expansion_mount_point() {
	Rig rig("hostd_tree_mount_point");
	const std::map<int64_t, hosted::TreeSpec> want = { { 1,
			spec_of(kBase, "s3://bucket/data/dep/trees/1/", "1.7.5.3") } };
	std::string error;
	TEST_EXPECT(rig.store->expansion_mount_point(1, "jox01", error).empty() &&
			error == "tree 1 is not present");
	rig.store->want(want);
	rig.fetched(kBase, want);
	const std::string point = rig.store->expansion_mount_point(1, "jox01", error);
	TEST_EXPECT(point == rig.root + "/1/expansion/jox01" && fs::is_directory(point));
	// Made again: the same, and the tree's bytes unchanged.
	TEST_EXPECT(rig.store->expansion_mount_point(1, "jox01", error) == point);
	TEST_EXPECT(rig.store->state_of(1)->bytes == bytes_of(kBase));
	TEST_EXPECT(rig.store->expansion_mount_point(1, "JOX01", error).empty() &&
			error == "no expansion code");
	TEST_EXPECT(rig.store->expansion_mount_point(1, "../x", error).empty());
	TEST_EXPECT(hostd::tree_bytes_on_disk(rig.root + "/1") == bytes_of(kBase));
	return 0;
}

// The real fetcher: a file:// URL copied at the call (a developer's store), the
// `aws s3 sync` argv, and the sync run by programs that stand in for the CLI.
int test_process_fetcher() {
	test_temp::TempDir temp("hostd_tree_process_fetcher");
	const std::string source = temp.file("store/trees/1");
	Files files = kBase;
	files["manifest.json"] = "{}";
	write_tree(source, files);
	hostd::ProcessTreeFetcher copier(temp.file("no-such-aws"), {}, temp.root());
	TEST_EXPECT(copier.sync_argv("s3://bucket/data/dep/trees/1/", "/opt/game/1.partial") ==
			(std::vector<std::string>{ temp.file("no-such-aws"), "s3", "sync",
					"s3://bucket/data/dep/trees/1/", "/opt/game/1.partial", "--no-progress",
					"--only-show-errors" }));
	std::string error;
	const std::string dest = temp.file("trees/1.partial");
	fs::create_directories(dest);
	TEST_EXPECT(copier.start("file://" + source + "/", dest, error));
	hostd::FetchPoll poll = copier.poll();
	TEST_EXPECT(!poll.running && poll.ok && poll.error.empty());
	for (const auto &file : files)
		TEST_EXPECT(read_text(dest + "/" + file.first) == file.second);
	// Ended, it is forgotten.
	poll = copier.poll();
	TEST_EXPECT(!poll.running && !poll.ok);
	// A directory that is not there.
	TEST_EXPECT(copier.start("file://" + temp.file("store/trees/2") + "/", dest, error));
	poll = copier.poll();
	TEST_EXPECT(
			!poll.running && !poll.ok && poll.error.find("is no directory") != std::string::npos);
	// The store over it, end to end.
	{
		const std::string root = temp.file("trees2");
		fs::create_directories(root);
		hostd::TreeStore store(root, copier);
		const std::map<int64_t, hosted::TreeSpec> want = { { 1,
				spec_of(kBase, "file://" + source + "/", "1.7.5.3") } };
		store.want(want);
		TEST_EXPECT(store.state_of(1)->state == TreeFetchState::Fetching);
		store.want(want);
		TEST_EXPECT(store.state_of(1)->state == TreeFetchState::Present);
		TEST_EXPECT(store.state_of(1)->bytes == bytes_of(kBase));
		TEST_EXPECT(read_text(root + "/1/manifest.json") == "{}" &&
				fs::is_regular_file(root + "/1/.ready"));
	}

	const auto poll_until_done = [](hostd::TreeFetcher &f) {
		hostd::FetchPoll p;
		for (int i = 0; i < 500; ++i) {
			p = f.poll();
			if (!p.running)
				return p;
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
		return p;
	};
	// A sync that succeeds; a second start while it runs is refused.
	hostd::ProcessTreeFetcher ok("/bin/true", {}, temp.root());
	TEST_EXPECT(ok.start("s3://bucket/data/dep/trees/1/", dest, error));
	TEST_EXPECT(!ok.start("s3://bucket/data/dep/trees/2/", dest, error) &&
			error == "a fetch is already under way");
	poll = poll_until_done(ok);
	TEST_EXPECT(!poll.running && poll.ok);
	// One that fails, with its standard error's first line.
	const std::string failing = temp.file("failing-aws");
	{
		std::ofstream script(failing);
		script << "#!/bin/sh\necho \"fatal error: An error occurred (403) when calling the "
				  "ListObjectsV2 operation: Forbidden\" >&2\necho more >&2\nexit 1\n";
	}
	::chmod(failing.c_str(), 0755);
	hostd::ProcessTreeFetcher fails(failing, {}, temp.root());
	TEST_EXPECT(fails.start("s3://bucket/data/dep/trees/1/", dest, error));
	poll = poll_until_done(fails);
	TEST_EXPECT(!poll.running && !poll.ok);
	TEST_EXPECT(poll.error ==
			"aws s3 sync exited 1: fatal error: An error occurred (403) when calling the "
			"ListObjectsV2 operation: Forbidden");
	// One that runs on until it is ended.
	const std::string slow = temp.file("slow-aws");
	{
		std::ofstream script(slow);
		script << "#!/bin/sh\nexec sleep 30\n";
	}
	::chmod(slow.c_str(), 0755);
	hostd::ProcessTreeFetcher slow_fetch(slow, {}, temp.root());
	TEST_EXPECT(slow_fetch.start("s3://bucket/data/dep/trees/1/", dest, error));
	TEST_EXPECT(slow_fetch.poll().running);
	slow_fetch.cancel();
	poll = slow_fetch.poll();
	TEST_EXPECT(!poll.running && !poll.ok && poll.error == "no fetch is under way");
	// One whose CLI is not there does not start.
	hostd::ProcessTreeFetcher no_cli(temp.file("no-such-aws"), {}, temp.root());
	TEST_EXPECT(!no_cli.start("s3://bucket/data/dep/trees/1/", dest, error) &&
			error.find("aws s3 sync did not start") == 0);
	// No error file stays behind.
	size_t left = 0;
	for (const auto &entry : fs::directory_iterator(temp.root()))
		left += entry.path().filename().string().rfind("fetch-", 0) == 0;
	TEST_EXPECT(left == 0);
	return 0;
}

} // namespace

int main() {
	opennova::io::set_log_sink(capture_log);
	struct Case {
		const char *name;
		int (*fn)();
	};
	const Case cases[] = {
		{ "fetch_and_verify", test_fetch_and_verify },
		{ "verify_failures", test_verify_failures },
		{ "retries", test_retries },
		{ "one_at_a_time", test_one_at_a_time },
		{ "gc", test_gc },
		{ "restart", test_restart },
		{ "expansion_mount_point", test_expansion_mount_point },
		{ "process_fetcher", test_process_fetcher },
	};
	for (const Case &c : cases) {
		std::printf("-- %s\n", c.name);
		if (c.fn() != 0) {
			std::fprintf(stderr, "FAIL: %s\n%s", c.name, g_log.c_str());
			return 1;
		}
	}
	std::printf("OK: opennova-hostd tree store\n");
	return 0;
}
