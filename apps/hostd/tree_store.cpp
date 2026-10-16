#include "tree_store.h"

#include <base/io/file_io.h>
#include <base/io/log.h>
#include <base/io/strutil.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>
#include <utility>

namespace opennova::hostd {

namespace {

namespace fs = std::filesystem;
using io::LogLevel;

uint64_t steady_now_ms() {
	return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now().time_since_epoch())
					.count());
}

// The first line of a CLI's standard error, at most 200 bytes.
std::string first_line(const std::string &text) {
	std::string err = strutil::trim(text);
	if (const size_t eol = err.find('\n'); eol != std::string::npos)
		err.resize(eol);
	if (err.size() > 200)
		err.resize(200);
	return err;
}

// The error file's first line, and the file gone.
std::string take_error(const std::string &path) {
	if (path.empty())
		return {};
	const fs::path file = fs::u8path(path);
	std::string text;
	{
		std::ifstream in(file, std::ios::binary);
		std::ostringstream read;
		read << in.rdbuf();
		text = read.str();
	}
	std::error_code ec;
	fs::remove(file, ec);
	return first_line(text);
}

// A tree directory's name: its id, or nullopt for anything else.
std::optional<int64_t> tree_id_of(const std::string &name) {
	if (!strutil::all_digits(name) || name.size() > 15 || name[0] == '0')
		return std::nullopt;
	return strutil::parse_llong(name);
}

} // namespace

// --- the real fetcher ------------------------------------------------------------

ProcessTreeFetcher::ProcessTreeFetcher(
		std::string aws, std::vector<std::string> environment, std::string scratch_dir) :
		aws_(std::move(aws)),
		environment_(std::move(environment)),
		scratch_dir_(std::move(scratch_dir)) {}

ProcessTreeFetcher::~ProcessTreeFetcher() {
	cancel();
}

std::vector<std::string> ProcessTreeFetcher::sync_argv(
		const std::string &url, const std::string &dir) const {
	return { aws_, "s3", "sync", url, dir, "--no-progress", "--only-show-errors" };
}

bool ProcessTreeFetcher::start(const std::string &url, const std::string &dir, std::string &error) {
	if (child_running_ || copied_) {
		error = "a fetch is already under way";
		return false;
	}
	if (strutil::starts_with_icase(url, "file://")) {
		// A developer's store: the directory copied here and now.
		const fs::path source = fs::u8path(url.substr(7));
		std::error_code ec;
		FetchPoll outcome;
		outcome.running = false;
		if (!fs::is_directory(source, ec)) {
			outcome.error = url + " is no directory";
		} else {
			fs::copy(source, fs::u8path(dir), fs::copy_options::recursive, ec);
			if (ec)
				outcome.error = "cannot copy " + url + ": " + ec.message();
			else
				outcome.ok = true;
		}
		copied_ = std::move(outcome);
		return true;
	}
	error_path_ = (fs::u8path(scratch_dir_) / ("fetch-" + std::to_string(++fetches_) + ".err"))
						  .u8string();
	std::error_code ec;
	fs::remove(fs::u8path(error_path_), ec);
	proc::SpawnOptions options;
	options.argv = sync_argv(url, dir);
	options.error_path = error_path_;
	options.environment = environment_;
	// The agent's one thread starts it, and lives as long as it should.
	options.end_with_parent = true;
	std::string why;
	if (!child_.spawn(options, why)) {
		error = "aws s3 sync did not start: " + why;
		take_error(error_path_);
		error_path_.clear();
		return false;
	}
	child_running_ = true;
	return true;
}

FetchPoll ProcessTreeFetcher::poll() {
	FetchPoll out;
	if (copied_) {
		out = *copied_;
		copied_.reset();
		return out;
	}
	if (!child_running_) {
		out.running = false;
		out.error = "no fetch is under way";
		return out;
	}
	const std::optional<uint32_t> code = child_.poll();
	if (!code)
		return out;
	const std::string err = take_error(error_path_);
	error_path_.clear();
	child_running_ = false;
	out.running = false;
	if (*code == 0) {
		out.ok = true;
		return out;
	}
	out.error = "aws s3 sync exited " + std::to_string(*code) + (err.empty() ? "" : ": " + err);
	return out;
}

