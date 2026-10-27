#include "release_build.h"

#include "command_runner.h"
#include "core/loggable.h"
#include "jobs/job_queue.h"
#include "product_jobs.h"
#include "product_repository.h"
#include "project_repository.h"

#include <base/content/content_manifest.h>
#include <base/io/cp1252.h>
#include <base/io/file_io.h>
#include <base/io/json.h>
#include <base/io/os_path.h>
#include <base/proc/child_process.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace opennova::novaworld_server::hosted {

namespace {

namespace fs = std::filesystem;

constexpr const char *kTag = "[data]";
constexpr int kCapBuild = 1;
constexpr int kAttemptsBuild = 3;
// The files the runtime's own package installs in a game's folder that a game's export may hold
// too (a stock install's UPDATE.EXE, REVUPDAT.EXE and Readme.txt): a release never carries them,
// or the runtime's update and the game's would replace each other's on every pass.
const char *const kRuntimeFiles[] = { "update.exe", "revupdat.exe", "README.txt" };
// What a build's log keeps of what its tools said: the tail past this.
constexpr size_t kLogMax = 256 * 1024;
// The system's own variables a tool runs with (a name ending in '*' a prefix): nothing of the
// program's, whose environment holds the store's credentials.
const std::vector<std::string> kToolEnvironment = { "PATH", "HOME", "LANG", "LC_*", "TMPDIR",
	"TZ" };

// One revision a build lays out: its game's id (where the store keeps its files), its project's
// key (the folder it is laid out in), its number and its manifest's SHA-256.
struct Source {
	int64_t product_id = 0;
	std::string key;
	int64_t revision = 0;
	std::string manifest_sha256;

	std::string label() const { return key + " r" + std::to_string(revision); }
};

// What a build makes, read in one lease: the release, its game (the product its package
// updates, its project on its row), the revision it exports (an expansion's base game's beside
// it), a retail game's stock revisions, and the game's last published package.
struct BuildPlan {
	ReleaseRow release;
	ProductRow product;
	Source project;
	std::optional<Source> base;
	std::optional<Source> stock;
	std::optional<Source> base_stock;
	std::string previous_key; // the product's published package, "" for none
};

// The revision `number` of `project`'s (a game with a project) as a Source; false with `why`
// when it has none.
bool source_of(db::Database &db, const ProductRow &project, int64_t number, Source &out,
		std::string &why) {
	const std::optional<content::ContentRevision> revision =
			content::find_revision(db, project.id, number);
	if (!revision) {
		why = project.key + " has no revision " + std::to_string(number);
		return false;
	}
	out.product_id = project.id;
	out.key = project.key;
	out.revision = number;
	out.manifest_sha256 = revision->manifest_sha256;
	return true;
}

// The plan of the building release `release`; false with `why` (the release's refusal).
bool plan_of(db::Database &db, const ReleaseRow &release, BuildPlan &plan, std::string &why) {
	plan.release = release;
	const std::optional<ProductRow> product =
			release.product_id ? find_product(db, *release.product_id) : std::nullopt;
	if (!product) {
		why = "its game was deleted";
		return false;
	}
	if (product->key.empty()) {
		why = product->display_name + " has no project";
		return false;
	}
	if (product->command_file.empty()) {
		why = product->display_name + " names no updater product (a command file)";
		return false;
	}
	plan.product = *product;
	// A product an admin made carries no product name: ours is named as its product, the game
	// run by the runtime's executable (as the baseline names the seeded ones).
	if (plan.product.product_name.empty() && !plan.product.retail) {
		plan.product.product_name = plan.product.display_name;
		if (plan.product.kind == ProductKind::Base)
			plan.product.product_exe = "opennova.exe";
	}
	if (plan.product.product_name.empty()) {
		why = product->display_name + " names no updater product (a product name)";
		return false;
	}
	if (!source_of(db, *product, release.content_revision, plan.project, why))
		return false;
	std::optional<ProductRow> base;
	if (product->parent_id) {
		base = find_product(db, *product->parent_id);
		if (!base || base->key.empty()) {
			why = product->key + "'s base game has no project";
			return false;
		}
		Source source;
		if (!source_of(db, *base, release.base_revision, source, why))
			return false;
		plan.base = source;
	}
	if (product->retail) {
		Source stock;
		if (!source_of(db, *product, 1, stock, why))
			return false;
		plan.stock = stock;
		if (base) {
			Source base_stock;
			if (!source_of(db, *base, 1, base_stock, why))
				return false;
			plan.base_stock = base_stock;
		}
	}
	if (product->published_release_id) {
		const std::optional<ReleaseRow> published =
				find_release(db, *product->published_release_id);
		if (published && !published->sha256.empty())
			plan.previous_key = release_package_key(published->sha256);
	}
	return true;
}

// The last line a failed tool printed, for its refusal.
std::string last_line(const proc::CommandResult &result) {
	const bool err = result.err.find_first_not_of(" \t\r\n") != std::string::npos;
	const std::string &said = err ? result.err : result.out;
	const size_t end = said.find_last_not_of(" \t\r\n");
	if (end == std::string::npos)
		return result.error;
	const size_t newline = said.find_last_of("\r\n", end);
	const size_t start = newline == std::string::npos ? 0 : newline + 1;
	return said.substr(start, end + 1 - start);
}

// Where a step of the build came to: done, refused for good (the release invalid with the
// reason), or a store or disk failure the job runs again for.
enum class Step : uint8_t { Ok, Refused, Transient };

// A project laid out: its folder, and an expansion's base's.
struct LaidOut {
	std::string dir;
	std::optional<std::string> base_dir;
};

class Build {
public:
	Build(ObjectStore &store, const std::string &scratch_root, const ReleaseTools &tools,
			const BuildPlan &plan) :
			store_(store),
			tools_(tools),
			plan_(plan),
			work_(io::utf8_join(scratch_root, "build-" + std::to_string(plan.release.id))),
			cache_(io::utf8_join(scratch_root, "content-cache")),
			runner_(proc::filter_environment(kToolEnvironment), work_ + "-run") {}
	~Build() { clear(); }
	Build(const Build &) = delete;
	Build &operator=(const Build &) = delete;

