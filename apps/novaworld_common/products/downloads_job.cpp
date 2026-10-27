#include "downloads_job.h"

#include "core/loggable.h"
#include "core/zip_files.h"
#include "jobs/job_queue.h"
#include "product_jobs.h"
#include "product_repository.h"

#include <base/io/file_io.h>
#include <base/io/json.h>
#include <base/io/os_path.h>
#include <base/io/strutil.h>
#include <formats/update/apply.h>
#include <formats/update/update_wizard.h>

#include <cstdio>
#include <ctime>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <system_error>
#include <utility>
#include <vector>

namespace opennova::novaworld_server::hosted {

namespace {

namespace fs = std::filesystem;

constexpr const char *kTag = "[data]";
constexpr int kCapDownloads = 1;
constexpr int kAttemptsDownloads = 10;

// What a step came to: done, refused for good (the job fails with the reason, the host left as
// it was), or a store or disk failure the job runs again for.
enum class Outcome : uint8_t { Ok, Refused, Transient };

// A product as the job renders and lists it: its row and, while it has one, its published release
// with the version it carries.
struct Offered {
	ProductRow product;
	std::optional<ReleaseRow> release;
	std::string version;
};

// The products and their published releases, read in one go.
std::vector<Offered> read_offered(db::Database &db) {
	std::vector<Offered> out;
	for (ProductRow &product : list_products(db)) {
		Offered offered;
		if (product.published_release_id) {
			std::optional<ReleaseRow> release = find_release(db, *product.published_release_id);
			if (release && release->status == ReleaseStatus::Published) {
				offered.version = release->version;
				if (offered.version.empty())
					for (const ReleaseSection &s : list_sections(db, release->id))
						if (s.product_id == product.id)
							offered.version = s.version;
				offered.release = std::move(release);
			}
		}
		offered.product = std::move(product);
		out.push_back(std::move(offered));
	}
	return out;
}

// Each product's published release, what a run compares at its end.
std::map<int64_t, int64_t> published_of(const std::vector<Offered> &offered) {
	std::map<int64_t, int64_t> out;
	for (const Offered &o : offered)
		out[o.product.id] = o.release ? o.release->id : 0;
	return out;
}

// The database's "YYYY-MM-DD HH:MM:SS[.ffffff]+00" (every connection runs in UTC, DateStyle ISO)
// as ISO 8601 to the second.
std::string iso_of(const std::string &text) {
	if (text.size() < 19 || text[10] != ' ')
		return text;
	return text.substr(0, 10) + "T" + text.substr(11, 8) + "Z";
}

std::string iso_now() {
	const std::time_t t = std::time(nullptr);
	std::tm tm{};
#ifdef _WIN32
	gmtime_s(&tm, &t);
#else
	gmtime_r(&t, &tm);
#endif
	char out[32];
	std::strftime(out, sizeof(out), "%Y-%m-%dT%H:%M:%SZ", &tm);
	return out;
}

// One rendered download: its file on disk, its size and SHA-256.
struct Rendered {
	std::string path;
	uint64_t size = 0;
	std::string sha256;
};

// The run's directory under the worker's scratch directory, removed with it.
struct WorkDir {
	std::string path;

	explicit WorkDir(std::string path) : path(std::move(path)) {}
	~WorkDir() {
		std::error_code ec;
		fs::remove_all(io::os_path(path), ec);
	}
	WorkDir(const WorkDir &) = delete;
	WorkDir &operator=(const WorkDir &) = delete;

	bool make(std::string &error) {
		std::error_code ec;
		fs::remove_all(io::os_path(path), ec);
		return io::ensure_directory(path, error);
	}
	std::string file(const std::string &name) const { return io::utf8_join(path, name); }
};

// The published package `sha256` brought down and its section carrying `command_file` (the base
// section) applied into the empty directory `dir`, the command file written beside what its rows
// made: the folder an updater product's update.exe takes for its own install.
Outcome apply_package(ObjectStore &store, const WorkDir &work, const std::string &sha256,
		const std::string &command_file, const std::string &dir, std::string &why) {
	const std::string package = work.file("package.sfs");
	if (!store.get_file(release_package_key(sha256), package, why) ||
			!io::ensure_directory(dir, why))
		return Outcome::Transient;
	update::ApplyReport report;
	std::vector<uint8_t> command;
	switch (apply_section_to(package, "", command_file, dir, report, command, why)) {
		case ApplyOutcome::Ok:
			break;
		case ApplyOutcome::Refused:
			return Outcome::Refused;
		case ApplyOutcome::Transient:
			return Outcome::Transient;
	}
	std::error_code ec;
	fs::remove(io::os_path(package), ec);
	return io::write_file_atomic(
				   io::utf8_join(dir, command_file), command.data(), command.size(), why)
			? Outcome::Ok
			: Outcome::Transient;
}

// The wizard naming `product` (its command file, this deployment's update host, its wizard login
// or else its updater login) written as `folder`/UPDATE.WIZ.
Outcome write_wizard(const ProductRow &product, const std::string &update_host,
		const std::string &folder, std::string &why) {
	update::UpdateWizard wizard;
	wizard.cmd = product.command_file;
	wizard.url = update_host;
	wizard.product_login =
			product.wizard_login.empty() ? product.updater_login : product.wizard_login;
	const std::string text = update::write_update_wizard(wizard);
	if (text.empty()) {
		why = product.display_name + "'s UPDATE.WIZ does not write";
		return Outcome::Refused;
	}
	return io::ensure_directory(folder, why) &&
					io::write_file_atomic(io::utf8_join(folder, "UPDATE.WIZ"), text, why)
			? Outcome::Ok
			: Outcome::Transient;
}

class Render {
public:
	Render(ObjectStore &store, const WorkDir &work, std::string update_host,
			const std::vector<Offered> &offered) :
			store_(store), work_(work), update_host_(std::move(update_host)), offered_(offered) {}

