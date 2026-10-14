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
#include <editor/documents/mission_document.h>
#include <editor/session/problems_service.h>
#include <editor/session/request_factories.h>
#include <editor/session/session_core.h>
#include <editor/session/view/session_view.h>
#include <formats/cpt/cpt_io.h>
#include <base/io/file_io.h>
#include <editor/session/terrain_family.h>
#include <editor/session/terrain_follow.h>
#include <editor/session/terrain_uses.h>
#include <formats/cpt/trngen/heightmap_depth.h>
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
	// A heightmap image's (S25 A2b). Its seed: the .cpt its import makes, whose header the whole bake takes, and the
	// whole bake made (seeded_cpt). Its first region bake: that whole bake, laid under the regions (base).
	std::string cpt;
	CptFile::Header names;
	DepthFormat depth_format = DepthFormat::CDEP;
	std::shared_ptr<const CptFile> seeded_cpt;
	std::shared_ptr<const CptFile> base;
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

// A header's text field as a string (up to its NUL).
std::string header_text(const char *field, size_t size) { return std::string(field, strnlen(field, size)); }

// The header a heightmap image's import writes its .cpt with, read from that .cpt (the file itself: the image's
// document stands in for it through the project's files once the editor bakes); the import's own where it does not
// read (terrain_import.cpp's: the stem, "OpenNova", "opennova", CDEP).
void import_header(const SessionView &view, const std::string &cpt, CptFile::Header &out, DepthFormat &format) {
	out = CptFile::Header();
	format = DepthFormat::CDEP;
	std::vector<uint8_t> bytes;
	std::string why;
	CptFile read;
	if (io::read_file_bytes(join_path(view.project.root, cpt), bytes, why) && load_cpt(bytes.data(), bytes.size(), read, why)) {
		out = read.header;
		format = read.depth_format;
		return;
	}
	const std::string stem = basename_of(cpt).substr(0, basename_of(cpt).rfind('.'));
	std::strncpy(out.terrain_name, stem.c_str(), sizeof(out.terrain_name) - 1);
	std::strncpy(out.creator, "OpenNova", sizeof(out.creator) - 1);
	std::strncpy(out.version, "opennova", sizeof(out.version) - 1);
}

// What a seed holds, in bytes: its bake nodes and, an image's, its whole .cpt.
size_t seed_bytes(const trngen::TerrainBakeState &state, const CptFile *cpt) {
	size_t bytes = 0;
	for (const auto &node : state.nodes)
		if (node)
			bytes += sizeof(*node) + node->section.faces.size() * sizeof(uint16_t) +
			         node->section.records.size() * sizeof(float) + node->draw_back.positions.size() * sizeof(float) +
			         node->draw_back.faces.size() * sizeof(uint16_t);
	if (cpt) {
		bytes += cpt->depth_buffer.size() * sizeof(uint16_t);
		for (const CptTile &tile : cpt->tiles) {
			bytes += tile.vertex_indices.size() * sizeof(uint16_t);
			for (const CptTileLOD &lod : tile.lods) bytes += lod.indices.size() * sizeof(uint16_t);
		}
	}
	return bytes;
}

// The seeds kept: the last few files' within this many bytes, the newest always.
constexpr size_t kSeedBytes = size_t(512) << 20;
constexpr size_t kSeedCount = 4;

bool sculpting(TerrainTool tool) {
	return tool == TerrainTool::Raise || tool == TerrainTool::Lower || tool == TerrainTool::Smooth ||
	       tool == TerrainTool::Flatten || tool == TerrainTool::Set;
}

} // namespace