void ProcessTreeFetcher::cancel() {
	copied_.reset();
	if (!child_running_)
		return;
	child_.kill();
	child_.wait(5000);
	take_error(error_path_);
	error_path_.clear();
	child_running_ = false;
}

// --- the store -------------------------------------------------------------------

uint64_t tree_bytes_on_disk(const std::string &dir) {
	uint64_t bytes = 0;
	std::error_code ec;
	for (fs::directory_iterator it(fs::u8path(dir), ec), end; !ec && it != end; it.increment(ec)) {
		const std::string name = it->path().filename().u8string();
		if (name == TreeStore::kReadyMarker || name == TreeStore::kExpansionDir)
			continue;
		std::error_code fec;
		if (!it->is_regular_file(fec))
			continue;
		const uintmax_t size = it->file_size(fec);
		if (!fec)
			bytes += static_cast<uint64_t>(size);
	}
	return bytes;
}

TreeStore::TreeStore(std::string root, TreeFetcher &fetcher, Clock now_ms) :
		root_(std::move(root)),
		fetcher_(fetcher),
		now_(now_ms ? std::move(now_ms) : Clock(steady_now_ms)) {
	while (root_.size() > 1 && root_.back() == '/')
		root_.pop_back();
	scan_root();
}

std::string TreeStore::dir_of(int64_t id) const {
	return root_ + "/" + std::to_string(id);
}

std::string TreeStore::partial_dir_of(int64_t id) const {
	return dir_of(id) + kPartialSuffix;
}

void TreeStore::scan_root() {
	const uint64_t now = now_();
	std::error_code ec;
	std::vector<fs::path> remove;
	for (fs::directory_iterator it(fs::u8path(root_), ec), end; !ec && it != end;
			it.increment(ec)) {
		std::error_code fec;
		if (!it->is_directory(fec))
			continue;
		std::string name = it->path().filename().u8string();
		const bool partial = name.size() > std::strlen(kPartialSuffix) &&
				name.compare(name.size() - std::strlen(kPartialSuffix), std::string::npos,
						kPartialSuffix) == 0;
		if (partial)
			name.resize(name.size() - std::strlen(kPartialSuffix));
		const std::optional<int64_t> id = tree_id_of(name);
		if (!id)
			continue;
		if (!partial && fs::is_regular_file(it->path() / kReadyMarker, fec)) {
			Entry entry;
			entry.state = TreeFetchState::Present;
			entry.bytes = tree_bytes_on_disk(dir_of(*id));
			entry.named_at_ms = now;
			trees_[*id] = std::move(entry);
			io::logf(LogLevel::kInfo, "tree %lld is present (%llu bytes)",
					static_cast<long long>(*id), static_cast<unsigned long long>(entry.bytes));
			continue;
		}
		// A fetch cut short (a partial directory, or one renamed and never marked).
		remove.push_back(it->path());
	}
	for (const fs::path &path : remove) {
		io::logf(LogLevel::kInfo, "removing the unfinished %s", path.u8string().c_str());
		fs::remove_all(path, ec);
		if (ec)
			io::logf(LogLevel::kError, "cannot remove %s: %s", path.u8string().c_str(),
					ec.message().c_str());
	}
}

void TreeStore::remove_partial(int64_t id) {
	std::error_code ec;
	fs::remove_all(fs::u8path(partial_dir_of(id)), ec);
	if (ec)
		io::logf(LogLevel::kError, "cannot remove %s: %s", partial_dir_of(id).c_str(),
				ec.message().c_str());
}

void TreeStore::fail(int64_t id, Entry &entry, const std::string &why) {
	entry.state = TreeFetchState::Failed;
	entry.error = why;
	entry.failed_at_ms = now_();
	io::logf(LogLevel::kError, "tree %lld did not fetch (try %d of %d): %s",
			static_cast<long long>(id), entry.tries, kMaxTries, why.c_str());
	remove_partial(id);
}