	// `o`'s download rendered as `out` (`out.path` left empty when it offers none: no download,
	// no published release, a game while the runtime has none, an expansion).
	Outcome render(const Offered &o, Rendered &out, std::string &why) {
		out = Rendered{};
		const ProductRow &p = o.product;
		if (p.download.empty() || !o.release)
			return Outcome::Ok;
		const std::string tree = io::utf8_join(work_.file("products"), p.key);
		const std::string zip = tree + ".zip";
		Outcome step = Outcome::Ok;
		if (p.kind == ProductKind::Game) {
			step = runtime(why);
			if (step != Outcome::Ok || runtime_dir_.empty())
				return step;
			step = game(p, tree, why);
		} else if (p.kind == ProductKind::App && !p.updater_login.empty()) {
			step = apply_package(store_, work_, o.release->sha256, p.command_file, tree, why);
		} else if (p.kind == ProductKind::App) {
			if (!io::ensure_directory(work_.file("products"), why) ||
					!store_.get_file(release_download_key(o.release->sha256), zip, why))
				return Outcome::Transient;
		} else {
			return Outcome::Ok;
		}
		if (step != Outcome::Ok)
			return step;
		if (p.kind != ProductKind::App || !p.updater_login.empty()) {
			if (!zip_directory(tree, zip, why))
				return Outcome::Transient;
			std::error_code ec;
			fs::remove_all(io::os_path(tree), ec);
		}
		if (!digest_file(zip, out.size, out.sha256, why))
			return Outcome::Transient;
		out.path = zip;
		return Outcome::Ok;
	}

private:
	// The runtime's published package applied once per run (runtime_dir_ left empty while the
	// runtime has no published release).
	Outcome runtime(std::string &why) {
		if (runtime_read_)
			return Outcome::Ok;
		for (const Offered &o : offered_)
			if (o.product.kind == ProductKind::App && o.release &&
					strutil::iequals(o.product.updater_login, update::kRuntimeLogin)) {
				const std::string dir = work_.file("runtime");
				const Outcome step = apply_package(
						store_, work_, o.release->sha256, o.product.command_file, dir, why);
				if (step != Outcome::Ok)
					return step;
				runtime_dir_ = dir;
			}
		runtime_read_ = true;
		return Outcome::Ok;
	}

	// A game's folder: the runtime's, a retail game's own wizard and its expansions'.
	Outcome game(const ProductRow &p, const std::string &tree, std::string &why) {
		std::error_code ec;
		if (!io::ensure_directory(work_.file("products"), why))
			return Outcome::Transient;
		fs::copy(io::os_path(runtime_dir_), io::os_path(tree), fs::copy_options::recursive, ec);
		if (ec) {
			why = "cannot copy the runtime for " + p.display_name + ": " + ec.message();
			return Outcome::Transient;
		}
		if (p.retail) {
			const Outcome step = write_wizard(p, update_host_, tree, why);
			if (step != Outcome::Ok)
				return step;
		}
		for (const Offered &o : offered_) {
			const ProductRow &e = o.product;
			if (e.kind != ProductKind::Expansion || e.parent_id != p.id || e.updater_login.empty())
				continue;
			const Outcome step = write_wizard(
					e, update_host_, io::utf8_join(tree, "expansion/" + e.expansion_code), why);
			if (step != Outcome::Ok)
				return step;
		}
		return Outcome::Ok;
	}