// The quadrant lock flags of the first .trn (by its path) naming the .cpt `cpt_name` as its polytrn_polydata, from the
// session's view of the project's files; none where no .trn names it. `others` names each later .trn naming it with
// other flags (the conflict noted).
static std::array<trngen::CornerLockFlags, 4> terrain_bake_locks(const SessionCore &core, const std::string &cpt_name,
                                                          std::vector<std::string> *others = nullptr) {
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

const TerrainSculpt::Seed *TerrainSculpt::seed_for_(const TerrainHeightsDocument &document,
		const std::array<trngen::CornerLockFlags, 4> &locks) {
	const RasterPlane &input = document.heights().baked_input;
	uint64_t content = 0;
	bool hashed = false;
	for (Seed &seed : seeds_) {
		if (seed.path != document.path() || !same_locks(seed.locks, locks) || (document.image() && !seed.cpt)) continue;
		if (seed.input.shares_tiles_with(input)) return &seed;
		if (!hashed) {
			content = heights_content(input);
			hashed = true;
		}
		if (seed.content == content) {
			seed.input = input; // matched by its tiles from here on, not hashed each poll
			return &seed;
		}
	}
	return nullptr;
}

std::unique_ptr<TerrainSculpt::Job> TerrainSculpt::make_job(const TerrainHeightsDocument &document, bool force) {
	if (document.blocked() || document.is_snapshot() || document.plane_count() < 2) return nullptr;
	const HeightsState &state = document.heights();
	// A .cpt holds its bake as read; an image's first bake (its import's .cpt standing until then) lays its seed's.
	if (state.unbaked.empty() || (!state.baked && !document.image())) return nullptr;
	// A redo kept past the last step brings back the step the bake would land in: the bake waits (but for Save's).
	if (!force && document.can_redo()) return nullptr;
	// The bake failed on these heights: not again until they change.
	for (const Failure &failure : failures_)
		if (!failure.seed && failure.path == document.path() && failure.identity == document.identity() &&
		    failure.load == document.load_generation() && failure.revision == document.revision())
			return nullptr;
	auto job = std::make_unique<Job>();
	job->path = document.path();
	job->identity = document.identity();
	job->generation = document.load_generation();
	job->revision = document.revision();
	job->input = document.input();
	job->rects = state.unbaked;
	job->locks = locks_for_(document.cpt_path());
	job->state = state.bake;
	// The state the file's first bake starts from: its seed, made in the background (a region bake waits for it); an
	// image's whole bake laid under the regions.
	if (!job->state) {
		const Seed *seed = seed_for_(document, job->locks);
		if (!seed) return nullptr;
		job->state = seed->state;
		if (!state.baked) job->base = seed->cpt;
	}
	job->threads = bake_threads();
	return job;
}

std::unique_ptr<TerrainSculpt::Job> TerrainSculpt::seed_job_(const std::string &path, const RasterPlane &input,
		const std::array<trngen::CornerLockFlags, 4> &locks, const std::string &cpt) {
	auto job = std::make_unique<Job>();
	job->seed_only = true;
	job->path = path;
	job->seed_input = input;
	job->content = heights_content(input);
	job->locks = locks;
	job->threads = bake_threads();
	job->cpt = cpt;
	for (const Seed &seed : seeds_)
		if (seed.path == path && seed.content == job->content && same_locks(seed.locks, locks) && (cpt.empty() || seed.cpt))
			return nullptr;
	// One that failed on this input: not again.
	for (const Failure &failure : failures_)
		if (failure.seed && failure.path == path && failure.content == job->content) return nullptr;
	if (!cpt.empty()) import_header(core_.view(), cpt, job->names, job->depth_format);
	return job;
}

bool TerrainSculpt::sculpted_(const TerrainHeightsDocument &document) const {
	const SessionView &view = core_.view();
	// An import's output takes no stroke (its source does).
	if (view.project.scan)
		if (const AssetEntry *entry = view.project.scan->at_path(document.path()); entry && !entry->imported_from.empty())
			return false;
	if (!document.heights().unbaked.empty()) return true;
	if (!sculpting(view.workspace.terrain_brush.tool) || view.documents.active.empty()) return false;
	if (view.documents.active == document.path()) return true;
	const DocumentBase *active = core_.documents().document_for(view.documents.active);
	TerrainFamily family;
	return active && resolve_terrain_family(view, *active, family) &&
	       family.plane(TerrainPlane::Heights).path == document.path();
}

std::array<trngen::CornerLockFlags, 4> TerrainSculpt::locks_for_(const std::string &cpt) {
	const SessionView &view = core_.view();
	const uint64_t generation = view.findings.assets ? view.findings.assets->generation() : 0;
	const std::string name = strutil::to_lower(basename_of(cpt));
	for (const LocksMemo &memo : locks_memo_)
		if (memo.cpt == name && memo.generation == generation) return memo.locks;
	LocksMemo memo;
	memo.cpt = name;
	memo.generation = generation;
	memo.locks = terrain_bake_locks(core_, cpt);
	locks_memo_.erase(std::remove_if(locks_memo_.begin(), locks_memo_.end(), [&](const LocksMemo &each) { return each.cpt == name; }),
	                  locks_memo_.end());
	locks_memo_.push_back(memo);
	if (locks_memo_.size() > 8) locks_memo_.erase(locks_memo_.begin());
	return memo.locks;
}

std::unique_ptr<TerrainSculpt::Job> TerrainSculpt::next_seed_() {
	const SessionView &view = core_.view();
	// An open heights document with no bake state: its seed from the input its tiles were made from.
	for (const std::shared_ptr<DocumentBase> &each : core_.documents().documents()) {
		const auto *heights = dynamic_cast<const TerrainHeightsDocument *>(each.get());
		if (!heights || heights->blocked() || heights->is_snapshot() || heights->plane_count() < 2) continue;
		const HeightsState &state = heights->heights();
		if (state.bake || (!state.baked && !heights->image()) || !sculpted_(*heights)) continue;
		const auto locks = locks_for_(heights->cpt_path());
		if (seed_for_(*heights, locks)) continue;
		if (std::unique_ptr<Job> job =
		            seed_job_(heights->path(), state.baked_input, locks, heights->image() ? heights->import_cpt() : std::string()))
			return job;
	}
	// Sculpt picked over the active view's terrain, its heights file not open yet: seeded from the file's atlas
	// (what the document will open with), so the first stroke's bake does not wait for it.
	if (!sculpting(view.workspace.terrain_brush.tool) || view.documents.active.empty() || !view.findings.assets)
		return nullptr;
	// (A heights file an import makes is sculpted through its source: the family names that.)
	const DocumentBase *active = core_.documents().document_for(view.documents.active);
	if (!active) return nullptr;
	TerrainFamily family;
	if (!resolve_terrain_family(view, *active, family)) return nullptr;
	const TerrainFamilyFile &file = family.plane(TerrainPlane::Heights);
	if (!file.resolved() || core_.documents().document_for(file.path)) return nullptr;
	const AssetEntry *entry = view.project.scan ? view.project.scan->at_path(file.path) : nullptr;
	if (!entry) return nullptr;
	const uint64_t stamp = io::splitmix64(entry->size_bytes ^ io::splitmix64(uint64_t(entry->modified_ticks))) | 1;
	for (const auto &[path, tried] : tried_)
		if (path == file.path && tried == stamp) return nullptr;
	tried_.emplace_back(file.path, stamp);
	if (tried_.size() > 8) tried_.erase(tried_.begin());
	std::vector<uint8_t> bytes;
	std::string why;
	if (!io::read_file_bytes(join_path(view.project.root, file.path), bytes, why)) return nullptr;
	// An imported terrain's heightmap (A2b): decoded under its import's white, as its document will read it.
	if (entry->kind == AssetKind::HeightmapInput) {
		HeightmapImport import;
		trngen::HeightmapDepth depth;
		if (!heightmap_import_of(view, file.path, import) || import.cpt.empty() ||
		    !trngen::decode_heightmap_depth(file.path, bytes, import.top, depth, why))
			return nullptr;
		const std::vector<uint16_t> raw16 = depth.depth16.empty() ? trngen::smooth_depthmap(depth.depth8) : depth.depth16;
		const RasterPlane input = RasterPlane::from_bytes(RasterFormat::U16, kHeightsSide, kHeightsSide,
		                                                  reinterpret_cast<const uint8_t *>(raw16.data()));
		return seed_job_(file.path, input, locks_for_(import.cpt), import.cpt);
	}
	CptFile cpt;
	if (!strutil::ends_with_icase(file.path, ".cpt") || !load_cpt(bytes.data(), bytes.size(), cpt, why) ||
	    cpt.depth_buffer.size() != size_t(kHeightsSide) * kHeightsSide)
		return nullptr;
	const RasterPlane atlas = RasterPlane::from_bytes(RasterFormat::U16, kHeightsSide, kHeightsSide,
	                                                  reinterpret_cast<const uint8_t *>(cpt.depth_buffer.data()));
	return seed_job_(file.path, atlas, locks_for_(file.path));
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
		input.cancel = job.cancel; // a stroke never sets it for a seed; closing the session does
		// An image's: its import's header, so the whole bake is the .cpt the import makes of the image.
		if (!job.cpt.empty()) {
			input.terrain_name = header_text(job.names.terrain_name, sizeof(job.names.terrain_name));
			input.creator = header_text(job.names.creator, sizeof(job.names.creator));
			input.version = header_text(job.names.version, sizeof(job.names.version));
			input.depth_format = job.depth_format;
		}
		CptFile cold;
		trngen::TerrainBakeState made;
		if (!trngen::bake_terrain_state(input, cold, made, why)) {
			job.cancelled = why == "cancelled";
			if (!job.cancelled) job.error = why;
			return;
		}
		job.seeded = std::make_shared<const trngen::TerrainBakeState>(std::move(made));
		if (!job.cpt.empty()) job.seeded_cpt = std::make_shared<const CptFile>(std::move(cold));
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
		// A test's hold: the job waits here (wait_idle returns meanwhile) until the test lets it go or the session
		// closes.
		if (test_hold_) {
			held_ = true;
			wake_.notify_all();
			wake_.wait(hold, [this] { return stopping_ || !test_hold_; });
			held_ = false;
		}
		hold.unlock();
		run(*running_);
		hold.lock();
		if (test_hold_ && running_->cancelled) ++test_hold_->cancelled;
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
		++started_;
		if (!worker_.joinable()) worker_ = std::thread([this] { work(); });
	}
	wake_.notify_all();
}