	// The package made, at package(); on a refusal or a store failure `why` says what.
	Step run(std::string &why);
	std::string package() const { return io::utf8_join(work_, "release.sfs"); }
	// The log as a text column takes it: UTF-8 (a tool's other bytes read as cp1252) with no NUL,
	// never starting inside a character its trim cut.
	std::string log() const {
		size_t from = 0;
		while (from < log_.size() && (static_cast<unsigned char>(log_[from]) & 0xC0) == 0x80)
			++from;
		std::string text = retail_text_to_utf8(std::string_view(log_).substr(from));
		text.erase(std::remove(text.begin(), text.end(), '\0'), text.end());
		return text;
	}

private:
	void clear() {
		std::error_code ec;
		fs::remove_all(io::os_path(work_), ec);
		fs::remove_all(io::os_path(work_ + "-run"), ec);
	}
	void note(const std::string &line) {
		log_ += line + "\n";
		trim_log();
	}
	void trim_log() {
		if (log_.size() > kLogMax)
			log_.erase(0, log_.size() - kLogMax);
	}
	Step lay_out(const Source &source, const std::string &dir, std::string &why);
	Step lay_out_project(const Source &project, const std::optional<Source> &base,
			const std::string &under, LaidOut &out, std::string &why);
	bool tool(const std::vector<std::string> &argv, const std::string &what, std::string &why);
	bool export_to(
			const LaidOut &laid, const std::string &out, const std::string &what, std::string &why);
	bool write_wizard(const std::string &tree, std::string &why) const;
	std::vector<std::string> pack_command(
			const std::string &from, const std::string &to, const std::string &previous) const;

