#include <editor/session/terrain_sculpt.h>

#include <algorithm>
#include <cstring>
#include <sstream>
#include <utility>

#include <base/io/strutil.h>
#include <editor/assets/project_asset_source.h>
#include <editor/project/project_files.h>
#include <editor/session/document_set.h>
#include <editor/session/session_core.h>
#include <editor/session/view/session_view.h>
#include <formats/cpt/trngen/terrain_bake.h>
#include <formats/trn/trn_io.h>

namespace opennova::editor {

struct TerrainSculpt::Job {
	std::string path;
	uint64_t identity = 0, generation = 0, revision = 0;
	RasterPlane input;      // the input to bake
	RasterPlane seed_input; // the input the bake state is made from, where it has none yet
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
	job->state = state.bake;
	if (!job->state) {
		for (const Seed &seed : seeds_)
			if (seed.identity == job->identity && seed.generation == job->generation &&
			    seed.input.shares_tiles_with(state.baked_input))
				job->state = seed.state;
		if (!job->state) job->seed_input = state.baked_input;
	}
	job->locks = terrain_bake_locks(core_, document.path());
	job->threads = bake_threads();
	return job;
}

void TerrainSculpt::run(Job &job) {
	std::string why;
	if (!job.state) {
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
		job.state = job.seeded;
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
	CptFile none; // the state alone: the document holds the tiles and the atlas
	trngen::apply_region_bake(job.result, after, none);
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
	{
		std::lock_guard<std::mutex> hold(mutex_);
		cancel_ = false;
		job->cancel = &cancel_;
		running_path_ = job->path;
		queued_ = std::move(job);
		if (!worker_.joinable()) worker_ = std::thread([this] { work(); });
	}
	wake_.notify_all();
}

bool TerrainSculpt::land(Job &job) {
	DocumentSet &documents = core_.documents();
	auto *document = dynamic_cast<TerrainHeightsDocument *>(documents.document_for(job.path));
	const bool same = document && document->identity() == job.identity &&
	                  document->load_generation() == job.generation && document->revision() == job.revision;
	// A first bake's state outlives a dropped landing: the file's next bake starts from it.
	if (job.seeded && !(job.ok && same)) {
		seeds_.erase(std::remove_if(seeds_.begin(), seeds_.end(), [&](const Seed &seed) { return seed.identity == job.identity; }),
		             seeds_.end());
		seeds_.push_back({job.identity, job.generation, job.seed_input, job.seeded});
	}
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
	seeds_.erase(std::remove_if(seeds_.begin(), seeds_.end(), [&](const Seed &seed) { return seed.identity == job.identity; }),
	             seeds_.end());
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
	if (busy || !core_.view().project.open) return;
	for (const std::shared_ptr<DocumentBase> &each : core_.documents().documents()) {
		const auto *heights = dynamic_cast<const TerrainHeightsDocument *>(each.get());
		if (!heights || heights->heights().unbaked.empty() || gesture_open_in(core_.view(), heights->path())) continue;
		if (std::unique_ptr<Job> job = make_job(*heights)) {
			start(std::move(job));
			return;
		}
	}
}

void TerrainSculpt::interrupt(const std::string &path) {
	std::lock_guard<std::mutex> hold(mutex_);
	if (queued_ && queued_->path == path) {
		queued_.reset();
		running_path_.clear();
	}
	if (running_path_ == path) cancel_ = true;
}

bool TerrainSculpt::settle(const std::string &path, std::string &error) {
	// A bake running for the file: waited for, then landed; one queued and not started: run here instead.
	std::unique_ptr<Job> done;
	{
		std::unique_lock<std::mutex> hold(mutex_);
		if (queued_ && queued_->path == path) {
			queued_.reset();
			running_path_.clear();
		}
		wake_.wait(hold, [this, &path] { return running_path_ != path; });
		if (finished_ && finished_->path == path) done = std::move(finished_);
	}
	if (done) land(*done);
	auto *document = dynamic_cast<TerrainHeightsDocument *>(core_.documents().document_for(path));
	if (!document || document->heights().unbaked.empty()) return true;
	std::unique_ptr<Job> job = make_job(*document);
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
		if (running_path_ == path) return "Baking " + basename_of(path) + "...";
	}
	const auto *document = dynamic_cast<const TerrainHeightsDocument *>(core_.documents().document_for(path));
	if (!document) return std::string();
	const size_t due = document->heights().unbaked.size();
	if (!due) return std::string();
	return std::to_string(due) + (due == 1 ? " region waits" : " regions wait") + " for its bake (it starts as the stroke ends).";
}

} // namespace opennova::editor