void TerrainSculpt::failed_(const Job &job, TerrainHeightsDocument *document) {
	Failure failure;
	failure.path = job.path;
	failure.seed = job.seed_only;
	failure.identity = job.identity;
	failure.load = job.generation;
	failure.revision = job.revision;
	failure.content = job.content;
	failure.why = job.error;
	failures_.push_back(failure);
	if (failures_.size() > 32) failures_.erase(failures_.begin());
	core_.note((job.seed_only ? "Preparing " + basename_of(job.path) + " for sculpting failed: " : "The bake of " +
	                    basename_of(job.path) + " failed: ") + job.error + " (not tried again until its heights change)");
	if (document) {
		document->set_bake_refusal(job.error);
		core_.problems().validate_later();
	}
}

bool TerrainSculpt::land(Job &job, bool force, std::vector<std::string> *followed) {
	DocumentSet &documents = core_.documents();
	auto *document = dynamic_cast<TerrainHeightsDocument *>(documents.document_for(job.path));
	if (job.seed_only) {
		if (!job.ok) {
			if (!job.cancelled && !job.error.empty()) failed_(job, document);
			return false;
		}
		// An image's whole bake against the .cpt its import made: a difference is an import not run since the image
		// (or the importer) changed, which the first landing's bake replaces with what the import would make.
		if (job.seeded_cpt) {
			std::vector<uint8_t> made, file;
			std::string why;
			if (save_cpt(*job.seeded_cpt, made, why) &&
			    io::read_file_bytes(join_path(core_.view().project.root, job.cpt), file, why) && made != file)
				core_.note(basename_of(job.cpt) + " is not what its import makes of " + basename_of(job.path) +
				           " now: import it again, or sculpt on and its bake shows the terrain the import would make.");
		}
		// The file's seed, kept for its bakes (the last few files' within the budget, the newest always).
		seeds_.erase(std::remove_if(seeds_.begin(), seeds_.end(), [&](const Seed &seed) { return seed.path == job.path; }),
		             seeds_.end());
		seeds_.push_back({job.path, job.content, job.locks, job.seed_input, job.seeded, job.seeded_cpt,
		                  seed_bytes(*job.seeded, job.seeded_cpt.get())});
		size_t bytes = 0;
		for (const Seed &seed : seeds_) bytes += seed.bytes;
		while (seeds_.size() > 1 && (seeds_.size() > kSeedCount || bytes > kSeedBytes)) {
			bytes -= seeds_.front().bytes;
			seeds_.erase(seeds_.begin());
		}
		return true;
	}
	const bool same = document && document->identity() == job.identity &&
	                  document->load_generation() == job.generation && document->revision() == job.revision;
	if (!job.ok) {
		if (!job.cancelled && !job.error.empty() && same) failed_(job, document);
		return false;
	}
	if (!same) return false;
	// A redo kept past the last step: the bake waits for it (but for Save's, which lands as a step of its own).
	if (!force && document->can_redo()) return false;
	auto landing = std::make_shared<HeightsBakeLanding>();
	for (const trngen::DepthRect &rect : job.result.footprints) landing->footprints.push_back({rect.x, rect.y, rect.w, rect.h});
	landing->atlas = std::move(job.result.atlas);
	for (size_t i = 0; i < job.result.rebuilt.size() && i < job.result.tiles.size(); ++i)
		landing->tiles.emplace_back(job.result.rebuilt[i], std::move(job.result.tiles[i]));
	landing->bake = job.after;
	landing->input = job.input;
	landing->settled = job.rects;
	// Where the stroke's step cannot take it (saved since, a redo kept past it): a step of its own, its link kept.
	landing->alone = !document->can_amend();
	landing->link = document->undo_mark().link;
	// An image's first landing lays the whole bake its regions were baked over.
	if (!document->heights().baked) {
		if (!job.base) return false;
		landing->base = job.base;
	}
	// The ground the missions on the file read, before and after (the follow pass): an image's first, the .cpt its
	// import made; each as the .cpt written reads back (a fresh read's, the original editor's).
	std::vector<uint16_t> before = landing->base ? landing->base->depth_buffer : depth_of(document->atlas());
	before.resize(size_t(kHeightsSide) * kHeightsSide, 0);
	document->clamp_as_written(before);
	if (!documents.apply_bake_landing(*document, {RasterDocument::edit(landing)}, "Baked " + basename_of(job.path)))
		return false;
	++landed_;
	document->set_bake_refusal(std::string());
	std::vector<uint16_t> after = depth_of(document->atlas());
	document->clamp_as_written(after);
	FollowGround ground;
	ground.cpt = document->cpt_path();
	ground.view = document->path();
	ground.before = &before;
	ground.after = &after;
	ground.link = document->undo_mark().link;
	// The ground's window (none where no stroke opened one: an undo brought regions due back): every entity follows.
	const auto open = windows_.find(document->path());
	ground.marks = open != windows_.end() ? &open->second : nullptr;
	FollowResult result = follow_the_ground(core_, ground);
	if (followed) followed->insert(followed->end(), result.missions.begin(), result.missions.end());
	for (FollowDeferred &deferred : result.deferred) deferred_.push_back(std::move(deferred));
	// The window closes with the last due region; while regions stay due, the rows its follows moved are no edit of the
	// user's at the next landing.
	if (document->heights().unbaked.empty()) {
		windows_.erase(document->path());
	} else if (open != windows_.end()) {
		for (auto &[mission, rows] : result.rows) {
			FollowMark &mark = open->second[mission];
			mark.followed.insert(mark.followed.end(), rows.begin(), rows.end());
			std::sort(mark.followed.begin(), mark.followed.end());
		}
	}
	return true;
}

