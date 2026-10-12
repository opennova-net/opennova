#include <editor/session/terrain_sculpt.h>

#include <algorithm>
#include <cstring>
#include <sstream>
#include <utility>

#include <base/io/hash.h>
#include <base/io/strutil.h>
#include <editor/assets/project_asset_source.h>
#include <editor/project/project_files.h>
#include <editor/session/document_set.h>
#include <editor/session/session_core.h>
#include <editor/session/view/session_view.h>
#include <formats/cpt/cpt_io.h>
#include <editor/session/terrain_family.h>
#include <formats/cpt/trngen/terrain_bake.h>
#include <formats/trn/trn_io.h>

namespace opennova::editor {

struct TerrainSculpt::Job {
	std::string path;
	uint64_t identity = 0, generation = 0, revision = 0;
	// A seed: the bake state made from `seed_input` alone (the file's input its tiles were made from), in the
	// background, a stroke never stopping it; else a region bake of `input` over `state`.
	bool seed_only = false;
	uint64_t content = 0;   // a seed's input, hashed (heights_content)
	RasterPlane input;      // the input to bake
	RasterPlane seed_input; // the input the bake state is made from
	std::shared_ptr<const trngen::TerrainBakeState> state;
	std::array<trngen::CornerLockFlags, 4> locks{};
	std::vector<HeightsRect> rects;
	unsigned threads = 1;
	const std::atomic<bool> *cancel = nullptr;
	// What it came to.
	bool ok = false, cancelled = false;
	std::string error;
	std::shared_ptr<const trngen::TerrainBakeState> seeded; // made here from seed_input
	trngen::RegionBakeResult result;
	std::shared_ptr<const trngen::TerrainBakeState> after;
};

namespace {

std::vector<uint16_t> depth_of(const RasterPlane &plane) {
	std::vector<uint16_t> out(size_t(plane.width()) * size_t(plane.height()));
	if (!out.empty()) plane.copy_rect(0, 0, plane.width(), plane.height(), reinterpret_cast<uint8_t *>(out.data()));
	return out;
}

unsigned bake_threads() {
	const unsigned all = trngen::TerrainBakeInput::for_hardware();
	return all > 1 ? all - 1 : 1; // one left for the editor's own frames
}

bool gesture_open_in(const SessionView &view, const std::string &path) {
	for (const OpenGesture &gesture : view.documents.gestures)
		if (gesture.in(path)) return true;
	return false;
}

// A heights plane's texels hashed (FNV-1a 64), what a seed made from a file's atlas matches a document's input by.
uint64_t heights_content(const RasterPlane &plane) {
	const std::vector<uint16_t> depth = depth_of(plane);
	return io::fnv1a64_bytes(io::kFnv1a64Offset, depth.data(), depth.size() * sizeof(uint16_t));
}

bool same_locks(const std::array<trngen::CornerLockFlags, 4> &a, const std::array<trngen::CornerLockFlags, 4> &b) {
	for (size_t i = 0; i < 4; ++i)
		if (a[i].x != b[i].x || a[i].y != b[i].y) return false;
	return true;
}

bool sculpting(TerrainTool tool) {
	return tool == TerrainTool::Raise || tool == TerrainTool::Lower || tool == TerrainTool::Smooth ||
	       tool == TerrainTool::Flatten || tool == TerrainTool::Set;
}

} // namespace

std::array<trngen::CornerLockFlags, 4> terrain_bake_locks(const SessionCore &core, const std::string &cpt_name,
                                                          std::vector<std::string> *others) {
	std::array<trngen::CornerLockFlags, 4> out{};
	const SessionView &view = core.view();
	if (!view.project.scan || !view.findings.assets) return out;
	std::vector<const AssetEntry *> terrains;
	for (const AssetEntry &entry : view.project.scan->entries)
		if (entry.kind == AssetKind::Terrain) terrains.push_back(&entry);
	std::sort(terrains.begin(), terrains.end(),
	          [](const AssetEntry *a, const AssetEntry *b) { return a->relative_path < b->relative_path; });
	const std::string wanted = strutil::to_lower(basename_of(cpt_name));
	bool found = false;
	for (const AssetEntry *entry : terrains) {
		std::vector<uint8_t> bytes;
		if (!view.findings.assets->read(entry->logical_name, bytes)) continue;
		std::istringstream in(std::string(bytes.begin(), bytes.end()));
		TrnConfig trn;
		std::string error;
		load_trn(in, trn, error);
		if (strutil::to_lower(basename_of(trn.polydata)) != wanted) continue;
		const TerrainQuadrantLocks locks = trn.get_quadrant_locks();
		std::array<trngen::CornerLockFlags, 4> these{};
		for (size_t i = 0; i < 4; ++i) these[i] = {locks[i].x, locks[i].y};
		if (!found) {
			out = these;
			found = true;
			continue;
		}
		bool same = true;
		for (size_t i = 0; i < 4; ++i) same = same && these[i].x == out[i].x && these[i].y == out[i].y;
		if (!same && others) others->push_back(entry->relative_path);
	}
	return out;
}

TerrainSculpt::TerrainSculpt(SessionCore &core) : core_(core) {}

TerrainSculpt::~TerrainSculpt() {
	{
		std::lock_guard<std::mutex> hold(mutex_);
		stopping_ = true;
		cancel_ = true;
	}
	wake_.notify_all();
	if (worker_.joinable()) worker_.join();
}

std::shared_ptr<const trngen::TerrainBakeState> TerrainSculpt::seed_for_(const TerrainHeightsDocument &document,
		const std::array<trngen::CornerLockFlags, 4> &locks) const {
	const RasterPlane &input = document.heights().baked_input;
	uint64_t content = 0;
	bool hashed = false;
	for (const Seed &seed : seeds_) {
		if (seed.path != document.path() || !same_locks(seed.locks, locks)) continue;
		if (seed.input.shares_tiles_with(input)) return seed.state;
		if (!hashed) {
			content = heights_content(input);
			hashed = true;
		}
		if (seed.content == content) return seed.state;
	}
	return nullptr;
}

std::unique_ptr<TerrainSculpt::Job> TerrainSculpt::make_job(const TerrainHeightsDocument &document) const {
	if (document.blocked() || document.is_snapshot() || document.plane_count() < 2) return nullptr;
	const HeightsState &state = document.heights();
	// An image's first bake (its import's .cpt standing until then) is the image form's (A2's heightmap codec).
	if (state.unbaked.empty() || !state.baked) return nullptr;
	auto job = std::make_unique<Job>();
	job->path = document.path();
	job->identity = document.identity();
	job->generation = document.load_generation();
	job->revision = document.revision();
	job->input = document.input();
	job->rects = state.unbaked;
	job->locks = terrain_bake_locks(core_, document.path());
	job->state = state.bake;
	// The state the file's first bake starts from: its seed, made in the background (a region bake waits for it).
	if (!job->state) job->state = seed_for_(document, job->locks);
	if (!job->state) return nullptr;
	job->threads = bake_threads();
	return job;
}

std::unique_ptr<TerrainSculpt::Job> TerrainSculpt::seed_job_(const std::string &path, const RasterPlane &input,
		const std::array<trngen::CornerLockFlags, 4> &locks) const {
	auto job = std::make_unique<Job>();
	job->seed_only = true;
	job->path = path;
	job->seed_input = input;
	job->content = heights_content(input);
	job->locks = locks;
	job->threads = bake_threads();
	for (const Seed &seed : seeds_)
		if (seed.path == path && seed.content == job->content && same_locks(seed.locks, locks)) return nullptr;
	return job;
}

std::unique_ptr<TerrainSculpt::Job> TerrainSculpt::next_seed_() {
	const SessionView &view = core_.view();
	// An open heights document with no bake state: its seed from the input its tiles were made from.
	for (const std::shared_ptr<DocumentBase> &each : core_.documents().documents()) {
		const auto *heights = dynamic_cast<const TerrainHeightsDocument *>(each.get());
		if (!heights || heights->blocked() || heights->is_snapshot() || heights->plane_count() < 2) continue;
		const HeightsState &state = heights->heights();
		if (!state.baked || state.bake) continue;
		const auto locks = terrain_bake_locks(core_, heights->path());
		if (seed_for_(*heights, locks)) continue;
		if (std::unique_ptr<Job> job = seed_job_(heights->path(), state.baked_input, locks)) return job;
	}
	// Sculpt picked over the active view's terrain, its heights file not open yet: seeded from the file's atlas
	// (what the document will open with), so the first stroke's bake does not wait for it.
	if (!sculpting(view.workspace.terrain_brush.tool) || view.documents.active.empty() || !view.findings.assets)
		return nullptr;
	const DocumentBase *active = core_.documents().document_for(view.documents.active);
	if (!active) return nullptr;
	TerrainFamily family;
	if (!resolve_terrain_family(view, *active, family)) return nullptr;
	const TerrainFamilyFile &file = family.plane(TerrainPlane::Heights);
	if (!file.resolved() || !strutil::ends_with_icase(file.path, ".cpt") || core_.documents().document_for(file.path))
		return nullptr;
	const uint64_t stamp = view.findings.assets->stamp(file.name);
	for (const auto &[path, tried] : tried_)
		if (path == file.path && tried == stamp) return nullptr;
	tried_.emplace_back(file.path, stamp);
	if (tried_.size() > 8) tried_.erase(tried_.begin());
	std::vector<uint8_t> bytes;
	CptFile cpt;
	std::string why;
	if (!view.findings.assets->read(file.name, bytes) || !load_cpt(bytes.data(), bytes.size(), cpt, why) ||
	    cpt.depth_buffer.size() != size_t(kHeightsSide) * kHeightsSide)
		return nullptr;
	const RasterPlane atlas = RasterPlane::from_bytes(RasterFormat::U16, kHeightsSide, kHeightsSide,
	                                                  reinterpret_cast<const uint8_t *>(cpt.depth_buffer.data()));
	return seed_job_(file.path, atlas, terrain_bake_locks(core_, file.path));
}

void TerrainSculpt::run(Job &job) {
	std::string why;
	if (job.seed_only) {
		// The file's bake state: a cold in-memory bake of the input its tiles were made from (its .cpt discarded:
		// every tile the strokes do not reach keeps its bytes).
		trngen::TerrainBakeInput input;
		input.depth16 = depth_of(job.seed_input);
		input.locks = job.locks;
		input.threads = job.threads;
		CptFile cold;
		trngen::TerrainBakeState made;
		if (!trngen::bake_terrain_state(input, cold, made, why)) {
			job.error = why;
			return;
		}
		job.seeded = std::make_shared<const trngen::TerrainBakeState>(std::move(made));
		job.ok = true;
		return;
	}
	if (job.cancel && job.cancel->load()) {
		job.cancelled = true;
		return;
	}
	const std::vector<uint16_t> depth = depth_of(job.input);
	trngen::RegionBakeInput region;
	region.depth16 = &depth;
	for (const HeightsRect &rect : job.rects) region.dirty.push_back({rect.x, rect.y, rect.w, rect.h});
	region.threads = job.threads;
	region.cancel = job.cancel;
	if (!trngen::bake_terrain_region(*job.state, region, job.result, why)) {
		job.cancelled = why == "cancelled";
		if (!job.cancelled) job.error = why;
		return;
	}
	trngen::TerrainBakeState after = *job.state;
	trngen::apply_region_bake_state(job.result, after); // the document holds the tiles and the atlas
	job.after = std::make_shared<const trngen::TerrainBakeState>(std::move(after));
	job.ok = true;
}

void TerrainSculpt::work() {
	std::unique_lock<std::mutex> hold(mutex_);
	while (true) {
		wake_.wait(hold, [this] { return stopping_ || queued_; });
		if (stopping_) return;
		running_ = std::move(queued_);
		hold.unlock();
		run(*running_);
		hold.lock();
		finished_ = std::move(running_);
		running_path_.clear();
		wake_.notify_all();
	}
}

void TerrainSculpt::start(std::unique_ptr<Job> job) {
	(job->seed_only ? core_.view().documents.preparing : core_.view().documents.baking) = job->path;
	core_.touch(ViewConcern::Documents);
	{
		std::lock_guard<std::mutex> hold(mutex_);
		cancel_ = false;
		job->cancel = &cancel_;
		running_path_ = job->path;
		running_seed_ = job->seed_only;
		queued_ = std::move(job);
		if (!worker_.joinable()) worker_ = std::thread([this] { work(); });
	}
	wake_.notify_all();
}

bool TerrainSculpt::land(Job &job) {
	DocumentSet &documents = core_.documents();
	if (job.seed_only) {
		if (!job.ok) {
			if (!job.error.empty()) core_.note("Preparing " + basename_of(job.path) + " for sculpting failed: " + job.error);
			return false;
		}
		// The file's seed, kept for its bakes (the last few files').
		seeds_.erase(std::remove_if(seeds_.begin(), seeds_.end(), [&](const Seed &seed) { return seed.path == job.path; }),
		             seeds_.end());
		seeds_.push_back({job.path, job.content, job.locks, job.seed_input, job.seeded});
		if (seeds_.size() > 4) seeds_.erase(seeds_.begin());
		return true;
	}
	auto *document = dynamic_cast<TerrainHeightsDocument *>(documents.document_for(job.path));
	const bool same = document && document->identity() == job.identity &&
	                  document->load_generation() == job.generation && document->revision() == job.revision;
	if (!job.ok) {
		if (!job.error.empty()) core_.note("The bake of " + basename_of(job.path) + " failed: " + job.error);
		return false;
	}
	if (!same) return false;
	auto landing = std::make_shared<HeightsBakeLanding>();
	for (const trngen::DepthRect &rect : job.result.footprints) landing->footprints.push_back({rect.x, rect.y, rect.w, rect.h});
	landing->atlas = std::move(job.result.atlas);
	for (size_t i = 0; i < job.result.rebuilt.size() && i < job.result.tiles.size(); ++i)
		landing->tiles.emplace_back(job.result.rebuilt[i], std::move(job.result.tiles[i]));
	landing->bake = job.after;
	landing->input = job.input;
	landing->settled = job.rects;
	if (!documents.apply_bake_landing(*document, {RasterDocument::edit(landing)})) return false;
	++landed_;
	return true;
}

void TerrainSculpt::poll() {
	std::unique_ptr<Job> done;
	bool busy = false;
	{
		std::lock_guard<std::mutex> hold(mutex_);
		done = std::move(finished_);
		busy = queued_ || running_;
	}
	if (done) land(*done);
	if (!busy && (!core_.view().documents.baking.empty() || !core_.view().documents.preparing.empty())) {
		core_.view().documents.baking.clear();
		core_.view().documents.preparing.clear();
		core_.touch(ViewConcern::Documents);
	}
	if (busy || !core_.view().project.open) return;
	// A stroke's regions baked once it ends, over the file's state or its seed.
	for (const std::shared_ptr<DocumentBase> &each : core_.documents().documents()) {
		const auto *heights = dynamic_cast<const TerrainHeightsDocument *>(each.get());
		if (!heights || heights->heights().unbaked.empty() || gesture_open_in(core_.view(), heights->path())) continue;
		if (std::unique_ptr<Job> job = make_job(*heights)) {
			start(std::move(job));
			return;
		}
	}
	// Else a seed made in the background: a stroke goes on meanwhile, its bake waiting for it.
	if (std::unique_ptr<Job> job = next_seed_()) start(std::move(job));
}

void TerrainSculpt::interrupt(const std::string &path) {
	std::lock_guard<std::mutex> hold(mutex_);
	// A seed is never stopped: the file's every bake waits for it.
	if (queued_ && queued_->path == path && !queued_->seed_only) {
		queued_.reset();
		running_path_.clear();
	}
	if (running_path_ == path && !running_seed_) cancel_ = true;
}

bool TerrainSculpt::settle(const std::string &path, std::string &error) {
	// A bake running for the file: waited for, then landed; one queued and not started: run here instead.
	std::unique_ptr<Job> done;
	{
		std::unique_lock<std::mutex> hold(mutex_);
		if (queued_ && queued_->path == path && !queued_->seed_only) {
			queued_.reset();
			running_path_.clear();
		}
		wake_.wait(hold, [this, &path] { return running_path_ != path; });
		if (finished_ && finished_->path == path) done = std::move(finished_);
	}
	if (done) land(*done);
	if (core_.view().documents.baking == path || core_.view().documents.preparing == path) {
		core_.view().documents.baking.clear();
		core_.view().documents.preparing.clear();
		core_.touch(ViewConcern::Documents);
	}
	auto *document = dynamic_cast<TerrainHeightsDocument *>(core_.documents().document_for(path));
	if (!document || document->heights().unbaked.empty()) return true;
	std::unique_ptr<Job> job = make_job(*document);
	if (!job && document->heights().baked && !document->heights().bake) {
		// No seed yet: made here, the caller waiting.
		if (std::unique_ptr<Job> seed =
		            seed_job_(path, document->heights().baked_input, terrain_bake_locks(core_, path))) {
			core_.note("Preparing " + basename_of(path) + " for sculpting before writing it");
			run(*seed);
			land(*seed);
		}
		job = make_job(*document);
	}
	if (!job) {
		error = basename_of(path) + " has regions whose bake is due and cannot bake now.";
		return false;
	}
	core_.note("Baking " + basename_of(path) + " before writing it");
	run(*job);
	if (!land(*job)) {
		error = !job->error.empty() ? job->error
		                            : "the bake of " + basename_of(path) + " could not land in its stroke's step";
		return false;
	}
	return true;
}

void TerrainSculpt::remember_flatten(uint64_t gesture, int32_t raw) {
	for (auto &each : flatten_)
		if (each.first == gesture) {
			each.second = raw;
			return;
		}
	if (flatten_.size() >= 8) flatten_.erase(flatten_.begin());
	flatten_.emplace_back(gesture, raw);
}

bool TerrainSculpt::flatten_target(uint64_t gesture, int32_t &raw) const {
	for (const auto &each : flatten_)
		if (each.first == gesture) {
			raw = each.second;
			return true;
		}
	return false;
}

std::string TerrainSculpt::bake_words(const std::string &path) const {
	{
		std::lock_guard<std::mutex> hold(mutex_);
		if (running_path_ == path)
			return running_seed_ ? "Preparing " + basename_of(path) + " for sculpting..." : "Baking " + basename_of(path) + "...";
	}
	const auto *document = dynamic_cast<const TerrainHeightsDocument *>(core_.documents().document_for(path));
	if (!document) return std::string();
	const size_t due = document->heights().unbaked.size();
	if (!due) return std::string();
	return std::to_string(due) + (due == 1 ? " region waits" : " regions wait") + " for its bake (it starts as the stroke ends).";
}

} // namespace opennova::editor
