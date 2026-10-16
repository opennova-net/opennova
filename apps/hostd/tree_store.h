#pragma once

// The trees on the machine (ADR 0064): under the trees root (/opt/game) each
// tree the service's replies name is fetched from the object store into
// <root>/<id>.partial/ (one fetch at a time, through the TreeFetcher seam: the
// real one runs `aws s3 sync` as a background child, or copies a file:///
// directory on a developer's machine), then checked against the reply's
// manifest (every file present with its size, so an interrupted sync is
// caught; any other file but manifest.json removed), renamed to <root>/<id>/
// and marked with a .ready file. A container's base tree is bound read-only at
// /opt/game, its expansion tree at /opt/game/expansion/<code>, whose mount
// point the store makes under the base tree on the host (a read-only bind
// cannot have one made in it). A fetch or a check that fails leaves the tree
// failed with the reason, tried again kRetryAfterMs later while a reply still
// names it, kMaxTries times in all, then failed until no reply names it. A
// present tree no reply has named for kUnnamedGraceMs is removed (gc). A
// restarted agent finds its trees again: a directory with the marker is
// present, one without it (a fetch cut short) goes. The store never decides
// what a container does with a tree (reconcile.h does): the planner reads a
// tree's standing instead, where a failure with a try left still counts as
// fetching (the container keeps waiting through a passing S3 failure, instead
// of spending its server's restarts) and only the tries used up fail it, the
// last failure's reason then riding the container's report to the service.
// Service policy, not a port.

#include "hosted/agent_protocol.h"