void TerrainSculpt::stroke_began(const TerrainHeightsDocument &document) {
	if (windows_.count(document.path())) return;
	std::map<std::string, FollowMark> &window = windows_[document.path()];
	for (const std::shared_ptr<DocumentBase> &each : core_.documents().documents())
		if (const auto *mission = dynamic_cast<const MissionDocument *>(each.get()))
			window[mission->path()] = FollowMark{mission->identity(), mission->load_generation(), mission->revision(), {}};
}

void TerrainSculpt::refresh_imports_() {
	const SessionView &view = core_.view();
	if (view.project.scan.get() == imports_scan_) return;
	imports_scan_ = view.project.scan.get();
	// An open heightmap whose import's white (or .cpt) changed: read again under it where it holds no edits; else kept,
	// and said (its write would no longer decode to what it shows).
	std::vector<std::string> reread;
	for (const std::shared_ptr<DocumentBase> &each : core_.documents().documents()) {
		auto *heights = dynamic_cast<TerrainHeightsDocument *>(each.get());
		if (!heights || !heights->image()) continue;
		HeightmapImport import;
		heightmap_import_of(view, heights->path(), import);
		if (import.top == heights->import_top() && import.cpt == heights->import_cpt()) continue;
		if (heights->unsaved()) {
			core_.note("The import of " + basename_of(heights->path()) + " changed its white: its unsaved sculpt keeps the "
			           "old one until it is saved or read again.");
			continue;
		}
		reread.push_back(heights->path());
	}
	for (const std::string &path : reread) core_.documents().open_document(request::reload_document(path));
}