bool TreeStore::start_fetch(int64_t id, Entry &entry) {
	const std::string dir = partial_dir_of(id);
	remove_partial(id);
	std::error_code ec;
	fs::create_directories(fs::u8path(dir), ec);
	++entry.tries;
	entry.state = TreeFetchState::Fetching;
	entry.error.clear();
	if (ec) {
		fail(id, entry, "cannot make " + dir + ": " + ec.message());
		return false;
	}
	std::string error;
	if (!fetcher_.start(entry.spec.url, dir, error)) {
		fail(id, entry, error);
		return false;
	}
	io::logf(LogLevel::kInfo, "fetching tree %lld from %s", static_cast<long long>(id),
			entry.spec.url.c_str());
	fetching_ = id;
	return true;
}

void TreeStore::finish_fetch(int64_t id, Entry &entry, const FetchPoll &ended) {
	fetching_.reset();
	if (!ended.ok) {
		fail(id, entry, ended.error);
		return;
	}
	const std::string partial = partial_dir_of(id);
	const std::string dir = dir_of(id);
	uint64_t bytes = 0;
	std::string error;
	if (!verify(partial, entry.spec, bytes, error)) {
		fail(id, entry, error);
		return;
	}
	std::error_code ec;
	fs::remove_all(fs::u8path(dir), ec);
	fs::rename(fs::u8path(partial), fs::u8path(dir), ec);
	if (ec) {
		fail(id, entry, "cannot rename " + partial + ": " + ec.message());
		return;
	}
	if (!io::write_file_atomic(dir + "/" + kReadyMarker, std::string(), error)) {
		fs::remove_all(fs::u8path(dir), ec);
		fail(id, entry, error);
		return;
	}
	entry.state = TreeFetchState::Present;
	entry.bytes = bytes;
	entry.error.clear();
	io::logf(LogLevel::kInfo, "tree %lld is present (%llu bytes, version %s)",
			static_cast<long long>(id), static_cast<unsigned long long>(bytes),
			entry.spec.version.c_str());
}

bool TreeStore::verify(const std::string &dir, const hosted::TreeSpec &spec, uint64_t &bytes,
		std::string &error) const {
	std::map<std::string, const hosted::TreeFileSpec *> expected;
	for (const hosted::TreeFileSpec &file : spec.files)
		expected[file.name] = &file;
	std::set<std::string> seen;
	std::vector<fs::path> extras;
	std::error_code ec;
	bytes = 0;
	for (fs::directory_iterator it(fs::u8path(dir), ec), end; !ec && it != end; it.increment(ec)) {
		const std::string name = it->path().filename().u8string();
		if (name == kReadyMarker || name == kExpansionDir)
			continue;
		const auto want = expected.find(name);
		std::error_code fec;
		if (want == expected.end() || !it->is_regular_file(fec)) {
			if (name != kManifestFile || !it->is_regular_file(fec))
				extras.push_back(it->path());
			continue;
		}
		const uintmax_t size = it->file_size(fec);
		if (fec) {
			error = name + " does not read: " + fec.message();
			return false;
		}
		if (static_cast<uint64_t>(size) != want->second->size) {
			error = name + " is " + std::to_string(size) + " bytes, not " +
					std::to_string(want->second->size);
			return false;
		}
		bytes += static_cast<uint64_t>(size);
		seen.insert(name);
	}
	if (ec) {
		error = "cannot list " + dir + ": " + ec.message();
		return false;
	}
	for (const hosted::TreeFileSpec &file : spec.files) {
		if (!seen.count(file.name)) {
			error = file.name + " is missing";
			return false;
		}
	}
	// Nothing but the manifest rides along with the files.
	for (const fs::path &extra : extras) {
		fs::remove_all(extra, ec);
		if (ec) {
			error = "cannot remove " + extra.u8string() + ": " + ec.message();
			return false;
		}
	}
	return true;
}