#include <base/proc/child_process.h>

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace opennova::hostd {

namespace hosted = novaworld_server::hosted;

// Where the fetch under way stands (TreeFetcher::poll).
struct FetchPoll {
	bool running = true;
	bool ok = false; // ended: the sync or the copy ran to its end
	std::string error; // ended and not ok: why
};

// The seam a tree's files come through. ProcessTreeFetcher is the real one; the
// tests' fake answers from memory. One fetch at a time: the store never starts
// a second before the first ended or was cancelled.
class TreeFetcher {
public:
	virtual ~TreeFetcher() = default;
	// Starts fetching the tree at `url` (s3://... or file:///...) into `dir`, an
	// empty directory the store made, and returns at once. False with `error`
	// when nothing started.
	virtual bool start(const std::string &url, const std::string &dir, std::string &error) = 0;
	// The fetch under way: running, or ended (ok, or `error` says why) and then
	// forgotten. With none under way, ended with an error.
	virtual FetchPoll poll() = 0;
	// Ends the fetch under way now, and forgets it.
	virtual void cancel() = 0;
};

// The real fetcher. An s3:// URL runs `aws s3 sync <url> <dir> --no-progress
// --only-show-errors` as a child of its own (proc::ChildProcess, as
// DockerRuntime runs a pull: its standard error in a scratch file for the
// failure's text, polled, ended with this process), with `environment` alone
// (the machine's credentials come from its instance role). A file:/// URL (a
// developer's machine) copies the directory at the call, so its poll answers
// at once.
class ProcessTreeFetcher : public TreeFetcher {
public:
	// `aws`: the CLI's absolute path, run as given (no PATH search).
	ProcessTreeFetcher(
			std::string aws, std::vector<std::string> environment, std::string scratch_dir);
	~ProcessTreeFetcher() override;

	bool start(const std::string &url, const std::string &dir, std::string &error) override;
	FetchPoll poll() override;
	void cancel() override;

	// The argv a sync of `url` into `dir` runs.
	std::vector<std::string> sync_argv(const std::string &url, const std::string &dir) const;

private:
	std::string aws_;
	std::vector<std::string> environment_;
	std::string scratch_dir_;
	proc::ChildProcess child_;
	bool child_running_ = false;
	std::string error_path_;
	uint64_t fetches_ = 0; // names the error files
	std::optional<FetchPoll> copied_; // a file:/// copy's outcome, for the next poll
};

// Where a tree stands in the store.
enum class TreeFetchState : uint8_t {
	Present, // fetched and checked against its manifest
	Fetching, // a fetch in flight (or waiting its turn)
	Failed, // the last fetch failed (the error says why)
};

// What the store holds of one tree (state_of): its state, its bytes on disk
// (present: the tree's; fetching: so far), and the last failure's reason.
struct TreeStatus {
	TreeFetchState state = TreeFetchState::Fetching;
	uint64_t bytes = 0;
	std::string error;
};

// How the planner takes a tree (reconcile.h): Present; Fetching while the
// store fetches it, has it queued, or will try it again (a failure with a try
// left); Failed once the tries are spent, with the last failure's reason.
struct TreeStanding {
	TreeFetchState state = TreeFetchState::Fetching;
	std::string error;
};

class TreeStore {
public:
	using Clock = std::function<uint64_t()>; // a steady clock, ms

	// A failed fetch is tried again this much later (a passing S3 failure
	// costs a minute, not a server's restart), kMaxTries times in all.
	static constexpr uint64_t kRetryAfterMs = 60 * 1000;
	static constexpr int kMaxTries = 3;
	static constexpr uint64_t kUnnamedGraceMs = 30 * 60 * 1000;
	static constexpr const char *kReadyMarker = ".ready";
	static constexpr const char *kPartialSuffix = ".partial";
	static constexpr const char *kManifestFile = "manifest.json";
	static constexpr const char *kExpansionDir = "expansion";

	// `fetcher` must outlive the store. The root is scanned at once: a tree
	// directory with the marker is present (its bytes summed), one without it
	// and every .partial directory are removed. A root that is missing holds no
	// tree (the embedder makes it).
	TreeStore(std::string root, TreeFetcher &fetcher, Clock now_ms = {});

	const std::string &root() const { return root_; }
	// <root>/<id> and <root>/<id>.partial.
	std::string dir_of(int64_t id) const;
	std::string partial_dir_of(int64_t id) const;

	// The trees a reply names, with their manifests: each is noted as named now;
	// one the store lacks is queued (fetching). The fetch under way is polled:
	// ended well, its tree is checked, renamed and marked present; ended badly
	// or failing its check, the tree is failed with the reason (its partial
	// directory removed). With no fetch under way, the first named tree (by id)
	// that is queued, or failed with a try left and its wait over, is started.
	void want(const std::map<int64_t, hosted::TreeSpec> &trees);
	// Lets go of what `wanted` does not name: a present tree unnamed for
	// kUnnamedGraceMs is removed, a queued or failed one forgotten, the fetch
	// under way cancelled and its partial directory removed.
	void gc(const std::set<int64_t> &wanted);

	// What the store holds of tree `id`; nullopt for one it holds nothing of.
	std::optional<TreeStatus> state_of(int64_t id) const;
	// Every tree's standing, for the planner (TreeStanding).
	std::map<int64_t, TreeStanding> standings() const;

	// Makes <root>/<base>/expansion/<code> under the present base tree
	// `base_id` and returns it; "" with `error` when it cannot be made.
	std::string expansion_mount_point(int64_t base_id, const std::string &code, std::string &error);

	// The fetch under way's tree, if any (for the tests).
	std::optional<int64_t> fetching() const { return fetching_; }

private:
	struct Entry {
		TreeFetchState state = TreeFetchState::Fetching;
		uint64_t bytes = 0; // present: on disk
		std::string error; // failed: the last attempt's reason
		int tries = 0; // the fetches made
		uint64_t failed_at_ms = 0;
		uint64_t named_at_ms = 0; // when a reply last named it
		hosted::TreeSpec spec; // the manifest, kept from the reply that named it
	};

	void scan_root();
	bool start_fetch(int64_t id, Entry &entry);
	void finish_fetch(int64_t id, Entry &entry, const FetchPoll &ended);
	void fail(int64_t id, Entry &entry, const std::string &why);
	// The partial directory checked against the manifest: every file there with
	// its size, every other entry but the manifest, the expansion directory and
	// the marker removed; the files' bytes in `bytes`.
	bool verify(const std::string &dir, const hosted::TreeSpec &spec, uint64_t &bytes,
			std::string &error) const;
	void remove_partial(int64_t id);

	std::string root_;
	TreeFetcher &fetcher_;
	Clock now_;
	std::map<int64_t, Entry> trees_;
	std::optional<int64_t> fetching_; // the fetch under way's tree
};

// The bytes of the regular files right under `dir` (the marker and the
// expansion directory left out); 0 for no directory.
uint64_t tree_bytes_on_disk(const std::string &dir);

} // namespace opennova::hostd