void TerrainSculpt::poll() {
	std::unique_ptr<Job> done;
	bool busy = false;
	{
		std::lock_guard<std::mutex> hold(mutex_);
		done = std::move(finished_);
		busy = queued_ || running_;
	}
	if (done) land(*done, false);
	if (!busy && (!core_.view().documents.baking.empty() || !core_.view().documents.preparing.empty())) {
		core_.view().documents.baking.clear();
		core_.view().documents.preparing.clear();
		core_.touch(ViewConcern::Documents);
	}
	if (!core_.view().project.open) return;
	// The follows a drag held back, once it has ended.
	deferred_.erase(std::remove_if(deferred_.begin(), deferred_.end(),
	                               [&](const FollowDeferred &each) { return apply_deferred_follow(core_, each); }),
	                deferred_.end());
	// A refusal said for heights that changed since: gone.
	for (const std::shared_ptr<DocumentBase> &each : core_.documents().documents()) {
		auto *heights = dynamic_cast<TerrainHeightsDocument *>(each.get());
		if (!heights || heights->bake_refusal().empty()) continue;
		bool still = false;
		for (const Failure &failure : failures_)
			still = still || (failure.path == heights->path() &&
			                  (failure.seed || (failure.identity == heights->identity() &&
			                                    failure.load == heights->load_generation() &&
			                                    failure.revision == heights->revision())));
		if (!still) {
			heights->set_bake_refusal(std::string());
			core_.problems().validate_later();
		}
	}
	refresh_imports_();
	if (busy) return;
	// A stroke's regions baked once it ends, over the file's state or its seed.
	for (const std::shared_ptr<DocumentBase> &each : core_.documents().documents()) {
		const auto *heights = dynamic_cast<const TerrainHeightsDocument *>(each.get());
		if (!heights || heights->heights().unbaked.empty() || gesture_open_in(core_.view(), heights->path())) continue;
		if (std::unique_ptr<Job> job = make_job(*heights, false)) {
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

bool TerrainSculpt::settle(const std::string &path, std::string &error, std::vector<std::string> *followed) {
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
	if (done) land(*done, true, followed);
	if (core_.view().documents.baking == path || core_.view().documents.preparing == path) {
		core_.view().documents.baking.clear();
		core_.view().documents.preparing.clear();
		core_.touch(ViewConcern::Documents);
	}
	auto *document = dynamic_cast<TerrainHeightsDocument *>(core_.documents().document_for(path));
	if (!document || document->heights().unbaked.empty()) return true;
	// The bake refused these heights already: so it stays.
	for (const Failure &failure : failures_)
		if (failure.path == path &&
		    (failure.seed || (failure.identity == document->identity() && failure.load == document->load_generation() &&
		                      failure.revision == document->revision()))) {
			error = failure.why;
			return false;
		}
	std::unique_ptr<Job> job = make_job(*document, true);
	if (!job && (document->heights().baked || document->image()) && !document->heights().bake) {
		// No seed yet: made here, the caller waiting.
		if (std::unique_ptr<Job> seed = seed_job_(path, document->heights().baked_input, locks_for_(document->cpt_path()),
		                                          document->image() ? document->import_cpt() : std::string())) {
			core_.note("Preparing " + basename_of(path) + " for sculpting before writing it");
			run(*seed);
			if (!land(*seed, true)) {
				error = seed->error.empty() ? "its bake state could not be made" : seed->error;
				return false;
			}
		}
		job = make_job(*document, true);
	}
	if (!job) {
		error = basename_of(path) + " has regions whose bake is due and cannot bake now";
		return false;
	}
	core_.note("Baking " + basename_of(path) + " before writing it");
	run(*job);
	if (!land(*job, true, followed)) {
		error = !job->error.empty() ? job->error : "the bake of " + basename_of(path) + " could not land";
		return false;
	}
	return true;
}

void TerrainSculpt::wait_idle() {
	// The worker notifies after each job it finishes and as it holds one for a test; a job interrupt() takes back from
	// the queue is the session thread's own, so nothing is left to wake for.
	std::unique_lock<std::mutex> hold(mutex_);
	wake_.wait(hold, [this] { return !queued_ && (!running_ || held_); });
}

void TerrainSculpt::hold_for_test(BakeTestHold *hold) {
	{
		std::lock_guard<std::mutex> lock(mutex_);
		test_hold_ = hold;
		// Let go: the job the worker holds runs now (a wait_idle after this waits for it).
		if (!hold) held_ = false;
	}
	wake_.notify_all();
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