void TreeStore::want(const std::map<int64_t, hosted::TreeSpec> &trees) {
	const uint64_t now = now_();
	for (const auto &named : trees) {
		Entry &entry = trees_[named.first];
		entry.named_at_ms = now;
		// A tree is immutable: every reply carries the one manifest.
		if (entry.state != TreeFetchState::Present)
			entry.spec = named.second;
	}
	// The fetch under way, ended or not.
	if (fetching_) {
		const auto it = trees_.find(*fetching_);
		const FetchPoll ended = fetcher_.poll();
		if (it == trees_.end()) {
			// Forgotten meanwhile (gc cancels it; this cannot be reached).
			fetching_.reset();
		} else if (!ended.running) {
			finish_fetch(it->first, it->second, ended);
		}
	}
	if (fetching_)
		return;
	// The next one: the first named tree that waits its turn, or failed with a
	// try left and its wait over.
	for (const auto &named : trees) {
		Entry &entry = trees_[named.first];
		if (entry.state == TreeFetchState::Present)
			continue;
		if (entry.state == TreeFetchState::Failed &&
				(entry.tries >= kMaxTries || now - entry.failed_at_ms < kRetryAfterMs))
			continue;
		if (start_fetch(named.first, entry))
			return;
	}
}

void TreeStore::gc(const std::set<int64_t> &wanted) {
	const uint64_t now = now_();
	for (auto it = trees_.begin(); it != trees_.end();) {
		const int64_t id = it->first;
		Entry &entry = it->second;
		if (wanted.count(id)) {
			++it;
			continue;
		}
		if (entry.state == TreeFetchState::Present) {
			if (now - entry.named_at_ms < kUnnamedGraceMs) {
				++it;
				continue;
			}
			io::logf(LogLevel::kInfo, "removing tree %lld: no reply named it for %llu min",
					static_cast<long long>(id),
					static_cast<unsigned long long>(kUnnamedGraceMs / 60000));
			std::error_code ec;
			fs::remove_all(fs::u8path(dir_of(id)), ec);
			if (ec) {
				io::logf(LogLevel::kError, "cannot remove %s: %s", dir_of(id).c_str(),
						ec.message().c_str());
				++it;
				continue;
			}
		} else if (fetching_ == id) {
			io::logf(LogLevel::kInfo, "ending the fetch of tree %lld: no reply names it",
					static_cast<long long>(id));
			fetcher_.cancel();
			fetching_.reset();
			remove_partial(id);
		}
		it = trees_.erase(it);
	}
}

std::map<int64_t, TreeStanding> TreeStore::standings() const {
	std::map<int64_t, TreeStanding> out;
	for (const auto &tree : trees_) {
		TreeStanding standing;
		standing.state = tree.second.state;
		// A failure with a try left is a fetch still to come.
		if (standing.state == TreeFetchState::Failed && tree.second.tries < kMaxTries)
			standing.state = TreeFetchState::Fetching;
		if (standing.state == TreeFetchState::Failed)
			standing.error = tree.second.error;
		out[tree.first] = std::move(standing);
	}
	return out;
}

std::optional<TreeStatus> TreeStore::state_of(int64_t id) const {
	const auto it = trees_.find(id);
	if (it == trees_.end())
		return std::nullopt;
	TreeStatus status;
	status.state = it->second.state;
	status.error = it->second.error;
	if (it->second.state == TreeFetchState::Present)
		status.bytes = it->second.bytes;
	else if (fetching_ == id)
		status.bytes = tree_bytes_on_disk(partial_dir_of(id));
	return status;
}

std::string TreeStore::expansion_mount_point(
		int64_t base_id, const std::string &code, std::string &error) {
	const auto it = trees_.find(base_id);
	if (it == trees_.end() || it->second.state != TreeFetchState::Present) {
		error = "tree " + std::to_string(base_id) + " is not present";
		return {};
	}
	if (!hosted::is_expansion_code(code)) {
		error = "no expansion code";
		return {};
	}
	const std::string path = dir_of(base_id) + "/" + kExpansionDir + "/" + code;
	std::error_code ec;
	fs::create_directories(fs::u8path(path), ec);
	if (ec) {
		error = "cannot make " + path + ": " + ec.message();
		return {};
	}
	return path;
}

} // namespace opennova::hostd