	ObjectStore &store_;
	const WorkDir &work_;
	std::string update_host_;
	const std::vector<Offered> &offered_;
	bool runtime_read_ = false;
	std::string runtime_dir_;
};

io::JsonValue string_or_null(const std::string &text, bool present) {
	return present ? io::json_string(text) : io::JsonValue::make_null();
}

std::string manifest_json(
		const std::vector<Offered> &offered, const std::map<int64_t, Rendered> &rendered) {
	io::JsonValue list = io::JsonValue::make_array();
	for (const Offered &o : offered) {
		const ProductRow &p = o.product;
		const auto it = rendered.find(p.id);
		const bool has = it != rendered.end();
		io::JsonValue item = io::JsonValue::make_object();
		item.set("key", io::json_string(p.key));
		item.set("kind", io::json_string(product_kind_name(p.kind)));
		item.set("name", io::json_string(p.display_name));
		item.set("description", io::json_string(p.description));
		item.set("file", string_or_null(p.download, !p.download.empty()));
		item.set("size",
				has ? io::json_number(static_cast<double>(it->second.size))
					: io::JsonValue::make_null());
		item.set("sha256", string_or_null(has ? it->second.sha256 : "", has));
		item.set("version", string_or_null(o.version, has));
		item.set("release", string_or_null(has ? o.release->name : "", has));
		item.set("published", string_or_null(has ? iso_of(o.release->published_at) : "", has));
		list.push(std::move(item));
	}
	io::JsonValue root = io::JsonValue::make_object();
	root.set("written", io::json_string(iso_now()));
	root.set("products", std::move(list));
	return io::json_write(root);
}

jobs::JobResult settle(Outcome outcome, const std::string &why) {
	if (outcome == Outcome::Refused) {
		std::fprintf(stderr, "%s WARN the downloads host: not written: %s\n", kTag,
				loggable(why, 300).c_str());
		return jobs::JobResult::fail(why);
	}
	std::fprintf(
			stderr, "%s WARN the downloads host: %s; again\n", kTag, loggable(why, 300).c_str());
	return jobs::JobResult::retry(why);
}

jobs::JobResult publish(jobs::JobContext &ctx, ObjectStore &store, ObjectStore *downloads,
		const std::string &scratch_root, const std::string &update_host) {
	if (downloads == nullptr || update_host.empty()) {
		std::printf("%s the downloads host: no %s; nothing written\n", kTag,
				downloads == nullptr ? "downloads store (ONNET_DOWNLOADS_URL)"
									 : "update host (ONNET_UPDATE_HOST)");
		return jobs::JobResult::done();
	}
	std::vector<Offered> offered;
	{
		auto lease = ctx.pool.acquire();
		offered = read_offered(*lease);
	}
	WorkDir work(io::utf8_join(scratch_root, "downloads"));
	std::string why;
	if (!work.make(why))
		return settle(Outcome::Transient, why);

	// Every download rendered before any is put, so a refusal leaves the host as it was.
	Render render(store, work, update_host, offered);
	std::map<int64_t, Rendered> rendered; // product id -> its download
	for (const Offered &o : offered) {
		Rendered r;
		const Outcome step = render.render(o, r, why);
		if (step != Outcome::Ok)
			return settle(step, o.product.display_name + ": " + why);
		if (!r.path.empty())
			rendered[o.product.id] = std::move(r);
	}
	std::set<std::string> keep = { kDownloadsManifestName };
	for (const Offered &o : offered) {
		const auto it = rendered.find(o.product.id);
		if (it == rendered.end())
			continue;
		if (!downloads->put_file(o.product.download, it->second.path, why,
					{ "application/zip", kDownloadCacheControl }))
			return settle(Outcome::Transient, why);
		keep.insert(o.product.download);
	}
	const std::string manifest = work.file(kDownloadsManifestName);
	if (!io::write_file_atomic(manifest, manifest_json(offered, rendered), why) ||
			!downloads->put_file(kDownloadsManifestName, manifest, why,
					{ "application/json", kManifestCacheControl }))
		return settle(Outcome::Transient, why);
	std::vector<StoredObject> objects;
	if (!downloads->list("", objects, why))
		return settle(Outcome::Transient, why);
	size_t removed = 0;
	for (const StoredObject &object : objects) {
		if (keep.count(object.key) != 0)
			continue;
		if (!downloads->remove(object.key, why))
			return settle(Outcome::Transient, why);
		++removed;
	}
	std::printf("%s the downloads host: %zu download%s and the manifest written, %zu stale "
				"object%s removed\n",
			kTag, rendered.size(), rendered.size() == 1 ? "" : "s", removed,
			removed == 1 ? "" : "s");

	// A release published or retired meanwhile (its enqueue found this job's key held): again.
	std::map<int64_t, int64_t> now;
	{
		auto lease = ctx.pool.acquire();
		now = published_of(read_offered(*lease));
	}
	if (now != published_of(offered))
		return jobs::JobResult::retry("a release was published or retired meanwhile", 0);
	return jobs::JobResult::done();
}

} // namespace

int64_t enqueue_downloads_publish(db::Database &db) {
	jobs::NewJob job;
	job.kind = kJobDownloadsPublish;
	job.dedupe_key = kJobDownloadsPublish;
	job.max_attempts = kAttemptsDownloads;
	return jobs::enqueue(db, job);
}

void register_downloads_publish(jobs::JobRegistry &registry, ObjectStore &store,
		ObjectStore *downloads, std::string scratch_dir, std::string update_host) {
	jobs::JobKind kind{ kJobDownloadsPublish,
		[&store, downloads, scratch_dir, update_host](jobs::JobContext &ctx) {
			return publish(ctx, store, downloads, scratch_dir, update_host);
		},
		kCapDownloads };
	kind.retry_base_ms = 10 * 1000;
	kind.retry_max_ms = 10 * 60 * 1000;
	registry.add(std::move(kind));
}

} // namespace opennova::novaworld_server::hosted