	ObjectStore &store_;
	const ReleaseTools &tools_;
	const BuildPlan &plan_;
	std::string work_;
	std::string cache_;
	proc::ProcessCommandRunner runner_;
	std::string log_;
	std::vector<int64_t> synced_; // the games whose project's files the cache took this build
};

// The revision's files laid out in `dir` from the cache, its project's files synced down first.
Step Build::lay_out(const Source &source, const std::string &dir, std::string &why) {
	const std::string manifest_file =
			io::utf8_join(work_, "manifest-" + source.key + "-" + std::to_string(source.revision));
	std::string text;
	if (!store_.get_file(content::manifest_key(source.product_id, source.manifest_sha256),
				manifest_file, why) ||
			!io::read_file_text(manifest_file, text, why))
		return Step::Transient;
	std::vector<opennova::content::ContentFile> files;
	if (opennova::content::manifest_sha256(text) != source.manifest_sha256 ||
			!opennova::content::decode_manifest(text, files, why)) {
		why = source.label() + "'s manifest does not read" + (why.empty() ? "" : ": " + why);
		return Step::Refused;
	}
	const std::string blobs = io::utf8_join(cache_, std::to_string(source.product_id));
	if (std::find(synced_.begin(), synced_.end(), source.product_id) == synced_.end()) {
		if (!store_.sync_down(content::project_prefix(source.product_id) + "blobs/", blobs, why))
			return Step::Transient;
		synced_.push_back(source.product_id);
	}
	for (const opennova::content::ContentFile &file : files) {
		const fs::path from = io::os_path(io::utf8_join(blobs, file.sha256));
		const fs::path to = io::os_path(io::utf8_join(dir, file.path));
		std::error_code ec;
		if (!fs::is_regular_file(from, ec) || fs::file_size(from, ec) != file.size) {
			why = "the store lacks " + source.label() + "'s " + file.path;
			return Step::Transient;
		}
		fs::create_directories(to.parent_path(), ec);
		fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
		if (ec) {
			why = "cannot lay out " + source.label() + "'s " + file.path + ": " + ec.message();
			return Step::Transient;
		}
	}
	note("laid out " + source.label() + ": " + std::to_string(files.size()) + " files in " + dir);
	return Step::Ok;
}

// `project` laid out in a folder of its key's name under `under`; an expansion's base beside it, in a
// folder of its base game's key's name, and the laid-out copy's project.opennova pointed at it
// (base_project "../<base key>"), whatever folder the author kept the base game's project in: refused
// only where the expansion's project names no base game's project at all.
Step Build::lay_out_project(const Source &project, const std::optional<Source> &base,
		const std::string &under, LaidOut &out, std::string &why) {
	out.dir = io::utf8_join(under, project.key);
	out.base_dir.reset();
	const Step laid = lay_out(project, out.dir, why);
	if (laid != Step::Ok || !base)
		return laid;
	const std::string file = io::utf8_join(out.dir, "project.opennova");
	std::string text;
	io::JsonValue document;
	if (!io::read_file_text(file, text, why) || !io::json_parse(text, document, why) ||
			!document.is_object()) {
		why = project.label() + " holds no project.opennova that reads";
		return Step::Refused;
	}
	io::JsonValue *expansion = document.get("expansion");
	const std::string named = expansion && expansion->is_object()
			? expansion->get_string("base_project", "")
			: std::string();
	if (named.empty()) {
		why = project.label() + "'s project names no base game's project (its base_project)";
		return Step::Refused;
	}
	expansion->set("base_project", io::JsonValue::make_string("../" + base->key));
	if (!io::write_file_atomic(file, io::json_write(document), why))
		return Step::Transient;
	note("pointed " + project.label() + "'s base_project \"" + named + "\" at ../" + base->key);
	out.base_dir = io::utf8_join(under, base->key);
	return lay_out(*base, *out.base_dir, why);
}

bool Build::tool(const std::vector<std::string> &argv, const std::string &what, std::string &why) {
	const proc::CommandResult result = runner_.run(argv, tools_.timeout_ms);
	std::string line = "$";
	for (const std::string &arg : argv)
		line += " " + arg;
	log_ += line + "\n" + result.out + result.err;
	if (!result.error.empty())
		log_ += result.error + "\n";
	trim_log();
	if (result.ok())
		return true;
	switch (result.outcome) {
		case proc::CommandResult::Outcome::TimedOut:
			why = what + " took too long";
			break;
		case proc::CommandResult::Outcome::NotStarted:
			why = what + " did not start: " + result.error;
			break;
		case proc::CommandResult::Outcome::Exited:
			why = what + " failed (exit " + std::to_string(result.exit_code) +
					"): " + last_line(result);
			break;
	}
	return false;
}

// The project laid out as `laid` exported to `out`: an expansion's base exported first, at its
// own output, where the expansion's build reads it.
bool Build::export_to(
		const LaidOut &laid, const std::string &out, const std::string &what, std::string &why) {
	if (laid.base_dir &&
			!tool({ tools_.project_tool, "export", *laid.base_dir },
					"the export of " + what + "'s base", why))
		return false;
	return tool({ tools_.project_tool, "export", laid.dir, "--out", out }, "the export of " + what,
			why);
}

// Our own product's UPDATE.WIZ in its tree, which marks the folder it installs into as that
// product's: the updater installs OpenNova content only into a folder with no game data or one
// its wizard names (apps/update client.cpp, ADR 0067 d2). A retail product's players keep their
// stock one.
bool Build::write_wizard(const std::string &tree, std::string &why) const {
	const ProductRow &ds = plan_.product;
	const std::string folder = ds.expansion_code.empty()
			? tree
			: io::utf8_join(tree, "expansion/" + ds.expansion_code);
	std::string text = "[HEADER]\r\nCMD=" + ds.command_file + "\r\n";
	if (!tools_.update_host.empty())
		text += "URL=" + tools_.update_host + "\r\n";
	text += "PRODUCT_LOGIN=" + ds.updater_login + "\r\n";
	return io::ensure_directory(folder, why) &&
			io::write_file_atomic(io::utf8_join(folder, "UPDATE.WIZ"), text, why);
}

// opennova-update-tool's build of the package from `from` to `to`, as the product names its
// product: an expansion's section under its code, a base's with its executable. Every release is
// whole (ADR 0067 d6, amended 2026-10-25: the jointops and dfx2 zips install their game into an
// empty folder from it), with the removals since `previous` ("" for none) and, for a retail game,
// since its stock install (`from`, revision 1's export). The runtime's own files in a game's
// folder are left out (kRuntimeFiles).
std::vector<std::string> Build::pack_command(
		const std::string &from, const std::string &to, const std::string &previous) const {
	const ProductRow &ds = plan_.product;
	std::vector<std::string> argv = { tools_.update_tool, "build", "--from", from, "--to", to,
		"--out", package(), "--product-name", ds.product_name, "--product", ds.product_name,
		"--version", plan_.release.version, "--exclude", "export.json", "--full" };
	for (const char *name : kRuntimeFiles)
		argv.insert(argv.end(), { "--exclude", name });
	if (!ds.expansion_code.empty()) {
		argv.insert(argv.end(), { "--expansion", ds.expansion_code + "=" + ds.command_file });
	} else {
		argv.insert(argv.end(), { "--base", ds.command_file });
		if (!ds.product_exe.empty())
			argv.insert(argv.end(), { "--exe", ds.product_exe });
	}
	if (!previous.empty())
		argv.insert(argv.end(), { "--previous", previous });
	return argv;
}

Step Build::run(std::string &why) {
	clear();
	if (!io::ensure_directory(work_, why) || !io::ensure_directory(work_ + "-run", why))
		return Step::Transient;
	const std::string what = plan_.project.label();
	LaidOut edited;
	Step step = lay_out_project(
			plan_.project, plan_.base, io::utf8_join(work_, "projects"), edited, why);
	if (step != Step::Ok)
		return step;
	const std::string edited_export = io::utf8_join(work_, "export");
	if (!export_to(edited, edited_export, what, why))
		return Step::Refused;
	// What the package's removals count from: a retail game's stock revision exported as the
	// edited one is (the stock install's files in our own writer's form), else nothing. Every
	// file of the edited export is a row either way (pack_command's --full).
	std::string from = io::utf8_join(work_, "empty");
	std::string previous;
	if (plan_.product.retail) {
		LaidOut stock;
		step = lay_out_project(
				*plan_.stock, plan_.base_stock, io::utf8_join(work_, "stock"), stock, why);
		if (step != Step::Ok)
			return step;
		from = io::utf8_join(work_, "stock-export");
		if (!export_to(stock, from, plan_.stock->label(), why))
			return Step::Refused;
	} else if (!io::ensure_directory(from, why) || !write_wizard(edited_export, why)) {
		return Step::Transient;
	}
	if (!plan_.previous_key.empty()) {
		previous = io::utf8_join(work_, "previous.sfs");
		if (!store_.get_file(plan_.previous_key, previous, why))
			return Step::Transient;
	}
	if (!tool(pack_command(from, edited_export, previous), "the package of " + what, why))
		return Step::Refused;
	return Step::Ok;
}

// The build's end: the release invalid with `why` and the log, the job done.
jobs::JobResult refuse(
		jobs::JobContext &ctx, int64_t release_id, const std::string &why, const std::string &log) {
	auto lease = ctx.pool.acquire();
	set_release_build_failed(*lease, release_id, why, log);
	std::fprintf(stderr, "%s release %s build refused: %s\n", kTag,
			std::to_string(release_id).c_str(), loggable(why, 200).c_str());
	return jobs::JobResult::done();
}

// A store or disk failure: the job runs again; the release is invalid once no attempt is left.
jobs::JobResult transient(
		jobs::JobContext &ctx, int64_t release_id, const std::string &why, const std::string &log) {
	if (ctx.job.attempts >= ctx.job.max_attempts) {
		refuse(ctx, release_id, "the store: " + why, log);
		return jobs::JobResult::fail(why);
	}
	std::fprintf(stderr, "%s WARN release %s build: %s; the job runs again\n", kTag,
			std::to_string(release_id).c_str(), loggable(why, 200).c_str());
	return jobs::JobResult::retry(why);
}

jobs::JobResult build(jobs::JobContext &ctx, ObjectStore &store, const std::string &scratch_root,
		const ReleaseTools &tools) {
	const int64_t release_id = jobs::member_int(ctx.job.payload, "release_id");
	BuildPlan plan;
	std::string why;
	{
		auto lease = ctx.pool.acquire();
		const std::optional<ReleaseRow> release = find_release(*lease, release_id);
		if (!release || release->status != ReleaseStatus::Building)
			return jobs::JobResult::done();
		if (!plan_of(*lease, *release, plan, why)) {
			set_release_build_failed(*lease, release_id, why, std::string());
			std::fprintf(stderr, "%s release %s build refused: %s\n", kTag,
					std::to_string(release_id).c_str(), loggable(why, 200).c_str());
			return jobs::JobResult::done();
		}
	}
	std::printf("%s release %s: building %s version %s\n", kTag, std::to_string(release_id).c_str(),
			plan.project.label().c_str(), loggable(plan.release.version).c_str());
	Build made(store, scratch_root, tools, plan);
	const Step step = made.run(why);
	if (step == Step::Refused)
		return refuse(ctx, release_id, why, made.log());
	if (step == Step::Transient)
		return transient(ctx, release_id, why, made.log());
	// The package where an upload lands, then the row on to validating with its check queued, as a
	// completed upload's is: a run cut short between them puts the package again.
	if (!store.put_file(release_upload_key(release_id), made.package(), why))
		return transient(ctx, release_id, why, made.log());
	{
		auto lease = ctx.pool.acquire();
		db::Transaction tx(*lease);
		if (!set_release_built(*lease, release_id, made.log())) {
			std::printf("%s release %s left building meanwhile\n", kTag,
					std::to_string(release_id).c_str());
			return jobs::JobResult::done();
		}
		enqueue_release_validate(*lease, release_id);
		tx.commit();
	}
	std::printf("%s release %s built: validating\n", kTag, std::to_string(release_id).c_str());
	return jobs::JobResult::done();
}

} // namespace

std::string release_build_key(int64_t release_id) {
	return std::string(kJobReleaseBuild) + ":" + std::to_string(release_id);
}

int64_t enqueue_release_build(db::Database &db, int64_t release_id) {
	jobs::NewJob job;
	job.kind = kJobReleaseBuild;
	job.payload.set("release_id", io::json_number(static_cast<double>(release_id)));
	job.dedupe_key = release_build_key(release_id);
	job.max_attempts = kAttemptsBuild;
	return jobs::enqueue(db, job);
}

void register_release_build(jobs::JobRegistry &registry, ObjectStore &store,
		std::string scratch_dir, ReleaseTools tools) {
	jobs::JobKind kind{ kJobReleaseBuild,
		[&store, scratch_dir, tools](
				jobs::JobContext &ctx) { return build(ctx, store, scratch_dir, tools); },
		kCapBuild };
	kind.retry_base_ms = 5 * 1000;
	kind.retry_max_ms = 60 * 1000;
	registry.add(std::move(kind));
}

} // namespace opennova::novaworld_server::hosted
